// 文件：server/include/database.h
#ifndef SECURE_CHAT_DATABASE_H
#define SECURE_CHAT_DATABASE_H

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "../common/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// 数据库配置
typedef struct {
    char *database_path;
    char *backup_path;
    int backup_interval_hours;
    int max_backup_files;
    int vacuum_threshold_mb;
    int connection_timeout_ms;
    int max_connections;
    bool enable_wal;  // Write-Ahead Logging
    bool enable_foreign_keys;
    bool enable_auto_vacuum;
} database_config_t;

// 数据库连接池
typedef struct {
    sqlite3 *write_conn;
    sqlite3 **read_conns;
    int read_conn_count;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool initialized;
    bool shutting_down;
    database_config_t config;
} database_pool_t;

// 事务句柄
typedef struct {
    sqlite3 *conn;
    bool is_active;
    bool is_readonly;
    int64_t transaction_id;
} database_transaction_t;

// 查询结果
typedef struct {
    int column_count;
    char **column_names;
    char ***rows;
    int row_count;
    int affected_rows;
    int64_t last_insert_id;
    char *error_msg;
} query_result_t;

// 数据库统计
typedef struct {
    int64_t total_users;
    int64_t total_messages;
    int64_t total_groups;
    int64_t total_files;
    double database_size_mb;
    double cache_hit_rate;
    int64_t query_count;
    int64_t transaction_count;
    int64_t backup_count;
} database_stats_t;

// 初始化函数
database_pool_t *database_init(const database_config_t *config);
bool database_shutdown(database_pool_t *pool);

// 连接管理
sqlite3 *database_get_write_connection(database_pool_t *pool);
sqlite3 *database_get_read_connection(database_pool_t *pool);
void database_release_connection(database_pool_t *pool, sqlite3 *conn);

// 事务管理
database_transaction_t *database_begin_transaction(database_pool_t *pool, bool read_only);
bool database_commit_transaction(database_transaction_t *trans);
bool database_rollback_transaction(database_transaction_t *trans);
void database_free_transaction(database_transaction_t *trans);

// 查询执行
query_result_t *database_execute_query(database_pool_t *pool, 
                                      const char *sql, 
                                      const char **params, 
                                      int param_count);
bool database_execute_update(database_pool_t *pool,
                           const char *sql,
                           const char **params,
                           int param_count);

// 预编译语句
typedef struct prepared_statement prepared_statement_t;
prepared_statement_t *database_prepare_statement(database_pool_t *pool, const char *sql);
bool database_bind_statement(prepared_statement_t *stmt, int index, const char *value);
query_result_t *database_execute_statement(prepared_statement_t *stmt);
void database_free_statement(prepared_statement_t *stmt);

// 用户操作
bool database_create_user(database_pool_t *pool,
                         const char *username,
                         const char *password_hash,
                         const char *password_salt,
                         const char *email,
                         const char *nickname,
                         user_info_t *user_info);

bool database_authenticate_user(database_pool_t *pool,
                               const char *username,
                               const char *password_hash,
                               user_info_t *user_info);

bool database_get_user_by_id(database_pool_t *pool, uint64_t user_id, user_info_t *user_info);
bool database_get_user_by_username(database_pool_t *pool, const char *username, user_info_t *user_info);
bool database_update_user_status(database_pool_t *pool, uint64_t user_id, user_status_t status, const char *status_message);
bool database_update_user_last_seen(database_pool_t *pool, uint64_t user_id, uint64_t timestamp);

// 会话管理
bool database_create_session(database_pool_t *pool,
                            uint64_t user_id,
                            const char *access_token,
                            const char *refresh_token,
                            const char *client_ip,
                            const char *user_agent,
                            uint64_t expires_at,
                            uint64_t refresh_expires_at);

bool database_validate_session(database_pool_t *pool,
                              const char *access_token,
                              uint64_t *user_id);

bool database_refresh_session(database_pool_t *pool,
                             const char *old_refresh_token,
                             const char *new_access_token,
                             const char *new_refresh_token,
                             uint64_t new_expires_at,
                             uint64_t new_refresh_expires_at);

bool database_invalidate_session(database_pool_t *pool, const char *access_token);
bool database_invalidate_all_sessions(database_pool_t *pool, uint64_t user_id);

// 好友管理
bool database_add_friend(database_pool_t *pool,
                        uint64_t user_id,
                        uint64_t friend_id,
                        const char *alias);

bool database_remove_friend(database_pool_t *pool,
                           uint64_t user_id,
                           uint64_t friend_id);

query_result_t *database_get_friends(database_pool_t *pool, uint64_t user_id);
query_result_t *database_get_online_friends(database_pool_t *pool, uint64_t user_id);

// 消息操作
bool database_save_message(database_pool_t *pool,
                          const message_content_t *message);

bool database_update_message_status(database_pool_t *pool,
                                   uint64_t message_id,
                                   message_status_t status);

bool database_mark_message_read(database_pool_t *pool,
                               uint64_t message_id,
                               uint64_t user_id,
                               uint64_t read_time);

query_result_t *database_get_messages(database_pool_t *pool,
                                     uint64_t user_id1,
                                     uint64_t user_id2,
                                     uint64_t since_timestamp,
                                     int limit);

query_result_t *database_get_unread_messages(database_pool_t *pool,
                                            uint64_t user_id);

// 群组管理
bool database_create_group(database_pool_t *pool,
                          const char *group_name,
                          const char *description,
                          uint64_t owner_id,
                          uint64_t *group_id);

bool database_add_group_member(database_pool_t *pool,
                              uint64_t group_id,
                              uint64_t user_id,
                              int role);

bool database_remove_group_member(database_pool_t *pool,
                                 uint64_t group_id,
                                 uint64_t user_id);

query_result_t *database_get_group_members(database_pool_t *pool, uint64_t group_id);
query_result_t *database_get_user_groups(database_pool_t *pool, uint64_t user_id);

// 文件管理
bool database_save_file_metadata(database_pool_t *pool,
                                const file_metadata_t *metadata);

bool database_create_file_share(database_pool_t *pool,
                               uint64_t file_id,
                               uint64_t user_id,
                               int share_type,
                               const char *share_token,
                               uint64_t expires_at,
                               int max_downloads);

query_result_t *database_get_file_by_hash(database_pool_t *pool, const char *file_hash);
query_result_t *database_get_user_files(database_pool_t *pool, uint64_t user_id);

// 搜索功能
query_result_t *database_search_users(database_pool_t *pool,
                                     const char *query,
                                     int limit);

query_result_t *database_search_groups(database_pool_t *pool,
                                      const char *query,
                                      int limit);

// 维护和监控
bool database_backup(database_pool_t *pool, const char *backup_path);
bool database_vacuum(database_pool_t *pool);
bool database_optimize(database_pool_t *pool);
database_stats_t *database_get_statistics(database_pool_t *pool);

// 工具函数
void database_free_result(query_result_t *result);
char *database_escape_string(const char *str);
uint64_t database_get_current_timestamp(void);

// 错误处理
const char *database_get_last_error(database_pool_t *pool);
void database_clear_last_error(database_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif // SECURE_CHAT_DATABASE_H