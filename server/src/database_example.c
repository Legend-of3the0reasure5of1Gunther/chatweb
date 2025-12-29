#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "database.h"

// 数据库配置
static db_config_t db_config = {
    .db_path = "secure_chat.db",
    .backup_path = "./backups",
    .max_connections = 5,
    .query_timeout_ms = 5000,
    .max_retries = 3,
    .backup_interval = 3600,  // 1小时
    .wal_mode = true,
    .page_size = 4096,
    .cache_size = -2000,      // ~8MB
    .foreign_keys = true,
    .synchronous = true,
    .journal_mode = true,
    .temp_store = true,
    .autovacuum = true,
    .secure_delete = false
};

// 查询回调函数
static void query_callback(const char* sql, db_error_code_t error,
                          int64_t elapsed_ms, void* user_data) {
    printf("Query: %s\n", sql);
    printf("Result: %s (%lld ms)\n", db_error_message(error), elapsed_ms);
    if (error != DB_SUCCESS) {
        printf("Error details logged\n");
    }
}

// 备份回调函数
static void backup_callback(const char* backup_path, uint64_t size,
                           time_t backup_time, void* user_data) {
    printf("Backup created: %s\n", backup_path);
    printf("Size: %llu bytes, Time: %s", size, ctime(&backup_time));
}

// 示例：创建用户表
static db_error_code_t create_user_table(void) {
    const char* sql = 
        "CREATE TABLE IF NOT EXISTS users ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "username TEXT UNIQUE NOT NULL,"
        "password_hash TEXT NOT NULL,"
        "email TEXT UNIQUE NOT NULL,"
        "nickname TEXT NOT NULL,"
        "avatar_id INTEGER DEFAULT 0,"
        "status INTEGER DEFAULT 0,"
        "status_message TEXT DEFAULT '',"
        "last_seen INTEGER DEFAULT 0,"
        "friend_count INTEGER DEFAULT 0,"
        "group_count INTEGER DEFAULT 0,"
        "is_verified INTEGER DEFAULT 0,"
        "is_premium INTEGER DEFAULT 0,"
        "created_at INTEGER NOT NULL,"
        "updated_at INTEGER NOT NULL"
        ");";
    
    db_result_t result;
    db_error_code_t db_result = db_execute(sql, &result);
    db_free_result(&result);
    
    return db_result;
}

// 示例：创建消息表
static db_error_code_t create_message_table(void) {
    const char* sql = 
        "CREATE TABLE IF NOT EXISTS messages ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "sender_id INTEGER NOT NULL,"
        "receiver_id INTEGER NOT NULL,"
        "message_type INTEGER NOT NULL,"
        "content TEXT NOT NULL,"
        "timestamp INTEGER NOT NULL,"
        "status INTEGER DEFAULT 0,"
        "is_encrypted INTEGER DEFAULT 0,"
        "content_hash TEXT,"
        "reply_to_id INTEGER DEFAULT 0,"
        "FOREIGN KEY (sender_id) REFERENCES users(id) ON DELETE CASCADE,"
        "FOREIGN KEY (receiver_id) REFERENCES users(id) ON DELETE CASCADE"
        ");";
    
    db_result_t result;
    db_error_code_t db_result = db_execute(sql, &result);
    db_free_result(&result);
    
    return db_result;
}

// 示例：插入用户
static db_error_code_t insert_user(const char* username, const char* password_hash,
                                  const char* email, const char* nickname) {
    const char* sql = 
        "INSERT INTO users (username, password_hash, email, nickname, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?);";
    
    time_t now = time(NULL);
    
    db_param_t params[6];
    
    params[0].type = DB_PARAM_TEXT;
    params[0].value.text_value = username;
    params[0].copy_text = true;
    
    params[1].type = DB_PARAM_TEXT;
    params[1].value.text_value = password_hash;
    params[1].copy_text = true;
    
    params[2].type = DB_PARAM_TEXT;
    params[2].value.text_value = email;
    params[2].copy_text = true;
    
    params[3].type = DB_PARAM_TEXT;
    params[3].value.text_value = nickname;
    params[3].copy_text = true;
    
    params[4].type = DB_PARAM_INT64;
    params[4].value.int64_value = now;
    
    params[5].type = DB_PARAM_INT64;
    params[5].value.int64_value = now;
    
    db_error_code_t db_result = db_execute_params(sql, params, 6, NULL);
    
    if (db_result == DB_SUCCESS) {
        printf("User '%s' inserted successfully\n", username);
    } else {
        printf("Failed to insert user '%s': %s\n", username, db_error_message(db_result));
    }
    
    return db_result;
}

// 示例：查询用户
static db_error_code_t query_user_by_username(const char* username) {
    const char* sql = 
        "SELECT id, username, email, nickname, status, last_seen "
        "FROM users WHERE username = ?;";
    
    db_param_t params[1];
    params[0].type = DB_PARAM_TEXT;
    params[0].value.text_value = username;
    params[0].copy_text = true;
    
    db_result_t result;
    db_error_code_t db_result = db_execute_params(sql, params, 1, &result);
    
    if (db_result == DB_SUCCESS) {
        if (result.row_count > 0) {
            printf("Found user:\n");
            printf("  ID: %s\n", result.rows[0][0]);
            printf("  Username: %s\n", result.rows[0][1]);
            printf("  Email: %s\n", result.rows[0][2]);
            printf("  Nickname: %s\n", result.rows[0][3]);
            printf("  Status: %s\n", result.rows[0][4]);
            printf("  Last seen: %s\n", result.rows[0][5]);
        } else {
            printf("User '%s' not found\n", username);
        }
    } else {
        printf("Query failed: %s\n", db_error_message(db_result));
    }
    
    db_free_result(&result);
    return db_result;
}

// 示例：批量插入消息
static db_error_code_t batch_insert_messages(int count) {
    printf("Inserting %d messages...\n", count);
    
    // 准备批量插入参数
    db_param_t** rows = (db_param_t**)safe_calloc(count, sizeof(db_param_t*), "batch_rows");
    if (!rows) {
        return DB_ERROR_MEMORY;
    }
    
    time_t now = time(NULL);
    
    for (int i = 0; i < count; i++) {
        rows[i] = (db_param_t*)safe_calloc(6, sizeof(db_param_t), "row_params");
        if (!rows[i]) {
            for (int j = 0; j < i; j++) {
                safe_free((void**)&rows[j]);
            }
            safe_free((void**)&rows);
            return DB_ERROR_MEMORY;
        }
        
        // 发送者ID (1-100之间的随机用户)
        rows[i][0].type = DB_PARAM_INT;
        rows[i][0].value.int_value = (rand() % 100) + 1;
        
        // 接收者ID (1-100之间的随机用户)
        rows[i][1].type = DB_PARAM_INT;
        rows[i][1].value.int_value = (rand() % 100) + 1;
        
        // 消息类型 (0=文本)
        rows[i][2].type = DB_PARAM_INT;
        rows[i][2].value.int_value = 0;
        
        // 消息内容
        char message[256];
        snprintf(message, sizeof(message), "Test message %d from user %d", i, rows[i][0].value.int_value);
        rows[i][3].type = DB_PARAM_TEXT;
        rows[i][3].value.text_value = message;
        rows[i][3].copy_text = true;
        
        // 时间戳
        rows[i][4].type = DB_PARAM_INT64;
        rows[i][4].value.int64_value = now - (rand() % 86400); // 过去24小时内
        
        // 消息状态 (1=已发送)
        rows[i][5].type = DB_PARAM_INT;
        rows[i][5].value.int_value = 1;
    }
    
    const char* columns[] = {"sender_id", "receiver_id", "message_type", 
                            "content", "timestamp", "status"};
    
    db_error_code_t db_result = db_insert_batch("messages", columns, 
                                               (const db_param_t**)rows, 
                                               count, 6);
    
    // 清理
    for (int i = 0; i < count; i++) {
        // 注意：db_insert_batch已经复制了文本数据，所以这里只需要释放数组本身
        safe_free((void**)&rows[i]);
    }
    safe_free((void**)&rows);
    
    if (db_result == DB_SUCCESS) {
        printf("Successfully inserted %d messages\n", count);
    } else {
        printf("Failed to insert messages: %s\n", db_error_message(db_result));
    }
    
    return db_result;
}

// 示例：使用事务
static db_error_code_t transaction_example(void) {
    sqlite3* conn = NULL;
    db_error_code_t db_result = db_get_connection(&conn);
    if (db_result != DB_SUCCESS) {
        return db_result;
    }
    
    printf("Starting transaction example...\n");
    
    // 开始事务
    db_result = db_begin_transaction(conn);
    if (db_result != DB_SUCCESS) {
        db_release_connection(conn);
        return db_result;
    }
    
    // 创建保存点
    db_result = db_savepoint(conn, "before_updates");
    if (db_result != DB_SUCCESS) {
        db_rollback_transaction(conn);
        db_release_connection(conn);
        return db_result;
    }
    
    // 执行一些更新操作
    const char* update_sql = "UPDATE users SET status = 1 WHERE id = 1;";
    char* err_msg = NULL;
    int rc = sqlite3_exec(conn, update_sql, NULL, NULL, &err_msg);
    
    if (rc != SQLITE_OK) {
        if (err_msg) {
            fprintf(stderr, "Update failed: %s\n", err_msg);
            sqlite3_free(err_msg);
        }
        
        // 回滚到保存点
        db_rollback_to_savepoint(conn, "before_updates");
        db_release_savepoint(conn, "before_updates");
        db_rollback_transaction(conn);
        db_release_connection(conn);
        return DB_ERROR_QUERY;
    }
    
    // 提交保存点
    db_result = db_release_savepoint(conn, "before_updates");
    if (db_result != DB_SUCCESS) {
        db_rollback_transaction(conn);
        db_release_connection(conn);
        return db_result;
    }
    
    // 提交事务
    db_result = db_commit_transaction(conn);
    if (db_result != DB_SUCCESS) {
        db_release_connection(conn);
        return db_result;
    }
    
    printf("Transaction completed successfully\n");
    
    db_release_connection(conn);
    return DB_SUCCESS;
}

// 示例：数据库维护
static db_error_code_t maintenance_example(void) {
    printf("Running database maintenance...\n");
    
    // 检查数据库完整性
    bool integrity_ok = false;
    db_error_code_t db_result = db_check_integrity(&integrity_ok);
    if (db_result != DB_SUCCESS) {
        printf("Integrity check failed: %s\n", db_error_message(db_result));
    } else if (!integrity_ok) {
        printf("WARNING: Database integrity check failed!\n");
    } else {
        printf("Database integrity check passed\n");
    }
    
    // 执行VACUUM
    db_result = db_vacuum();
    if (db_result != DB_SUCCESS) {
        printf("Vacuum failed: %s\n", db_error_message(db_result));
    } else {
        printf("Vacuum completed\n");
    }
    
    // 执行ANALYZE
    db_result = db_analyze();
    if (db_result != DB_SUCCESS) {
        printf("Analyze failed: %s\n", db_error_message(db_result));
    } else {
        printf("Analyze completed\n");
    }
    
    return DB_SUCCESS;
}

// 主测试函数
int main(int argc, char* argv[]) {
    printf("=== Database Module Test ===\n");
    
    // 初始化随机数种子
    srand(time(NULL));
    
    // 初始化数据库模块
    db_error_code_t result = db_init(&db_config);
    if (result != DB_SUCCESS) {
        fprintf(stderr, "Failed to initialize database: %s\n", db_error_message(result));
        return 1;
    }
    
    printf("Database module initialized successfully\n");
    
    // 设置回调函数
    db_set_query_callback(query_callback, NULL);
    db_set_backup_callback(backup_callback, NULL);
    
    // 创建表
    result = create_user_table();
    if (result != DB_SUCCESS) {
        fprintf(stderr, "Failed to create user table: %s\n", db_error_message(result));
        db_cleanup();
        return 1;
    }
    
    result = create_message_table();
    if (result != DB_SUCCESS) {
        fprintf(stderr, "Failed to create message table: %s\n", db_error_message(result));
        db_cleanup();
        return 1;
    }
    
    printf("Tables created successfully\n");
    
    // 插入测试用户
    result = insert_user("alice", "hash1", "alice@example.com", "Alice");
    result = insert_user("bob", "hash2", "bob@example.com", "Bob");
    result = insert_user("charlie", "hash3", "charlie@example.com", "Charlie");
    
    // 查询用户
    result = query_user_by_username("alice");
    result = query_user_by_username("bob");
    result = query_user_by_username("nonexistent");
    
    // 批量插入消息
    result = batch_insert_messages(100);
    
    // 事务示例
    result = transaction_example();
    
    // 数据库维护示例
    result = maintenance_example();
    
    // 获取统计信息
    db_stats_t stats = db_get_statistics();
    printf("\n=== Database Statistics ===\n");
    printf("Total queries: %lld\n", stats.total_queries);
    printf("Failed queries: %lld\n", stats.failed_queries);
    printf("Transactions: %lld\n", stats.transaction_count);
    printf("Rollbacks: %lld\n", stats.rollback_count);
    printf("Avg query time: %lld ms\n", stats.avg_query_time_ms);
    printf("Max query time: %lld ms\n", stats.max_query_time_ms);
    printf("Active connections: %lld\n", stats.active_connections);
    
    // 创建备份
    printf("\nCreating database backup...\n");
    result = db_create_backup("./backups/test_backup.db", true);
    if (result == DB_SUCCESS) {
        printf("Backup created successfully\n");
    } else {
        printf("Backup failed: %s\n", db_error_message(result));
    }
    
    // 清理
    db_cleanup();
    
    printf("\n=== Database Test Completed ===\n");
    return 0;
}