#ifndef SECURE_CHAT_MONITOR_PROTOCOL_H
#define SECURE_CHAT_MONITOR_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 监控常量定义 ====================
#define MONITOR_DEFAULT_PORT 8890
#define MONITOR_MAX_METRICS 256
#define MONITOR_HISTORY_SIZE 3600     // 1小时历史数据（每秒一个点）
#define MONITOR_ALERT_BUFFER_SIZE 100
#define MONITOR_MAX_THREADS 32

// ==================== 监控指标类型 ====================
typedef enum {
    // 系统指标
    METRIC_CPU_USAGE = 0x0001,
    METRIC_MEMORY_USAGE = 0x0002,
    METRIC_DISK_USAGE = 0x0003,
    METRIC_NETWORK_RX = 0x0004,
    METRIC_NETWORK_TX = 0x0005,
    METRIC_SYSTEM_LOAD = 0x0006,
    METRIC_PROCESS_COUNT = 0x0007,
    METRIC_UPTIME = 0x0008,
    METRIC_TEMPERATURE = 0x0009,
    
    // 服务器指标
    METRIC_CONNECTIONS_TOTAL = 0x0101,
    METRIC_CONNECTIONS_ACTIVE = 0x0102,
    METRIC_CONNECTIONS_PENDING = 0x0103,
    METRIC_MESSAGES_PER_SECOND = 0x0104,
    METRIC_BYTES_PER_SECOND = 0x0105,
    METRIC_ERROR_RATE = 0x0106,
    METRIC_RESPONSE_TIME = 0x0107,
    METRIC_QUEUE_LENGTH = 0x0108,
    
    // 线程池指标
    METRIC_THREADS_TOTAL = 0x0201,
    METRIC_THREADS_ACTIVE = 0x0202,
    METRIC_THREADS_IDLE = 0x0203,
    METRIC_TASKS_QUEUED = 0x0204,
    METRIC_TASKS_COMPLETED = 0x0205,
    METRIC_TASKS_FAILED = 0x0206,
    METRIC_THREADPOOL_LOAD = 0x0207,
    
    // 文件传输指标
    METRIC_FILE_UPLOADS_ACTIVE = 0x0301,
    METRIC_FILE_DOWNLOADS_ACTIVE = 0x0302,
    METRIC_FILE_TRANSFER_RATE = 0x0303,
    METRIC_FILE_TRANSFER_TOTAL = 0x0304,
    METRIC_FILE_TRANSFER_FAILED = 0x0305,
    METRIC_FILE_STORAGE_USAGE = 0x0306,
    
    // 数据库指标
    METRIC_DB_CONNECTIONS = 0x0401,
    METRIC_DB_QUERIES_PER_SECOND = 0x0402,
    METRIC_DB_QUERY_TIME = 0x0403,
    METRIC_DB_CONNECTION_POOL = 0x0404,
    
    // 安全指标
    METRIC_AUTH_ATTEMPTS = 0x0501,
    METRIC_AUTH_FAILURES = 0x0502,
    METRIC_SECURITY_EVENTS = 0x0503,
    METRIC_IP_BLOCKS = 0x0504,
    METRIC_RATE_LIMIT_HITS = 0x0505
} metric_type_t;

// ==================== 监控数据点 ====================
#pragma pack(push, 1)
typedef struct {
    uint64_t timestamp;           // 时间戳（毫秒）
    double value;                 // 指标值
    uint16_t metric_id;          // 指标ID
    uint8_t quality;             // 数据质量 (0-100)
    uint8_t flags;               // 标志位
} metric_point_t;

// 监控指标定义
typedef struct {
    uint16_t id;                 // 指标ID
    char name[64];               // 指标名称
    char description[256];       // 指标描述
    char unit[16];               // 单位
    double min_value;            // 最小值
    double max_value;            // 最大值
    double warning_threshold;    // 警告阈值
    double critical_threshold;   // 严重阈值
    uint8_t enabled;             // 是否启用
    uint8_t aggregation_type;    // 聚合类型 (0=平均, 1=最大, 2=最小, 3=总和)
    uint32_t collection_interval; // 收集间隔（毫秒）
    uint64_t last_collection;    // 最后收集时间
} metric_definition_t;

// 监控警报
typedef enum {
    ALERT_LEVEL_INFO = 0,        // 信息
    ALERT_LEVEL_WARNING = 1,     // 警告
    ALERT_LEVEL_ERROR = 2,       // 错误
    ALERT_LEVEL_CRITICAL = 3     // 严重
} alert_level_t;

typedef struct {
    uint64_t alert_id;           // 警报ID
    uint64_t timestamp;          // 时间戳
    alert_level_t level;         // 警报级别
    uint16_t metric_id;          // 关联的指标ID
    char source[64];             // 来源
    char message[256];           // 警报消息
    char details[512];           // 详细描述
    uint8_t acknowledged;        // 是否已确认
    uint8_t active;              // 是否活跃
    uint64_t acknowledged_by;    // 确认者用户ID
    uint64_t acknowledged_at;    // 确认时间
} alert_t;

// 监控统计
typedef struct {
    uint64_t metrics_collected;      // 收集的指标数量
    uint64_t alerts_generated;       // 生成的警报数量
    uint64_t alerts_acknowledged;    // 已确认的警报数量
    uint64_t data_points_stored;     // 存储的数据点数量
    uint64_t queries_served;         // 服务的查询数量
    uint64_t bytes_transferred;      // 传输的数据量
    uint64_t last_cleanup_time;      // 最后清理时间
    uint64_t startup_time;           // 启动时间
} monitor_stats_t;

// 监控配置
typedef struct {
    uint16_t port;                  // 监控端口
    uint32_t history_retention;     // 历史数据保留时间（秒）
    uint32_t cleanup_interval;      // 清理间隔（秒）
    uint32_t alert_retention;       // 警报保留时间（秒）
    uint32_t max_data_points;       // 最大数据点数
    uint32_t max_alerts;            // 最大警报数
    uint8_t enable_snmp;            // 启用SNMP
    uint8_t enable_prometheus;      // 启用Prometheus导出
    uint8_t enable_http_api;        // 启用HTTP API
    uint8_t enable_websocket;       // 启用WebSocket推送
    char bind_address[46];          // 绑定地址
    char auth_token[256];           // 认证令牌
} monitor_config_t;

// 监控查询请求
typedef struct {
    uint64_t request_id;           // 请求ID
    uint16_t metric_id;           // 指标ID
    uint64_t start_time;          // 开始时间
    uint64_t end_time;            // 结束时间
    uint32_t resolution;          // 分辨率（秒）
    uint8_t aggregation;          // 聚合方式
    uint8_t format;               // 格式 (0=二进制, 1=JSON, 2=CSV)
} monitor_query_t;

// 监控查询响应
typedef struct {
    uint64_t request_id;           // 请求ID
    uint16_t metric_id;           // 指标ID
    uint32_t point_count;         // 数据点数量
    uint64_t start_time;          // 开始时间
    uint64_t end_time;            // 结束时间
    metric_point_t points[];      // 数据点数组
} monitor_query_response_t;

// 实时数据订阅
typedef struct {
    uint64_t subscription_id;     // 订阅ID
    uint64_t client_id;           // 客户端ID
    uint16_t metric_ids[32];      // 指标ID数组
    uint8_t metric_count;         // 指标数量
    uint32_t update_interval;     // 更新间隔（毫秒）
    uint64_t last_update;         // 最后更新时间
} monitor_subscription_t;
#pragma pack(pop)

// ==================== 监控接口类型 ====================
typedef enum {
    MONITOR_INTERFACE_HTTP = 0,
    MONITOR_INTERFACE_WEBSOCKET = 1,
    MONITOR_INTERFACE_SNMP = 2,
    MONITOR_INTERFACE_PROMETHEUS = 3,
    MONITOR_INTERFACE_GRAPHITE = 4
} monitor_interface_t;

// ==================== 监控状态 ====================
typedef enum {
    MONITOR_STATE_STOPPED = 0,
    MONITOR_STATE_STARTING = 1,
    MONITOR_STATE_RUNNING = 2,
    MONITOR_STATE_STOPPING = 3,
    MONITOR_STATE_ERROR = 4
} monitor_state_t;

// ==================== 监控器主结构 ====================
typedef struct monitor {
    // 配置
    monitor_config_t config;              // 配置
    monitor_stats_t stats;                // 统计
    
    // 数据存储
    metric_definition_t *metrics;         // 指标定义数组
    metric_point_t *history_buffer;       // 历史数据缓冲区
    alert_t *alerts;                      // 警报数组
    
    // 管理
    uint32_t metric_count;                // 指标数量
    uint32_t max_metrics;                 // 最大指标数
    uint32_t history_size;                // 历史数据大小
    uint32_t history_index;               // 历史数据索引
    uint32_t alert_count;                 // 警报数量
    uint32_t max_alerts;                  // 最大警报数
    
    // 接口
    void *http_server;                    // HTTP服务器
    void *websocket_server;               // WebSocket服务器
    monitor_subscription_t *subscriptions; // 订阅列表
    uint32_t subscription_count;          // 订阅数量
    
    // 线程
    pthread_t collector_thread;           // 收集器线程
    pthread_t processor_thread;           // 处理器线程
    pthread_t alert_thread;               // 警报线程
    pthread_t api_thread;                 // API线程
    
    // 同步原语
    pthread_mutex_t data_mutex;           // 数据互斥锁
    pthread_mutex_t alert_mutex;          // 警报互斥锁
    pthread_cond_t data_cond;             // 数据条件变量
    
    // 控制标志
    volatile monitor_state_t state;       // 状态
    volatile bool shutdown_requested;     // 关闭请求
    
    // 回调函数
    void (*on_alert)(alert_t *alert);     // 警报回调
    void (*on_metric_update)(uint16_t metric_id, double value); // 指标更新回调
    
    // 外部系统引用
    void *websocket_server_ref;           // WebSocket服务器引用
    void *threadpool_ref;                 // 线程池引用
    void *file_transfer_ref;              // 文件传输引用
} monitor_t;

// ==================== 函数声明 ====================

// 监控器管理
monitor_t* monitor_create(const monitor_config_t *config);
int monitor_start(monitor_t *monitor);
int monitor_stop(monitor_t *monitor);
void monitor_destroy(monitor_t *monitor);

// 配置管理
monitor_config_t monitor_get_default_config(void);
int monitor_set_config(monitor_t *monitor, const monitor_config_t *config);

// 指标管理
int monitor_register_metric(monitor_t *monitor, const metric_definition_t *metric);
int monitor_unregister_metric(monitor_t *monitor, uint16_t metric_id);
int monitor_update_metric(monitor_t *monitor, uint16_t metric_id, double value);
int monitor_get_metric(monitor_t *monitor, uint16_t metric_id, metric_definition_t *metric);
int monitor_list_metrics(monitor_t *monitor, metric_definition_t *metrics, uint32_t *count);

// 数据查询
int monitor_query_data(monitor_t *monitor, const monitor_query_t *query,
                      monitor_query_response_t **response);
int monitor_get_latest(monitor_t *monitor, uint16_t metric_id, metric_point_t *point);
int monitor_get_history(monitor_t *monitor, uint16_t metric_id, 
                       metric_point_t *points, uint32_t *count,
                       uint64_t start_time, uint64_t end_time);

// 警报管理
int monitor_add_alert(monitor_t *monitor, const alert_t *alert);
int monitor_acknowledge_alert(monitor_t *monitor, uint64_t alert_id, uint64_t user_id);
int monitor_get_alerts(monitor_t *monitor, alert_t *alerts, uint32_t *count,
                      alert_level_t min_level, uint8_t active_only);
int monitor_clear_alerts(monitor_t *monitor, uint64_t older_than);

// 订阅管理
int monitor_subscribe(monitor_t *monitor, uint64_t client_id, 
                     const uint16_t *metric_ids, uint8_t count,
                     uint32_t update_interval, uint64_t *subscription_id);
int monitor_unsubscribe(monitor_t *monitor, uint64_t subscription_id);
int monitor_notify_subscribers(monitor_t *monitor, uint16_t metric_id, double value);

// 统计信息
int monitor_get_statistics(monitor_t *monitor, monitor_stats_t *stats);
void monitor_reset_statistics(monitor_t *monitor);

// 系统监控
int monitor_collect_system_metrics(monitor_t *monitor);
int monitor_collect_server_metrics(monitor_t *monitor);
int monitor_collect_threadpool_metrics(monitor_t *monitor);
int monitor_collect_file_transfer_metrics(monitor_t *monitor);

// 工具函数
const char* metric_type_to_string(metric_type_t type);
const char* alert_level_to_string(alert_level_t level);
char* format_metric_value(double value, const char *unit, char *buffer, size_t size);

// 回调设置
void monitor_set_alert_callback(monitor_t *monitor, void (*callback)(alert_t *));
void monitor_set_metric_update_callback(monitor_t *monitor, 
                                       void (*callback)(uint16_t, double));

// 外部系统设置
void monitor_set_websocket_server(monitor_t *monitor, void *server);
void monitor_set_threadpool(monitor_t *monitor, void *threadpool);
void monitor_set_file_transfer(monitor_t *monitor, void *file_transfer);

#ifdef __cplusplus
}
#endif

#endif // SECURE_CHAT_MONITOR_PROTOCOL_H