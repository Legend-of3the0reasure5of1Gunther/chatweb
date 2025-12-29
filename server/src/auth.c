#include "auth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <regex.h>
#include <sqlite3.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// ==================== 内部数据结构 ====================
typedef struct {
    sqlite3* db;
    pthread_mutex_t db_mutex;
    char secret_key[256];
    uint8_t key_derived[EVP_MAX_MD_SIZE];
    size_t key_length;
    pthread_mutex_t sessions_mutex;
    pthread_mutex_t users_mutex;
    auth_callback_t event_callback;
} auth_context_t;

static auth_context_t* g_auth_ctx = NULL;

// ==================== 内部函数声明 ====================
static auth_error_code_t init_database(void);
static auth_error_code_t create_tables(void);
static auth_error_code_t hash_password_internal(const char* password,
                                               const uint8_t* salt,
                                               uint8_t* hash,
                                               uint32_t* hash_length);
static auth_error_code_t generate_access_token(uint64_t user_id,
                                              const char* client_ip,
                                              char* token);
static auth_error_code_t generate_refresh_token(uint64_t user_id,
                                               const char* client_ip,
                                               char* token);
static bool validate_token_signature(const char* token);
static auth_error_code_t check_password_strength(const char* password);
static auth_error_code_t rate_limit_check(const char* client_ip,
                                         const char* username);
static auth_error_code_t log_authentication_attempt(uint64_t user_id,
                                                   const char* client_ip,
                                                   bool success,
                                                   const char* reason);

// ==================== 初始化/清理函数 ====================

auth_error_code_t auth_init(const char* secret_key, const char* db_path) {
    if (g_auth_ctx != NULL) {
        return AUTH_SUCCESS; // 已经初始化
    }
    
    if (strlen(secret_key) < AUTH_TOKEN_SECRET_MIN_LENGTH) {
        fprintf(stderr, "Secret key too short, minimum %d characters required\n",
                AUTH_TOKEN_SECRET_MIN_LENGTH);
        return AUTH_INVALID_CREDENTIALS;
    }
    
    // 分配上下文
    g_auth_ctx = (auth_context_t*)safe_calloc(1, sizeof(auth_context_t), "auth_context");
    if (!g_auth_ctx) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    // 保存密钥
    strncpy(g_auth_ctx->secret_key, secret_key, sizeof(g_auth_ctx->secret_key) - 1);
    g_auth_ctx->secret_key[sizeof(g_auth_ctx->secret_key) - 1] = '\0';
    
    // 派生密钥
    if (!PKCS5_PBKDF2_HMAC(secret_key, strlen(secret_key),
                           (const unsigned char*)"auth_salt", 9,
                           100000, EVP_sha256(),
                           EVP_MAX_MD_SIZE, g_auth_ctx->key_derived)) {
        fprintf(stderr, "Failed to derive key\n");
        safe_free((void**)&g_auth_ctx);
        return AUTH_INVALID_CREDENTIALS;
    }
    g_auth_ctx->key_length = EVP_MAX_MD_SIZE;
    
    // 初始化互斥锁
    if (pthread_mutex_init(&g_auth_ctx->db_mutex, NULL) != 0 ||
        pthread_mutex_init(&g_auth_ctx->sessions_mutex, NULL) != 0 ||
        pthread_mutex_init(&g_auth_ctx->users_mutex, NULL) != 0) {
        fprintf(stderr, "Failed to initialize mutexes\n");
        safe_free((void**)&g_auth_ctx);
        return AUTH_INVALID_CREDENTIALS;
    }
    
    // 初始化数据库
    auth_error_code_t result = init_database();
    if (result != AUTH_SUCCESS) {
        auth_cleanup();
        return result;
    }
    
    printf("Authentication module initialized successfully\n");
    return AUTH_SUCCESS;
}

void auth_cleanup(void) {
    if (!g_auth_ctx) {
        return;
    }
    
    pthread_mutex_destroy(&g_auth_ctx->users_mutex);
    pthread_mutex_destroy(&g_auth_ctx->sessions_mutex);
    pthread_mutex_destroy(&g_auth_ctx->db_mutex);
    
    if (g_auth_ctx->db) {
        sqlite3_close(g_auth_ctx->db);
    }
    
    memset(g_auth_ctx->secret_key, 0, sizeof(g_auth_ctx->secret_key));
    memset(g_auth_ctx->key_derived, 0, sizeof(g_auth_ctx->key_derived));
    
    safe_free((void**)&g_auth_ctx);
    printf("Authentication module cleaned up\n");
}

// ==================== 数据库初始化 ====================

static auth_error_code_t init_database(void) {
    int rc;
    
    rc = sqlite3_open(":memory:", &g_auth_ctx->db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(g_auth_ctx->db));
        return AUTH_INVALID_CREDENTIALS;
    }
    
    // 启用外键约束
    sqlite3_exec(g_auth_ctx->db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
    
    // 创建表
    return create_tables();
}

static auth_error_code_t create_tables(void) {
    const char* users_table_sql =
        "CREATE TABLE IF NOT EXISTS users ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "username TEXT UNIQUE NOT NULL,"
        "nickname TEXT NOT NULL,"
        "email TEXT UNIQUE NOT NULL,"
        "password_hash BLOB NOT NULL,"
        "password_salt BLOB NOT NULL,"
        "login_attempts INTEGER DEFAULT 0,"
        "last_login_attempt INTEGER DEFAULT 0,"
        "account_locked_until INTEGER DEFAULT 0,"
        "created_at INTEGER NOT NULL,"
        "updated_at INTEGER NOT NULL,"
        "is_active INTEGER DEFAULT 1,"
        "is_verified INTEGER DEFAULT 0,"
        "is_admin INTEGER DEFAULT 0,"
        "last_login INTEGER DEFAULT 0,"
        "last_login_ip TEXT"
        ");";
    
    const char* sessions_table_sql =
        "CREATE TABLE IF NOT EXISTS sessions ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "user_id INTEGER NOT NULL,"
        "access_token TEXT UNIQUE NOT NULL,"
        "refresh_token TEXT UNIQUE NOT NULL,"
        "client_ip TEXT NOT NULL,"
        "user_agent TEXT,"
        "created_at INTEGER NOT NULL,"
        "expires_at INTEGER NOT NULL,"
        "refresh_expires_at INTEGER NOT NULL,"
        "is_valid INTEGER DEFAULT 1,"
        "FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
        ");";
    
    const char* permissions_table_sql =
        "CREATE TABLE IF NOT EXISTS permissions ("
        "user_id INTEGER NOT NULL,"
        "permission_name TEXT NOT NULL,"
        "resource TEXT NOT NULL,"
        "action TEXT NOT NULL,"
        "granted_at INTEGER NOT NULL,"
        "expires_at INTEGER DEFAULT 0,"
        "PRIMARY KEY (user_id, resource, action),"
        "FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
        ");";
    
    const char* roles_table_sql =
        "CREATE TABLE IF NOT EXISTS roles ("
        "user_id INTEGER NOT NULL,"
        "role_name TEXT NOT NULL,"
        "assigned_at INTEGER NOT NULL,"
        "PRIMARY KEY (user_id, role_name),"
        "FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
        ");";
    
    const char* security_logs_table_sql =
        "CREATE TABLE IF NOT EXISTS security_logs ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "user_id INTEGER,"
        "event_type TEXT NOT NULL,"
        "details TEXT,"
        "client_ip TEXT NOT NULL,"
        "created_at INTEGER NOT NULL"
        ");";
    
    const char* indexes_sql[] = {
        "CREATE INDEX IF NOT EXISTS idx_users_username ON users(username);",
        "CREATE INDEX IF NOT EXISTS idx_users_email ON users(email);",
        "CREATE INDEX IF NOT EXISTS idx_sessions_access_token ON sessions(access_token);",
        "CREATE INDEX IF NOT EXISTS idx_sessions_refresh_token ON sessions(refresh_token);",
        "CREATE INDEX IF NOT EXISTS idx_sessions_user_id ON sessions(user_id);",
        "CREATE INDEX IF NOT EXISTS idx_sessions_expires_at ON sessions(expires_at);",
        "CREATE INDEX IF NOT EXISTS idx_permissions_user_id ON permissions(user_id);",
        "CREATE INDEX IF NOT EXISTS idx_roles_user_id ON roles(user_id);",
        "CREATE INDEX IF NOT EXISTS idx_security_logs_user_id ON security_logs(user_id);",
        "CREATE INDEX IF NOT EXISTS idx_security_logs_created_at ON security_logs(created_at);"
    };
    
    char* err_msg = NULL;
    int rc;
    
    pthread_mutex_lock(&g_auth_ctx->db_mutex);
    
    // 创建表
    rc = sqlite3_exec(g_auth_ctx->db, users_table_sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (users): %s\n", err_msg);
        sqlite3_free(err_msg);
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        return AUTH_INVALID_CREDENTIALS;
    }
    
    rc = sqlite3_exec(g_auth_ctx->db, sessions_table_sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (sessions): %s\n", err_msg);
        sqlite3_free(err_msg);
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        return AUTH_INVALID_CREDENTIALS;
    }
    
    rc = sqlite3_exec(g_auth_ctx->db, permissions_table_sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (permissions): %s\n", err_msg);
        sqlite3_free(err_msg);
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        return AUTH_INVALID_CREDENTIALS;
    }
    
    rc = sqlite3_exec(g_auth_ctx->db, roles_table_sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (roles): %s\n", err_msg);
        sqlite3_free(err_msg);
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        return AUTH_INVALID_CREDENTIALS;
    }
    
    rc = sqlite3_exec(g_auth_ctx->db, security_logs_table_sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (security_logs): %s\n", err_msg);
        sqlite3_free(err_msg);
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        return AUTH_INVALID_CREDENTIALS;
    }
    
    // 创建索引
    for (size_t i = 0; i < sizeof(indexes_sql) / sizeof(indexes_sql[0]); i++) {
        rc = sqlite3_exec(g_auth_ctx->db, indexes_sql[i], NULL, NULL, &err_msg);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "SQL error (index %zu): %s\n", i, err_msg);
            sqlite3_free(err_msg);
        }
    }
    
    pthread_mutex_unlock(&g_auth_ctx->db_mutex);
    
    // 创建默认管理员用户（仅当用户表为空时）
    sqlite3_stmt* stmt;
    const char* check_sql = "SELECT COUNT(*) FROM users;";
    
    pthread_mutex_lock(&g_auth_ctx->db_mutex);
    rc = sqlite3_prepare_v2(g_auth_ctx->db, check_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        return AUTH_SUCCESS;
    }
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int count = sqlite3_column_int(stmt, 0);
        if (count == 0) {
            // 创建默认管理员
            auth_user_t admin_user;
            strncpy(admin_user.username, "admin", MAX_USERNAME_LEN - 1);
            strncpy(admin_user.nickname, "Administrator", MAX_NICKNAME_LEN - 1);
            strncpy(admin_user.email, "admin@localhost", MAX_EMAIL_LEN - 1);
            admin_user.is_admin = 1;
            admin_user.is_active = 1;
            admin_user.is_verified = 1;
            admin_user.created_at = time(NULL);
            admin_user.updated_at = admin_user.created_at;
            
            // 创建默认密码 "admin123"
            uint8_t salt[AUTH_SALT_LENGTH];
            uint8_t hash[EVP_MAX_MD_SIZE];
            uint32_t hash_length;
            
            auth_generate_random_salt(salt);
            auth_generate_password_hash("admin123", salt, hash, &hash_length);
            
            memcpy(admin_user.password_salt, salt, AUTH_SALT_LENGTH);
            memcpy(admin_user.password_hash, hash, hash_length);
            admin_user.password_hash_length = hash_length;
            
            // 插入数据库
            const char* insert_sql = 
                "INSERT INTO users (username, nickname, email, password_hash, "
                "password_salt, login_attempts, last_login_attempt, account_locked_until, "
                "created_at, updated_at, is_active, is_verified, is_admin) "
                "VALUES (?, ?, ?, ?, ?, 0, 0, 0, ?, ?, 1, 1, 1);";
            
            sqlite3_stmt* insert_stmt;
            rc = sqlite3_prepare_v2(g_auth_ctx->db, insert_sql, -1, &insert_stmt, NULL);
            if (rc == SQLITE_OK) {
                sqlite3_bind_text(insert_stmt, 1, admin_user.username, -1, SQLITE_STATIC);
                sqlite3_bind_text(insert_stmt, 2, admin_user.nickname, -1, SQLITE_STATIC);
                sqlite3_bind_text(insert_stmt, 3, admin_user.email, -1, SQLITE_STATIC);
                sqlite3_bind_blob(insert_stmt, 4, admin_user.password_hash, 
                                 admin_user.password_hash_length, SQLITE_STATIC);
                sqlite3_bind_blob(insert_stmt, 5, admin_user.password_salt, 
                                 AUTH_SALT_LENGTH, SQLITE_STATIC);
                sqlite3_bind_int64(insert_stmt, 6, admin_user.created_at);
                sqlite3_bind_int64(insert_stmt, 7, admin_user.updated_at);
                
                if (sqlite3_step(insert_stmt) != SQLITE_DONE) {
                    fprintf(stderr, "Failed to create admin user: %s\n", 
                            sqlite3_errmsg(g_auth_ctx->db));
                }
                
                sqlite3_finalize(insert_stmt);
            }
            
            printf("Created default admin user with password 'admin123'\n");
            printf("WARNING: Please change the default password immediately!\n");
        }
    }
    
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_auth_ctx->db_mutex);
    
    return AUTH_SUCCESS;
}

// ==================== 用户注册 ====================

auth_error_code_t auth_register_user(const char* username, const char* password,
                                    const char* email, const char* nickname,
                                    const char* client_ip, auth_token_pair_t* tokens) {
    if (!g_auth_ctx) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    // 参数验证
    if (!username || !password || !email || !nickname || !client_ip) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    // 验证输入
    if (!auth_validate_username(username)) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    if (!auth_validate_password(password)) {
        return AUTH_PASSWORD_TOO_WEAK;
    }
    
    if (!auth_validate_email(email)) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    // 检查用户名是否已存在
    pthread_mutex_lock(&g_auth_ctx->db_mutex);
    
    const char* check_user_sql = "SELECT id FROM users WHERE username = ? OR email = ?;";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(g_auth_ctx->db, check_user_sql, -1, &stmt, NULL);
    
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        return AUTH_INVALID_CREDENTIALS;
    }
    
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, email, -1, SQLITE_STATIC);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        return AUTH_INVALID_CREDENTIALS; // 用户已存在
    }
    
    sqlite3_finalize(stmt);
    
    // 创建密码哈希
    uint8_t salt[AUTH_SALT_LENGTH];
    uint8_t hash[EVP_MAX_MD_SIZE];
    uint32_t hash_length;
    
    auth_generate_random_salt(salt);
    auth_error_code_t hash_result = auth_generate_password_hash(password, salt, hash, &hash_length);
    if (hash_result != AUTH_SUCCESS) {
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        return hash_result;
    }
    
    // 插入用户
    time_t now = time(NULL);
    const char* insert_sql = 
        "INSERT INTO users (username, nickname, email, password_hash, password_salt, "
        "login_attempts, last_login_attempt, account_locked_until, "
        "created_at, updated_at, is_active, is_verified, is_admin) "
        "VALUES (?, ?, ?, ?, ?, 0, 0, 0, ?, ?, 1, 0, 0);";
    
    rc = sqlite3_prepare_v2(g_auth_ctx->db, insert_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        return AUTH_INVALID_CREDENTIALS;
    }
    
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, nickname, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, email, -1, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 4, hash, hash_length, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 5, salt, AUTH_SALT_LENGTH, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, now);
    sqlite3_bind_int64(stmt, 7, now);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        return AUTH_INVALID_CREDENTIALS;
    }
    
    uint64_t user_id = sqlite3_last_insert_rowid(g_auth_ctx->db);
    pthread_mutex_unlock(&g_auth_ctx->db_mutex);
    
    // 记录安全事件
    char details[256];
    snprintf(details, sizeof(details), "User registered: %s", username);
    auth_log_security_event(user_id, "USER_REGISTERED", details, client_ip);
    
    // 触发回调
    if (g_auth_ctx->event_callback) {
        g_auth_ctx->event_callback(user_id, "USER_REGISTERED", username, strlen(username));
    }
    
    // 可选：自动登录
    if (tokens) {
        return auth_login_user(username, password, client_ip, "Registration", tokens);
    }
    
    return AUTH_SUCCESS;
}

// ==================== 用户登录 ====================

auth_error_code_t auth_login_user(const char* username, const char* password,
                                 const char* client_ip, const char* user_agent,
                                 auth_token_pair_t* tokens) {
    if (!g_auth_ctx || !username || !password || !client_ip) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    // 速率限制检查
    auth_error_code_t rate_limit = rate_limit_check(client_ip, username);
    if (rate_limit != AUTH_SUCCESS) {
        return rate_limit;
    }
    
    pthread_mutex_lock(&g_auth_ctx->db_mutex);
    
    // 获取用户信息
    const char* get_user_sql = 
        "SELECT id, username, nickname, email, password_hash, password_salt, "
        "login_attempts, last_login_attempt, account_locked_until, is_active, "
        "is_verified, is_admin, created_at FROM users WHERE username = ? OR email = ?;";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(g_auth_ctx->db, get_user_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        return AUTH_INVALID_CREDENTIALS;
    }
    
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, username, -1, SQLITE_STATIC);
    
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        
        // 记录失败尝试
        log_authentication_attempt(0, client_ip, false, "User not found");
        return AUTH_INVALID_CREDENTIALS;
    }
    
    // 提取用户数据
    auth_user_t user;
    user.user_id = sqlite3_column_int64(stmt, 0);
    strncpy(user.username, (const char*)sqlite3_column_text(stmt, 1), MAX_USERNAME_LEN - 1);
    strncpy(user.nickname, (const char*)sqlite3_column_text(stmt, 2), MAX_NICKNAME_LEN - 1);
    strncpy(user.email, (const char*)sqlite3_column_text(stmt, 3), MAX_EMAIL_LEN - 1);
    
    const void* stored_hash_blob = sqlite3_column_blob(stmt, 4);
    user.password_hash_length = sqlite3_column_bytes(stmt, 4);
    memcpy(user.password_hash, stored_hash_blob, user.password_hash_length);
    
    const void* salt_blob = sqlite3_column_blob(stmt, 5);
    memcpy(user.password_salt, salt_blob, AUTH_SALT_LENGTH);
    
    user.login_attempts = sqlite3_column_int(stmt, 6);
    user.last_login_attempt = sqlite3_column_int64(stmt, 7);
    user.account_locked_until = sqlite3_column_int64(stmt, 8);
    user.is_active = sqlite3_column_int(stmt, 9);
    user.is_verified = sqlite3_column_int(stmt, 10);
    user.is_admin = sqlite3_column_int(stmt, 11);
    user.created_at = sqlite3_column_int64(stmt, 12);
    
    sqlite3_finalize(stmt);
    
    // 检查账户状态
    time_t now = time(NULL);
    
    if (user.account_locked_until > now) {
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        log_authentication_attempt(user.user_id, client_ip, false, "Account locked");
        return AUTH_ACCOUNT_LOCKED;
    }
    
    if (!user.is_active) {
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        log_authentication_attempt(user.user_id, client_ip, false, "Account disabled");
        return AUTH_ACCOUNT_DISABLED;
    }
    
    // 验证密码
    bool password_valid = auth_verify_password(password, user.password_salt,
                                              user.password_hash, user.password_hash_length);
    
    if (!password_valid) {
        // 增加失败尝试计数
        user.login_attempts++;
        user.last_login_attempt = now;
        
        if (user.login_attempts >= AUTH_MAX_LOGIN_ATTEMPTS) {
            user.account_locked_until = now + AUTH_LOCKOUT_DURATION;
            user.login_attempts = 0;
            
            // 记录锁定事件
            char details[256];
            snprintf(details, sizeof(details), 
                    "Account locked after %d failed attempts", AUTH_MAX_LOGIN_ATTEMPTS);
            auth_log_security_event(user.user_id, "ACCOUNT_LOCKED", details, client_ip);
        }
        
        // 更新数据库
        const char* update_sql = 
            "UPDATE users SET login_attempts = ?, last_login_attempt = ?, "
            "account_locked_until = ? WHERE id = ?;";
        
        rc = sqlite3_prepare_v2(g_auth_ctx->db, update_sql, -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, user.login_attempts);
            sqlite3_bind_int64(stmt, 2, user.last_login_attempt);
            sqlite3_bind_int64(stmt, 3, user.account_locked_until);
            sqlite3_bind_int64(stmt, 4, user.user_id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        
        // 记录失败尝试
        log_authentication_attempt(user.user_id, client_ip, false, "Invalid password");
        
        if (user.account_locked_until > now) {
            return AUTH_ACCOUNT_LOCKED;
        }
        
        return AUTH_INVALID_CREDENTIALS;
    }
    
    // 登录成功，重置失败计数
    user.login_attempts = 0;
    user.last_login_attempt = now;
    user.account_locked_until = 0;
    user.last_login = now;
    strncpy(user.last_login_ip, client_ip, sizeof(user.last_login_ip) - 1);
    
    // 更新数据库
    const char* update_sql = 
        "UPDATE users SET login_attempts = 0, last_login_attempt = ?, "
        "account_locked_until = 0, last_login = ?, last_login_ip = ? WHERE id = ?;";
    
    rc = sqlite3_prepare_v2(g_auth_ctx->db, update_sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, now);
        sqlite3_bind_int64(stmt, 2, now);
        sqlite3_bind_text(stmt, 3, client_ip, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 4, user.user_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    
    pthread_mutex_unlock(&g_auth_ctx->db_mutex);
    
    // 记录成功登录
    log_authentication_attempt(user.user_id, client_ip, true, "Login successful");
    
    // 生成令牌
    if (tokens) {
        auth_error_code_t token_result = auth_generate_tokens(user.user_id, client_ip, tokens);
        if (token_result != AUTH_SUCCESS) {
            return token_result;
        }
        
        // 填充用户信息
        tokens->user_info.user_id = user.user_id;
        strncpy(tokens->user_info.username, user.username, MAX_USERNAME_LEN - 1);
        strncpy(tokens->user_info.nickname, user.nickname, MAX_NICKNAME_LEN - 1);
        strncpy(tokens->user_info.email, user.email, MAX_EMAIL_LEN - 1);
        tokens->user_info.is_verified = user.is_verified;
        tokens->user_info.is_premium = 0; // 可根据需要修改
        tokens->user_info.created_at = user.created_at;
        tokens->user_info.last_seen = now;
        tokens->user_info.status = USER_STATUS_ONLINE;
        
        // 创建会话
        auth_session_t session;
        auth_create_session(user.user_id, client_ip, user_agent ? user_agent : "Unknown", &session);
        
        // 触发回调
        if (g_auth_ctx->event_callback) {
            g_auth_ctx->event_callback(user.user_id, "USER_LOGIN", client_ip, strlen(client_ip));
        }
    }
    
    return AUTH_SUCCESS;
}

// ==================== 令牌生成和验证 ====================

auth_error_code_t auth_generate_tokens(uint64_t user_id, const char* client_ip,
                                      auth_token_pair_t* tokens) {
    if (!tokens || !client_ip) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    time_t now = time(NULL);
    
    // 生成访问令牌
    auth_error_code_t access_result = generate_access_token(user_id, client_ip, 
                                                           tokens->access_token);
    if (access_result != AUTH_SUCCESS) {
        return access_result;
    }
    
    // 生成刷新令牌
    auth_error_code_t refresh_result = generate_refresh_token(user_id, client_ip,
                                                            tokens->refresh_token);
    if (refresh_result != AUTH_SUCCESS) {
        return refresh_result;
    }
    
    tokens->expires_in = auth_get_token_expiration_time();
    tokens->refresh_expires_in = auth_get_refresh_token_expiration_time();
    
    // 保存会话到数据库
    pthread_mutex_lock(&g_auth_ctx->db_mutex);
    
    const char* insert_session_sql = 
        "INSERT INTO sessions (user_id, access_token, refresh_token, client_ip, "
        "created_at, expires_at, refresh_expires_at, is_valid) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, 1);";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(g_auth_ctx->db, insert_session_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        return AUTH_INVALID_CREDENTIALS;
    }
    
    sqlite3_bind_int64(stmt, 1, user_id);
    sqlite3_bind_text(stmt, 2, tokens->access_token, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, tokens->refresh_token, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, client_ip, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, now);
    sqlite3_bind_int64(stmt, 6, now + tokens->expires_in);
    sqlite3_bind_int64(stmt, 7, now + tokens->refresh_expires_in);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    pthread_mutex_unlock(&g_auth_ctx->db_mutex);
    
    if (rc != SQLITE_DONE) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    return AUTH_SUCCESS;
}

static auth_error_code_t generate_access_token(uint64_t user_id, const char* client_ip,
                                              char* token) {
    if (!token || !client_ip) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    time_t now = time(NULL);
    time_t expires_at = now + AUTH_SESSION_TIMEOUT;
    
    // 创建令牌载荷
    char payload[512];
    snprintf(payload, sizeof(payload),
            "{\"user_id\":%llu,\"ip\":\"%s\",\"type\":\"access\","
            "\"iat\":%ld,\"exp\":%ld,\"jti\":\"",
            (unsigned long long)user_id, client_ip, now, expires_at);
    
    // 生成随机JWT ID
    char jti[33];
    auth_generate_random_token(jti, sizeof(jti));
    strncat(payload, jti, sizeof(payload) - strlen(payload) - 1);
    strncat(payload, "\"}", sizeof(payload) - strlen(payload) - 1);
    
    // 计算HMAC-SHA256签名
    unsigned char* hmac = HMAC(EVP_sha256(), g_auth_ctx->key_derived, g_auth_ctx->key_length,
                              (const unsigned char*)payload, strlen(payload), NULL, NULL);
    if (!hmac) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    // Base64编码载荷和签名
    char encoded_payload[1024];
    char encoded_signature[1024];
    
    // 这里简化处理，实际应该使用Base64编码
    // 为了简化，我们生成一个简化的令牌格式
    snprintf(token, AUTH_TOKEN_LENGTH, "AT_%llu_%ld_", 
            (unsigned long long)user_id, expires_at);
    
    // 添加签名部分（简化）
    char signature_hex[65];
    for (int i = 0; i < 32; i++) {
        sprintf(&signature_hex[i*2], "%02x", hmac[i]);
    }
    signature_hex[64] = '\0';
    
    strncat(token, signature_hex, AUTH_TOKEN_LENGTH - strlen(token) - 1);
    
    return AUTH_SUCCESS;
}

static auth_error_code_t generate_refresh_token(uint64_t user_id, const char* client_ip,
                                               char* token) {
    if (!token || !client_ip) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    time_t now = time(NULL);
    time_t expires_at = now + AUTH_REFRESH_TOKEN_LIFETIME;
    
    // 生成随机令牌
    char random_part[33];
    auth_generate_random_token(random_part, sizeof(random_part));
    
    // 计算HMAC
    char data[256];
    snprintf(data, sizeof(data), "%llu:%s:%ld:refresh", 
            (unsigned long long)user_id, client_ip, expires_at);
    
    unsigned char* hmac = HMAC(EVP_sha256(), g_auth_ctx->key_derived, g_auth_ctx->key_length,
                              (const unsigned char*)data, strlen(data), NULL, NULL);
    if (!hmac) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    // 生成令牌
    snprintf(token, AUTH_TOKEN_LENGTH, "RT_%llu_%ld_", 
            (unsigned long long)user_id, expires_at);
    
    char hmac_hex[65];
    for (int i = 0; i < 32; i++) {
        sprintf(&hmac_hex[i*2], "%02x", hmac[i]);
    }
    hmac_hex[64] = '\0';
    
    strncat(token, hmac_hex, AUTH_TOKEN_LENGTH - strlen(token) - 1);
    
    return AUTH_SUCCESS;
}

auth_error_code_t auth_validate_token(const char* access_token, uint64_t* user_id) {
    if (!access_token || !user_id) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    // 检查令牌格式
    if (strlen(access_token) < 10 || strncmp(access_token, "AT_", 3) != 0) {
        return AUTH_INVALID_TOKEN;
    }
    
    // 验证签名
    if (!validate_token_signature(access_token)) {
        return AUTH_INVALID_TOKEN;
    }
    
    // 从令牌中提取用户ID
    char* underscore = strchr(access_token + 3, '_');
    if (!underscore) {
        return AUTH_INVALID_TOKEN;
    }
    
    char user_id_str[32];
    size_t len = underscore - (access_token + 3);
    if (len >= sizeof(user_id_str)) {
        return AUTH_INVALID_TOKEN;
    }
    
    strncpy(user_id_str, access_token + 3, len);
    user_id_str[len] = '\0';
    
    char* endptr;
    *user_id = strtoull(user_id_str, &endptr, 10);
    if (*endptr != '\0') {
        return AUTH_INVALID_TOKEN;
    }
    
    // 检查令牌是否在数据库中且有效
    pthread_mutex_lock(&g_auth_ctx->db_mutex);
    
    const char* check_sql = 
        "SELECT user_id, expires_at FROM sessions "
        "WHERE access_token = ? AND is_valid = 1 AND expires_at > ?;";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(g_auth_ctx->db, check_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        return AUTH_INVALID_CREDENTIALS;
    }
    
    time_t now = time(NULL);
    sqlite3_bind_text(stmt, 1, access_token, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, now);
    
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        return AUTH_TOKEN_EXPIRED;
    }
    
    *user_id = sqlite3_column_int64(stmt, 0);
    time_t expires_at = sqlite3_column_int64(stmt, 1);
    
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_auth_ctx->db_mutex);
    
    if (expires_at <= now) {
        return AUTH_TOKEN_EXPIRED;
    }
    
    return AUTH_SUCCESS;
}

static bool validate_token_signature(const char* token) {
    if (!token || strlen(token) < 64) {
        return false;
    }
    
    // 查找最后一个下划线（分隔符）
    const char* last_underscore = strrchr(token, '_');
    if (!last_underscore) {
        return false;
    }
    
    // 提取签名部分
    const char* signature_hex = last_underscore + 1;
    if (strlen(signature_hex) != 64) {
        return false;
    }
    
    // 重新计算签名进行比较
    char* payload_end = (char*)last_underscore;
    size_t payload_len = payload_end - token;
    
    char payload[512];
    if (payload_len >= sizeof(payload)) {
        return false;
    }
    
    strncpy(payload, token, payload_len);
    payload[payload_len] = '\0';
    
    // 计算HMAC
    unsigned char computed_hmac[32];
    unsigned int hmac_len;
    
    HMAC(EVP_sha256(), g_auth_ctx->key_derived, g_auth_ctx->key_length,
        (const unsigned char*)payload, payload_len, computed_hmac, &hmac_len);
    
    // 将十六进制签名转换为二进制
    unsigned char stored_hmac[32];
    for (int i = 0; i < 32; i++) {
        char hex_byte[3] = {signature_hex[i*2], signature_hex[i*2+1], '\0'};
        stored_hmac[i] = (unsigned char)strtoul(hex_byte, NULL, 16);
    }
    
    // 比较签名
    return CRYPTO_memcmp(computed_hmac, stored_hmac, 32) == 0;
}

// ==================== 密码管理 ====================

auth_error_code_t auth_generate_password_hash(const char* password,
                                             uint8_t* salt, uint8_t* hash,
                                             uint32_t* hash_length) {
    if (!password || !salt || !hash || !hash_length) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    // 检查密码强度
    auth_error_code_t strength_result = check_password_strength(password);
    if (strength_result != AUTH_SUCCESS) {
        return strength_result;
    }
    
    // 使用PBKDF2-HMAC-SHA256
    *hash_length = EVP_MAX_MD_SIZE;
    
    if (!PKCS5_PBKDF2_HMAC(password, strlen(password),
                          salt, AUTH_SALT_LENGTH,
                          100000,  // 迭代次数
                          EVP_sha256(),
                          *hash_length, hash)) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    return AUTH_SUCCESS;
}

bool auth_verify_password(const char* password, const uint8_t* salt,
                         const uint8_t* stored_hash, uint32_t hash_length) {
    if (!password || !salt || !stored_hash || hash_length == 0) {
        return false;
    }
    
    uint8_t computed_hash[EVP_MAX_MD_SIZE];
    uint32_t computed_hash_length = EVP_MAX_MD_SIZE;
    
    if (!PKCS5_PBKDF2_HMAC(password, strlen(password),
                          salt, AUTH_SALT_LENGTH,
                          100000,
                          EVP_sha256(),
                          computed_hash_length, computed_hash)) {
        return false;
    }
    
    // 比较哈希值
    if (computed_hash_length != hash_length) {
        return false;
    }
    
    return CRYPTO_memcmp(computed_hash, stored_hash, hash_length) == 0;
}

static auth_error_code_t check_password_strength(const char* password) {
    if (!password) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    size_t len = strlen(password);
    
    if (len < AUTH_PASSWORD_MIN_LENGTH) {
        return AUTH_PASSWORD_TOO_WEAK;
    }
    
    if (len > AUTH_PASSWORD_MAX_LENGTH) {
        return AUTH_PASSWORD_TOO_WEAK;
    }
    
    // 检查密码复杂性
    int has_upper = 0, has_lower = 0, has_digit = 0, has_special = 0;
    
    for (size_t i = 0; i < len; i++) {
        if (isupper(password[i])) has_upper = 1;
        else if (islower(password[i])) has_lower = 1;
        else if (isdigit(password[i])) has_digit = 1;
        else has_special = 1;
    }
    
    // 至少需要三种字符类型
    int char_types = has_upper + has_lower + has_digit + has_special;
    if (char_types < 3) {
        return AUTH_PASSWORD_TOO_WEAK;
    }
    
    // 检查常见弱密码
    const char* weak_passwords[] = {
        "password", "123456", "qwerty", "admin", "welcome",
        "password123", "admin123", "12345678", "123456789",
        NULL
    };
    
    for (int i = 0; weak_passwords[i] != NULL; i++) {
        if (strcasecmp(password, weak_passwords[i]) == 0) {
            return AUTH_PASSWORD_TOO_WEAK;
        }
    }
    
    return AUTH_SUCCESS;
}

// ==================== 会话管理 ====================

auth_error_code_t auth_create_session(uint64_t user_id, const char* client_ip,
                                     const char* user_agent, auth_session_t* session) {
    if (!session || !client_ip) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    time_t now = time(NULL);
    
    session->user_id = user_id;
    session->created_at = now;
    session->expires_at = now + AUTH_SESSION_TIMEOUT;
    session->refresh_expires_at = now + AUTH_REFRESH_TOKEN_LIFETIME;
    session->is_valid = 1;
    
    strncpy(session->client_ip, client_ip, sizeof(session->client_ip) - 1);
    if (user_agent) {
        strncpy(session->user_agent, user_agent, sizeof(session->user_agent) - 1);
    } else {
        session->user_agent[0] = '\0';
    }
    
    // 生成令牌
    auth_token_pair_t tokens;
    auth_error_code_t token_result = auth_generate_tokens(user_id, client_ip, &tokens);
    if (token_result != AUTH_SUCCESS) {
        return token_result;
    }
    
    strncpy(session->access_token, tokens.access_token, AUTH_TOKEN_LENGTH - 1);
    strncpy(session->refresh_token, tokens.refresh_token, AUTH_TOKEN_LENGTH - 1);
    
    return AUTH_SUCCESS;
}

auth_error_code_t auth_logout_user(uint64_t user_id, const char* access_token) {
    if (!access_token) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    pthread_mutex_lock(&g_auth_ctx->db_mutex);
    
    // 使令牌失效
    const char* update_sql = 
        "UPDATE sessions SET is_valid = 0 WHERE access_token = ? AND user_id = ?;";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(g_auth_ctx->db, update_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        return AUTH_INVALID_CREDENTIALS;
    }
    
    sqlite3_bind_text(stmt, 1, access_token, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, user_id);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    pthread_mutex_unlock(&g_auth_ctx->db_mutex);
    
    if (rc != SQLITE_DONE) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    // 触发回调
    if (g_auth_ctx->event_callback) {
        g_auth_ctx->event_callback(user_id, "USER_LOGOUT", access_token, strlen(access_token));
    }
    
    return AUTH_SUCCESS;
}

// ==================== 工具函数 ====================

void auth_generate_random_salt(uint8_t* salt) {
    if (!salt) return;
    
    // 使用系统随机源
    FILE* urandom = fopen("/dev/urandom", "rb");
    if (urandom) {
        fread(salt, 1, AUTH_SALT_LENGTH, urandom);
        fclose(urandom);
    } else {
        // 回退到伪随机
        srand(time(NULL));
        for (int i = 0; i < AUTH_SALT_LENGTH; i++) {
            salt[i] = rand() % 256;
        }
    }
}

void auth_generate_random_token(char* buffer, size_t length) {
    if (!buffer || length < 1) return;
    
    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    size_t charset_len = strlen(charset);
    
    FILE* urandom = fopen("/dev/urandom", "rb");
    if (urandom) {
        for (size_t i = 0; i < length - 1; i++) {
            unsigned char byte;
            fread(&byte, 1, 1, urandom);
            buffer[i] = charset[byte % charset_len];
        }
        fclose(urandom);
    } else {
        srand(time(NULL));
        for (size_t i = 0; i < length - 1; i++) {
            buffer[i] = charset[rand() % charset_len];
        }
    }
    
    buffer[length - 1] = '\0';
}

time_t auth_get_token_expiration_time(void) {
    return AUTH_SESSION_TIMEOUT;
}

time_t auth_get_refresh_token_expiration_time(void) {
    return AUTH_REFRESH_TOKEN_LIFETIME;
}

bool auth_validate_username(const char* username) {
    if (!username) return false;
    
    size_t len = strlen(username);
    if (len < 3 || len >= MAX_USERNAME_LEN) {
        return false;
    }
    
    // 只允许字母、数字、下划线和连字符
    for (size_t i = 0; i < len; i++) {
        if (!isalnum(username[i]) && username[i] != '_' && username[i] != '-') {
            return false;
        }
    }
    
    return true;
}

bool auth_validate_email(const char* email) {
    if (!email) return false;
    
    size_t len = strlen(email);
    if (len < 3 || len >= MAX_EMAIL_LEN) {
        return false;
    }
    
    // 简单的电子邮件验证
    int at_count = 0;
    int dot_after_at = 0;
    
    for (size_t i = 0; i < len; i++) {
        if (email[i] == '@') {
            at_count++;
            if (i == 0 || i == len - 1) {
                return false;
            }
        } else if (email[i] == '.' && at_count > 0) {
            dot_after_at = 1;
        }
    }
    
    return at_count == 1 && dot_after_at > 0;
}

const char* auth_error_to_string(auth_error_code_t error) {
    switch (error) {
        case AUTH_SUCCESS: return "Success";
        case AUTH_INVALID_CREDENTIALS: return "Invalid credentials";
        case AUTH_ACCOUNT_LOCKED: return "Account locked";
        case AUTH_ACCOUNT_DISABLED: return "Account disabled";
        case AUTH_SESSION_EXPIRED: return "Session expired";
        case AUTH_INVALID_TOKEN: return "Invalid token";
        case AUTH_TOKEN_EXPIRED: return "Token expired";
        case AUTH_PERMISSION_DENIED: return "Permission denied";
        case AUTH_RATE_LIMIT_EXCEEDED: return "Rate limit exceeded";
        case AUTH_IP_BLOCKED: return "IP blocked";
        case AUTH_2FA_REQUIRED: return "Two-factor authentication required";
        case AUTH_PASSWORD_TOO_WEAK: return "Password too weak";
        case AUTH_PASSWORD_REUSE_NOT_ALLOWED: return "Password reuse not allowed";
        case AUTH_INVALID_2FA_CODE: return "Invalid 2FA code";
        default: return "Unknown error";
    }
}

// ==================== 安全日志和监控 ====================

static auth_error_code_t log_authentication_attempt(uint64_t user_id,
                                                   const char* client_ip,
                                                   bool success,
                                                   const char* reason) {
    if (!g_auth_ctx) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    pthread_mutex_lock(&g_auth_ctx->db_mutex);
    
    const char* insert_sql = 
        "INSERT INTO security_logs (user_id, event_type, details, client_ip, created_at) "
        "VALUES (?, ?, ?, ?, ?);";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(g_auth_ctx->db, insert_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        return AUTH_INVALID_CREDENTIALS;
    }
    
    const char* event_type = success ? "LOGIN_SUCCESS" : "LOGIN_FAILED";
    char details[512];
    snprintf(details, sizeof(details), "%s: %s", 
            success ? "Successful login" : "Failed login", reason ? reason : "Unknown");
    
    time_t now = time(NULL);
    
    if (user_id == 0) {
        sqlite3_bind_null(stmt, 1);
    } else {
        sqlite3_bind_int64(stmt, 1, user_id);
    }
    
    sqlite3_bind_text(stmt, 2, event_type, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, details, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, client_ip, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, now);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    pthread_mutex_unlock(&g_auth_ctx->db_mutex);
    
    return rc == SQLITE_DONE ? AUTH_SUCCESS : AUTH_INVALID_CREDENTIALS;
}

auth_error_code_t auth_log_security_event(uint64_t user_id, const char* event_type,
                                         const char* details, const char* client_ip) {
    return log_authentication_attempt(user_id, client_ip, true, details);
}

// ==================== 速率限制 ====================

static auth_error_code_t rate_limit_check(const char* client_ip,
                                         const char* username) {
    if (!g_auth_ctx) {
        return AUTH_SUCCESS;
    }
    
    pthread_mutex_lock(&g_auth_ctx->db_mutex);
    
    time_t now = time(NULL);
    time_t one_hour_ago = now - 3600;
    
    // 检查IP地址的失败尝试
    const char* check_ip_sql = 
        "SELECT COUNT(*) FROM security_logs "
        "WHERE client_ip = ? AND event_type = 'LOGIN_FAILED' AND created_at > ?;";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(g_auth_ctx->db, check_ip_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        return AUTH_SUCCESS;
    }
    
    sqlite3_bind_text(stmt, 1, client_ip, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, one_hour_ago);
    
    int ip_fail_count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ip_fail_count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    
    // 如果同一IP在过去一小时内失败超过20次，限制访问
    if (ip_fail_count > 20) {
        pthread_mutex_unlock(&g_auth_ctx->db_mutex);
        return AUTH_RATE_LIMIT_EXCEEDED;
    }
    
    // 检查用户名的失败尝试
    if (username) {
        const char* check_user_sql = 
            "SELECT COUNT(*) FROM security_logs sl "
            "JOIN users u ON sl.user_id = u.id "
            "WHERE (u.username = ? OR u.email = ?) "
            "AND sl.event_type = 'LOGIN_FAILED' AND sl.created_at > ?;";
        
        rc = sqlite3_prepare_v2(g_auth_ctx->db, check_user_sql, -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, username, -1, SQLITE_STATIC);
            sqlite3_bind_int64(stmt, 3, one_hour_ago);
            
            int user_fail_count = 0;
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                user_fail_count = sqlite3_column_int(stmt, 0);
            }
            sqlite3_finalize(stmt);
            
            if (user_fail_count > 10) {
                pthread_mutex_unlock(&g_auth_ctx->db_mutex);
                return AUTH_RATE_LIMIT_EXCEEDED;
            }
        }
    }
    
    pthread_mutex_unlock(&g_auth_ctx->db_mutex);
    return AUTH_SUCCESS;
}

// ==================== 回调函数支持 ====================

auth_error_code_t auth_register_callback(auth_callback_t callback) {
    if (!g_auth_ctx) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    g_auth_ctx->event_callback = callback;
    return AUTH_SUCCESS;
}

auth_error_code_t auth_trigger_event(uint64_t user_id, const char* event_type,
                                    const void* data, size_t data_size) {
    if (!g_auth_ctx || !event_type) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    if (g_auth_ctx->event_callback) {
        g_auth_ctx->event_callback(user_id, event_type, data, data_size);
    }
    
    return AUTH_SUCCESS;
}

// ==================== WebSocket支持 ====================

auth_error_code_t auth_websocket_handshake(const char* access_token,
                                          const char* client_ip,
                                          uint64_t* user_id) {
    return auth_validate_token(access_token, user_id);
}

auth_error_code_t auth_websocket_validate_message(uint64_t user_id,
                                                 const message_header_t* header) {
    if (!header) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    // 验证消息头
    if (!validate_message_header(header)) {
        return AUTH_INVALID_CREDENTIALS;
    }
    
    // 检查用户是否在线（可选）
    // 这里可以添加额外的验证逻辑
    
    return AUTH_SUCCESS;
}

// ==================== 统计函数 ====================

uint64_t auth_get_active_sessions_count(void) {
    if (!g_auth_ctx) {
        return 0;
    }
    
    pthread_mutex_lock(&g_auth_ctx->db_mutex);
    
    const char* sql = "SELECT COUNT(*) FROM sessions WHERE is_valid = 1 AND expires_at > ?;";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(g_auth_ctx->db, sql, -1, &stmt, NULL);
    
    uint64_t count = 0;
    if (rc == SQLITE_OK) {
        time_t now = time(NULL);
        sqlite3_bind_int64(stmt, 1, now);
        
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    
    pthread_mutex_unlock(&g_auth_ctx->db_mutex);
    return count;
}

uint64_t auth_get_total_users_count(void) {
    if (!g_auth_ctx) {
        return 0;
    }
    
    pthread_mutex_lock(&g_auth_ctx->db_mutex);
    
    const char* sql = "SELECT COUNT(*) FROM users;";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(g_auth_ctx->db, sql, -1, &stmt, NULL);
    
    uint64_t count = 0;
    if (rc == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    
    pthread_mutex_unlock(&g_auth_ctx->db_mutex);
    return count;
}

uint64_t auth_get_failed_login_attempts_last_hour(void) {
    if (!g_auth_ctx) {
        return 0;
    }
    
    pthread_mutex_lock(&g_auth_ctx->db_mutex);
    
    time_t now = time(NULL);
    time_t one_hour_ago = now - 3600;
    
    const char* sql = 
        "SELECT COUNT(*) FROM security_logs "
        "WHERE event_type = 'LOGIN_FAILED' AND created_at > ?;";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(g_auth_ctx->db, sql, -1, &stmt, NULL);
    
    uint64_t count = 0;
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, one_hour_ago);
        
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    
    pthread_mutex_unlock(&g_auth_ctx->db_mutex);
    return count;
}