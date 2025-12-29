#ifndef SECURE_CHAT_SECURITY_H
#define SECURE_CHAT_SECURITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

// ==================== 密码学配置 ====================
#define HASH_ITERATIONS 100000
#define SALT_SIZE 32
#define TOKEN_SIZE 64
#define NONCE_SIZE 24
#define KEY_SIZE 32
#define IV_SIZE 16
#define TAG_SIZE 16

// ==================== 错误码 ====================
typedef enum {
    SECURITY_SUCCESS = 0,
    SECURITY_INVALID_INPUT = 1,
    SECURITY_BUFFER_TOO_SMALL = 2,
    SECURITY_RANDOM_FAILURE = 3,
    SECURITY_HASH_FAILURE = 4,
    SECURITY_ENCRYPT_FAILURE = 5,
    SECURITY_DECRYPT_FAILURE = 6,
    SECURITY_SIGN_FAILURE = 7,
    SECURITY_VERIFY_FAILURE = 8,
    SECURITY_KEY_INVALID = 9,
    SECURITY_TOKEN_EXPIRED = 10,
    SECURITY_TOKEN_INVALID = 11
} security_error_t;

// ==================== 密码哈希 ====================
typedef struct {
    uint8_t salt[SALT_SIZE];
    uint8_t hash[64];  // SHA512
    uint32_t iterations;
} password_hash_t;

/**
 * @brief 生成密码哈希
 * @param password 明文密码
 * @param password_len 密码长度
 * @param hash 输出哈希结构
 * @return 错误码
 */
security_error_t generate_password_hash(
    const char *password,
    size_t password_len,
    password_hash_t *hash
);

/**
 * @brief 验证密码
 * @param password 明文密码
 * @param password_len 密码长度
 * @param hash 存储的哈希
 * @return 是否验证成功
 */
bool verify_password(
    const char *password,
    size_t password_len,
    const password_hash_t *hash
);

// ==================== 会话令牌 ====================
typedef struct {
    char token[TOKEN_SIZE];
    uint64_t user_id;
    uint64_t created_at;
    uint64_t expires_at;
    uint32_t version;
    uint8_t signature[64];  // Ed25519签名
} session_token_t;

/**
 * @brief 生成会话令牌
 * @param user_id 用户ID
 * @param token 输出令牌
 * @return 错误码
 */
security_error_t generate_session_token(
    uint64_t user_id,
    session_token_t *token
);

/**
 * @brief 验证会话令牌
 * @param token 令牌
 * @param user_id 输出用户ID
 * @return 是否有效
 */
bool verify_session_token(
    const session_token_t *token,
    uint64_t *user_id
);

// ==================== 加密模块 ====================
typedef struct {
    uint8_t key[KEY_SIZE];
    uint8_t iv[IV_SIZE];
    uint64_t key_id;
    uint64_t expires_at;
} encryption_key_t;

/**
 * @brief 初始化加密模块
 * @return 是否成功
 */
bool security_init();

/**
 * @brief 清理加密模块
 */
void security_cleanup();

/**
 * @brief 生成加密密钥
 * @param key 输出密钥
 * @return 错误码
 */
security_error_t generate_encryption_key(encryption_key_t *key);

/**
 * @brief 加密数据 (AES-256-GCM)
 * @param plaintext 明文
 * @param plaintext_len 明文长度
 * @param key 加密密钥
 * @param ciphertext 输出密文缓冲区
 * @param ciphertext_len 输入为缓冲区大小，输出为密文长度
 * @param tag 输出认证标签
 * @return 错误码
 */
security_error_t encrypt_data(
    const uint8_t *plaintext,
    size_t plaintext_len,
    const encryption_key_t *key,
    uint8_t *ciphertext,
    size_t *ciphertext_len,
    uint8_t tag[TAG_SIZE]
);

/**
 * @brief 解密数据 (AES-256-GCM)
 * @param ciphertext 密文
 * @param ciphertext_len 密文长度
 * @param key 加密密钥
 * @param tag 认证标签
 * @param plaintext 输出明文缓冲区
 * @param plaintext_len 输入为缓冲区大小，输出为明文长度
 * @return 错误码
 */
security_error_t decrypt_data(
    const uint8_t *ciphertext,
    size_t ciphertext_len,
    const encryption_key_t *key,
    const uint8_t tag[TAG_SIZE],
    uint8_t *plaintext,
    size_t *plaintext_len
);

// ==================== 输入验证 ====================
typedef enum {
    VALIDATION_SUCCESS = 0,
    VALIDATION_EMPTY = 1,
    VALIDATION_TOO_SHORT = 2,
    VALIDATION_TOO_LONG = 3,
    VALIDATION_INVALID_CHARS = 4,
    VALIDATION_INVALID_FORMAT = 5,
    VALIDATION_SQL_INJECTION = 6,
    VALIDATION_XSS_ATTACK = 7,
    VALIDATION_COMMAND_INJECTION = 8
} validation_result_t;

/**
 * @brief 验证用户名
 * @param username 用户名
 * @param len 用户名长度
 * @return 验证结果
 */
validation_result_t validate_username(const char *username, size_t len);

/**
 * @brief 验证密码强度
 * @param password 密码
 * @param len 密码长度
 * @return 验证结果
 */
validation_result_t validate_password(const char *password, size_t len);

/**
 * @brief 验证邮箱地址
 * @param email 邮箱
 * @param len 邮箱长度
 * @return 验证结果
 */
validation_result_t validate_email(const char *email, size_t len);

/**
 * @brief 验证文件名安全性
 * @param filename 文件名
 * @param len 文件名长度
 * @return 验证结果
 */
validation_result_t validate_filename(const char *filename, size_t len);

/**
 * @brief 净化HTML内容 (防止XSS)
 * @param input 输入HTML
 * @param output 输出缓冲区
 * @param output_len 缓冲区大小
 * @return 净化后的长度
 */
size_t sanitize_html(const char *input, char *output, size_t output_len);

/**
 * @brief 检查SQL注入
 * @param input 输入字符串
 * @return 是否包含SQL注入
 */
bool check_sql_injection(const char *input);

// ==================== 速率限制 ====================
typedef struct {
    uint64_t last_request;
    uint32_t request_count;
    uint32_t max_requests;
    uint64_t window_ms;
    char identifier[64];
} rate_limiter_t;

/**
 * @brief 初始化速率限制器
 * @param limiter 限制器
 * @param identifier 标识符
 * @param max_requests 最大请求数
 * @param window_ms 时间窗口(毫秒)
 */
void rate_limiter_init(
    rate_limiter_t *limiter,
    const char *identifier,
    uint32_t max_requests,
    uint64_t window_ms
);

/**
 * @brief 检查是否超过速率限制
 * @param limiter 限制器
 * @return 是否允许请求
 */
bool rate_limiter_check(rate_limiter_t *limiter);

// ==================== 审计日志 ====================
typedef struct {
    uint64_t timestamp;
    uint64_t user_id;
    const char *action;
    const char *resource;
    const char *ip_address;
    bool success;
    const char *details;
} audit_log_t;

/**
 * @brief 记录审计日志
 * @param log 日志条目
 */
void log_audit_event(const audit_log_t *log);

#endif // SECURE_CHAT_SECURITY_H