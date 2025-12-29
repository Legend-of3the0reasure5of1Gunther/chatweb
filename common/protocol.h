#ifndef SECURE_CHAT_PROTOCOL_H
#define SECURE_CHAT_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 版本控制 ====================
#define PROTOCOL_VERSION_MAJOR 3
#define PROTOCOL_VERSION_MINOR 0
#define PROTOCOL_VERSION_PATCH 0

// 协议版本号宏
#define PROTOCOL_VERSION ((PROTOCOL_VERSION_MAJOR << 16) | \
                          (PROTOCOL_VERSION_MINOR << 8) | \
                           PROTOCOL_VERSION_PATCH)

// ==================== 常量定义 ====================
#define PROTOCOL_MAGIC 0x53435400  // "SCT\0"
#define MAX_USERNAME_LEN 64
#define MAX_PASSWORD_LEN 128
#define MAX_NICKNAME_LEN 64
#define MAX_EMAIL_LEN 128
#define MAX_MESSAGE_LEN 4096
#define MAX_FILENAME_LEN 256
#define MAX_STATUS_MSG_LEN 128
#define MAX_GROUP_NAME_LEN 128

#define DEFAULT_PORT 8888
#define WEBSOCKET_PORT 8889
#define MAX_CLIENTS 10000
#define BUFFER_SIZE 65536
#define FILE_CHUNK_SIZE 16384
#define MAX_FILE_SIZE (100 * 1024 * 1024)  // 100MB

// ==================== 错误码定义 ====================
typedef enum {
    ERR_SUCCESS = 0,
    
    // 客户端错误 (1xxx)
    ERR_INVALID_REQUEST = 1001,
    ERR_AUTH_FAILED = 1002,
    ERR_INVALID_TOKEN = 1003,
    ERR_PERMISSION_DENIED = 1004,
    ERR_USER_NOT_FOUND = 1005,
    ERR_USER_ALREADY_EXISTS = 1006,
    ERR_INVALID_PARAMETERS = 1007,
    ERR_RATE_LIMIT_EXCEEDED = 1008,
    
    // 服务器错误 (2xxx)
    ERR_SERVER_ERROR = 2001,
    ERR_DATABASE_ERROR = 2002,
    ERR_FILESYSTEM_ERROR = 2003,
    ERR_SERVICE_UNAVAILABLE = 2004,
    ERR_MAINTENANCE_MODE = 2005,
    
    // 网络错误 (3xxx)
    ERR_NETWORK_ERROR = 3001,
    ERR_CONNECTION_TIMEOUT = 3002,
    ERR_PROTOCOL_ERROR = 3003,
    
    // 文件传输错误 (4xxx)
    ERR_FILE_TOO_LARGE = 4001,
    ERR_FILE_TYPE_NOT_ALLOWED = 4002,
    ERR_FILE_UPLOAD_FAILED = 4003,
    ERR_FILE_NOT_FOUND = 4004,
    ERR_FILE_HASH_MISMATCH = 4005,
    
    // 资源错误 (5xxx)
    ERR_RESOURCE_LIMIT_EXCEEDED = 5001,
    ERR_STORAGE_FULL = 5002,
    
    // 安全错误 (6xxx)
    ERR_SECURITY_VIOLATION = 6001,
    ERR_IP_BLOCKED = 6002,
    ERR_ACCOUNT_LOCKED = 6003,
    
    ERR_UNKNOWN = 9999
} error_code_t;

// ==================== 消息类型定义 ====================
typedef enum {
    // 系统消息
    MSG_SYSTEM_HANDSHAKE = 0x00000001,
    MSG_SYSTEM_HEARTBEAT = 0x00000002,
    MSG_SYSTEM_ERROR = 0x00000003,
    MSG_SYSTEM_NOTIFICATION = 0x00000004,
    MSG_SYSTEM_MAINTENANCE = 0x00000005,
    
    // 认证消息
    MSG_AUTH_LOGIN = 0x01000001,
    MSG_AUTH_LOGIN_RESPONSE = 0x01000002,
    MSG_AUTH_REGISTER = 0x01000003,
    MSG_AUTH_REGISTER_RESPONSE = 0x01000004,
    MSG_AUTH_LOGOUT = 0x01000005,
    MSG_AUTH_LOGOUT_RESPONSE = 0x01000006,
    MSG_AUTH_TOKEN_REFRESH = 0x01000007,
    MSG_AUTH_TOKEN_REFRESH_RESPONSE = 0x01000008,
    MSG_AUTH_PASSWORD_RESET = 0x01000009,
    MSG_AUTH_PASSWORD_RESET_RESPONSE = 0x0100000A,
    
    // 用户消息
    MSG_USER_PROFILE_GET = 0x02000001,
    MSG_USER_PROFILE_GET_RESPONSE = 0x02000002,
    MSG_USER_PROFILE_UPDATE = 0x02000003,
    MSG_USER_PROFILE_UPDATE_RESPONSE = 0x02000004,
    MSG_USER_STATUS_UPDATE = 0x02000005,
    MSG_USER_STATUS_UPDATE_RESPONSE = 0x02000006,
    MSG_USER_LIST_GET = 0x02000007,
    MSG_USER_LIST_GET_RESPONSE = 0x02000008,
    MSG_USER_SEARCH = 0x02000009,
    MSG_USER_SEARCH_RESPONSE = 0x0200000A,
    
    // 聊天消息
    MSG_CHAT_TEXT_SEND = 0x03000001,
    MSG_CHAT_TEXT_RECEIVE = 0x03000002,
    MSG_CHAT_TEXT_ACK = 0x03000003,
    MSG_CHAT_FILE_SEND = 0x03000004,
    MSG_CHAT_FILE_RECEIVE = 0x03000005,
    MSG_CHAT_FILE_ACK = 0x03000006,
    MSG_CHAT_IMAGE_SEND = 0x03000007,
    MSG_CHAT_IMAGE_RECEIVE = 0x03000008,
    MSG_CHAT_VOICE_SEND = 0x03000009,
    MSG_CHAT_VOICE_RECEIVE = 0x0300000A,
    MSG_CHAT_HISTORY_GET = 0x0300000B,
    MSG_CHAT_HISTORY_GET_RESPONSE = 0x0300000C,
    MSG_CHAT_TYPING_NOTIFY = 0x0300000D,
    MSG_CHAT_READ_RECEIPT = 0x0300000E,
    
    // 群组消息
    MSG_GROUP_CREATE = 0x04000001,
    MSG_GROUP_CREATE_RESPONSE = 0x04000002,
    MSG_GROUP_JOIN = 0x04000003,
    MSG_GROUP_JOIN_RESPONSE = 0x04000004,
    MSG_GROUP_LEAVE = 0x04000005,
    MSG_GROUP_LEAVE_RESPONSE = 0x04000006,
    MSG_GROUP_MESSAGE_SEND = 0x04000007,
    MSG_GROUP_MESSAGE_RECEIVE = 0x04000008,
    MSG_GROUP_INFO_GET = 0x04000009,
    MSG_GROUP_INFO_GET_RESPONSE = 0x0400000A,
    MSG_GROUP_MEMBERS_GET = 0x0400000B,
    MSG_GROUP_MEMBERS_GET_RESPONSE = 0x0400000C,
    
    // 文件传输
    MSG_FILE_UPLOAD_REQUEST = 0x05000001,
    MSG_FILE_UPLOAD_REQUEST_RESPONSE = 0x05000002,
    MSG_FILE_UPLOAD_CHUNK = 0x05000003,
    MSG_FILE_UPLOAD_CHUNK_RESPONSE = 0x05000004,
    MSG_FILE_UPLOAD_COMPLETE = 0x05000005,
    MSG_FILE_UPLOAD_COMPLETE_RESPONSE = 0x05000006,
    MSG_FILE_DOWNLOAD_REQUEST = 0x05000007,
    MSG_FILE_DOWNLOAD_REQUEST_RESPONSE = 0x05000008,
    MSG_FILE_DOWNLOAD_CHUNK = 0x05000009,
    MSG_FILE_DOWNLOAD_CHUNK_RESPONSE = 0x0500000A,
    MSG_FILE_DOWNLOAD_COMPLETE = 0x0500000B,
    MSG_FILE_DOWNLOAD_COMPLETE_RESPONSE = 0x0500000C,
    MSG_FILE_PROGRESS_UPDATE = 0x0500000D,
    
    // 加密消息
    MSG_CRYPTO_KEY_EXCHANGE = 0x06000001,
    MSG_CRYPTO_KEY_EXCHANGE_RESPONSE = 0x06000002,
    MSG_CRYPTO_MESSAGE_ENCRYPTED = 0x06000003,
    MSG_CRYPTO_MESSAGE_DECRYPTED = 0x06000004
} message_type_t;

// ==================== 数据结构定义 ====================

#pragma pack(push, 1)

// 消息头结构 (32字节)
typedef struct {
    uint32_t magic;                 // 魔数 0x53435400
    uint32_t version;               // 协议版本
    uint32_t type;                  // 消息类型
    uint32_t flags;                 // 标志位
    uint64_t timestamp;             // 时间戳 (毫秒)
    uint64_t message_id;            // 消息ID
    uint64_t correlation_id;        // 关联ID
    uint32_t body_length;           // 消息体长度
    uint32_t checksum;              // CRC32校验和
    uint8_t reserved[16];           // 保留字段
} message_header_t;

// 标志位定义
#define FLAG_ENCRYPTED       (1 << 0)   // 消息已加密
#define FLAG_COMPRESSED      (1 << 1)   // 消息已压缩
#define FLAG_PRIORITY_HIGH   (1 << 2)   // 高优先级
#define FLAG_PRIORITY_URGENT (1 << 3)   // 紧急优先级
#define FLAG_RESPONSE        (1 << 4)   // 响应消息
#define FLAG_MULTIPART       (1 << 5)   // 多部分消息
#define FLAG_SIGNED          (1 << 6)   // 消息已签名

// 用户状态
typedef enum {
    USER_STATUS_OFFLINE = 0,
    USER_STATUS_ONLINE = 1,
    USER_STATUS_AWAY = 2,
    USER_STATUS_BUSY = 3,
    USER_STATUS_INVISIBLE = 4,
    USER_STATUS_DND = 5          // 请勿打扰
} user_status_t;

// 消息状态
typedef enum {
    MESSAGE_STATUS_SENDING = 0,
    MESSAGE_STATUS_SENT = 1,
    MESSAGE_STATUS_DELIVERED = 2,
    MESSAGE_STATUS_READ = 3,
    MESSAGE_STATUS_FAILED = 4
} message_status_t;

// 文件类型
typedef enum {
    FILE_TYPE_UNKNOWN = 0,
    FILE_TYPE_IMAGE = 1,
    FILE_TYPE_AUDIO = 2,
    FILE_TYPE_VIDEO = 3,
    FILE_TYPE_DOCUMENT = 4,
    FILE_TYPE_ARCHIVE = 5,
    FILE_TYPE_EXECUTABLE = 6
} file_type_t;

// 用户信息结构
typedef struct {
    uint64_t user_id;
    char username[MAX_USERNAME_LEN];
    char nickname[MAX_NICKNAME_LEN];
    char email[MAX_EMAIL_LEN];
    uint32_t avatar_id;
    user_status_t status;
    char status_message[MAX_STATUS_MSG_LEN];
    uint64_t last_seen;
    uint32_t friend_count;
    uint32_t group_count;
    uint8_t is_verified;
    uint8_t is_premium;
    uint64_t created_at;
} user_info_t;

// 消息内容结构
typedef struct {
    uint64_t message_id;
    uint64_t sender_id;
    uint64_t receiver_id;
    uint64_t timestamp;
    uint32_t message_type;          // 0=文本, 1=图片, 2=语音, 3=视频, 4=文件
    uint64_t reply_to_id;           // 回复的消息ID
    uint8_t encrypted;              // 是否端到端加密
    char content_hash[64];          // 内容哈希
    uint32_t content_length;
    char content[];                 // 灵活数组成员
} message_content_t;

// 文件元数据
typedef struct {
    uint64_t file_id;
    uint64_t uploader_id;
    char filename[MAX_FILENAME_LEN];
    char original_name[MAX_FILENAME_LEN];
    char mime_type[128];
    uint64_t file_size;
    file_type_t file_type;
    char file_hash[65];             // SHA256 + null terminator
    uint8_t is_encrypted;
    uint8_t is_compressed;
    uint64_t uploaded_at;
    uint64_t expires_at;            // 0表示永不过期
} file_metadata_t;

// 文件传输请求
typedef struct {
    uint64_t transfer_id;
    uint64_t sender_id;
    uint64_t receiver_id;
    file_metadata_t metadata;
    uint32_t chunk_size;
    uint8_t transfer_mode;          // 0=直接, 1=中继, 2=P2P
    uint8_t encryption_enabled;
    char encryption_key[128];       // 加密密钥 (加密传输)
} file_transfer_request_t;

// 文件传输块
typedef struct {
    uint64_t transfer_id;
    uint32_t chunk_index;
    uint32_t chunk_size;
    uint32_t total_chunks;
    uint32_t data_length;
    uint8_t data[];                 // 灵活数组成员
} file_transfer_chunk_t;

// 响应结构
typedef struct {
    uint32_t status_code;
    error_code_t error_code;
    uint64_t request_id;
    uint32_t data_length;
    char data[];                    // 灵活数组成员
} response_t;

// 错误信息结构
typedef struct {
    error_code_t error_code;
    uint64_t request_id;
    char error_message[256];
    char debug_info[512];
    uint64_t timestamp;
} error_info_t;

#pragma pack(pop)

// ==================== 函数声明 ====================

// 消息验证
bool validate_message_header(const message_header_t *header);
bool verify_message_checksum(const message_header_t *header, const void *data);
uint32_t calculate_message_checksum(const void *data, size_t length);

// 序列化/反序列化
size_t serialize_message_header(const message_header_t *header, uint8_t *buffer);
bool deserialize_message_header(const uint8_t *data, message_header_t *header);

// 工具函数
const char* error_code_to_string(error_code_t code);
const char* message_type_to_string(uint32_t type);
const char* user_status_to_string(user_status_t status);
const char* file_type_to_string(file_type_t type);

// 时间函数
uint64_t get_current_timestamp_ms();
uint64_t get_timestamp_ns();
struct tm* timestamp_to_tm(uint64_t timestamp_ms);

// 内存安全函数
void* safe_malloc(size_t size, const char *purpose);
void* safe_calloc(size_t count, size_t size, const char *purpose);
void safe_free(void **ptr);

#ifdef __cplusplus
}
#endif

#endif // SECURE_CHAT_PROTOCOL_H