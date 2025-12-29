#ifndef SECURE_CHAT_CONNECTION_H
#define SECURE_CHAT_CONNECTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <openssl/ssl.h>
#include <netinet/in.h>

// 连接状态
typedef enum {
    CONNECTION_STATE_NEW = 0,      // 新连接，未认证
    CONNECTION_STATE_AUTHENTICATED, // 已认证
    CONNECTION_STATE_CLOSING,      // 正在关闭
    CONNECTION_STATE_CLOSED,       // 已关闭
    CONNECTION_STATE_ERROR         // 错误状态
} connection_state_t;

// 连接信息
typedef struct {
    uint64_t connection_id;
    uint64_t user_id;
    char username[64];
    char ip_address[INET_ADDRSTRLEN];
    int port;
    uint64_t connected_at;
    uint64_t last_activity;
    connection_state_t state;
    size_t bytes_sent;
    size_t bytes_received;
} connection_info_t;

// 客户端连接
typedef struct client_connection_t {
    // 基本信息
    uint64_t id;
    int fd;
    struct sockaddr_in client_addr;
    connection_state_t state;
    
    // SSL/TLS
    SSL *ssl;
    bool encryption_enabled;
    
    // 用户信息
    uint64_t user_id;
    char username[64];
    char nickname[64];
    uint8_t user_status;
    
    // 时间戳
    uint64_t connected_at;
    uint64_t last_activity;
    uint64_t last_heartbeat;
    
    // 缓冲区
    uint8_t *recv_buffer;
    size_t recv_buffer_size;
    size_t recv_buffer_used;
    uint8_t *send_buffer;
    size_t send_buffer_size;
    size_t send_buffer_used;
    
    // 统计
    size_t bytes_sent;
    size_t bytes_received;
    uint32_t messages_sent;
    uint32_t messages_received;
    
    // 速率限制
    struct {
        uint32_t messages_per_minute;
        uint32_t bytes_per_minute;
        uint64_t last_reset_time;
        uint32_t message_count;
        uint32_t byte_count;
    } rate_limiter;
    
    // 线程安全
    pthread_mutex_t mutex;
    pthread_cond_t data_ready;
    
    // 自定义数据
    void *user_data;
    
    // 链表指针
    struct client_connection_t *prev;
    struct client_connection_t *next;
} client_connection_t;

// 连接池
typedef struct connection_pool_t {
    client_connection_t *connections;
    size_t capacity;
    size_t count;
    uint64_t next_connection_id;
    
    // 线程安全
    pthread_mutex_t lock;
    pthread_rwlock_t rwlock;
    
    // 统计信息
    struct {
        uint64_t total_connections;
        uint64_t active_connections;
        uint64_t max_concurrent_connections;
        uint64_t rejected_connections;
        uint64_t connection_errors;
    } stats;
} connection_pool_t;

// 消息队列
typedef struct message_queue_t {
    void **messages;
    size_t capacity;
    size_t count;
    size_t head;
    size_t tail;
    
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} message_queue_t;

// ==================== 连接池API ====================

/**
 * @brief 创建连接池
 * @param initial_capacity 初始容量
 * @return 连接池指针，失败返回NULL
 */
connection_pool_t* connection_pool_create(size_t initial_capacity);

/**
 * @brief 销毁连接池
 * @param pool 连接池指针
 */
void connection_pool_destroy(connection_pool_t *pool);

/**
 * @brief 添加新连接
 * @param pool 连接池指针
 * @param fd 套接字文件描述符
 * @param addr 客户端地址
 * @return 连接指针，失败返回NULL
 */
client_connection_t* connection_pool_add(connection_pool_t *pool, int fd, struct sockaddr_in *addr);

/**
 * @brief 通过ID获取连接
 * @param pool 连接池指针
 * @param connection_id 连接ID
 * @return 连接指针，未找到返回NULL
 */
client_connection_t* connection_pool_get_by_id(connection_pool_t *pool, uint64_t connection_id);

/**
 * @brief 通过文件描述符获取连接
 * @param pool 连接池指针
 * @param fd 套接字文件描述符
 * @return 连接指针，未找到返回NULL
 */
client_connection_t* connection_pool_get_by_fd(connection_pool_t *pool, int fd);

/**
 * @brief 通过用户ID获取连接
 * @param pool 连接池指针
 * @param user_id 用户ID
 * @param connections 输出连接数组（需调用free释放）
 * @param count 输出连接数量
 * @return 0表示成功，负数表示错误
 */
int connection_pool_get_by_user(connection_pool_t *pool, uint64_t user_id, 
                               client_connection_t ***connections, size_t *count);

/**
 * @brief 移除连接
 * @param pool 连接池指针
 * @param connection_id 连接ID
 * @return 0表示成功，负数表示错误
 */
int connection_pool_remove(connection_pool_t *pool, uint64_t connection_id);

/**
 * @brief 获取活跃连接数
 * @param pool 连接池指针
 * @return 活跃连接数量
 */
size_t connection_pool_get_active_count(connection_pool_t *pool);

/**
 * @brief 清理过期连接
 * @param pool 连接池指针
 * @param timeout_ms 超时时间（毫秒）
 * @return 清理的连接数
 */
size_t connection_pool_cleanup_expired(connection_pool_t *pool, uint64_t timeout_ms);

/**
 * @brief 获取连接池统计信息
 * @param pool 连接池指针
 * @param stats 输出统计信息
 * @return 0表示成功，负数表示错误
 */
int connection_pool_get_statistics(connection_pool_t *pool, connection_pool_stats_t *stats);

/**
 * @brief 获取所有活跃连接信息
 * @param pool 连接池指针
 * @param connections 输出连接信息数组（需调用connection_pool_free_connection_list释放）
 * @param count 输出连接数量
 * @return 0表示成功，负数表示错误
 */
int connection_pool_get_active_connections(connection_pool_t *pool, 
                                         connection_info_t **connections, size_t *count);

/**
 * @brief 释放连接信息数组
 * @param connections 连接信息数组
 * @param count 连接数量
 */
void connection_pool_free_connection_list(connection_info_t *connections, size_t count);

// ==================== 连接管理API ====================

/**
 * @brief 创建客户端连接
 * @param fd 套接字文件描述符
 * @param addr 客户端地址
 * @param ssl_ctx SSL上下文（可为NULL）
 * @return 连接指针，失败返回NULL
 */
client_connection_t* connection_create(int fd, struct sockaddr_in *addr, SSL_CTX *ssl_ctx);

/**
 * @brief 销毁客户端连接
 * @param conn 连接指针
 */
void connection_destroy(client_connection_t *conn);

/**
 * @brief 关闭连接
 * @param conn 连接指针
 * @param graceful 是否优雅关闭
 * @return 0表示成功，负数表示错误
 */
int connection_close(client_connection_t *conn, bool graceful);

/**
 * @brief 发送数据
 * @param conn 连接指针
 * @param data 数据指针
 * @param len 数据长度
 * @return 发送的字节数，负数表示错误
 */
ssize_t connection_send(client_connection_t *conn, const void *data, size_t len);

/**
 * @brief 接收数据
 * @param conn 连接指针
 * @param buffer 接收缓冲区
 * @param buffer_len 缓冲区长度
 * @return 接收的字节数，负数表示错误
 */
ssize_t connection_receive(client_connection_t *conn, void *buffer, size_t buffer_len);

/**
 * @brief 发送消息
 * @param conn 连接指针
 * @param message_type 消息类型
 * @param data 消息数据
 * @param len 数据长度
 * @return 0表示成功，负数表示错误
 */
int connection_send_message(client_connection_t *conn, uint32_t message_type, 
                           const void *data, size_t len);

/**
 * @brief 接收消息
 * @param conn 连接指针
 * @param message_type 输出消息类型
 * @param data 输出消息数据（需调用free释放）
 * @param len 输出数据长度
 * @return 0表示成功，负数表示错误
 */
int connection_receive_message(client_connection_t *conn, uint32_t *message_type, 
                              void **data, size_t *len);

/**
 * @brief 处理接收到的数据
 * @param conn 连接指针
 * @param data 数据指针
 * @param len 数据长度
 * @return 处理的消息数，负数表示错误
 */
int connection_process_data(client_connection_t *conn, const void *data, size_t len);

/**
 * @brief 处理发送队列
 * @param conn 连接指针
 * @return 发送的消息数，负数表示错误
 */
int connection_process_send_queue(client_connection_t *conn);

/**
 * @brief 更新连接活动时间
 * @param conn 连接指针
 */
void connection_update_activity(client_connection_t *conn);

/**
 * @brief 设置连接用户信息
 * @param conn 连接指针
 * @param user_id 用户ID
 * @param username 用户名
 * @param nickname 昵称
 * @return 0表示成功，负数表示错误
 */
int connection_set_user_info(client_connection_t *conn, uint64_t user_id, 
                            const char *username, const char *nickname);

/**
 * @brief 清除连接用户信息
 * @param conn 连接指针
 */
void connection_clear_user_info(client_connection_t *conn);

/**
 * @brief 获取连接信息
 * @param conn 连接指针
 * @param info 输出连接信息
 * @return 0表示成功，负数表示错误
 */
int connection_get_info(client_connection_t *conn, connection_info_t *info);

/**
 * @brief 检查连接是否存活
 * @param conn 连接指针
 * @param timeout_ms 超时时间（毫秒）
 * @return true表示存活，false表示不存活
 */
bool connection_is_alive(client_connection_t *conn, uint64_t timeout_ms);

/**
 * @brief 设置连接自定义数据
 * @param conn 连接指针
 * @param user_data 自定义数据
 */
void connection_set_user_data(client_connection_t *conn, void *user_data);

/**
 * @brief 获取连接自定义数据
 * @param conn 连接指针
 * @return 自定义数据
 */
void* connection_get_user_data(client_connection_t *conn);

/**
 * @brief 启用SSL加密
 * @param conn 连接指针
 * @param ssl_ctx SSL上下文
 * @return 0表示成功，负数表示错误
 */
int connection_enable_ssl(client_connection_t *conn, SSL_CTX *ssl_ctx);

/**
 * @brief 禁用SSL加密
 * @param conn 连接指针
 */
void connection_disable_ssl(client_connection_t *conn);

// ==================== 消息队列API ====================

/**
 * @brief 创建消息队列
 * @param capacity 队列容量
 * @return 消息队列指针，失败返回NULL
 */
message_queue_t* message_queue_create(size_t capacity);

/**
 * @brief 销毁消息队列
 * @param queue 消息队列指针
 */
void message_queue_destroy(message_queue_t *queue);

/**
 * @brief 推送消息到队列
 * @param queue 消息队列指针
 * @param message 消息指针
 * @param timeout_ms 超时时间（毫秒）
 * @return 0表示成功，负数表示错误
 */
int message_queue_push(message_queue_t *queue, void *message, int timeout_ms);

/**
 * @brief 从队列弹出消息
 * @param queue 消息队列指针
 * @param timeout_ms 超时时间（毫秒）
 * @return 消息指针，超时返回NULL
 */
void* message_queue_pop(message_queue_t *queue, int timeout_ms);

/**
 * @brief 获取队列大小
 * @param queue 消息队列指针
 * @return 队列中的消息数量
 */
size_t message_queue_size(message_queue_t *queue);

/**
 * @brief 清空队列
 * @param queue 消息队列指针
 */
void message_queue_clear(message_queue_t *queue);

// ==================== 工具函数 ====================

/**
 * @brief 将连接状态转换为字符串
 * @param state 连接状态
 * @return 状态字符串
 */
const char* connection_state_to_string(connection_state_t state);

/**
 * @brief 验证连接地址
 * @param addr 客户端地址
 * @return true表示有效，false表示无效
 */
bool connection_validate_address(struct sockaddr_in *addr);

/**
 * @brief 初始化速率限制器
 * @param conn 连接指针
 * @param messages_per_minute 每分钟消息限制
 * @param bytes_per_minute 每分钟字节限制
 */
void connection_init_rate_limiter(client_connection_t *conn, 
                                 uint32_t messages_per_minute, 
                                 uint32_t bytes_per_minute);

/**
 * @brief 检查速率限制
 * @param conn 连接指针
 * @param message_size 消息大小
 * @return true表示允许，false表示限制
 */
bool connection_check_rate_limit(client_connection_t *conn, size_t message_size);

// ==================== 宏定义 ====================

// 连接超时时间（毫秒）
#define CONNECTION_TIMEOUT_MS                120000  // 2分钟
#define CONNECTION_HEARTBEAT_INTERVAL_MS     30000   // 30秒

// 缓冲区大小
#define CONNECTION_RECV_BUFFER_SIZE         65536
#define CONNECTION_SEND_BUFFER_SIZE         65536

// 速率限制默认值
#define DEFAULT_MESSAGES_PER_MINUTE         60
#define DEFAULT_BYTES_PER_MINUTE            (1024 * 1024)  // 1MB

// 消息队列默认容量
#define DEFAULT_MESSAGE_QUEUE_CAPACITY      1024

#endif // SECURE_CHAT_CONNECTION_H