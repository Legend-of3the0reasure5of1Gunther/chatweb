#ifndef SECURE_CHAT_MONITORING_PROTOCOL_H
#define SECURE_CHAT_MONITORING_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 监控常量定义 ====================
#define MONITORING_INTERVAL_MS 1000        // 监控数据收集间隔 (1秒)
#define MONITORING_HISTORY_SIZE 3600       // 历史数据保存1小时 (3600秒)
#define MONITORING_MAX_ALERTS 1000         // 最大告警记录数
#define MONITORING_MAX_METRICS 128         // 最大监控指标数
#define MONITORING_ALERT_THRESHOLD 90.0    // 告警阈值 (90%)

// ==================== 监控指标类型 ====================
typedef enum {
    METRIC_CPU_USAGE = 0,
    METRIC_MEMORY_USAGE = 1,
    METRIC_DISK_USAGE = 2,
    METRIC_NETWORK_RX = 3,
    METRIC_NETWORK_TX = 4,
    METRIC_CONNECTIONS = 5,
    METRIC_MESSAGES_PER_SECOND = 6,
    METRIC_ACTIVE_THREADS = 7,
    METRIC_QUEUE_LENGTH = 8,
    METRIC_RESPONSE_TIME = 9,
    METRIC_ERROR_RATE = 10,
    METRIC_THROUGHPUT = 11,
    METRIC_UPTIME = 12,
    METRIC_FILE_TRANSFERS = 13,
    METRIC_SSL_HANDSHAKES = 14,
    METRIC_CACHE_HITS = 15
} metric_type_t;

// ==================== 告警级别 ====================
typedef enum {
    ALERT_LEVEL_INFO = 0,      // 信息级别
    ALERT_LEVEL_WARNING = 1,   // 警告级别
    ALERT_LEVEL_ERROR = 2,     // 错误级别
    ALERT_LEVEL_CRITICAL = 3,  // 严重级别
    ALERT_LEVEL_EMERGENCY = 4  // 紧急级别
} alert_level_t;

// ==================== 告警状态 ====================
typedef enum {
    ALERT_STATUS_ACTIVE = 0,   // 活跃告警
    ALERT_STATUS_ACKNOWLEDGED = 1, // 已确认
    ALERT_STATUS_RESOLVED = 2, // 已解决
    ALERT_STATUS_SILENCED = 3  // 已静音
} alert_status_t;

// ==================== 监控数据结构 ====================
#pragma pack(push, 1)

// 单个监控数据点
typedef struct {
    uint64_t timestamp;        // 时间戳 (毫秒)
    double value;              // 监控值
    double min_value;          // 最小值
    double max_value;          // 最大值
    double avg_value;          // 平均值
    uint32_t sample_count;     // 采样数量
} metric_data_point_t;

// 监控指标
typedef struct {
    metric_type_t type;        // 指标类型
    char name[64];             // 指标名称
    char unit[16];             // 单位
    char description[256];     // 描述
    double current_value;      // 当前值
    double last_value;         // 上一次的值
    double min_value;          // 最小值
    double max_value;          // 最大值
    double avg_value;          // 平均值
    double warning_threshold;  // 警告阈值
    double error_threshold;    // 错误阈值
    uint64_t update_count;     // 更新次数
    uint64_t last_update;      // 最后更新时间
    bool enabled;              // 是否启用
    uint8_t trend;             // 趋势 (0=稳定, 1=上升, 2=下降)
} metric_t;

// 告警信息
typedef struct {
    uint64_t alert_id;         // 告警ID
    alert_level_t level;       // 告警级别
    alert_status_t status;     // 告警状态
    metric_type_t metric_type; // 关联的指标类型
    char title[128];           // 告警标题
    char message[512];         // 告警消息
    char source[64];           // 告警源
    double threshold;          // 触发阈值
    double actual_value;       // 实际值
    uint64_t trigger_time;     // 触发时间
    uint64_t acknowledge_time; // 确认时间
    uint64_t resolve_time;     // 解决时间
    char acknowledge_by[64];   // 确认人
    char resolve_by[64];       // 解决人
} alert_t;

// 系统资源信息
typedef struct {
    double cpu_usage;          // CPU使用率 (百分比)
    double memory_usage;       // 内存使用率 (百分比)
    uint64_t memory_total;     // 总内存 (字节)
    uint64_t memory_used;      // 已使用内存 (字节)
    uint64_t memory_free;      // 空闲内存 (字节)
    uint64_t memory_cached;    // 缓存内存 (字节)
    double disk_usage;         // 磁盘使用率 (百分比)
    uint64_t disk_total;       // 总磁盘空间 (字节)
    uint64_t disk_used;        // 已使用磁盘空间 (字节)
    uint64_t disk_free;        // 空闲磁盘空间 (字节)
    uint64_t network_rx_bytes; // 接收字节数
    uint64_t network_tx_bytes; // 发送字节数
    double network_rx_rate;    // 接收速率 (字节/秒)
    double network_tx_rate;    // 发送速率 (字节/秒)
    uint32_t open_files;       // 打开文件数
    uint32_t processes;        // 进程数
    uint32_t threads;          // 线程数
    uint64_t uptime;           // 系统运行时间 (秒)
} system_resources_t;

// 网络连接统计
typedef struct {
    uint32_t total_connections;    // 总连接数
    uint32_t active_connections;   // 活跃连接数
    uint32_t closed_connections;   // 已关闭连接数
    uint32_t failed_connections;   // 失败连接数
    uint32_t rejected_connections; // 拒绝连接数
    uint32_t max_concurrent;       // 最大并发连接数
    double connection_rate;        // 连接速率 (连接/秒)
    double avg_duration;           // 平均连接时长 (秒)
    uint64_t total_bytes_rx;       // 总接收字节数
    uint64_t total_bytes_tx;       // 总发送字节数
    uint64_t total_messages_rx;    // 总接收消息数
    uint64_t total_messages_tx;    // 总发送消息数
} connection_stats_t;

// 性能指标
typedef struct {
    double response_time_avg;      // 平均响应时间 (毫秒)
    double response_time_p95;      // 95%响应时间 (毫秒)
    double response_time_p99;      // 99%响应时间 (毫秒)
    double throughput;             // 吞吐量 (请求/秒)
    double error_rate;             // 错误率 (百分比)
    double success_rate;           // 成功率 (百分比)
    uint32_t active_requests;      // 活跃请求数
    uint32_t queued_requests;      // 排队请求数
    uint32_t completed_requests;   // 完成请求数
    uint32_t failed_requests;      // 失败请求数
    uint64_t cache_hits;           // 缓存命中数
    uint64_t cache_misses;         // 缓存未命中数
    double cache_hit_rate;         // 缓存命中率 (百分比)
} performance_metrics_t;

// 业务指标
typedef struct {
    uint32_t active_users;         // 活跃用户数
    uint32_t registered_users;     // 注册用户数
    uint32_t online_users;         // 在线用户数
    uint32_t groups_count;         // 群组数量
    uint32_t messages_today;       // 今日消息数
    uint32_t files_uploaded;       // 上传文件数
    uint64_t total_storage_used;   // 总存储使用量 (字节)
    double avg_session_duration;   // 平均会话时长 (秒)
    uint32_t api_calls;            // API调用次数
    uint32_t api_errors;           // API错误次数
    double api_success_rate;       // API成功率 (百分比)
} business_metrics_t;

// 监控快照
typedef struct {
    uint64_t snapshot_id;          // 快照ID
    uint64_t timestamp;            // 时间戳
    system_resources_t resources;  // 系统资源
    connection_stats_t connections; // 连接统计
    performance_metrics_t performance; // 性能指标
    business_metrics_t business;   // 业务指标
    uint32_t alert_count;          // 告警数量
    alert_level_t highest_alert;   // 最高告警级别
} monitoring_snapshot_t;

// 监控配置
typedef struct {
    uint32_t collection_interval_ms; // 收集间隔 (毫秒)
    uint32_t history_retention_seconds; // 历史数据保留时间 (秒)
    bool enable_system_monitoring;   // 启用系统监控
    bool enable_network_monitoring;  // 启用网络监控
    bool enable_performance_monitoring; // 启性能监控
    bool enable_business_monitoring; // 启用业务监控
    bool enable_alerting;           // 启用告警
    char alert_email[128];          // 告警邮箱
    char alert_webhook[256];        // 告警Webhook URL
    double warning_threshold_percent; // 警告阈值 (百分比)
    double error_threshold_percent; // 错误阈值 (百分比)
    uint32_t alert_cooldown_seconds; // 告警冷却时间 (秒)
    bool enable_logging;            // 启用日志记录
    char log_file_path[256];        // 日志文件路径
} monitoring_config_t;

// 监控消息 (用于WebSocket传输)
typedef struct {
    uint32_t type;                  // 消息类型
    uint64_t timestamp;             // 时间戳
    uint32_t data_length;           // 数据长度
    uint8_t data[];                 // 灵活数组成员
} monitoring_message_t;

// 监控消息类型
#define MONITORING_MSG_SNAPSHOT     0x00000001
#define MONITORING_MSG_ALERT        0x00000002
#define MONITORING_MSG_CONFIG       0x00000003
#define MONITORING_MSG_HISTORY      0x00000004
#define MONITORING_MSG_HEALTH_CHECK 0x00000005
#define MONITORING_MSG_METRIC_UPDATE 0x00000006

#pragma pack(pop)

// ==================== 监控管理器结构 ====================
typedef struct monitoring_manager monitoring_manager_t;

// ==================== 回调函数类型 ====================
typedef void (*alert_callback_t)(const alert_t *alert, void *user_data);
typedef void (*metric_update_callback_t)(metric_type_t type, double value, void *user_data);
typedef void (*health_check_callback_t)(bool healthy, const char *message, void *user_data);

// ==================== 函数声明 ====================

// 监控管理器创建和销毁
monitoring_manager_t* monitoring_manager_create(const monitoring_config_t *config);
void monitoring_manager_destroy(monitoring_manager_t *manager);

// 监控控制
int monitoring_start(monitoring_manager_t *manager);
int monitoring_stop(monitoring_manager_t *manager);
bool monitoring_is_running(const monitoring_manager_t *manager);

// 配置管理
int monitoring_set_config(monitoring_manager_t *manager, const monitoring_config_t *config);
int monitoring_get_config(const monitoring_manager_t *manager, monitoring_config_t *config);
monitoring_config_t monitoring_get_default_config(void);

// 数据收集
int monitoring_collect_snapshot(monitoring_manager_t *manager, monitoring_snapshot_t *snapshot);
int monitoring_get_history(monitoring_manager_t *manager, metric_type_t type, 
                          uint64_t start_time, uint64_t end_time,
                          metric_data_point_t **points, uint32_t *count);

// 告警管理
int monitoring_add_alert(monitoring_manager_t *manager, alert_level_t level,
                        metric_type_t metric_type, const char *title,
                        const char *message, const char *source,
                        double threshold, double actual_value);
int monitoring_get_alerts(monitoring_manager_t *manager, alert_t **alerts, 
                         uint32_t *count, alert_status_t status_filter);
int monitoring_acknowledge_alert(monitoring_manager_t *manager, uint64_t alert_id,
                                const char *acknowledged_by);
int monitoring_resolve_alert(monitoring_manager_t *manager, uint64_t alert_id,
                            const char *resolved_by);
int monitoring_silence_alert(monitoring_manager_t *manager, uint64_t alert_id,
                            uint32_t duration_seconds);

// 指标管理
int monitoring_update_metric(monitoring_manager_t *manager, metric_type_t type,
                            double value);
int monitoring_get_metric(monitoring_manager_t *manager, metric_type_t type,
                         metric_t *metric);
int monitoring_set_threshold(monitoring_manager_t *manager, metric_type_t type,
                           double warning, double error);

// 回调注册
int monitoring_register_alert_callback(monitoring_manager_t *manager,
                                      alert_callback_t callback, void *user_data);
int monitoring_register_metric_callback(monitoring_manager_t *manager,
                                       metric_update_callback_t callback, 
                                       void *user_data);
int monitoring_register_health_check_callback(monitoring_manager_t *manager,
                                             health_check_callback_t callback,
                                             void *user_data);

// 工具函数
const char* metric_type_to_string(metric_type_t type);
const char* alert_level_to_string(alert_level_t level);
const char* alert_status_to_string(alert_status_t status);
const char* get_system_hostname(void);
uint64_t get_system_uptime(void);

// WebSocket集成
int monitoring_ws_broadcast_snapshot(monitoring_manager_t *manager);
int monitoring_ws_send_alert(monitoring_manager_t *manager, const alert_t *alert);
int monitoring_ws_send_metric_update(monitoring_manager_t *manager,
                                    metric_type_t type, double value);

#ifdef __cplusplus
}
#endif

#endif // SECURE_CHAT_MONITORING_PROTOCOL_H