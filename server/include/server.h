#ifndef SECURE_CHAT_SERVER_H
#define SECURE_CHAT_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <openssl/ssl.h>
#include <sys/epoll.h>

#include "connection.h"
#include "database.h"
#include "thread_pool.h"
#include "websocket_server.h"
#include "monitoring.h"
#include "auth.h"
#include "message_handler.h"
#include "file_transfer.h"

// 服务器配置
typedef struct {
    // 网络配置
    int tcp_port;
    int websocket_port;
    int max_clients;
    int backlog_size;
    
    // SSL配置
    bool enable_ssl;
    const char *ssl_cert_path;
    const char *ssl_key_path;
    const char *ssl_ca_path;
    
    // 连接管理
    int thread_pool_size;
    int heartbeat_interval;
    int heartbeat_timeout;
    int session_timeout;
    int rate_limit_per_minute;
    int max_message_size;
    int max_file_size;
    
    // 数据库配置
    const char *database_path;
    bool database_encryption;
    const char *database_key;
    
    // 存储配置
    const char *upload_dir;
    const char *temp_dir;
    size_t max_upload_size;
    bool enable_file_scan;
    
    // 日志配置
    int log_level;
    const char *log_file;
    size_t log_max_size;
    int log_max_files;
    
    // 安全配置
    bool enable_audit;
    bool enable_encryption;
    bool require_client_auth;
    int max_login_attempts;
    int login_lockout_time;
    
    // 监控配置
    bool enable_monitoring;
    int monitoring_port;
    const char *monitoring_host;
    
    // 运行配置
    bool run_as_daemon;
    const char *run_as_user;
    const char *pid_file;
    bool test_mode;
} server_config_t;

// 服务器状态
typedef enum {
    SERVER_STATE_INITIALIZING = 0,
    SERVER_STATE_RUNNING,
    SERVER_STATE_SHUTTING_DOWN,
    SERVER_STATE_SHUTDOWN,
    SERVER_STATE_ERROR
} server_state_t;

// 服务器统计信息
typedef struct {
    // 连接统计
    uint64_t total_connections;
    uint64_t active_connections;
    uint64_t rejected_connections;
    uint64_t max_concurrent_connections;
    
    // 消息统计
    uint64_t messages_received;
    uint64_t messages_sent;
    uint64_t messages_processed;
    uint64_t messages_failed;
    
    // 文件传输统计
    uint64_t file_transfers;
    uint64_t file_transfer_bytes;
    uint64_t file_transfer_failed;
    
    // 用户统计
    uint64_t total_users;
    uint64_t online_users;
    uint64_t auth_failures;
    
    // 性能统计
    uint64_t cpu_usage;
    uint64_t memory_usage;
    uint64_t disk_usage;
    uint64_t network_in_bytes;
    uint64_t network_out_bytes;
    
    // 时间统计
    uint64_t startup_time;
    uint64_t uptime;
    uint64_t last_backup_time;
} server_statistics_t;

// 服务器事件回调
typedef void (*server_event_callback_t)(int event_type, void *event_data, void *user_data);

// 服务器实例
typedef struct server_instance_t {
    // 配置
    server_config_t config;
    server_state_t state;
    
    // 网络
    int tcp_fd;
    int ws_fd;
    int epoll_fd;
    struct epoll_event *events;
    
    // SSL
    SSL_CTX *ssl_ctx;
    
    // 数据库
    sqlite3 *db_conn;
    
    // 线程池
    thread_pool_t *thread_pool;
    
    // WebSocket服务器
    websocket_server_t *ws_server;
    
    // 监控
    monitoring_server_t *monitoring;
    
    // 连接管理
    connection_pool_t *connection_pool;
    
    // 用户管理
    user_manager_t *user_manager;
    
    // 消息队列
    message_queue_t *message_queue;
    
    // 文件传输管理器
    file_transfer_manager_t *file_transfer_mgr;
    
    // 统计信息
    server_statistics_t stats;
    pthread_mutex_t stats_mutex;
    
    // 事件回调
    server_event_callback_t event_callback;
    void *event_callback_data;
    
    // 同步
    pthread_mutex_t state_mutex;
    pthread_cond_t state_cond;
    
    // 运行标志
    volatile bool running;
    volatile bool shutdown_requested;
    
    // 线程
    pthread_t accept_thread;
    pthread_t cleanup_thread;
    pthread_t maintenance_thread;
    
    // 定时器
    void *timer_manager;
    
    // 日志
    void *logger;
    
    // 审计日志
    FILE *audit_log;
    pthread_mutex_t audit_mutex;
    
    // 用户数据
    void *user_data;
} server_instance_t;

// ==================== 服务器API ====================

/**
 * @brief 创建服务器实例
 * @param config 服务器配置
 * @return 服务器实例指针，失败返回NULL
 */
server_instance_t* server_create(const server_config_t *config);

/**
 * @brief 销毁服务器实例
 * @param server 服务器实例
 */
void server_destroy(server_instance_t *server);

/**
 * @brief 初始化服务器
 * @param server 服务器实例
 * @return 0表示成功，负数表示错误
 */
int server_init(server_instance_t *server);

/**
 * @brief 启动服务器
 * @param server 服务器实例
 * @return 0表示成功，负数表示错误
 */
int server_start(server_instance_t *server);

/**
 * @brief 停止服务器
 * @param server 服务器实例
 * @param graceful 是否优雅关闭
 * @return 0表示成功，负数表示错误
 */
int server_stop(server_instance_t *server, bool graceful);

/**
 * @brief 重新加载服务器配置
 * @param server 服务器实例
 * @param new_config 新配置
 * @return 0表示成功，负数表示错误
 */
int server_reload_config(server_instance_t *server, const server_config_t *new_config);

/**
 * @brief 获取服务器状态
 * @param server 服务器实例
 * @return 当前状态
 */
server_state_t server_get_state(const server_instance_t *server);

/**
 * @brief 等待服务器状态改变
 * @param server 服务器实例
 * @param target_state 目标状态
 * @param timeout_ms 超时时间（毫秒）
 * @return 0表示达到目标状态，-1表示超时，-2表示错误
 */
int server_wait_for_state(server_instance_t *server, server_state_t target_state, int timeout_ms);

/**
 * @brief 获取服务器统计信息
 * @param server 服务器实例
 * @param stats 输出统计信息（可以为NULL）
 * @return 0表示成功，负数表示错误
 */
int server_get_statistics(server_instance_t *server, server_statistics_t *stats);

/**
 * @brief 重置服务器统计信息
 * @param server 服务器实例
 * @return 0表示成功，负数表示错误
 */
int server_reset_statistics(server_instance_t *server);

/**
 * @brief 广播消息给所有客户端
 * @param server 服务器实例
 * @param message_type 消息类型
 * @param data 消息数据
 * @param len 数据长度
 * @param exclude_user_id 排除的用户ID（0表示不排除）
 * @return 成功发送的客户端数量
 */
size_t server_broadcast_message(server_instance_t *server, uint32_t message_type, 
                               const void *data, size_t len, uint64_t exclude_user_id);

/**
 * @brief 发送消息给指定用户
 * @param server 服务器实例
 * @param user_id 用户ID
 * @param message_type 消息类型
 * @param data 消息数据
 * @param len 数据长度
 * @return 0表示成功，负数表示错误
 */
int server_send_to_user(server_instance_t *server, uint64_t user_id, 
                       uint32_t message_type, const void *data, size_t len);

/**
 * @brief 发送消息给指定连接
 * @param server 服务器实例
 * @param conn_id 连接ID
 * @param message_type 消息类型
 * @param data 消息数据
 * @param len 数据长度
 * @return 0表示成功，负数表示错误
 */
int server_send_to_connection(server_instance_t *server, uint64_t conn_id,
                             uint32_t message_type, const void *data, size_t len);

/**
 * @brief 踢出用户
 * @param server 服务器实例
 * @param user_id 用户ID
 * @param reason 踢出原因
 * @return 0表示成功，负数表示错误
 */
int server_kick_user(server_instance_t *server, uint64_t user_id, const char *reason);

/**
 * @brief 踢出连接
 * @param server 服务器实例
 * @param conn_id 连接ID
 * @param reason 踢出原因
 * @return 0表示成功，负数表示错误
 */
int server_kick_connection(server_instance_t *server, uint64_t conn_id, const char *reason);

/**
 * @brief 获取在线用户列表
 * @param server 服务器实例
 * @param users 输出用户列表（需要调用server_free_user_list释放）
 * @param count 输出用户数量
 * @return 0表示成功，负数表示错误
 */
int server_get_online_users(server_instance_t *server, user_info_t **users, size_t *count);

/**
 * @brief 释放用户列表
 * @param users 用户列表
 * @param count 用户数量
 */
void server_free_user_list(user_info_t *users, size_t count);

/**
 * @brief 获取活跃连接列表
 * @param server 服务器实例
 * @param connections 输出连接列表（需要调用server_free_connection_list释放）
 * @param count 输出连接数量
 * @return 0表示成功，负数表示错误
 */
int server_get_active_connections(server_instance_t *server, 
                                 connection_info_t **connections, size_t *count);

/**
 * @brief 释放连接列表
 * @param connections 连接列表
 * @param count 连接数量
 */
void server_free_connection_list(connection_info_t *connections, size_t count);

/**
 * @brief 设置服务器事件回调
 * @param server 服务器实例
 * @param callback 回调函数
 * @param user_data 用户数据
 */
void server_set_event_callback(server_instance_t *server, 
                              server_event_callback_t callback, void *user_data);

/**
 * @brief 设置用户数据
 * @param server 服务器实例
 * @param user_data 用户数据
 */
void server_set_user_data(server_instance_t *server, void *user_data);

/**
 * @brief 获取用户数据
 * @param server 服务器实例
 * @return 用户数据
 */
void* server_get_user_data(const server_instance_t *server);

/**
 * @brief 备份服务器数据
 * @param server 服务器实例
 * @param backup_path 备份路径
 * @return 0表示成功，负数表示错误
 */
int server_backup_data(server_instance_t *server, const char *backup_path);

/**
 * @brief 恢复服务器数据
 * @param server 服务器实例
 * @param backup_path 备份路径
 * @return 0表示成功，负数表示错误
 */
int server_restore_data(server_instance_t *server, const char *backup_path);

// ==================== 服务器事件 ====================

// 服务器事件类型
typedef enum {
    SERVER_EVENT_STARTUP = 1,
    SERVER_EVENT_SHUTDOWN,
    SERVER_EVENT_CONFIG_RELOAD,
    SERVER_EVENT_CLIENT_CONNECT,
    SERVER_EVENT_CLIENT_DISCONNECT,
    SERVER_EVENT_USER_LOGIN,
    SERVER_EVENT_USER_LOGOUT,
    SERVER_EVENT_MESSAGE_RECEIVED,
    SERVER_EVENT_MESSAGE_SENT,
    SERVER_EVENT_FILE_TRANSFER_START,
    SERVER_EVENT_FILE_TRANSFER_COMPLETE,
    SERVER_EVENT_FILE_TRANSFER_FAILED,
    SERVER_EVENT_ERROR,
    SERVER_EVENT_WARNING,
    SERVER_EVENT_AUDIT,
    SERVER_EVENT_MAINTENANCE,
    SERVER_EVENT_BACKUP,
    SERVER_EVENT_RESTORE
} server_event_type_t;

// 服务器事件数据
typedef struct {
    server_event_type_t type;
    uint64_t timestamp;
    uint64_t user_id;
    uint64_t connection_id;
    const char *description;
    void *data;
    size_t data_len;
} server_event_t;

// ==================== 配置管理函数 ====================

/**
 * @brief 初始化默认服务器配置
 * @param config 配置结构体指针
 */
void server_config_init(server_config_t *config);

/**
 * @brief 从JSON文件加载服务器配置
 * @param config 配置结构体指针
 * @param filename JSON文件名
 * @return true表示成功，false表示失败
 */
bool server_config_load_from_file(server_config_t *config, const char *filename);

/**
 * @brief 保存服务器配置到JSON文件
 * @param config 配置结构体指针
 * @param filename JSON文件名
 * @return true表示成功，false表示失败
 */
bool server_config_save_to_file(const server_config_t *config, const char *filename);

/**
 * @brief 验证服务器配置
 * @param config 配置结构体指针
 * @param error_buffer 错误信息缓冲区（可以为NULL）
 * @param error_buffer_size 错误信息缓冲区大小
 * @return true表示配置有效，false表示无效
 */
bool server_config_validate(const server_config_t *config, 
                           char *error_buffer, size_t error_buffer_size);

// ==================== 工具函数 ====================

/**
 * @brief 生成服务器状态报告
 * @param server 服务器实例
 * @param report_buffer 报告缓冲区
 * @param buffer_size 缓冲区大小
 * @return 报告字符串长度
 */
int server_generate_status_report(server_instance_t *server, 
                                 char *report_buffer, size_t buffer_size);

/**
 * @brief 检查服务器健康状态
 * @param server 服务器实例
 * @return 健康状态码（0表示健康，其他表示不健康）
 */
int server_check_health(server_instance_t *server);

/**
 * @brief 执行服务器维护任务
 * @param server 服务器实例
 * @return 0表示成功，负数表示错误
 */
int server_perform_maintenance(server_instance_t *server);

/**
 * @brief 清理过期数据
 * @param server 服务器实例
 * @return 清理的数据数量
 */
size_t server_cleanup_expired_data(server_instance_t *server);

// ==================== 宏定义 ====================

// 默认配置值
#define DEFAULT_PORT                   8888
#define WEBSOCKET_PORT                8889
#define MAX_CLIENTS                   1000
#define BACKLOG_SIZE                  128
#define HEARTBEAT_INTERVAL            30000   // 30秒
#define HEARTBEAT_TIMEOUT             120000  // 2分钟
#define SESSION_TIMEOUT               3600000 // 1小时
#define RATE_LIMIT_PER_MINUTE         60
#define MAX_MESSAGE_SIZE              65536
#define MAX_FILE_SIZE                 (100 * 1024 * 1024) // 100MB

// 服务器事件宏
#define SERVER_TRIGGER_EVENT(server, type, user_id, conn_id, desc, data, len) \
    do { \
        if ((server)->event_callback) { \
            server_event_t event = { \
                .type = (type), \
                .timestamp = get_current_timestamp_ms(), \
                .user_id = (user_id), \
                .connection_id = (conn_id), \
                .description = (desc), \
                .data = (data), \
                .data_len = (len) \
            }; \
            (server)->event_callback((type), &event, (server)->event_callback_data); \
        } \
    } while(0)

#endif // SECURE_CHAT_SERVER_H