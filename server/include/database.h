#ifndef DATABASE_H
#define DATABASE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <sqlite3.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 数据库配置 ====================
#define DB_MAX_CONNECTIONS 10
#define DB_QUERY_TIMEOUT_MS 5000
#define DB_MAX_RETRIES 3
#define DB_BACKUP_INTERVAL 3600  // 1小时备份一次
#define DB_WAL_MODE 1            // 启用WAL模式
#define DB_PAGE_SIZE 4096
#define DB_CACHE_SIZE -2000      // 2000页缓存 (~8MB)
#define DB_JOURNAL_MODE "WAL"    // WAL日志模式

// ==================== 错误码定义 ====================
typedef enum {
    DB_SUCCESS = 0,
    DB_ERROR_CONNECTION = 1,
    DB_ERROR_QUERY = 2,
    DB_ERROR_TRANSACTION = 3,
    DB_ERROR_PREPARE = 4,
    DB_ERROR_BIND = 5,
    DB_ERROR_EXECUTE = 6,
    DB_ERROR_NO_DATA = 7,
    DB_ERROR_CONSTRAINT = 8,
    DB_ERROR_TIMEOUT = 9,
    DB_ERROR_BUSY = 10,
    DB_ERROR_LOCKED = 11,
    DB_ERROR_SCHEMA = 12,
    DB_ERROR_DISK_FULL = 13,
    DB_ERROR_READONLY = 14,
    DB_ERROR_INTERRUPT = 15,
    DB_ERROR_CORRUPT = 16,
    DB_ERROR_NOT_FOUND = 17,
    DB_ERROR_FULL = 18,
    DB_ERROR_CANTOPEN = 19,
    DB_ERROR_PROTOCOL = 20,
    DB_ERROR_EMPTY = 21,
    DB_ERROR_MISMATCH = 22,
    DB_ERROR_RANGE = 23,
    DB_ERROR_NOTADB = 24,
    DB_ERROR_AUTH = 25,
    DB_ERROR_FORMAT = 26,
    DB_ERROR_OVERFLOW = 27,
    DB_ERROR_INTERNAL = 28,
    DB_ERROR_MEMORY = 29
} db_error_code_t;

// ==================== 数据结构定义 ====================

// 数据库连接池结构
typedef struct {
    sqlite3* connections[DB_MAX_CONNECTIONS];
    bool in_use[DB_MAX_CONNECTIONS];
    pthread_mutex_t pool_mutex;
    pthread_cond_t pool_cond;
    int active_count;
    int max_connections;
    char db_path[512];
} db_pool_t;

// 查询结果结构
typedef struct {
    int column_count;
    char** column_names;
    char*** rows;
    int row_count;
    sqlite3_stmt* stmt;
    db_error_code_t error_code;
    char error_msg[512];
} db_result_t;

// 查询参数结构
typedef struct {
    enum {
        DB_PARAM_NULL,
        DB_PARAM_INT,
        DB_PARAM_INT64,
        DB_PARAM_DOUBLE,
        DB_PARAM_TEXT,
        DB_PARAM_BLOB,
        DB_PARAM_BOOL
    } type;
    
    union {
        int int_value;
        int64_t int64_value;
        double double_value;
        const char* text_value;
        const void* blob_value;
        bool bool_value;
    } value;
    
    int blob_size;
    bool copy_text;  // 是否复制文本内容
} db_param_t;

// 数据库配置结构
typedef struct {
    char db_path[512];
    char backup_path[512];
    int max_connections;
    int query_timeout_ms;
    int max_retries;
    int backup_interval;
    bool wal_mode;
    int page_size;
    int cache_size;
    bool foreign_keys;
    bool synchronous;
    bool journal_mode;
    bool temp_store;
    bool autovacuum;
    bool secure_delete;
} db_config_t;

// 数据库统计信息
typedef struct {
    int64_t total_queries;
    int64_t failed_queries;
    int64_t transaction_count;
    int64_t rollback_count;
    int64_t avg_query_time_ms;
    int64_t max_query_time_ms;
    int64_t active_connections;
    int64_t cache_hit_rate;
    int64_t disk_reads;
    int64_t disk_writes;
    int64_t page_cache_size;
    int64_t page_cache_used;
} db_stats_t;

// 数据库备份信息
typedef struct {
    char backup_name[256];
    time_t backup_time;
    uint64_t backup_size;
    char checksum[65];  // SHA256
    bool compressed;
    bool encrypted;
} db_backup_info_t;

// 数据库迁移结构
typedef struct {
    int version;
    char description[256];
    char sql[2048];
    time_t applied_at;
} db_migration_t;

// ==================== 函数声明 ====================

// 初始化/清理
db_error_code_t db_init(const db_config_t* config);
db_error_code_t db_init_with_pool(const db_config_t* config);
void db_cleanup(void);

// 连接管理
db_error_code_t db_get_connection(sqlite3** conn);
db_error_code_t db_release_connection(sqlite3* conn);
db_error_code_t db_close_all_connections(void);
int db_get_active_connections(void);

// 查询执行
db_error_code_t db_execute(const char* sql, db_result_t* result);
db_error_code_t db_execute_params(const char* sql, const db_param_t* params, 
                                 int param_count, db_result_t* result);
db_error_code_t db_execute_prepared(sqlite3_stmt* stmt, db_result_t* result);
db_error_code_t db_prepare_statement(const char* sql, sqlite3_stmt** stmt);
db_error_code_t db_bind_parameters(sqlite3_stmt* stmt, const db_param_t* params,
                                  int param_count);

// 事务管理
db_error_code_t db_begin_transaction(sqlite3* conn);
db_error_code_t db_commit_transaction(sqlite3* conn);
db_error_code_t db_rollback_transaction(sqlite3* conn);
db_error_code_t db_savepoint(sqlite3* conn, const char* name);
db_error_code_t db_release_savepoint(sqlite3* conn, const char* name);
db_error_code_t db_rollback_to_savepoint(sqlite3* conn, const char* name);

// 结果集处理
db_error_code_t db_fetch_next(db_result_t* result);
db_error_code_t db_reset_result(db_result_t* result);
void db_free_result(db_result_t* result);

// 数据提取
const char* db_get_string(db_result_t* result, int column);
int db_get_int(db_result_t* result, int column);
int64_t db_get_int64(db_result_t* result, int column);
double db_get_double(db_result_t* result, int column);
const void* db_get_blob(db_result_t* result, int column, int* size);
bool db_get_bool(db_result_t* result, int column);
time_t db_get_timestamp(db_result_t* result, int column);

// 批量操作
db_error_code_t db_execute_batch(const char* sql, const char** values, int count);
db_error_code_t db_insert_batch(const char* table, const char** columns,
                               const db_param_t** rows, int row_count,
                               int column_count);
db_error_code_t db_update_batch(const char* table, const char* where_clause,
                               const db_param_t* update_params,
                               const db_param_t* where_params,
                               int update_count, int where_count);

// 备份与恢复
db_error_code_t db_create_backup(const char* backup_path, bool compress);
db_error_code_t db_restore_backup(const char* backup_path);
db_error_code_t db_verify_backup(const char* backup_path);
db_error_code_t db_auto_backup(void);
db_backup_info_t* db_list_backups(int* count);

// 数据库维护
db_error_code_t db_vacuum(void);
db_error_code_t db_analyze(void);
db_error_code_t db_reindex(void);
db_error_code_t db_check_integrity(bool* integrity_ok);
db_error_code_t db_optimize(void);
db_error_code_t db_shrink(void);

// 迁移管理
db_error_code_t db_migrate_init(void);
db_error_code_t db_migrate_add(const db_migration_t* migration);
db_error_code_t db_migrate_apply(int target_version);
db_error_code_t db_migrate_rollback(int target_version);
db_migration_t* db_migrate_list(int* count);
int db_migrate_get_current_version(void);

// 性能监控
db_stats_t db_get_statistics(void);
db_error_code_t db_reset_statistics(void);
db_error_code_t db_enable_profiling(bool enable);
db_error_code_t db_set_cache_size(int size_pages);
db_error_code_t db_set_timeout(int timeout_ms);

// 实用函数
db_error_code_t db_table_exists(const char* table_name, bool* exists);
db_error_code_t db_column_exists(const char* table_name, const char* column_name,
                                bool* exists);
db_error_code_t db_get_table_info(const char* table_name, db_result_t* result);
int64_t db_get_last_insert_id(sqlite3* conn);
int64_t db_get_row_count(const char* table_name);
db_error_code_t db_create_index(const char* table_name, const char* column_name,
                               bool unique, const char* index_name);
db_error_code_t db_drop_index(const char* index_name);

// 错误处理
const char* db_error_message(db_error_code_t error);
db_error_code_t db_get_last_error(char* buffer, size_t size);
db_error_code_t db_log_error(const char* sql, db_error_code_t error,
                            const char* additional_info);

// 加密支持（如果SQLite支持加密）
db_error_code_t db_encrypt_database(const char* password);
db_error_code_t db_decrypt_database(const char* password);
db_error_code_t db_change_encryption_key(const char* old_password,
                                        const char* new_password);

// 回调函数
typedef void (*db_query_callback_t)(const char* sql, db_error_code_t error,
                                   int64_t elapsed_ms, void* user_data);
typedef void (*db_backup_callback_t)(const char* backup_path, uint64_t size,
                                    time_t backup_time, void* user_data);

db_error_code_t db_set_query_callback(db_query_callback_t callback, void* user_data);
db_error_code_t db_set_backup_callback(db_backup_callback_t callback, void* user_data);

// 并发控制
db_error_code_t db_acquire_lock(sqlite3* conn, const char* lock_name,
                               int timeout_ms);
db_error_code_t db_release_lock(sqlite3* conn, const char* lock_name);
db_error_code_t db_set_isolation_level(sqlite3* conn, const char* level);

#ifdef __cplusplus
}
#endif

#endif // DATABASE_H