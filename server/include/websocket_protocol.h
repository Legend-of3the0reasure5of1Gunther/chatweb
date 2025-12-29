#ifndef SECURE_CHAT_WEBSOCKET_PROTOCOL_H
#define SECURE_CHAT_WEBSOCKET_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== WebSocket常量定义 ====================
#define WEBSOCKET_MAGIC_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WEBSOCKET_DEFAULT_PORT 8889
#define WEBSOCKET_MAX_FRAME_SIZE (64 * 1024)      // 64KB
#define WEBSOCKET_MAX_MESSAGE_SIZE (10 * 1024 * 1024)  // 10MB
#define WEBSOCKET_PING_INTERVAL 30                // 30秒
#define WEBSOCKET_TIMEOUT 300                     // 5分钟超时

// ==================== WebSocket操作码 ====================
typedef enum {
    WS_OPCODE_CONTINUATION = 0x0,
    WS_OPCODE_TEXT         = 0x1,
    WS_OPCODE_BINARY       = 0x2,
    WS_OPCODE_CLOSE        = 0x8,
    WS_OPCODE_PING         = 0x9,
    WS_OPCODE_PONG         = 0xA
} websocket_opcode_t;

// ==================== WebSocket状态码 ====================
typedef enum {
    WS_STATUS_NORMAL_CLOSURE      = 1000,
    WS_STATUS_GOING_AWAY          = 1001,
    WS_STATUS_PROTOCOL_ERROR      = 1002,
    WS_STATUS_UNSUPPORTED_DATA    = 1003,
    WS_STATUS_NO_STATUS_RECEIVED  = 1005,
    WS_STATUS_ABNORMAL_CLOSURE    = 1006,
    WS_STATUS_INVALID_PAYLOAD     = 1007,
    WS_STATUS_POLICY_VIOLATION    = 1008,
    WS_STATUS_MESSAGE_TOO_BIG     = 1009,
    WS_STATUS_MISSING_EXTENSION   = 1010,
    WS_STATUS_INTERNAL_ERROR      = 1011,
    WS_STATUS_SERVICE_RESTART     = 1012,
    WS_STATUS_TRY_AGAIN_LATER     = 1013,
    WS_STATUS_TLS_HANDSHAKE_FAIL  = 1015
} websocket_status_code_t;

// ==================== WebSocket帧头 ====================
#pragma pack(push, 1)
typedef struct {
    uint8_t fin : 1;          // 最终帧标志
    uint8_t rsv1 : 1;         // 保留位1
    uint8_t rsv2 : 1;         // 保留位2
    uint8_t rsv3 : 1;         // 保留位3
    uint8_t opcode : 4;       // 操作码
    uint8_t mask : 1;         // 掩码标志
    uint8_t payload_len : 7;  // 负载长度
} websocket_frame_header_t;

typedef struct {
    uint16_t extended_len;    // 扩展长度 (16位)
} websocket_extended_len_16_t;

typedef struct {
    uint64_t extended_len;    // 扩展长度 (64位)
} websocket_extended_len_64_t;

typedef struct {
    uint32_t masking_key;     // 掩码密钥
} websocket_masking_key_t;
#pragma pack(pop)

// ==================== WebSocket连接状态 ====================
typedef enum {
    WS_CONNECTION_CONNECTING = 0,
    WS_CONNECTION_OPEN = 1,
    WS_CONNECTION_CLOSING = 2,
    WS_CONNECTION_CLOSED = 3,
    WS_CONNECTION_ERROR = 4
} websocket_connection_state_t;

// ==================== WebSocket客户端信息 ====================
typedef struct {
    int fd;                                 // 套接字描述符
    uint64_t client_id;                     // 客户端ID
    char client_ip[46];                     // IP地址 (IPv6长度)
    uint16_t client_port;                   // 端口号
    websocket_connection_state_t state;     // 连接状态
    
    // 握手信息
    char sec_websocket_key[256];           // WebSocket密钥
    char sec_websocket_protocol[128];      // 子协议
    char sec_websocket_extensions[256];    // 扩展
    
    // 消息处理
    uint8_t *message_buffer;               // 消息缓冲区
    size_t message_length;                 // 消息长度
    size_t message_capacity;               // 缓冲区容量
    
    // 分片消息
    uint8_t opcode;                        // 当前操作码
    bool message_complete;                 // 消息是否完整
    size_t fragment_size;                  // 分片大小
    
    // 统计信息
    uint64_t messages_received;            // 接收消息数
    uint64_t messages_sent;                // 发送消息数
    uint64_t bytes_received;               // 接收字节数
    uint64_t bytes_sent;                   // 发送字节数
    uint64_t last_activity;                // 最后活动时间
    uint64_t connected_at;                 // 连接时间
    
    // 用户关联
    uint64_t user_id;                      // 关联的用户ID
    bool authenticated;                    // 是否认证
    char session_token[256];               // 会话令牌
    
    // 心跳
    uint64_t last_ping_sent;               // 最后发送ping时间
    uint64_t last_pong_received;           // 最后接收pong时间
    
    // 自定义数据
    void *user_data;                       // 用户自定义数据
    void (*user_data_free)(void *);        // 用户数据释放函数
} websocket_client_t;

// ==================== WebSocket消息 ====================
typedef struct {
    websocket_opcode_t opcode;             // 操作码
    uint8_t *data;                         // 消息数据
    size_t length;                         // 数据长度
    uint64_t client_id;                    // 客户端ID
    uint64_t timestamp;                    // 时间戳
    bool is_final;                         // 是否是最终帧
} websocket_message_t;

// ==================== WebSocket事件回调 ====================
typedef void (*websocket_connect_cb)(websocket_client_t *client);
typedef void (*websocket_message_cb)(websocket_client_t *client, const websocket_message_t *message);
typedef void (*websocket_close_cb)(websocket_client_t *client, uint16_t status_code, const char *reason);
typedef void (*websocket_error_cb)(websocket_client_t *client, const char *error);
typedef void (*websocket_binary_cb)(websocket_client_t *client, const uint8_t *data, size_t length);

// ==================== WebSocket服务器配置 ====================
typedef struct {
    uint16_t port;                         // 监听端口
    uint32_t max_clients;                  // 最大客户端数
    uint32_t max_message_size;             // 最大消息大小
    uint32_t ping_interval;                // Ping间隔 (秒)
    uint32_t timeout;                      // 超时时间 (秒)
    size_t buffer_size;                    // 缓冲区大小
    bool enable_ssl;                       // 启用SSL
    const char *ssl_cert_path;             // SSL证书路径
    const char *ssl_key_path;              // SSL密钥路径
    const char *bind_address;              // 绑定地址
    bool reuse_addr;                       // 地址重用
    int backlog;                           // 监听队列大小
} websocket_config_t;

// ==================== WebSocket服务器统计 ====================
typedef struct {
    uint64_t connections_total;            // 总连接数
    uint64_t connections_active;           // 活动连接数
    uint64_t connections_closed;           // 已关闭连接数
    uint64_t messages_received;            // 接收消息总数
    uint64_t messages_sent;                // 发送消息总数
    uint64_t bytes_received;               // 接收字节总数
    uint64_t bytes_sent;                   // 发送字节总数
    uint64_t errors_total;                 // 错误总数
    uint64_t ping_count;                   // Ping次数
    uint64_t pong_count;                   // Pong次数
    uint64_t start_time;                   // 启动时间
} websocket_stats_t;

// ==================== WebSocket服务器主结构 ====================
typedef struct websocket_server {
    // 网络
    int server_fd;                         // 服务器套接字
    struct sockaddr_in server_addr;        // 服务器地址
    
    // 配置
    websocket_config_t config;             // 服务器配置
    
    // 客户端管理
    websocket_client_t **clients;          // 客户端数组
    uint32_t client_count;                 // 当前客户端数
    uint32_t max_clients;                  // 最大客户端数
    uint64_t next_client_id;               // 下一个客户端ID
    
    // 回调函数
    websocket_connect_cb on_connect;       // 连接回调
    websocket_message_cb on_message;       // 消息回调
    websocket_close_cb on_close;           // 关闭回调
    websocket_error_cb on_error;           // 错误回调
    websocket_binary_cb on_binary;         // 二进制消息回调
    
    // 统计
    websocket_stats_t stats;               // 服务器统计
    
    // 线程管理
    pthread_t accept_thread;               // 接受连接线程
    pthread_t worker_thread;               // 工作线程
    pthread_t cleanup_thread;              // 清理线程
    
    // 同步原语
    pthread_mutex_t clients_mutex;         // 客户端互斥锁
    pthread_mutex_t stats_mutex;           // 统计互斥锁
    pthread_cond_t activity_cond;          // 活动条件变量
    
    // 控制标志
    volatile bool running;                 // 运行标志
    volatile bool shutdown_requested;      // 关闭请求
    
    // SSL
    void *ssl_ctx;                         // SSL上下文
    void *ssl_method;                      // SSL方法
    
    // 自定义数据
    void *user_data;                       // 用户自定义数据
} websocket_server_t;

// ==================== 函数声明 ====================

// 服务器管理
websocket_server_t* websocket_server_create(const websocket_config_t *config);
int websocket_server_start(websocket_server_t *server);
int websocket_server_stop(websocket_server_t *server);
void websocket_server_destroy(websocket_server_t *server);

// 配置管理
websocket_config_t websocket_get_default_config(void);
int websocket_server_set_config(websocket_server_t *server, const websocket_config_t *config);

// 回调设置
void websocket_set_connect_callback(websocket_server_t *server, websocket_connect_cb callback);
void websocket_set_message_callback(websocket_server_t *server, websocket_message_cb callback);
void websocket_set_close_callback(websocket_server_t *server, websocket_close_cb callback);
void websocket_set_error_callback(websocket_server_t *server, websocket_error_cb callback);
void websocket_set_binary_callback(websocket_server_t *server, websocket_binary_cb callback);

// 消息发送
int websocket_send_text(websocket_server_t *server, uint64_t client_id, const char *text);
int websocket_send_binary(websocket_server_t *server, uint64_t client_id, const uint8_t *data, size_t length);
int websocket_send_json(websocket_server_t *server, uint64_t client_id, const char *json);
int websocket_broadcast_text(websocket_server_t *server, const char *text);
int websocket_broadcast_binary(websocket_server_t *server, const uint8_t *data, size_t length);

// 客户端管理
int websocket_close_client(websocket_server_t *server, uint64_t client_id, uint16_t status_code, const char *reason);
websocket_client_t* websocket_get_client(websocket_server_t *server, uint64_t client_id);
int websocket_get_client_count(websocket_server_t *server);

// 统计信息
int websocket_get_stats(websocket_server_t *server, websocket_stats_t *stats);
void websocket_reset_stats(websocket_server_t *server);

// 工具函数
int websocket_generate_accept_key(const char *client_key, char *accept_key, size_t accept_key_len);
const char* websocket_status_code_to_string(uint16_t status_code);
const char* websocket_opcode_to_string(websocket_opcode_t opcode);

// 帧处理
int websocket_parse_frame(const uint8_t *data, size_t length, 
                         websocket_opcode_t *opcode, uint8_t **payload, 
                         size_t *payload_len, bool *is_final);
int websocket_create_frame(websocket_opcode_t opcode, const uint8_t *payload, 
                          size_t payload_len, uint8_t **frame, size_t *frame_len,
                          bool mask, uint32_t masking_key);

// 握手处理
int websocket_handshake(websocket_client_t *client, const char *request);

#ifdef __cplusplus
}
#endif

#endif // SECURE_CHAT_WEBSOCKET_PROTOCOL_H