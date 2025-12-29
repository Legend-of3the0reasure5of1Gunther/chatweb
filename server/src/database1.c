// 文件：server/src/database.c
#include "database.h"
#include "../common/logger.h"
#include "../common/security.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>

// 预编译语句定义
struct prepared_statement {
    sqlite3_stmt *stmt;
    sqlite3 *conn;
    char *sql;
    bool is_finalized;
};

// 内部连接结构
typedef struct {
    sqlite3 *conn;
    bool in_use;
    time_t last_used;
    bool is_write_conn;
} db_connection_t;

// 数据库池实现
struct database_pool {
    db_connection_t *connections;
    int max_connections;
    int active_connections;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    database_config_t config;
    char last_error[512];
    bool initialized;
    bool shutting_down;
    int64_t query_counter;
    int64_t transaction_counter;
};

// 线程局部存储错误信息
static __thread char thread_last_error[512];

// SQLite busy handler
static int sqlite_busy_handler(void *data, int attempts) {
    if (attempts >= 10) {
        LOG_WARN("数据库忙，尝试 %d 次后放弃", attempts);
        return 0;
    }
    
    usleep(100000); // 100ms
    return 1;
}

// 初始化数据库连接
static sqlite3 *db_open_connection(const database_config_t *config, bool is_write) {
    sqlite3 *db = NULL;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
    
    if (!is_write) {
        flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX;
    }
    
    int rc = sqlite3_open_v2(config->database_path, &db, flags, NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR("无法打开数据库: %s", sqlite3_errmsg(db));
        if (db) sqlite3_close(db);
        return NULL;
    }
    
    // 设置忙处理器
    sqlite3_busy_handler(db, sqlite_busy_handler, NULL);
    
    // 设置超时
    sqlite3_busy_timeout(db, config->connection_timeout_ms);
    
    // 启用外键约束
    if (config->enable_foreign_keys) {
        sqlite3_exec(db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
    }
    
    // 启用WAL模式
    if (config->enable_wal && is_write) {
        sqlite3_exec(db, "PRAGMA journal_mode = WAL;", NULL, NULL, NULL);
        sqlite3_exec(db, "PRAGMA synchronous = NORMAL;", NULL, NULL, NULL);
    }
    
    // 设置缓存大小
    sqlite3_exec(db, "PRAGMA cache_size = -2000;", NULL, NULL, NULL);
    
    // 设置自动清理
    if (config->enable_auto_vacuum) {
        sqlite3_exec(db, "PRAGMA auto_vacuum = INCREMENTAL;", NULL, NULL, NULL);
    }
    
    // 启用扩展功能
    sqlite3_exec(db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA encoding = 'UTF-8';", NULL, NULL, NULL);
    
    // 创建必要的函数
    sqlite3_create_function(db, "unix_timestamp", 0, SQLITE_UTF8, NULL,
                           db_unix_timestamp_func, NULL, NULL);
    
    return db;
}

// Unix时间戳函数
static void db_unix_timestamp_func(sqlite3_context *context, int argc, sqlite3_value **argv) {
    sqlite3_result_int64(context, (sqlite3_int64)time(NULL));
}

// 初始化数据库池
database_pool_t *database_init(const database_config_t *config) {
    database_pool_t *pool = (database_pool_t *)calloc(1, sizeof(database_pool_t));
    if (!pool) {
        LOG_ERROR("分配数据库池内存失败");
        return NULL;
    }
    
    pool->config = *config;
    if (pool->config.max_connections <= 0) {
        pool->config.max_connections = 10;
    }
    
    pool->connections = (db_connection_t *)calloc(pool->config.max_connections, 
                                                 sizeof(db_connection_t));
    if (!pool->connections) {
        LOG_ERROR("分配连接内存失败");
        free(pool);
        return NULL;
    }
    
    // 初始化互斥锁和条件变量
    if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
        LOG_ERROR("初始化互斥锁失败");
        free(pool->connections);
        free(pool);
        return NULL;
    }
    
    if (pthread_cond_init(&pool->cond, NULL) != 0) {
        LOG_ERROR("初始化条件变量失败");
        pthread_mutex_destroy(&pool->mutex);
        free(pool->connections);
        free(pool);
        return NULL;
    }
    
    // 创建数据库目录（如果不存在）
    char db_dir[1024];
    strncpy(db_dir, config->database_path, sizeof(db_dir) - 1);
    char *last_slash = strrchr(db_dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkdir(db_dir, 0755);
    }
    
    // 打开写入连接
    pool->connections[0].conn = db_open_connection(config, true);
    if (!pool->connections[0].conn) {
        LOG_ERROR("无法创建写入连接");
        goto cleanup;
    }
    
    pool->connections[0].is_write_conn = true;
    pool->active_connections = 1;
    pool->max_connections = config->max_connections;
    pool->initialized = true;
    
    // 检查数据库架构
    if (!db_check_schema(pool)) {
        LOG_ERROR("数据库架构检查失败");
        goto cleanup;
    }
    
    LOG_INFO("数据库池初始化成功，最大连接数: %d", pool->max_connections);
    return pool;
    
cleanup:
    database_shutdown(pool);
    return NULL;
}

// 检查数据库架构
static bool db_check_schema(database_pool_t *pool) {
    sqlite3 *conn = pool->connections[0].conn;
    
    // 检查用户表是否存在
    const char *check_sql = 
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='users';";
    
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(conn, check_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR("准备架构检查语句失败: %s", sqlite3_errmsg(conn));
        return false;
    }
    
    bool table_exists = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        table_exists = sqlite3_column_int(stmt, 0) > 0;
    }
    
    sqlite3_finalize(stmt);
    
    if (!table_exists) {
        LOG_INFO("数据库架构不存在，正在创建...");
        return db_create_schema(pool);
    }
    
    // 检查版本
    return db_check_version(pool);
}

// 创建数据库架构
static bool db_create_schema(database_pool_t *pool) {
    sqlite3 *conn = pool->connections[0].conn;
    
    // 开启事务
    if (sqlite3_exec(conn, "BEGIN TRANSACTION;", NULL, NULL, NULL) != SQLITE_OK) {
        LOG_ERROR("开启事务失败: %s", sqlite3_errmsg(conn));
        return false;
    }
    
    // 读取SQL文件
    FILE *sql_file = fopen("database_schema.sql", "r");
    if (!sql_file) {
        LOG_ERROR("无法打开数据库架构文件");
        sqlite3_exec(conn, "ROLLBACK;", NULL, NULL, NULL);
        return false;
    }
    
    char sql_buffer[4096];
    char *err_msg = NULL;
    bool success = true;
    
    // 逐行执行SQL
    while (fgets(sql_buffer, sizeof(sql_buffer), sql_file)) {
        // 跳过注释和空行
        if (sql_buffer[0] == '-' || sql_buffer[0] == '\n' || sql_buffer[0] == '\0') {
            continue;
        }
        
        if (sqlite3_exec(conn, sql_buffer, NULL, NULL, &err_msg) != SQLITE_OK) {
            LOG_ERROR("执行SQL失败: %s", err_msg);
            sqlite3_free(err_msg);
            success = false;
            break;
        }
    }
    
    fclose(sql_file);
    
    if (success) {
        if (sqlite3_exec(conn, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
            LOG_ERROR("提交事务失败: %s", sqlite3_errmsg(conn));
            success = false;
        }
    } else {
        sqlite3_exec(conn, "ROLLBACK;", NULL, NULL, NULL);
    }
    
    return success;
}

// 检查数据库版本
static bool db_check_version(database_pool_t *pool) {
    sqlite3 *conn = pool->connections[0].conn;
    const char *sql = "SELECT config_value FROM system_config WHERE config_key = 'schema_version';";
    
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_WARN("无法检查数据库版本，可能没有版本表");
        sqlite3_finalize(stmt);
        return true;
    }
    
    int version = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *value = sqlite3_column_text(stmt, 0);
        if (value) {
            version = atoi((const char *)value);
        }
    }
    
    sqlite3_finalize(stmt);
    
    LOG_INFO("数据库版本: %d", version);
    
    // 这里可以添加版本迁移逻辑
    // if (version < REQUIRED_VERSION) {
    //     return db_migrate_schema(pool, version);
    // }
    
    return true;
}

// 获取数据库连接
static sqlite3 *db_get_connection(database_pool_t *pool, bool write) {
    pthread_mutex_lock(&pool->mutex);
    
    // 等待可用连接或关闭信号
    while (pool->shutting_down) {
        pthread_cond_wait(&pool->cond, &pool->mutex);
    }
    
    // 查找可用连接
    int conn_index = -1;
    for (int i = 0; i < pool->max_connections; i++) {
        if (write && !pool->connections[i].is_write_conn) {
            continue;
        }
        
        if (!pool->connections[i].in_use) {
            conn_index = i;
            break;
        }
    }
    
    // 如果没有可用连接且未达到上限，创建新连接
    if (conn_index == -1 && pool->active_connections < pool->max_connections) {
        for (int i = 0; i < pool->max_connections; i++) {
            if (!pool->connections[i].conn) {
                pool->connections[i].conn = db_open_connection(&pool->config, write);
                if (pool->connections[i].conn) {
                    pool->connections[i].is_write_conn = write;
                    conn_index = i;
                    pool->active_connections++;
                    break;
                }
            }
        }
    }
    
    if (conn_index == -1) {
        LOG_WARN("数据库连接池已满，等待可用连接...");
        
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 5; // 5秒超时
        
        int ret = 0;
        while (conn_index == -1 && ret != ETIMEDOUT) {
            ret = pthread_cond_timedwait(&pool->cond, &pool->mutex, &timeout);
            
            for (int i = 0; i < pool->max_connections; i++) {
                if (write && !pool->connections[i].is_write_conn) {
                    continue;
                }
                
                if (!pool->connections[i].in_use) {
                    conn_index = i;
                    break;
                }
            }
        }
    }
    
    sqlite3 *conn = NULL;
    if (conn_index != -1) {
        pool->connections[conn_index].in_use = true;
        pool->connections[conn_index].last_used = time(NULL);
        conn = pool->connections[conn_index].conn;
    }
    
    pthread_mutex_unlock(&pool->mutex);
    
    if (!conn) {
        LOG_ERROR("无法获取数据库连接");
    }
    
    return conn;
}

// 释放数据库连接
static void db_release_connection(database_pool_t *pool, sqlite3 *conn) {
    pthread_mutex_lock(&pool->mutex);
    
    for (int i = 0; i < pool->max_connections; i++) {
        if (pool->connections[i].conn == conn) {
            pool->connections[i].in_use = false;
            break;
        }
    }
    
    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);
}

// 获取写入连接
sqlite3 *database_get_write_connection(database_pool_t *pool) {
    return db_get_connection(pool, true);
}

// 获取读取连接
sqlite3 *database_get_read_connection(database_pool_t *pool) {
    return db_get_connection(pool, false);
}

// 释放连接
void database_release_connection(database_pool_t *pool, sqlite3 *conn) {
    db_release_connection(pool, conn);
}

// 执行查询
query_result_t *database_execute_query(database_pool_t *pool, 
                                      const char *sql, 
                                      const char **params, 
                                      int param_count) {
    sqlite3 *conn = db_get_connection(pool, false);
    if (!conn) {
        return NULL;
    }
    
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        snprintf(thread_last_error, sizeof(thread_last_error),
                "准备查询失败: %s", sqlite3_errmsg(conn));
        db_release_connection(pool, conn);
        return NULL;
    }
    
    // 绑定参数
    for (int i = 0; i < param_count; i++) {
        if (params[i]) {
            sqlite3_bind_text(stmt, i + 1, params[i], -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(stmt, i + 1);
        }
    }
    
    // 获取列信息
    int column_count = sqlite3_column_count(stmt);
    query_result_t *result = (query_result_t *)calloc(1, sizeof(query_result_t));
    if (!result) {
        sqlite3_finalize(stmt);
        db_release_connection(pool, conn);
        return NULL;
    }
    
    result->column_count = column_count;
    result->column_names = (char **)calloc(column_count, sizeof(char *));
    
    // 获取列名
    for (int i = 0; i < column_count; i++) {
        const char *col_name = sqlite3_column_name(stmt, i);
        result->column_names[i] = strdup(col_name ? col_name : "");
    }
    
    // 收集结果
    int row_capacity = 100;
    result->rows = (char ***)calloc(row_capacity, sizeof(char **));
    
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (result->row_count >= row_capacity) {
            row_capacity *= 2;
            result->rows = (char ***)realloc(result->rows, row_capacity * sizeof(char **));
        }
        
        char **row = (char **)calloc(column_count, sizeof(char *));
        for (int i = 0; i < column_count; i++) {
            const unsigned char *value = sqlite3_column_text(stmt, i);
            if (value) {
                row[i] = strdup((const char *)value);
            } else {
                row[i] = strdup("");
            }
        }
        
        result->rows[result->row_count++] = row;
    }
    
    if (rc != SQLITE_DONE) {
        snprintf(thread_last_error, sizeof(thread_last_error),
                "执行查询失败: %s", sqlite3_errmsg(conn));
        database_free_result(result);
        result = NULL;
    }
    
    sqlite3_finalize(stmt);
    db_release_connection(pool, conn);
    
    pthread_mutex_lock(&pool->mutex);
    pool->query_counter++;
    pthread_mutex_unlock(&pool->mutex);
    
    return result;
}

// 执行更新
bool database_execute_update(database_pool_t *pool,
                           const char *sql,
                           const char **params,
                           int param_count) {
    sqlite3 *conn = db_get_connection(pool, true);
    if (!conn) {
        return false;
    }
    
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        snprintf(thread_last_error, sizeof(thread_last_error),
                "准备更新语句失败: %s", sqlite3_errmsg(conn));
        db_release_connection(pool, conn);
        return false;
    }
    
    // 绑定参数
    for (int i = 0; i < param_count; i++) {
        if (params[i]) {
            sqlite3_bind_text(stmt, i + 1, params[i], -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(stmt, i + 1);
        }
    }
    
    rc = sqlite3_step(stmt);
    bool success = (rc == SQLITE_DONE || rc == SQLITE_ROW);
    
    if (!success) {
        snprintf(thread_last_error, sizeof(thread_last_error),
                "执行更新失败: %s", sqlite3_errmsg(conn));
    }
    
    sqlite3_finalize(stmt);
    db_release_connection(pool, conn);
    
    return success;
}

// 创建用户
bool database_create_user(database_pool_t *pool,
                         const char *username,
                         const char *password_hash,
                         const char *password_salt,
                         const char *email,
                         const char *nickname,
                         user_info_t *user_info) {
    uint64_t timestamp = database_get_current_timestamp();
    
    const char *sql = 
        "INSERT INTO users (username, password_hash, password_salt, email, "
        "nickname, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?);";
    
    const char *params[] = {username, password_hash, password_salt, email, nickname};
    char timestamp_str[32];
    
    snprintf(timestamp_str, sizeof(timestamp_str), "%lu", timestamp);
    params[5] = timestamp_str;
    params[6] = timestamp_str;
    
    bool success = database_execute_update(pool, sql, params, 7);
    
    if (success && user_info) {
        // 获取新创建的用户信息
        query_result_t *result = database_get_user_by_username(pool, username);
        if (result && result->row_count > 0) {
            // 填充user_info结构
            memset(user_info, 0, sizeof(user_info_t));
            user_info->user_id = strtoull(result->rows[0][0], NULL, 10);
            strncpy(user_info->username, result->rows[0][1], MAX_USERNAME_LEN - 1);
            strncpy(user_info->nickname, result->rows[0][4], MAX_NICKNAME_LEN - 1);
            strncpy(user_info->email, result->rows[0][3], MAX_EMAIL_LEN - 1);
            user_info->created_at = timestamp;
            user_info->status = USER_STATUS_OFFLINE;
            
            database_free_result(result);
        }
    }
    
    // 创建用户设置
    if (success) {
        const char *settings_sql = 
            "INSERT INTO user_settings (user_id, created_at, updated_at) "
            "VALUES ((SELECT id FROM users WHERE username = ?), ?, ?);";
        
        params[0] = username;
        params[1] = timestamp_str;
        params[2] = timestamp_str;
        
        database_execute_update(pool, settings_sql, params, 3);
    }
    
    return success;
}

// 用户认证
bool database_authenticate_user(database_pool_t *pool,
                               const char *username,
                               const char *password_hash,
                               user_info_t *user_info) {
    const char *sql = 
        "SELECT id, username, password_hash, nickname, email, avatar_id, status, "
        "status_message, last_seen, friend_count, group_count, is_verified, "
        "is_premium, created_at, login_attempts, account_locked_until "
        "FROM users WHERE username = ?;";
    
    const char *params[] = {username};
    query_result_t *result = database_execute_query(pool, sql, params, 1);
    
    if (!result || result->row_count == 0) {
        database_free_result(result);
        return false;
    }
    
    // 检查账户是否被锁定
    const char *locked_until = result->rows[0][15];
    if (locked_until && atoll(locked_until) > time(NULL)) {
        database_free_result(result);
        snprintf(thread_last_error, sizeof(thread_last_error),
                "账户被锁定，请稍后重试");
        return false;
    }
    
    // 验证密码
    const char *stored_hash = result->rows[0][2];
    if (!stored_hash || strcmp(stored_hash, password_hash) != 0) {
        // 增加登录尝试次数
        const char *update_sql = 
            "UPDATE users SET login_attempts = login_attempts + 1, "
            "last_login_attempt = ? WHERE username = ?;";
        
        char attempt_time[32];
        snprintf(attempt_time, sizeof(attempt_time), "%lu", time(NULL));
        const char *update_params[] = {attempt_time, username};
        
        database_execute_update(pool, update_sql, update_params, 2);
        
        // 如果超过5次失败，锁定账户
        const char *lock_sql = 
            "UPDATE users SET account_locked_until = ? WHERE username = ? "
            "AND login_attempts >= 5;";
        
        char lock_time[32];
        snprintf(lock_time, sizeof(lock_time), "%lu", time(NULL) + 900); // 锁定15分钟
        const char *lock_params[] = {lock_time, username};
        
        database_execute_update(pool, lock_sql, lock_params, 2);
        
        database_free_result(result);
        snprintf(thread_last_error, sizeof(thread_last_error),
                "用户名或密码错误");
        return false;
    }
    
    // 认证成功，重置登录尝试次数
    const char *reset_sql = 
        "UPDATE users SET login_attempts = 0, last_login = ?, "
        "last_login_ip = ? WHERE username = ?;";
    
    char login_time[32];
    snprintf(login_time, sizeof(login_time), "%lu", time(NULL));
    
    // 注意：这里需要实际获取客户端IP
    const char *client_ip = "127.0.0.1";
    const char *reset_params[] = {login_time, client_ip, username};
    
    database_execute_update(pool, reset_sql, reset_params, 3);
    
    // 填充用户信息
    if (user_info) {
        memset(user_info, 0, sizeof(user_info_t));
        user_info->user_id = strtoull(result->rows[0][0], NULL, 10);
        strncpy(user_info->username, result->rows[0][1], MAX_USERNAME_LEN - 1);
        strncpy(user_info->nickname, result->rows[0][3], MAX_NICKNAME_LEN - 1);
        strncpy(user_info->email, result->rows[0][4], MAX_EMAIL_LEN - 1);
        user_info->avatar_id = atoi(result->rows[0][5]);
        user_info->status = atoi(result->rows[0][6]);
        strncpy(user_info->status_message, result->rows[0][7], MAX_STATUS_MSG_LEN - 1);
        user_info->last_seen = strtoull(result->rows[0][8], NULL, 10);
        user_info->friend_count = atoi(result->rows[0][9]);
        user_info->group_count = atoi(result->rows[0][10]);
        user_info->is_verified = atoi(result->rows[0][11]);
        user_info->is_premium = atoi(result->rows[0][12]);
        user_info->created_at = strtoull(result->rows[0][13], NULL, 10);
    }
    
    database_free_result(result);
    return true;
}

// 保存消息
bool database_save_message(database_pool_t *pool,
                          const message_content_t *message) {
    const char *sql = 
        "INSERT INTO messages (sender_id, receiver_id, message_type, content, "
        "content_hash, timestamp, status, is_encrypted, reply_to_id) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";
    
    char sender_id[32], receiver_id[32], timestamp[32];
    char message_type[4], status[4], encrypted[4], reply_to_id[32];
    
    snprintf(sender_id, sizeof(sender_id), "%lu", message->sender_id);
    snprintf(receiver_id, sizeof(receiver_id), "%lu", message->receiver_id);
    snprintf(timestamp, sizeof(timestamp), "%lu", message->timestamp);
    snprintf(message_type, sizeof(message_type), "%u", message->message_type);
    snprintf(status, sizeof(status), "%u", MESSAGE_STATUS_SENT);
    snprintf(encrypted, sizeof(encrypted), "%u", message->encrypted);
    snprintf(reply_to_id, sizeof(reply_to_id), "%lu", message->reply_to_id);
    
    const char *params[] = {
        sender_id, receiver_id, message_type, message->content,
        message->content_hash, timestamp, status, encrypted, reply_to_id
    };
    
    return database_execute_update(pool, sql, params, 9);
}

// 获取消息历史
query_result_t *database_get_messages(database_pool_t *pool,
                                     uint64_t user_id1,
                                     uint64_t user_id2,
                                     uint64_t since_timestamp,
                                     int limit) {
    const char *sql = 
        "SELECT m.id, m.sender_id, m.receiver_id, m.message_type, m.content, "
        "m.content_hash, m.timestamp, m.status, m.is_encrypted, m.reply_to_id, "
        "u1.nickname as sender_name, u2.nickname as receiver_name "
        "FROM messages m "
        "LEFT JOIN users u1 ON m.sender_id = u1.id "
        "LEFT JOIN users u2 ON m.receiver_id = u2.id "
        "WHERE ((m.sender_id = ? AND m.receiver_id = ?) OR "
        "(m.sender_id = ? AND m.receiver_id = ?)) "
        "AND m.timestamp >= ? "
        "AND m.deleted = 0 "
        "ORDER BY m.timestamp DESC "
        "LIMIT ?;";
    
    char uid1[32], uid2[32], timestamp[32], limit_str[32];
    snprintf(uid1, sizeof(uid1), "%lu", user_id1);
    snprintf(uid2, sizeof(uid2), "%lu", user_id2);
    snprintf(timestamp, sizeof(timestamp), "%lu", since_timestamp);
    snprintf(limit_str, sizeof(limit_str), "%d", limit);
    
    const char *params[] = {uid1, uid2, uid2, uid1, timestamp, limit_str};
    
    return database_execute_query(pool, sql, params, 6);
}

// 备份数据库
bool database_backup(database_pool_t *pool, const char *backup_path) {
    sqlite3 *src = db_get_connection(pool, true);
    if (!src) {
        return false;
    }
    
    sqlite3 *dst = NULL;
    int rc = sqlite3_open(backup_path, &dst);
    if (rc != SQLITE_OK) {
        LOG_ERROR("无法创建备份数据库: %s", sqlite3_errmsg(dst));
        db_release_connection(pool, src);
        return false;
    }
    
    sqlite3_backup *backup = sqlite3_backup_init(dst, "main", src, "main");
    if (!backup) {
        LOG_ERROR("无法初始化备份: %s", sqlite3_errmsg(dst));
        sqlite3_close(dst);
        db_release_connection(pool, src);
        return false;
    }
    
    rc = sqlite3_backup_step(backup, -1); // 复制所有页面
    sqlite3_backup_finish(backup);
    
    bool success = (rc == SQLITE_DONE);
    if (!success) {
        LOG_ERROR("备份失败: %s", sqlite3_errmsg(dst));
    } else {
        LOG_INFO("数据库备份成功: %s", backup_path);
    }
    
    sqlite3_close(dst);
    db_release_connection(pool, src);
    
    pthread_mutex_lock(&pool->mutex);
    pool->query_counter++;
    pthread_mutex_unlock(&pool->mutex);
    
    return success;
}

// 清理数据库
bool database_vacuum(database_pool_t *pool) {
    sqlite3 *conn = db_get_connection(pool, true);
    if (!conn) {
        return false;
    }
    
    int rc = sqlite3_exec(conn, "VACUUM;", NULL, NULL, NULL);
    bool success = (rc == SQLITE_OK);
    
    if (success) {
        LOG_INFO("数据库VACUUM完成");
    } else {
        LOG_ERROR("数据库VACUUM失败: %s", sqlite3_errmsg(conn));
    }
    
    db_release_connection(pool, conn);
    return success;
}

// 获取数据库统计信息
database_stats_t *database_get_statistics(database_pool_t *pool) {
    database_stats_t *stats = (database_stats_t *)calloc(1, sizeof(database_stats_t));
    if (!stats) {
        return NULL;
    }
    
    // 获取用户数
    query_result_t *result = database_execute_query(pool, 
        "SELECT COUNT(*) FROM users;", NULL, 0);
    if (result && result->row_count > 0) {
        stats->total_users = atoll(result->rows[0][0]);
    }
    database_free_result(result);
    
    // 获取消息数
    result = database_execute_query(pool,
        "SELECT COUNT(*) FROM messages;", NULL, 0);
    if (result && result->row_count > 0) {
        stats->total_messages = atoll(result->rows[0][0]);
    }
    database_free_result(result);
    
    // 获取群组数
    result = database_execute_query(pool,
        "SELECT COUNT(*) FROM groups;", NULL, 0);
    if (result && result->row_count > 0) {
        stats->total_groups = atoll(result->rows[0][0]);
    }
    database_free_result(result);
    
    // 获取文件数
    result = database_execute_query(pool,
        "SELECT COUNT(*) FROM files;", NULL, 0);
    if (result && result->row_count > 0) {
        stats->total_files = atoll(result->rows[0][0]);
    }
    database_free_result(result);
    
    // 获取数据库大小
    result = database_execute_query(pool,
        "SELECT page_count * page_size FROM pragma_page_count(), pragma_page_size();",
        NULL, 0);
    if (result && result->row_count > 0) {
        stats->database_size_mb = atof(result->rows[0][0]) / (1024.0 * 1024.0);
    }
    database_free_result(result);
    
    pthread_mutex_lock(&pool->mutex);
    stats->query_count = pool->query_counter;
    stats->transaction_count = pool->transaction_counter;
    pthread_mutex_unlock(&pool->mutex);
    
    return stats;
}

// 释放查询结果
void database_free_result(query_result_t *result) {
    if (!result) return;
    
    for (int i = 0; i < result->column_count; i++) {
        free(result->column_names[i]);
    }
    free(result->column_names);
    
    for (int i = 0; i < result->row_count; i++) {
        for (int j = 0; j < result->column_count; j++) {
            free(result->rows[i][j]);
        }
        free(result->rows[i]);
    }
    free(result->rows);
    
    if (result->error_msg) {
        free(result->error_msg);
    }
    
    free(result);
}

// 获取当前时间戳
uint64_t database_get_current_timestamp(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 获取最后错误
const char *database_get_last_error(database_pool_t *pool) {
    return thread_last_error;
}

// 清理错误
void database_clear_last_error(database_pool_t *pool) {
    thread_last_error[0] = '\0';
}

// 关闭数据库池
bool database_shutdown(database_pool_t *pool) {
    if (!pool) return false;
    
    pool->shutting_down = true;
    pthread_cond_broadcast(&pool->cond);
    
    pthread_mutex_lock(&pool->mutex);
    
    for (int i = 0; i < pool->max_connections; i++) {
        if (pool->connections[i].conn) {
            sqlite3_close(pool->connections[i].conn);
            pool->connections[i].conn = NULL;
        }
    }
    
    free(pool->connections);
    pool->connections = NULL;
    
    pthread_mutex_unlock(&pool->mutex);
    pthread_cond_destroy(&pool->cond);
    pthread_mutex_destroy(&pool->mutex);
    
    free(pool);
    
    LOG_INFO("数据库池已关闭");
    return true;
}