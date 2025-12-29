#ifndef SECURE_CHAT_CLIENT_H
#define SECURE_CHAT_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <openssl/ssl.h>

// 客户端配置
typedef struct {
    char server_host[256];
    int server_port;
    char username[64];
    char password[128];
    uint64_t user_id;
    char session_token[128];
    bool use_ssl;
    bool enable_encryption;
    int connection_timeout;
    int heartbeat_interval;
    int reconnect_attempts;
    int reconnect_delay;
    char log_file[256];
    int log_level;
} client_config_t;

// 客户端状态
typedef enum {
    CLIENT_STATE_DISCONNECTED = 0,
    CLIENT_STATE_CONNECTING,
    CLIENT_STATE_CONNECTED,
    CLIENT_STATE_AUTHENTICATING,
    CLIENT_STATE_AUTHENTICATED,
    CLIENT_STATE_RECONNECTING,
    CLIENT_STATE_DISCONNECTING,
    CLIENT_STATE_ERROR
} client_state_t;

// 消息回调函数类型
typedef void (*message_callback_t)(uint32_t msg_type, const void *data, size_t len, void *user_data);
typedef void (*error_callback_t)(int error_code, const char *error_msg, void *user_data);
typedef void (*state_change_callback_t)(client_state_t old_state, client_state_t new_state, void *user_data);

// 消息处理器
typedef struct {
    uint32_t message_type;
    message_callback_t callback;
    void *user_data;
} message_handler_t;

// 文件传输状态
typedef struct {
    uint64_t transfer_id;
    char filename[256];
    uint64_t file_size;
    uint64_t transferred;
    uint32_t progress;
    uint8_t status;  // 0:等待 1:传输中 2:完成 3:失败 4:取消
    uint64_t start_time;
    uint64_t end_time;
} file_transfer_status_t;

// 聊天会话
typedef struct {
    uint64_t user_id;
    char username[64];
    char nickname[64];
    uint8_t online;
    uint64_t last_message_time;
    uint32_t unread_count;
} chat_session_t;

// 主客户端结构
typedef struct secure_chat_client_t {
    // 配置
    client_config_t config;
    
    // 状态
    client_state_t state;
    client_state_t target_state;
    pthread_mutex_t state_mutex;
    
    // 连接
    int sockfd;
    SSL_CTX *ssl_ctx;
    SSL *ssl;
    struct sockaddr_in server_addr;
    bool connection_active;
    
    // 线程
    pthread_t receive_thread;
    pthread_t heartbeat_thread;
    pthread_t reconnect_thread;
    bool threads_running;
    
    // 同步
    pthread_mutex_t send_mutex;
    pthread_mutex_t recv_mutex;
    pthread_cond_t data_ready;
    
    // 缓冲区
    uint8_t *recv_buffer;
    size_t recv_buffer_size;
    size_t recv_buffer_used;
    
    // 消息处理
    message_handler_t *message_handlers;
    size_t message_handler_count;
    size_t message_handler_capacity;
    
    // 回调函数
    error_callback_t error_callback;
    state_change_callback_t state_change_callback;
    void *callback_user_data;
    
    // 心跳
    uint64_t last_heartbeat_sent;
    uint64_t last_heartbeat_received;
    uint32_t heartbeat_missed;
    
    // 重连
    uint32_t reconnect_count;
    uint64_t last_connection_attempt;
    
    // 用户数据
    uint64_t current_user_id;
    char current_username[64];
    char current_nickname[64];
    uint8_t user_status;
    
    // 聊天会话
    chat_session_t *chat_sessions;
    size_t chat_session_count;
    size_t chat_session_capacity;
    
    // 文件传输
    file_transfer_status_t *file_transfers;
    size_t file_transfer_count;
    size_t file_transfer_capacity;
    
    // 统计
    uint64_t messages_sent;
    uint64_t messages_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t connection_start_time;
    
    // 日志
    void *logger;
    
    // 用户自定义数据
    void *user_data;
} secure_chat_client_t;

// ==================== 客户端API ====================

/**
 * @brief 创建新的客户端实例
 * @param config 客户端配置（可以为NULL使用默认配置）
 * @return 客户端实例指针，失败返回NULL
 */
secure_chat_client_t* client_create(const client_config_t *config);

/**
 * @brief 销毁客户端实例并释放所有资源
 * @param client 客户端实例
 */
void client_destroy(secure_chat_client_t *client);

/**
 * @brief 连接到服务器
 * @param client 客户端实例
 * @return 0表示成功，负数表示错误
 */
int client_connect(secure_chat_client_t *client);

/**
 * @brief 断开与服务器的连接
 * @param client 客户端实例
 * @param graceful 是否优雅断开（发送断开消息）
 * @return 0表示成功，负数表示错误
 */
int client_disconnect(secure_chat_client_t *client, bool graceful);

/**
 * @brief 登录到服务器
 * @param client 客户端实例
 * @param username 用户名
 * @param password 密码（客户端会进行哈希）
 * @return 0表示成功，负数表示错误
 */
int client_login(secure_chat_client_t *client, const char *username, const char *password);

/**
 * @brief 注册新用户
 * @param client 客户端实例
 * @param user_info 用户信息结构（包含用户名、密码、昵称等）
 * @return 0表示成功，负数表示错误
 */
int client_register(secure_chat_client_t *client, const void *user_info);

/**
 * @brief 发送文本消息
 * @param client 客户端实例
 * @param receiver_id 接收者ID（0表示广播）
 * @param message 消息内容
 * @param message_len 消息长度
 * @param message_id 输出消息ID（可以为NULL）
 * @return 0表示成功，负数表示错误
 */
int client_send_text_message(secure_chat_client_t *client, uint64_t receiver_id, 
                             const char *message, size_t message_len, uint64_t *message_id);

/**
 * @brief 发送文件
 * @param client 客户端实例
 * @param receiver_id 接收者ID
 * @param filepath 文件路径
 * @param transfer_id 输出传输ID（可以为NULL）
 * @return 0表示成功，负数表示错误
 */
int client_send_file(secure_chat_client_t *client, uint64_t receiver_id, 
                     const char *filepath, uint64_t *transfer_id);

/**
 * @brief 取消文件传输
 * @param client 客户端实例
 * @param transfer_id 传输ID
 * @return 0表示成功，负数表示错误
 */
int client_cancel_file_transfer(secure_chat_client_t *client, uint64_t transfer_id);

/**
 * @brief 获取文件传输状态
 * @param client 客户端实例
 * @param transfer_id 传输ID
 * @param status 输出状态结构（可以为NULL）
 * @return 0表示成功，负数表示错误
 */
int client_get_file_transfer_status(secure_chat_client_t *client, uint64_t transfer_id, 
                                    file_transfer_status_t *status);

/**
 * @brief 请求用户列表
 * @param client 客户端实例
 * @param online_only 是否只获取在线用户
 * @return 0表示成功，负数表示错误
 */
int client_request_user_list(secure_chat_client_t *client, bool online_only);

/**
 * @brief 更新用户状态
 * @param client 客户端实例
 * @param status 新状态
 * @param status_message 状态消息（可以为NULL）
 * @return 0表示成功，负数表示错误
 */
int client_update_status(secure_chat_client_t *client, uint8_t status, const char *status_message);

/**
 * @brief 注册消息处理器
 * @param client 客户端实例
 * @param message_type 消息类型
 * @param callback 回调函数
 * @param user_data 用户数据
 * @return 0表示成功，负数表示错误
 */
int client_register_message_handler(secure_chat_client_t *client, uint32_t message_type,
                                    message_callback_t callback, void *user_data);

/**
 * @brief 移除消息处理器
 * @param client 客户端实例
 * @param message_type 消息类型
 * @param callback 要移除的回调函数
 * @return 0表示成功，负数表示错误
 */
int client_unregister_message_handler(secure_chat_client_t *client, uint32_t message_type,
                                      message_callback_t callback);

/**
 * @brief 设置错误回调
 * @param client 客户端实例
 * @param callback 回调函数
 * @param user_data 用户数据
 */
void client_set_error_callback(secure_chat_client_t *client, error_callback_t callback, void *user_data);

/**
 * @brief 设置状态变更回调
 * @param client 客户端实例
 * @param callback 回调函数
 * @param user_data 用户数据
 */
void client_set_state_change_callback(secure_chat_client_t *client, state_change_callback_t callback, void *user_data);

/**
 * @brief 获取客户端状态
 * @param client 客户端实例
 * @return 当前状态
 */
client_state_t client_get_state(const secure_chat_client_t *client);

/**
 * @brief 获取连接统计信息
 * @param client 客户端实例
 * @param messages_sent 输出发送消息数（可以为NULL）
 * @param messages_received 输出接收消息数（可以为NULL）
 * @param bytes_sent 输出发送字节数（可以为NULL）
 * @param bytes_received 输出接收字节数（可以为NULL）
 * @param connection_time 输出连接时间（毫秒，可以为NULL）
 */
void client_get_statistics(const secure_chat_client_t *client,
                          uint64_t *messages_sent, uint64_t *messages_received,
                          uint64_t *bytes_sent, uint64_t *bytes_received,
                          uint64_t *connection_time);

/**
 * @brief 获取聊天会话列表
 * @param client 客户端实例
 * @param sessions 输出会话数组（需要调用client_free_chat_sessions释放）
 * @param count 输出会话数量
 * @return 0表示成功，负数表示错误
 */
int client_get_chat_sessions(secure_chat_client_t *client, chat_session_t **sessions, size_t *count);

/**
 * @brief 释放聊天会话数组
 * @param sessions 会话数组
 * @param count 会话数量
 */
void client_free_chat_sessions(chat_session_t *sessions, size_t count);

/**
 * @brief 获取文件传输列表
 * @param client 客户端实例
 * @param transfers 输出传输数组（需要调用client_free_file_transfers释放）
 * @param count 输出传输数量
 * @return 0表示成功，负数表示错误
 */
int client_get_file_transfers(secure_chat_client_t *client, file_transfer_status_t **transfers, size_t *count);

/**
 * @brief 释放文件传输数组
 * @param transfers 传输数组
 * @param count 传输数量
 */
void client_free_file_transfers(file_transfer_status_t *transfers, size_t count);

/**
 * @brief 设置用户自定义数据
 * @param client 客户端实例
 * @param user_data 用户数据
 */
void client_set_user_data(secure_chat_client_t *client, void *user_data);

/**
 * @brief 获取用户自定义数据
 * @param client 客户端实例
 * @return 用户数据
 */
void* client_get_user_data(const secure_chat_client_t *client);

/**
 * @brief 阻塞等待直到客户端状态改变
 * @param client 客户端实例
 * @param target_state 目标状态
 * @param timeout_ms 超时时间（毫秒，0表示无限等待）
 * @return 0表示达到目标状态，-1表示超时，-2表示错误
 */
int client_wait_for_state(secure_chat_client_t *client, client_state_t target_state, int timeout_ms);

// ==================== 工具函数 ====================

/**
 * @brief 初始化默认配置
 * @param config 配置结构体指针
 */
void client_config_init(client_config_t *config);

/**
 * @brief 从JSON文件加载配置
 * @param config 配置结构体指针
 * @param filename JSON文件名
 * @return 0表示成功，负数表示错误
 */
int client_config_load_from_file(client_config_t *config, const char *filename);

/**
 * @brief 保存配置到JSON文件
 * @param config 配置结构体指针
 * @param filename JSON文件名
 * @return 0表示成功，负数表示错误
 */
int client_config_save_to_file(const client_config_t *config, const char *filename);

/**
 * @brief 验证客户端配置
 * @param config 配置结构体指针
 * @param error_buffer 错误信息缓冲区（可以为NULL）
 * @param error_buffer_size 错误信息缓冲区大小
 * @return true表示配置有效，false表示无效
 */
bool client_config_validate(const client_config_t *config, char *error_buffer, size_t error_buffer_size);

// ==================== 消息处理宏 ====================

// 注册消息处理器的便捷宏
#define CLIENT_REGISTER_HANDLER(client, type, func, data) \
    client_register_message_handler(client, type, (message_callback_t)func, data)

// 注销消息处理器的便捷宏  
#define CLIENT_UNREGISTER_HANDLER(client, type, func) \
    client_unregister_message_handler(client, type, (message_callback_t)func)

#endif // SECURE_CHAT_CLIENT_H