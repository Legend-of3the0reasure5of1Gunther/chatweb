#include "database.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <zlib.h>
#include <openssl/sha.h>
#include <sys/time.h>

// ==================== 内部数据结构 ====================

// 连接池上下文
static db_pool_t* g_db_pool = NULL;
static db_config_t g_db_config;
static pthread_once_t g_db_init_once = PTHREAD_ONCE_INIT;

// 统计信息
static db_stats_t g_db_stats;
static pthread_mutex_t g_stats_mutex = PTHREAD_MUTEX_INITIALIZER;

// 回调函数
static db_query_callback_t g_query_callback = NULL;
static void* g_query_callback_data = NULL;
static db_backup_callback_t g_backup_callback = NULL;
static void* g_backup_callback_data = NULL;

// 错误日志
#define DB_ERROR_LOG_SIZE 1000
typedef struct {
    char sql[1024];
    db_error_code_t error_code;
    char error_msg[512];
    char additional_info[256];
    time_t timestamp;
} db_error_log_t;

static db_error_log_t g_error_logs[DB_ERROR_LOG_SIZE];
static int g_error_log_index = 0;
static pthread_mutex_t g_error_log_mutex = PTHREAD_MUTEX_INITIALIZER;

// 迁移管理
static sqlite3* g_migration_db = NULL;
static pthread_mutex_t g_migration_mutex = PTHREAD_MUTEX_INITIALIZER;

// ==================== 内部函数声明 ====================
static void db_init_once(void);
static db_error_code_t db_init_pool(void);
static db_error_code_t db_open_connection(sqlite3** conn);
static void db_close_connection(sqlite3* conn);
static db_error_code_t db_configure_connection(sqlite3* conn);
static db_error_code_t db_execute_internal(sqlite3* conn, const char* sql,
                                          db_result_t* result);
static db_error_code_t db_execute_prepared_internal(sqlite3* conn,
                                                   sqlite3_stmt* stmt,
                                                   db_result_t* result);
static void db_update_statistics(int64_t query_time_ms, bool success);
static db_error_code_t db_retry_operation(db_error_code_t (*operation)(void*),
                                         void* context, int max_retries);
static db_error_code_t db_compress_file(const char* source, const char* dest);
static db_error_code_t db_decompress_file(const char* source, const char* dest);
static db_error_code_t db_calculate_checksum(const char* filename, char* checksum);
static db_error_code_t db_create_migration_table(void);
static db_error_code_t db_apply_migration(const db_migration_t* migration);
static const char* db_sqlite_error_to_string(int sqlite_error);

// ==================== 初始化/清理函数 ====================

db_error_code_t db_init(const db_config_t* config) {
    if (!config) {
        return DB_ERROR_CONNECTION;
    }
    
    // 复制配置
    memcpy(&g_db_config, config, sizeof(db_config_t));
    
    // 确保路径以null结尾
    g_db_config.db_path[sizeof(g_db_config.db_path) - 1] = '\0';
    g_db_config.backup_path[sizeof(g_db_config.backup_path) - 1] = '\0';
    
    // 初始化连接池
    return db_init_pool();
}

static void db_init_once(void) {
    // 初始化统计信息
    memset(&g_db_stats, 0, sizeof(g_db_stats));
    
    // 初始化错误日志
    memset(g_error_logs, 0, sizeof(g_error_logs));
    g_error_log_index = 0;
}

static db_error_code_t db_init_pool(void) {
    pthread_once(&g_db_init_once, db_init_once);
    
    if (g_db_pool != NULL) {
        return DB_SUCCESS; // 已经初始化
    }
    
    // 分配连接池
    g_db_pool = (db_pool_t*)safe_calloc(1, sizeof(db_pool_t), "db_pool");
    if (!g_db_pool) {
        return DB_ERROR_MEMORY;
    }
    
    // 初始化连接池
    strncpy(g_db_pool->db_path, g_db_config.db_path, sizeof(g_db_pool->db_path) - 1);
    g_db_pool->max_connections = g_db_config.max_connections > 0 ? 
                                 g_db_config.max_connections : DB_MAX_CONNECTIONS;
    
    if (g_db_pool->max_connections > DB_MAX_CONNECTIONS) {
        g_db_pool->max_connections = DB_MAX_CONNECTIONS;
    }
    
    // 初始化互斥锁和条件变量
    if (pthread_mutex_init(&g_db_pool->pool_mutex, NULL) != 0 ||
        pthread_cond_init(&g_db_pool->pool_cond, NULL) != 0) {
        safe_free((void**)&g_db_pool);
        return DB_ERROR_CONNECTION;
    }
    
    // 初始化迁移数据库
    pthread_mutex_init(&g_migration_mutex, NULL);
    
    printf("Database pool initialized with %d connections\n", g_db_pool->max_connections);
    return DB_SUCCESS;
}

void db_cleanup(void) {
    if (!g_db_pool) {
        return;
    }
    
    // 关闭所有连接
    db_close_all_connections();
    
    // 销毁互斥锁和条件变量
    pthread_mutex_destroy(&g_db_pool->pool_mutex);
    pthread_cond_destroy(&g_db_pool->pool_cond);
    
    // 关闭迁移数据库
    if (g_migration_db) {
        sqlite3_close(g_migration_db);
        g_migration_db = NULL;
    }
    
    pthread_mutex_destroy(&g_migration_mutex);
    pthread_mutex_destroy(&g_stats_mutex);
    pthread_mutex_destroy(&g_error_log_mutex);
    
    // 释放连接池
    safe_free((void**)&g_db_pool);
    
    printf("Database module cleaned up\n");
}

// ==================== 连接管理 ====================

static db_error_code_t db_open_connection(sqlite3** conn) {
    int rc;
    
    // 打开数据库连接
    rc = sqlite3_open(g_db_pool->db_path, conn);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(*conn));
        sqlite3_close(*conn);
        *conn = NULL;
        return DB_ERROR_CONNECTION;
    }
    
    // 配置连接
    return db_configure_connection(*conn);
}

static void db_close_connection(sqlite3* conn) {
    if (conn) {
        sqlite3_close(conn);
    }
}

static db_error_code_t db_configure_connection(sqlite3* conn) {
    char* err_msg = NULL;
    int rc;
    
    // 启用外键约束
    rc = sqlite3_exec(conn, "PRAGMA foreign_keys = ON;", NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to enable foreign keys: %s\n", err_msg);
        sqlite3_free(err_msg);
        // 继续执行，不是致命错误
    }
    
    // 设置WAL模式（如果启用）
    if (g_db_config.wal_mode) {
        char sql[256];
        snprintf(sql, sizeof(sql), "PRAGMA journal_mode = %s;", DB_JOURNAL_MODE);
        rc = sqlite3_exec(conn, sql, NULL, NULL, &err_msg);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "Failed to set journal mode: %s\n", err_msg);
            sqlite3_free(err_msg);
        }
    }
    
    // 设置页面大小
    if (g_db_config.page_size > 0) {
        char sql[256];
        snprintf(sql, sizeof(sql), "PRAGMA page_size = %d;", g_db_config.page_size);
        sqlite3_exec(conn, sql, NULL, NULL, NULL);
    }
    
    // 设置缓存大小
    if (g_db_config.cache_size != 0) {
        char sql[256];
        snprintf(sql, sizeof(sql), "PRAGMA cache_size = %d;", g_db_config.cache_size);
        sqlite3_exec(conn, sql, NULL, NULL, NULL);
    }
    
    // 设置查询超时
    if (g_db_config.query_timeout_ms > 0) {
        sqlite3_busy_timeout(conn, g_db_config.query_timeout_ms);
    }
    
    // 设置同步模式
    if (!g_db_config.synchronous) {
        sqlite3_exec(conn, "PRAGMA synchronous = OFF;", NULL, NULL, NULL);
    }
    
    // 设置临时存储
    if (g_db_config.temp_store) {
        sqlite3_exec(conn, "PRAGMA temp_store = MEMORY;", NULL, NULL, NULL);
    }
    
    return DB_SUCCESS;
}

db_error_code_t db_get_connection(sqlite3** conn) {
    if (!g_db_pool || !conn) {
        return DB_ERROR_CONNECTION;
    }
    
    pthread_mutex_lock(&g_db_pool->pool_mutex);
    
    // 查找空闲连接
    int connection_index = -1;
    for (int i = 0; i < g_db_pool->max_connections; i++) {
        if (!g_db_pool->in_use[i]) {
            connection_index = i;
            break;
        }
    }
    
    // 如果没有空闲连接且未达到最大连接数，创建新连接
    if (connection_index == -1 && g_db_pool->active_count < g_db_pool->max_connections) {
        for (int i = 0; i < g_db_pool->max_connections; i++) {
            if (g_db_pool->connections[i] == NULL) {
                connection_index = i;
                break;
            }
        }
    }
    
    // 等待可用连接
    if (connection_index == -1) {
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 5; // 5秒超时
        
        int wait_result = pthread_cond_timedwait(&g_db_pool->pool_cond,
                                                &g_db_pool->pool_mutex, &timeout);
        if (wait_result == ETIMEDOUT) {
            pthread_mutex_unlock(&g_db_pool->pool_mutex);
            return DB_ERROR_TIMEOUT;
        }
        
        // 再次尝试查找空闲连接
        for (int i = 0; i < g_db_pool->max_connections; i++) {
            if (!g_db_pool->in_use[i]) {
                connection_index = i;
                break;
            }
        }
        
        if (connection_index == -1) {
            pthread_mutex_unlock(&g_db_pool->pool_mutex);
            return DB_ERROR_BUSY;
        }
    }
    
    // 获取或创建连接
    db_error_code_t result = DB_SUCCESS;
    if (g_db_pool->connections[connection_index] == NULL) {
        result = db_open_connection(&g_db_pool->connections[connection_index]);
        if (result != DB_SUCCESS) {
            pthread_mutex_unlock(&g_db_pool->pool_mutex);
            return result;
        }
        g_db_pool->active_count++;
    }
    
    *conn = g_db_pool->connections[connection_index];
    g_db_pool->in_use[connection_index] = true;
    
    pthread_mutex_unlock(&g_db_pool->pool_mutex);
    
    return DB_SUCCESS;
}

db_error_code_t db_release_connection(sqlite3* conn) {
    if (!g_db_pool || !conn) {
        return DB_ERROR_CONNECTION;
    }
    
    pthread_mutex_lock(&g_db_pool->pool_mutex);
    
    // 查找连接
    int connection_index = -1;
    for (int i = 0; i < g_db_pool->max_connections; i++) {
        if (g_db_pool->connections[i] == conn) {
            connection_index = i;
            break;
        }
    }
    
    if (connection_index == -1) {
        pthread_mutex_unlock(&g_db_pool->pool_mutex);
        return DB_ERROR_CONNECTION;
    }
    
    // 标记为未使用
    g_db_pool->in_use[connection_index] = false;
    
    // 通知等待的线程
    pthread_cond_signal(&g_db_pool->pool_cond);
    
    pthread_mutex_unlock(&g_db_pool->pool_mutex);
    
    return DB_SUCCESS;
}

db_error_code_t db_close_all_connections(void) {
    if (!g_db_pool) {
        return DB_SUCCESS;
    }
    
    pthread_mutex_lock(&g_db_pool->pool_mutex);
    
    for (int i = 0; i < g_db_pool->max_connections; i++) {
        if (g_db_pool->connections[i]) {
            sqlite3_close(g_db_pool->connections[i]);
            g_db_pool->connections[i] = NULL;
            g_db_pool->in_use[i] = false;
        }
    }
    
    g_db_pool->active_count = 0;
    
    pthread_mutex_unlock(&g_db_pool->pool_mutex);
    
    return DB_SUCCESS;
}

int db_get_active_connections(void) {
    if (!g_db_pool) {
        return 0;
    }
    
    pthread_mutex_lock(&g_db_pool->pool_mutex);
    int count = g_db_pool->active_count;
    pthread_mutex_unlock(&g_db_pool->pool_mutex);
    
    return count;
}

// ==================== 查询执行 ====================

db_error_code_t db_execute(const char* sql, db_result_t* result) {
    if (!sql) {
        return DB_ERROR_QUERY;
    }
    
    sqlite3* conn = NULL;
    db_error_code_t db_result = db_get_connection(&conn);
    if (db_result != DB_SUCCESS) {
        return db_result;
    }
    
    struct timeval start_time, end_time;
    gettimeofday(&start_time, NULL);
    
    db_result = db_execute_internal(conn, sql, result);
    
    gettimeofday(&end_time, NULL);
    int64_t elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000 +
                        (end_time.tv_usec - start_time.tv_usec) / 1000;
    
    // 更新统计信息
    pthread_mutex_lock(&g_stats_mutex);
    g_db_stats.total_queries++;
    if (db_result != DB_SUCCESS) {
        g_db_stats.failed_queries++;
    }
    g_db_stats.avg_query_time_ms = (g_db_stats.avg_query_time_ms * 
                                   (g_db_stats.total_queries - 1) + elapsed_ms) / 
                                   g_db_stats.total_queries;
    if (elapsed_ms > g_db_stats.max_query_time_ms) {
        g_db_stats.max_query_time_ms = elapsed_ms;
    }
    pthread_mutex_unlock(&g_stats_mutex);
    
    // 调用查询回调
    if (g_query_callback) {
        g_query_callback(sql, db_result, elapsed_ms, g_query_callback_data);
    }
    
    // 记录错误
    if (db_result != DB_SUCCESS) {
        db_log_error(sql, db_result, NULL);
    }
    
    db_release_connection(conn);
    
    return db_result;
}

static db_error_code_t db_execute_internal(sqlite3* conn, const char* sql,
                                          db_result_t* result) {
    if (!conn || !sql) {
        return DB_ERROR_CONNECTION;
    }
    
    sqlite3_stmt* stmt = NULL;
    int rc;
    
    // 准备语句
    rc = sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (result) {
            strncpy(result->error_msg, sqlite3_errmsg(conn), sizeof(result->error_msg) - 1);
            result->error_code = DB_ERROR_PREPARE;
        }
        return DB_ERROR_PREPARE;
    }
    
    // 执行查询
    db_error_code_t db_result = db_execute_prepared_internal(conn, stmt, result);
    
    // 清理语句
    sqlite3_finalize(stmt);
    
    return db_result;
}

db_error_code_t db_execute_params(const char* sql, const db_param_t* params,
                                 int param_count, db_result_t* result) {
    if (!sql) {
        return DB_ERROR_QUERY;
    }
    
    sqlite3* conn = NULL;
    db_error_code_t db_result = db_get_connection(&conn);
    if (db_result != DB_SUCCESS) {
        return db_result;
    }
    
    sqlite3_stmt* stmt = NULL;
    int rc;
    
    // 准备语句
    rc = sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        db_release_connection(conn);
        if (result) {
            strncpy(result->error_msg, sqlite3_errmsg(conn), sizeof(result->error_msg) - 1);
            result->error_code = DB_ERROR_PREPARE;
        }
        return DB_ERROR_PREPARE;
    }
    
    // 绑定参数
    db_result = db_bind_parameters(stmt, params, param_count);
    if (db_result != DB_SUCCESS) {
        sqlite3_finalize(stmt);
        db_release_connection(conn);
        return db_result;
    }
    
    struct timeval start_time, end_time;
    gettimeofday(&start_time, NULL);
    
    // 执行查询
    db_result = db_execute_prepared_internal(conn, stmt, result);
    
    gettimeofday(&end_time, NULL);
    int64_t elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000 +
                        (end_time.tv_usec - start_time.tv_usec) / 1000;
    
    // 更新统计信息
    pthread_mutex_lock(&g_stats_mutex);
    g_db_stats.total_queries++;
    if (db_result != DB_SUCCESS) {
        g_db_stats.failed_queries++;
    }
    g_db_stats.avg_query_time_ms = (g_db_stats.avg_query_time_ms * 
                                   (g_db_stats.total_queries - 1) + elapsed_ms) / 
                                   g_db_stats.total_queries;
    if (elapsed_ms > g_db_stats.max_query_time_ms) {
        g_db_stats.max_query_time_ms = elapsed_ms;
    }
    pthread_mutex_unlock(&g_stats_mutex);
    
    // 调用查询回调
    if (g_query_callback) {
        g_query_callback(sql, db_result, elapsed_ms, g_query_callback_data);
    }
    
    // 记录错误
    if (db_result != DB_SUCCESS) {
        db_log_error(sql, db_result, NULL);
    }
    
    sqlite3_finalize(stmt);
    db_release_connection(conn);
    
    return db_result;
}

static db_error_code_t db_execute_prepared_internal(sqlite3* conn,
                                                   sqlite3_stmt* stmt,
                                                   db_result_t* result) {
    if (!conn || !stmt) {
        return DB_ERROR_CONNECTION;
    }
    
    int rc;
    int column_count = sqlite3_column_count(stmt);
    
    // 初始化结果集
    if (result) {
        memset(result, 0, sizeof(db_result_t));
        result->stmt = stmt;
        result->column_count = column_count;
        
        // 获取列名
        result->column_names = (char**)safe_calloc(column_count, sizeof(char*), "column_names");
        if (!result->column_names) {
            return DB_ERROR_MEMORY;
        }
        
        for (int i = 0; i < column_count; i++) {
            const char* column_name = sqlite3_column_name(stmt, i);
            result->column_names[i] = column_name ? strdup(column_name) : strdup("");
            if (!result->column_names[i]) {
                for (int j = 0; j < i; j++) {
                    free(result->column_names[j]);
                }
                free(result->column_names);
                result->column_names = NULL;
                return DB_ERROR_MEMORY;
            }
        }
        
        // 分配行存储
        result->rows = (char***)safe_calloc(100, sizeof(char**), "rows_initial");
        if (!result->rows) {
            for (int i = 0; i < column_count; i++) {
                free(result->column_names[i]);
            }
            free(result->column_names);
            return DB_ERROR_MEMORY;
        }
        
        result->row_count = 0;
        int row_capacity = 100;
        
        // 获取所有行
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            // 如果容量不足，重新分配
            if (result->row_count >= row_capacity) {
                row_capacity *= 2;
                char*** new_rows = (char***)realloc(result->rows, row_capacity * sizeof(char**));
                if (!new_rows) {
                    db_free_result(result);
                    return DB_ERROR_MEMORY;
                }
                result->rows = new_rows;
            }
            
            // 分配当前行
            result->rows[result->row_count] = (char**)safe_calloc(column_count, sizeof(char*), "row_cells");
            if (!result->rows[result->row_count]) {
                db_free_result(result);
                return DB_ERROR_MEMORY;
            }
            
            // 获取每个列的值
            for (int i = 0; i < column_count; i++) {
                const char* text = (const char*)sqlite3_column_text(stmt, i);
                if (text) {
                    result->rows[result->row_count][i] = strdup(text);
                    if (!result->rows[result->row_count][i]) {
                        for (int j = 0; j < i; j++) {
                            free(result->rows[result->row_count][j]);
                        }
                        free(result->rows[result->row_count]);
                        db_free_result(result);
                        return DB_ERROR_MEMORY;
                    }
                } else {
                    result->rows[result->row_count][i] = strdup(""); // NULL转为空字符串
                }
            }
            
            result->row_count++;
        }
        
        if (rc != SQLITE_DONE) {
            strncpy(result->error_msg, sqlite3_errmsg(conn), sizeof(result->error_msg) - 1);
            result->error_code = DB_ERROR_EXECUTE;
            db_free_result(result);
            return DB_ERROR_EXECUTE;
        }
        
        // 重置语句以便重用
        sqlite3_reset(stmt);
    } else {
        // 不需要结果集，只执行
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            return DB_ERROR_EXECUTE;
        }
        sqlite3_reset(stmt);
    }
    
    return DB_SUCCESS;
}

db_error_code_t db_prepare_statement(const char* sql, sqlite3_stmt** stmt) {
    if (!sql || !stmt) {
        return DB_ERROR_QUERY;
    }
    
    sqlite3* conn = NULL;
    db_error_code_t db_result = db_get_connection(&conn);
    if (db_result != DB_SUCCESS) {
        return db_result;
    }
    
    int rc = sqlite3_prepare_v2(conn, sql, -1, stmt, NULL);
    db_release_connection(conn);
    
    if (rc != SQLITE_OK) {
        return DB_ERROR_PREPARE;
    }
    
    return DB_SUCCESS;
}

db_error_code_t db_bind_parameters(sqlite3_stmt* stmt, const db_param_t* params,
                                  int param_count) {
    if (!stmt || !params || param_count <= 0) {
        return DB_SUCCESS; // 没有参数需要绑定
    }
    
    // 重置所有参数绑定
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    
    for (int i = 0; i < param_count; i++) {
        int bind_index = i + 1; // SQLite参数索引从1开始
        
        switch (params[i].type) {
            case DB_PARAM_NULL:
                sqlite3_bind_null(stmt, bind_index);
                break;
                
            case DB_PARAM_INT:
                sqlite3_bind_int(stmt, bind_index, params[i].value.int_value);
                break;
                
            case DB_PARAM_INT64:
                sqlite3_bind_int64(stmt, bind_index, params[i].value.int64_value);
                break;
                
            case DB_PARAM_DOUBLE:
                sqlite3_bind_double(stmt, bind_index, params[i].value.double_value);
                break;
                
            case DB_PARAM_TEXT:
                if (params[i].value.text_value) {
                    if (params[i].copy_text) {
                        // 复制文本，SQLite会负责释放
                        char* copy = strdup(params[i].value.text_value);
                        sqlite3_bind_text(stmt, bind_index, copy, -1, sqlite3_free);
                    } else {
                        // 不复制文本，调用者负责生命周期
                        sqlite3_bind_text(stmt, bind_index, params[i].value.text_value,
                                         -1, SQLITE_STATIC);
                    }
                } else {
                    sqlite3_bind_null(stmt, bind_index);
                }
                break;
                
            case DB_PARAM_BLOB:
                if (params[i].value.blob_value && params[i].blob_size > 0) {
                    if (params[i].copy_text) {
                        // 复制BLOB数据
                        void* copy = malloc(params[i].blob_size);
                        if (copy) {
                            memcpy(copy, params[i].value.blob_value, params[i].blob_size);
                            sqlite3_bind_blob(stmt, bind_index, copy, params[i].blob_size, free);
                        } else {
                            return DB_ERROR_MEMORY;
                        }
                    } else {
                        sqlite3_bind_blob(stmt, bind_index, params[i].value.blob_value,
                                         params[i].blob_size, SQLITE_STATIC);
                    }
                } else {
                    sqlite3_bind_null(stmt, bind_index);
                }
                break;
                
            case DB_PARAM_BOOL:
                sqlite3_bind_int(stmt, bind_index, params[i].value.bool_value ? 1 : 0);
                break;
                
            default:
                return DB_ERROR_BIND;
        }
    }
    
    return DB_SUCCESS;
}

// ==================== 事务管理 ====================

db_error_code_t db_begin_transaction(sqlite3* conn) {
    if (!conn) {
        return DB_ERROR_CONNECTION;
    }
    
    char* err_msg = NULL;
    int rc = sqlite3_exec(conn, "BEGIN TRANSACTION;", NULL, NULL, &err_msg);
    
    if (rc != SQLITE_OK) {
        if (err_msg) {
            fprintf(stderr, "Failed to begin transaction: %s\n", err_msg);
            sqlite3_free(err_msg);
        }
        return DB_ERROR_TRANSACTION;
    }
    
    pthread_mutex_lock(&g_stats_mutex);
    g_db_stats.transaction_count++;
    pthread_mutex_unlock(&g_stats_mutex);
    
    return DB_SUCCESS;
}

db_error_code_t db_commit_transaction(sqlite3* conn) {
    if (!conn) {
        return DB_ERROR_CONNECTION;
    }
    
    char* err_msg = NULL;
    int rc = sqlite3_exec(conn, "COMMIT TRANSACTION;", NULL, NULL, &err_msg);
    
    if (rc != SQLITE_OK) {
        if (err_msg) {
            fprintf(stderr, "Failed to commit transaction: %s\n", err_msg);
            sqlite3_free(err_msg);
        }
        return DB_ERROR_TRANSACTION;
    }
    
    return DB_SUCCESS;
}

db_error_code_t db_rollback_transaction(sqlite3* conn) {
    if (!conn) {
        return DB_ERROR_CONNECTION;
    }
    
    char* err_msg = NULL;
    int rc = sqlite3_exec(conn, "ROLLBACK TRANSACTION;", NULL, NULL, &err_msg);
    
    if (rc != SQLITE_OK) {
        if (err_msg) {
            fprintf(stderr, "Failed to rollback transaction: %s\n", err_msg);
            sqlite3_free(err_msg);
        }
        return DB_ERROR_TRANSACTION;
    }
    
    pthread_mutex_lock(&g_stats_mutex);
    g_db_stats.rollback_count++;
    pthread_mutex_unlock(&g_stats_mutex);
    
    return DB_SUCCESS;
}

db_error_code_t db_savepoint(sqlite3* conn, const char* name) {
    if (!conn || !name) {
        return DB_ERROR_CONNECTION;
    }
    
    char sql[256];
    snprintf(sql, sizeof(sql), "SAVEPOINT %s;", name);
    
    char* err_msg = NULL;
    int rc = sqlite3_exec(conn, sql, NULL, NULL, &err_msg);
    
    if (rc != SQLITE_OK) {
        if (err_msg) {
            fprintf(stderr, "Failed to create savepoint: %s\n", err_msg);
            sqlite3_free(err_msg);
        }
        return DB_ERROR_TRANSACTION;
    }
    
    return DB_SUCCESS;
}

db_error_code_t db_release_savepoint(sqlite3* conn, const char* name) {
    if (!conn || !name) {
        return DB_ERROR_CONNECTION;
    }
    
    char sql[256];
    snprintf(sql, sizeof(sql), "RELEASE SAVEPOINT %s;", name);
    
    char* err_msg = NULL;
    int rc = sqlite3_exec(conn, sql, NULL, NULL, &err_msg);
    
    if (rc != SQLITE_OK) {
        if (err_msg) {
            fprintf(stderr, "Failed to release savepoint: %s\n", err_msg);
            sqlite3_free(err_msg);
        }
        return DB_ERROR_TRANSACTION;
    }
    
    return DB_SUCCESS;
}

db_error_code_t db_rollback_to_savepoint(sqlite3* conn, const char* name) {
    if (!conn || !name) {
        return DB_ERROR_CONNECTION;
    }
    
    char sql[256];
    snprintf(sql, sizeof(sql), "ROLLBACK TO SAVEPOINT %s;", name);
    
    char* err_msg = NULL;
    int rc = sqlite3_exec(conn, sql, NULL, NULL, &err_msg);
    
    if (rc != SQLITE_OK) {
        if (err_msg) {
            fprintf(stderr, "Failed to rollback to savepoint: %s\n", err_msg);
            sqlite3_free(err_msg);
        }
        return DB_ERROR_TRANSACTION;
    }
    
    pthread_mutex_lock(&g_stats_mutex);
    g_db_stats.rollback_count++;
    pthread_mutex_unlock(&g_stats_mutex);
    
    return DB_SUCCESS;
}

// ==================== 结果集处理 ====================

void db_free_result(db_result_t* result) {
    if (!result) {
        return;
    }
    
    // 释放列名
    if (result->column_names) {
        for (int i = 0; i < result->column_count; i++) {
            if (result->column_names[i]) {
                free(result->column_names[i]);
            }
        }
        free(result->column_names);
        result->column_names = NULL;
    }
    
    // 释放行数据
    if (result->rows) {
        for (int i = 0; i < result->row_count; i++) {
            if (result->rows[i]) {
                for (int j = 0; j < result->column_count; j++) {
                    if (result->rows[i][j]) {
                        free(result->rows[i][j]);
                    }
                }
                free(result->rows[i]);
            }
        }
        free(result->rows);
        result->rows = NULL;
    }
    
    // 释放语句（如果存在）
    if (result->stmt) {
        sqlite3_finalize(result->stmt);
        result->stmt = NULL;
    }
    
    memset(result, 0, sizeof(db_result_t));
}

db_error_code_t db_fetch_next(db_result_t* result) {
    if (!result || !result->stmt) {
        return DB_ERROR_NO_DATA;
    }
    
    int rc = sqlite3_step(result->stmt);
    if (rc == SQLITE_ROW) {
        return DB_SUCCESS;
    } else if (rc == SQLITE_DONE) {
        return DB_ERROR_NO_DATA;
    } else {
        strncpy(result->error_msg, sqlite3_errmsg(sqlite3_db_handle(result->stmt)), 
                sizeof(result->error_msg) - 1);
        result->error_code = DB_ERROR_EXECUTE;
        return DB_ERROR_EXECUTE;
    }
}

db_error_code_t db_reset_result(db_result_t* result) {
    if (!result || !result->stmt) {
        return DB_ERROR_NO_DATA;
    }
    
    sqlite3_reset(result->stmt);
    return DB_SUCCESS;
}

// ==================== 数据提取函数 ====================

const char* db_get_string(db_result_t* result, int column) {
    if (!result || !result->stmt || column < 0 || column >= result->column_count) {
        return NULL;
    }
    
    return (const char*)sqlite3_column_text(result->stmt, column);
}

int db_get_int(db_result_t* result, int column) {
    if (!result || !result->stmt || column < 0 || column >= result->column_count) {
        return 0;
    }
    
    return sqlite3_column_int(result->stmt, column);
}

int64_t db_get_int64(db_result_t* result, int column) {
    if (!result || !result->stmt || column < 0 || column >= result->column_count) {
        return 0;
    }
    
    return sqlite3_column_int64(result->stmt, column);
}

double db_get_double(db_result_t* result, int column) {
    if (!result || !result->stmt || column < 0 || column >= result->column_count) {
        return 0.0;
    }
    
    return sqlite3_column_double(result->stmt, column);
}

const void* db_get_blob(db_result_t* result, int column, int* size) {
    if (!result || !result->stmt || column < 0 || column >= result->column_count) {
        if (size) *size = 0;
        return NULL;
    }
    
    if (size) {
        *size = sqlite3_column_bytes(result->stmt, column);
    }
    
    return sqlite3_column_blob(result->stmt, column);
}

bool db_get_bool(db_result_t* result, int column) {
    if (!result || !result->stmt || column < 0 || column >= result->column_count) {
        return false;
    }
    
    return sqlite3_column_int(result->stmt, column) != 0;
}

time_t db_get_timestamp(db_result_t* result, int column) {
    if (!result || !result->stmt || column < 0 || column >= result->column_count) {
        return 0;
    }
    
    return (time_t)sqlite3_column_int64(result->stmt, column);
}

// ==================== 批量操作 ====================

db_error_code_t db_execute_batch(const char* sql, const char** values, int count) {
    if (!sql || !values || count <= 0) {
        return DB_ERROR_QUERY;
    }
    
    sqlite3* conn = NULL;
    db_error_code_t db_result = db_get_connection(&conn);
    if (db_result != DB_SUCCESS) {
        return db_result;
    }
    
    // 开始事务
    db_result = db_begin_transaction(conn);
    if (db_result != DB_SUCCESS) {
        db_release_connection(conn);
        return db_result;
    }
    
    // 准备语句
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        db_rollback_transaction(conn);
        db_release_connection(conn);
        return DB_ERROR_PREPARE;
    }
    
    // 执行批量插入
    for (int i = 0; i < count; i++) {
        if (values[i]) {
            sqlite3_bind_text(stmt, 1, values[i], -1, SQLITE_STATIC);
        } else {
            sqlite3_bind_null(stmt, 1);
        }
        
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            db_rollback_transaction(conn);
            db_release_connection(conn);
            return DB_ERROR_EXECUTE;
        }
        
        sqlite3_reset(stmt);
    }
    
    sqlite3_finalize(stmt);
    
    // 提交事务
    db_result = db_commit_transaction(conn);
    db_release_connection(conn);
    
    return db_result;
}

db_error_code_t db_insert_batch(const char* table, const char** columns,
                               const db_param_t** rows, int row_count,
                               int column_count) {
    if (!table || !columns || !rows || row_count <= 0 || column_count <= 0) {
        return DB_ERROR_QUERY;
    }
    
    // 构建插入SQL
    char sql[1024];
    char placeholders[512] = "";
    
    // 构建占位符列表
    for (int i = 0; i < column_count; i++) {
        strcat(placeholders, "?");
        if (i < column_count - 1) {
            strcat(placeholders, ",");
        }
    }
    
    snprintf(sql, sizeof(sql), "INSERT INTO %s (%s) VALUES (%s);",
             table, columns[0], placeholders);
    
    sqlite3* conn = NULL;
    db_error_code_t db_result = db_get_connection(&conn);
    if (db_result != DB_SUCCESS) {
        return db_result;
    }
    
    // 开始事务
    db_result = db_begin_transaction(conn);
    if (db_result != DB_SUCCESS) {
        db_release_connection(conn);
        return db_result;
    }
    
    // 准备语句
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        db_rollback_transaction(conn);
        db_release_connection(conn);
        return DB_ERROR_PREPARE;
    }
    
    // 执行批量插入
    for (int row = 0; row < row_count; row++) {
        // 绑定参数
        for (int col = 0; col < column_count; col++) {
            const db_param_t* param = &rows[row][col];
            int bind_index = col + 1;
            
            switch (param->type) {
                case DB_PARAM_NULL:
                    sqlite3_bind_null(stmt, bind_index);
                    break;
                case DB_PARAM_INT:
                    sqlite3_bind_int(stmt, bind_index, param->value.int_value);
                    break;
                case DB_PARAM_INT64:
                    sqlite3_bind_int64(stmt, bind_index, param->value.int64_value);
                    break;
                case DB_PARAM_DOUBLE:
                    sqlite3_bind_double(stmt, bind_index, param->value.double_value);
                    break;
                case DB_PARAM_TEXT:
                    if (param->value.text_value) {
                        sqlite3_bind_text(stmt, bind_index, param->value.text_value,
                                         -1, SQLITE_STATIC);
                    } else {
                        sqlite3_bind_null(stmt, bind_index);
                    }
                    break;
                case DB_PARAM_BLOB:
                    if (param->value.blob_value && param->blob_size > 0) {
                        sqlite3_bind_blob(stmt, bind_index, param->value.blob_value,
                                         param->blob_size, SQLITE_STATIC);
                    } else {
                        sqlite3_bind_null(stmt, bind_index);
                    }
                    break;
                case DB_PARAM_BOOL:
                    sqlite3_bind_int(stmt, bind_index, param->value.bool_value ? 1 : 0);
                    break;
                default:
                    sqlite3_bind_null(stmt, bind_index);
                    break;
            }
        }
        
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            db_rollback_transaction(conn);
            db_release_connection(conn);
            return DB_ERROR_EXECUTE;
        }
        
        sqlite3_reset(stmt);
    }
    
    sqlite3_finalize(stmt);
    
    // 提交事务
    db_result = db_commit_transaction(conn);
    db_release_connection(conn);
    
    return db_result;
}

// ==================== 备份与恢复 ====================

db_error_code_t db_create_backup(const char* backup_path, bool compress) {
    if (!backup_path) {
        return DB_ERROR_CONNECTION;
    }
    
    sqlite3* conn = NULL;
    sqlite3* backup_conn = NULL;
    db_error_code_t result = DB_SUCCESS;
    
    // 获取主连接
    result = db_get_connection(&conn);
    if (result != DB_SUCCESS) {
        return result;
    }
    
    // 创建备份数据库连接
    int rc = sqlite3_open(backup_path, &backup_conn);
    if (rc != SQLITE_OK) {
        db_release_connection(conn);
        return DB_ERROR_CONNECTION;
    }
    
    // 执行备份
    sqlite3_backup* backup = sqlite3_backup_init(backup_conn, "main", conn, "main");
    if (!backup) {
        sqlite3_close(backup_conn);
        db_release_connection(conn);
        return DB_ERROR_CONNECTION;
    }
    
    // 分步备份
    do {
        rc = sqlite3_backup_step(backup, 100); // 每次100页
        if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_BUSY && rc != SQLITE_LOCKED) {
            break;
        }
        
        // 等待一下再继续（如果繁忙）
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            usleep(250000); // 250ms
        }
    } while (rc != SQLITE_DONE);
    
    // 完成备份
    sqlite3_backup_finish(backup);
    
    // 关闭备份连接
    sqlite3_close(backup_conn);
    db_release_connection(conn);
    
    if (rc != SQLITE_DONE) {
        return DB_ERROR_CONNECTION;
    }
    
    // 如果需要压缩
    if (compress) {
        char compressed_path[512];
        snprintf(compressed_path, sizeof(compressed_path), "%s.gz", backup_path);
        
        result = db_compress_file(backup_path, compressed_path);
        if (result == DB_SUCCESS) {
            // 删除未压缩的文件
            unlink(backup_path);
        }
    }
    
    // 计算校验和
    char checksum[65];
    const char* final_path = compress ? backup_path : compressed_path;
    db_calculate_checksum(final_path, checksum);
    
    // 记录备份信息
    time_t now = time(NULL);
    
    // 调用备份回调
    if (g_backup_callback) {
        struct stat st;
        uint64_t size = 0;
        if (stat(final_path, &st) == 0) {
            size = st.st_size;
        }
        g_backup_callback(final_path, size, now, g_backup_callback_data);
    }
    
    printf("Database backup created: %s (checksum: %s)\n", final_path, checksum);
    
    return DB_SUCCESS;
}

static db_error_code_t db_compress_file(const char* source, const char* dest) {
    FILE* source_file = fopen(source, "rb");
    if (!source_file) {
        return DB_ERROR_FILESYSTEM;
    }
    
    gzFile dest_file = gzopen(dest, "wb");
    if (!dest_file) {
        fclose(source_file);
        return DB_ERROR_FILESYSTEM;
    }
    
    char buffer[8192];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), source_file)) > 0) {
        if (gzwrite(dest_file, buffer, bytes_read) != bytes_read) {
            fclose(source_file);
            gzclose(dest_file);
            return DB_ERROR_FILESYSTEM;
        }
    }
    
    fclose(source_file);
    gzclose(dest_file);
    
    return DB_SUCCESS;
}

static db_error_code_t db_decompress_file(const char* source, const char* dest) {
    gzFile source_file = gzopen(source, "rb");
    if (!source_file) {
        return DB_ERROR_FILESYSTEM;
    }
    
    FILE* dest_file = fopen(dest, "wb");
    if (!dest_file) {
        gzclose(source_file);
        return DB_ERROR_FILESYSTEM;
    }
    
    char buffer[8192];
    int bytes_read;
    
    while ((bytes_read = gzread(source_file, buffer, sizeof(buffer))) > 0) {
        if (fwrite(buffer, 1, bytes_read, dest_file) != bytes_read) {
            fclose(dest_file);
            gzclose(source_file);
            return DB_ERROR_FILESYSTEM;
        }
    }
    
    fclose(dest_file);
    gzclose(source_file);
    
    return DB_SUCCESS;
}

static db_error_code_t db_calculate_checksum(const char* filename, char* checksum) {
    if (!filename || !checksum) {
        return DB_ERROR_FILESYSTEM;
    }
    
    FILE* file = fopen(filename, "rb");
    if (!file) {
        return DB_ERROR_FILESYSTEM;
    }
    
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    
    unsigned char buffer[8192];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        SHA256_Update(&sha256, buffer, bytes_read);
    }
    
    fclose(file);
    
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &sha256);
    
    // 转换为十六进制字符串
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(&checksum[i*2], "%02x", hash[i]);
    }
    checksum[64] = '\0';
    
    return DB_SUCCESS;
}

db_error_code_t db_auto_backup(void) {
    if (g_db_config.backup_interval <= 0) {
        return DB_SUCCESS; // 自动备份未启用
    }
    
    static time_t last_backup_time = 0;
    time_t now = time(NULL);
    
    if (now - last_backup_time >= g_db_config.backup_interval) {
        char backup_path[512];
        char timestamp[64];
        struct tm* tm_info = localtime(&now);
        
        strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);
        snprintf(backup_path, sizeof(backup_path), "%s/backup_%s.db",
                g_db_config.backup_path, timestamp);
        
        // 创建备份目录（如果不存在）
        mkdir(g_db_config.backup_path, 0755);
        
        db_error_code_t result = db_create_backup(backup_path, true);
        if (result == DB_SUCCESS) {
            last_backup_time = now;
            printf("Auto backup completed: %s\n", backup_path);
        }
        
        return result;
    }
    
    return DB_SUCCESS;
}

// ==================== 数据库维护 ====================

db_error_code_t db_vacuum(void) {
    sqlite3* conn = NULL;
    db_error_code_t result = db_get_connection(&conn);
    if (result != DB_SUCCESS) {
        return result;
    }
    
    char* err_msg = NULL;
    int rc = sqlite3_exec(conn, "VACUUM;", NULL, NULL, &err_msg);
    
    db_release_connection(conn);
    
    if (rc != SQLITE_OK) {
        if (err_msg) {
            fprintf(stderr, "Failed to vacuum database: %s\n", err_msg);
            sqlite3_free(err_msg);
        }
        return DB_ERROR_QUERY;
    }
    
    printf("Database vacuum completed\n");
    return DB_SUCCESS;
}

db_error_code_t db_analyze(void) {
    sqlite3* conn = NULL;
    db_error_code_t result = db_get_connection(&conn);
    if (result != DB_SUCCESS) {
        return result;
    }
    
    char* err_msg = NULL;
    int rc = sqlite3_exec(conn, "ANALYZE;", NULL, NULL, &err_msg);
    
    db_release_connection(conn);
    
    if (rc != SQLITE_OK) {
        if (err_msg) {
            fprintf(stderr, "Failed to analyze database: %s\n", err_msg);
            sqlite3_free(err_msg);
        }
        return DB_ERROR_QUERY;
    }
    
    printf("Database analysis completed\n");
    return DB_SUCCESS;
}

db_error_code_t db_check_integrity(bool* integrity_ok) {
    if (!integrity_ok) {
        return DB_ERROR_QUERY;
    }
    
    sqlite3* conn = NULL;
    db_error_code_t result = db_get_connection(&conn);
    if (result != DB_SUCCESS) {
        return result;
    }
    
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(conn, "PRAGMA integrity_check;", -1, &stmt, NULL);
    
    if (rc != SQLITE_OK) {
        db_release_connection(conn);
        return DB_ERROR_QUERY;
    }
    
    *integrity_ok = false;
    
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const char* result_text = (const char*)sqlite3_column_text(stmt, 0);
        if (result_text && strcmp(result_text, "ok") == 0) {
            *integrity_ok = true;
        }
    }
    
    sqlite3_finalize(stmt);
    db_release_connection(conn);
    
    printf("Database integrity check: %s\n", *integrity_ok ? "PASSED" : "FAILED");
    return DB_SUCCESS;
}

// ==================== 迁移管理 ====================

static db_error_code_t db_create_migration_table(void) {
    const char* sql = 
        "CREATE TABLE IF NOT EXISTS migrations ("
        "version INTEGER PRIMARY KEY,"
        "description TEXT NOT NULL,"
        "sql TEXT NOT NULL,"
        "applied_at INTEGER NOT NULL"
        ");";
    
    db_result_t result;
    db_error_code_t db_result = db_execute(sql, &result);
    db_free_result(&result);
    
    return db_result;
}

db_error_code_t db_migrate_init(void) {
    pthread_mutex_lock(&g_migration_mutex);
    
    db_error_code_t result = db_create_migration_table();
    if (result != DB_SUCCESS) {
        pthread_mutex_unlock(&g_migration_mutex);
        return result;
    }
    
    pthread_mutex_unlock(&g_migration_mutex);
    return DB_SUCCESS;
}

db_error_code_t db_migrate_add(const db_migration_t* migration) {
    if (!migration) {
        return DB_ERROR_QUERY;
    }
    
    pthread_mutex_lock(&g_migration_mutex);
    
    // 检查迁移是否已存在
    char sql[512];
    snprintf(sql, sizeof(sql), 
             "SELECT COUNT(*) FROM migrations WHERE version = %d;",
             migration->version);
    
    db_result_t result;
    db_error_code_t db_result = db_execute(sql, &result);
    
    if (db_result == DB_SUCCESS && result.row_count > 0) {
        int count = atoi(result.rows[0][0]);
        db_free_result(&result);
        
        if (count > 0) {
            pthread_mutex_unlock(&g_migration_mutex);
            return DB_SUCCESS; // 迁移已存在
        }
    }
    
    db_free_result(&result);
    
    // 插入迁移记录
    db_param_t params[4];
    params[0].type = DB_PARAM_INT;
    params[0].value.int_value = migration->version;
    
    params[1].type = DB_PARAM_TEXT;
    params[1].value.text_value = migration->description;
    params[1].copy_text = true;
    
    params[2].type = DB_PARAM_TEXT;
    params[2].value.text_value = migration->sql;
    params[2].copy_text = true;
    
    params[3].type = DB_PARAM_INT64;
    params[3].value.int64_value = migration->applied_at;
    
    const char* insert_sql = 
        "INSERT INTO migrations (version, description, sql, applied_at) "
        "VALUES (?, ?, ?, ?);";
    
    db_result = db_execute_params(insert_sql, params, 4, NULL);
    
    pthread_mutex_unlock(&g_migration_mutex);
    
    return db_result;
}

db_error_code_t db_migrate_apply(int target_version) {
    pthread_mutex_lock(&g_migration_mutex);
    
    // 获取当前版本
    int current_version = db_migrate_get_current_version();
    
    // 获取需要应用的迁移
    char sql[512];
    if (target_version == -1) {
        // 应用所有未应用的迁移
        snprintf(sql, sizeof(sql),
                 "SELECT version, description, sql FROM migrations "
                 "WHERE version > %d ORDER BY version ASC;",
                 current_version);
    } else {
        snprintf(sql, sizeof(sql),
                 "SELECT version, description, sql FROM migrations "
                 "WHERE version > %d AND version <= %d ORDER BY version ASC;",
                 current_version, target_version);
    }
    
    db_result_t result;
    db_error_code_t db_result = db_execute(sql, &result);
    
    if (db_result != DB_SUCCESS) {
        pthread_mutex_unlock(&g_migration_mutex);
        return db_result;
    }
    
    // 应用每个迁移
    for (int i = 0; i < result.row_count; i++) {
        int version = atoi(result.rows[i][0]);
        const char* description = result.rows[i][1];
        const char* migration_sql = result.rows[i][2];
        
        printf("Applying migration %d: %s\n", version, description);
        
        // 开始事务
        sqlite3* conn = NULL;
        db_error_code_t conn_result = db_get_connection(&conn);
        if (conn_result != DB_SUCCESS) {
            db_free_result(&result);
            pthread_mutex_unlock(&g_migration_mutex);
            return conn_result;
        }
        
        db_begin_transaction(conn);
        
        // 执行迁移SQL
        char* err_msg = NULL;
        int rc = sqlite3_exec(conn, migration_sql, NULL, NULL, &err_msg);
        
        if (rc != SQLITE_OK) {
            if (err_msg) {
                fprintf(stderr, "Migration %d failed: %s\n", version, err_msg);
                sqlite3_free(err_msg);
            }
            
            db_rollback_transaction(conn);
            db_release_connection(conn);
            db_free_result(&result);
            pthread_mutex_unlock(&g_migration_mutex);
            return DB_ERROR_QUERY;
        }
        
        // 更新迁移记录中的applied_at时间
        time_t now = time(NULL);
        char update_sql[256];
        snprintf(update_sql, sizeof(update_sql),
                 "UPDATE migrations SET applied_at = %ld WHERE version = %d;",
                 now, version);
        
        rc = sqlite3_exec(conn, update_sql, NULL, NULL, &err_msg);
        
        if (rc != SQLITE_OK) {
            if (err_msg) {
                fprintf(stderr, "Failed to update migration timestamp: %s\n", err_msg);
                sqlite3_free(err_msg);
            }
            
            db_rollback_transaction(conn);
            db_release_connection(conn);
            db_free_result(&result);
            pthread_mutex_unlock(&g_migration_mutex);
            return DB_ERROR_QUERY;
        }
        
        db_commit_transaction(conn);
        db_release_connection(conn);
        
        printf("Migration %d applied successfully\n", version);
    }
    
    db_free_result(&result);
    pthread_mutex_unlock(&g_migration_mutex);
    
    return DB_SUCCESS;
}

int db_migrate_get_current_version(void) {
    const char* sql = 
        "SELECT MAX(version) FROM migrations;";
    
    db_result_t result;
    db_error_code_t db_result = db_execute(sql, &result);
    
    int current_version = 0;
    
    if (db_result == DB_SUCCESS && result.row_count > 0 && result.rows[0][0]) {
        current_version = atoi(result.rows[0][0]);
    }
    
    db_free_result(&result);
    
    return current_version;
}

// ==================== 性能监控 ====================

db_stats_t db_get_statistics(void) {
    db_stats_t stats;
    
    pthread_mutex_lock(&g_stats_mutex);
    memcpy(&stats, &g_db_stats, sizeof(db_stats_t));
    pthread_mutex_unlock(&g_stats_mutex);
    
    // 获取当前活动连接数
    stats.active_connections = db_get_active_connections();
    
    // 获取数据库页面缓存信息（如果有）
    sqlite3* conn = NULL;
    if (db_get_connection(&conn) == DB_SUCCESS) {
        sqlite3_stmt* stmt;
        const char* pragma_sql = "PRAGMA cache_size;";
        
        if (sqlite3_prepare_v2(conn, pragma_sql, -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                stats.page_cache_size = sqlite3_column_int(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
        
        db_release_connection(conn);
    }
    
    return stats;
}

db_error_code_t db_reset_statistics(void) {
    pthread_mutex_lock(&g_stats_mutex);
    memset(&g_db_stats, 0, sizeof(g_db_stats));
    pthread_mutex_unlock(&g_stats_mutex);
    
    return DB_SUCCESS;
}

// ==================== 实用函数 ====================

db_error_code_t db_table_exists(const char* table_name, bool* exists) {
    if (!table_name || !exists) {
        return DB_ERROR_QUERY;
    }
    
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT name FROM sqlite_master WHERE type='table' AND name='%s';",
             table_name);
    
    db_result_t result;
    db_error_code_t db_result = db_execute(sql, &result);
    
    *exists = (db_result == DB_SUCCESS && result.row_count > 0);
    
    db_free_result(&result);
    
    return db_result;
}

int64_t db_get_last_insert_id(sqlite3* conn) {
    if (!conn) {
        return 0;
    }
    
    return sqlite3_last_insert_rowid(conn);
}

// ==================== 错误处理 ====================

const char* db_error_message(db_error_code_t error) {
    switch (error) {
        case DB_SUCCESS: return "Success";
        case DB_ERROR_CONNECTION: return "Database connection error";
        case DB_ERROR_QUERY: return "Query error";
        case DB_ERROR_TRANSACTION: return "Transaction error";
        case DB_ERROR_PREPARE: return "Statement preparation error";
        case DB_ERROR_BIND: return "Parameter binding error";
        case DB_ERROR_EXECUTE: return "Query execution error";
        case DB_ERROR_NO_DATA: return "No data found";
        case DB_ERROR_CONSTRAINT: return "Constraint violation";
        case DB_ERROR_TIMEOUT: return "Operation timeout";
        case DB_ERROR_BUSY: return "Database busy";
        case DB_ERROR_LOCKED: return "Database locked";
        case DB_ERROR_SCHEMA: return "Schema error";
        case DB_ERROR_DISK_FULL: return "Disk full";
        case DB_ERROR_READONLY: return "Database read-only";
        case DB_ERROR_INTERRUPT: return "Operation interrupted";
        case DB_ERROR_CORRUPT: return "Database corrupted";
        case DB_ERROR_NOT_FOUND: return "Not found";
        case DB_ERROR_FULL: return "Database full";
        case DB_ERROR_CANTOPEN: return "Cannot open database";
        case DB_ERROR_PROTOCOL: return "Protocol error";
        case DB_ERROR_EMPTY: return "Empty result";
        case DB_ERROR_MISMATCH: return "Type mismatch";
        case DB_ERROR_RANGE: return "Parameter out of range";
        case DB_ERROR_NOTADB: return "Not a database file";
        case DB_ERROR_AUTH: return "Authorization denied";
        case DB_ERROR_FORMAT: return "Format error";
        case DB_ERROR_OVERFLOW: return "Overflow error";
        case DB_ERROR_INTERNAL: return "Internal error";
        case DB_ERROR_MEMORY: return "Memory allocation error";
        default: return "Unknown error";
    }
}

db_error_code_t db_log_error(const char* sql, db_error_code_t error,
                            const char* additional_info) {
    pthread_mutex_lock(&g_error_log_mutex);
    
    int index = g_error_log_index % DB_ERROR_LOG_SIZE;
    
    strncpy(g_error_logs[index].sql, sql ? sql : "", sizeof(g_error_logs[index].sql) - 1);
    g_error_logs[index].error_code = error;
    strncpy(g_error_logs[index].error_msg, db_error_message(error),
            sizeof(g_error_logs[index].error_msg) - 1);
    
    if (additional_info) {
        strncpy(g_error_logs[index].additional_info, additional_info,
                sizeof(g_error_logs[index].additional_info) - 1);
    } else {
        g_error_logs[index].additional_info[0] = '\0';
    }
    
    g_error_logs[index].timestamp = time(NULL);
    
    g_error_log_index++;
    
    pthread_mutex_unlock(&g_error_log_mutex);
    
    return DB_SUCCESS;
}

// ==================== 回调函数支持 ====================

db_error_code_t db_set_query_callback(db_query_callback_t callback, void* user_data) {
    g_query_callback = callback;
    g_query_callback_data = user_data;
    return DB_SUCCESS;
}

db_error_code_t db_set_backup_callback(db_backup_callback_t callback, void* user_data) {
    g_backup_callback = callback;
    g_backup_callback_data = user_data;
    return DB_SUCCESS;
}

// ==================== SQLite错误码转换 ====================

static const char* db_sqlite_error_to_string(int sqlite_error) {
    switch (sqlite_error) {
        case SQLITE_OK: return "Successful result";
        case SQLITE_ERROR: return "SQL error or missing database";
        case SQLITE_INTERNAL: return "Internal logic error in SQLite";
        case SQLITE_PERM: return "Access permission denied";
        case SQLITE_ABORT: return "Callback routine requested an abort";
        case SQLITE_BUSY: return "The database file is locked";
        case SQLITE_LOCKED: return "A table in the database is locked";
        case SQLITE_NOMEM: return "A malloc() failed";
        case SQLITE_READONLY: return "Attempt to write a readonly database";
        case SQLITE_INTERRUPT: return "Operation terminated by sqlite3_interrupt()";
        case SQLITE_IOERR: return "Some kind of disk I/O error occurred";
        case SQLITE_CORRUPT: return "The database disk image is malformed";
        case SQLITE_NOTFOUND: return "Unknown opcode in sqlite3_file_control()";
        case SQLITE_FULL: return "Insertion failed because database is full";
        case SQLITE_CANTOPEN: return "Unable to open the database file";
        case SQLITE_PROTOCOL: return "Database lock protocol error";
        case SQLITE_EMPTY: return "Database is empty";
        case SQLITE_SCHEMA: return "The database schema changed";
        case SQLITE_TOOBIG: return "String or BLOB exceeds size limit";
        case SQLITE_CONSTRAINT: return "Abort due to constraint violation";
        case SQLITE_MISMATCH: return "Data type mismatch";
        case SQLITE_MISUSE: return "Library used incorrectly";
        case SQLITE_NOLFS: return "Uses OS features not supported on host";
        case SQLITE_AUTH: return "Authorization denied";
        case SQLITE_FORMAT: return "Auxiliary database format error";
        case SQLITE_RANGE: return "2nd parameter to sqlite3_bind out of range";
        case SQLITE_NOTADB: return "File opened that is not a database file";
        case SQLITE_NOTICE: return "Notifications from sqlite3_log()";
        case SQLITE_WARNING: return "Warnings from sqlite3_log()";
        case SQLITE_ROW: return "sqlite3_step() has another row ready";
        case SQLITE_DONE: return "sqlite3_step() has finished executing";
        default: return "Unknown SQLite error";
    }
}