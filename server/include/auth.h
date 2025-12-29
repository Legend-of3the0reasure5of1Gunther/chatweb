#ifndef AUTH_H
#define AUTH_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include "protocol.h"
#include "security.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 认证配置 ====================
#define AUTH_MAX_LOGIN_ATTEMPTS 5
#define AUTH_LOCKOUT_DURATION 300  // 5分钟
#define AUTH_SESSION_TIMEOUT 3600  // 1小时
#define AUTH_REFRESH_TOKEN_LIFETIME 604800  // 7天
#define AUTH_TOKEN_SECRET_MIN_LENGTH 32
#define AUTH_PASSWORD_MIN_LENGTH 8
#define AUTH_PASSWORD_MAX_LENGTH 128
#define AUTH_SALT_LENGTH 32
#define AUTH_TOKEN_LENGTH 64

// ==================== 数据结构定义 ====================
typedef struct {
    uint64_t user_id;
    char username[MAX_USERNAME_LEN];
    char nickname[MAX_NICKNAME_LEN];
    char email[MAX_EMAIL_LEN];
    uint8_t password_hash[EVP_MAX_MD_SIZE];
    uint8_t password_salt[AUTH_SALT_LENGTH];
    uint32_t password_hash_length;
    uint32_t login_attempts;
    time_t last_login_attempt;
    time_t account_locked_until;
    time_t created_at;
    time_t updated_at;
    uint8_t is_active;
    uint8_t is_verified;
    uint8_t is_admin;
    uint64_t last_login;
    char last_login_ip[46];  // IPv6最大长度
} auth_user_t;

typedef struct {
    uint64_t session_id;
    uint64_t user_id;
    char access_token[AUTH_TOKEN_LENGTH];
    char refresh_token[AUTH_TOKEN_LENGTH];
    char client_ip[46];
    char user_agent[256];
    time_t created_at;
    time_t expires_at;
    time_t refresh_expires_at;
    uint8_t is_valid;
} auth_session_t;

typedef struct {
    char access_token[AUTH_TOKEN_LENGTH];
    char refresh_token[AUTH_TOKEN_LENGTH];
    time_t expires_in;
    time_t refresh_expires_in;
    user_info_t user_info;
} auth_token_pair_t;

typedef struct {
    uint64_t user_id;
    char permission_name[64];
    char resource[128];
    char action[32];
    time_t granted_at;
    time_t expires_at;
} auth_permission_t;

typedef struct {
    uint64_t user_id;
    char role_name[64];
    time_t assigned_at;
} auth_role_t;

// ==================== 错误码扩展 ====================
typedef enum {
    AUTH_SUCCESS = 0,
    AUTH_INVALID_CREDENTIALS = 100,
    AUTH_ACCOUNT_LOCKED = 101,
    AUTH_ACCOUNT_DISABLED = 102,
    AUTH_SESSION_EXPIRED = 103,
    AUTH_INVALID_TOKEN = 104,
    AUTH_TOKEN_EXPIRED = 105,
    AUTH_PERMISSION_DENIED = 106,
    AUTH_RATE_LIMIT_EXCEEDED = 107,
    AUTH_IP_BLOCKED = 108,
    AUTH_2FA_REQUIRED = 109,
    AUTH_PASSWORD_TOO_WEAK = 110,
    AUTH_PASSWORD_REUSE_NOT_ALLOWED = 111,
    AUTH_INVALID_2FA_CODE = 112
} auth_error_code_t;

// ==================== 函数声明 ====================

// 初始化/清理函数
auth_error_code_t auth_init(const char* secret_key, const char* db_path);
void auth_cleanup(void);

// 用户管理函数
auth_error_code_t auth_register_user(const char* username, const char* password,
                                     const char* email, const char* nickname,
                                     const char* client_ip, auth_token_pair_t* tokens);
auth_error_code_t auth_login_user(const char* username, const char* password,
                                 const char* client_ip, const char* user_agent,
                                 auth_token_pair_t* tokens);
auth_error_code_t auth_logout_user(uint64_t user_id, const char* access_token);
auth_error_code_t auth_validate_token(const char* access_token, uint64_t* user_id);
auth_error_code_t auth_refresh_token(const char* refresh_token,
                                    const char* client_ip, auth_token_pair_t* new_tokens);

// 密码管理函数
auth_error_code_t auth_change_password(uint64_t user_id, const char* old_password,
                                      const char* new_password);
auth_error_code_t auth_reset_password_request(const char* email);
auth_error_code_t auth_reset_password_confirm(const char* reset_token,
                                             const char* new_password);
auth_error_code_t auth_generate_password_reset_token(const char* email, char* token);

// 会话管理函数
auth_error_code_t auth_create_session(uint64_t user_id, const char* client_ip,
                                     const char* user_agent, auth_session_t* session);
auth_error_code_t auth_validate_session(const char* access_token, auth_session_t* session);
auth_error_code_t auth_revoke_session(uint64_t session_id);
auth_error_code_t auth_revoke_all_user_sessions(uint64_t user_id);
auth_session_t* auth_get_user_sessions(uint64_t user_id, size_t* count);

// 权限管理函数
auth_error_code_t auth_check_permission(uint64_t user_id, const char* resource,
                                       const char* action);
auth_error_code_t auth_grant_permission(uint64_t user_id, const char* resource,
                                       const char* action, time_t expires_at);
auth_error_code_t auth_revoke_permission(uint64_t user_id, const char* resource,
                                        const char* action);
auth_permission_t* auth_get_user_permissions(uint64_t user_id, size_t* count);

// 角色管理函数
auth_error_code_t auth_assign_role(uint64_t user_id, const char* role_name);
auth_error_code_t auth_remove_role(uint64_t user_id, const char* role_name);
auth_error_code_t auth_check_role(uint64_t user_id, const char* role_name);
auth_role_t* auth_get_user_roles(uint64_t user_id, size_t* count);

// 安全函数
auth_error_code_t auth_generate_password_hash(const char* password,
                                             uint8_t* salt, uint8_t* hash,
                                             uint32_t* hash_length);
bool auth_verify_password(const char* password, const uint8_t* salt,
                         const uint8_t* stored_hash, uint32_t hash_length);
auth_error_code_t auth_generate_tokens(uint64_t user_id, const char* client_ip,
                                      auth_token_pair_t* tokens);
auth_error_code_t auth_decode_token(const char* token, uint64_t* user_id,
                                   time_t* expires_at, char* payload);

// 账户安全函数
auth_error_code_t auth_lock_account(uint64_t user_id, time_t duration);
auth_error_code_t auth_unlock_account(uint64_t user_id);
auth_error_code_t auth_check_login_attempts(uint64_t user_id, const char* client_ip);
auth_error_code_t auth_log_security_event(uint64_t user_id, const char* event_type,
                                         const char* details, const char* client_ip);

// 验证函数
bool auth_validate_username(const char* username);
bool auth_validate_password(const char* password);
bool auth_validate_email(const char* email);

// 工具函数
const char* auth_error_to_string(auth_error_code_t error);
void auth_generate_random_salt(uint8_t* salt);
void auth_generate_random_token(char* buffer, size_t length);
time_t auth_get_token_expiration_time(void);
time_t auth_get_refresh_token_expiration_time(void);

// WebSocket认证支持
auth_error_code_t auth_websocket_handshake(const char* access_token,
                                          const char* client_ip,
                                          uint64_t* user_id);
auth_error_code_t auth_websocket_validate_message(uint64_t user_id,
                                                 const message_header_t* header);

// 统计函数
uint64_t auth_get_active_sessions_count(void);
uint64_t auth_get_total_users_count(void);
uint64_t auth_get_failed_login_attempts_last_hour(void);

// 回调函数类型定义
typedef void (*auth_callback_t)(uint64_t user_id, const char* event_type,
                               const void* data, size_t data_size);
auth_error_code_t auth_register_callback(auth_callback_t callback);
auth_error_code_t auth_trigger_event(uint64_t user_id, const char* event_type,
                                    const void* data, size_t data_size);

#ifdef __cplusplus
}
#endif

#endif // AUTH_H