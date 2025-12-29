#ifndef SECURE_CHAT_MONITORING_H
#define SECURE_CHAT_MONITORING_H

#include "monitoring_protocol.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 监控管理器API ====================

/**
 * 创建监控管理器
 * @param config 监控配置，如果为NULL则使用默认配置
 * @return 监控管理器指针，失败返回NULL
 */
monitoring_manager_t* monitoring_manager_create(const monitoring_config_t *config);

/**
 * 销毁监控管理器
 * @param manager 监控管理器指针
 */
void monitoring_manager_destroy(monitoring_manager_t *manager);

/**
 * 启动监控
 * @param manager 监控管理器指针
 * @return 成功返回0，失败返回-1
 */
int monitoring_start(monitoring_manager_t *manager);

/**
 * 停止监控
 * @param manager 监控管理器指针
 * @return 成功返回0，失败返回-1
 */
int monitoring_stop(monitoring_manager_t *manager);

/**
 * 检查监控是否运行
 * @param manager 监控管理器指针
 * @return 运行返回true，否则返回false
 */
bool monitoring_is_running(const monitoring_manager_t *manager);

/**
 * 获取默认配置
 * @return 默认配置
 */
monitoring_config_t monitoring_get_default_config(void);

/**
 * 设置监控配置
 * @param manager 监控管理器指针
 * @param config 新配置
 * @return 成功返回0，失败返回-1
 */
int monitoring_set_config(monitoring_manager_t *manager, const monitoring_config_t *config);

/**
 * 获取监控配置
 * @param manager 监控管理器指针
 * @param config 存储配置的位置
 * @return 成功返回0，失败返回-1
 */
int monitoring_get_config(const monitoring_manager_t *manager, monitoring_config_t *config);

/**
 * 收集监控快照
 * @param manager 监控管理器指针
 * @param snapshot 存储快照的位置
 * @return 成功返回0，失败返回-1
 */
int monitoring_collect_snapshot(monitoring_manager_t *manager, monitoring_snapshot_t *snapshot);

/**
 * 获取历史数据
 * @param manager 监控管理器指针
 * @param type 指标类型
 * @param start_time 开始时间戳
 * @param end_time 结束时间戳
 * @param points 返回的数据点数组（需要调用者释放）
 * @param count 返回的数据点数量
 * @return 成功返回0，失败返回-1
 */
int monitoring_get_history(monitoring_manager_t *manager, metric_type_t type, 
                          uint64_t start_time, uint64_t end_time,
                          metric_data_point_t **points, uint32_t *count);

/**
 * 添加告警
 * @param manager 监控管理器指针
 * @param level 告警级别
 * @param metric_type 关联的指标类型
 * @param title 告警标题
 * @param message 告警消息
 * @param source 告警源
 * @param threshold 触发阈值
 * @param actual_value 实际值
 * @return 成功返回0，失败返回-1
 */
int monitoring_add_alert(monitoring_manager_t *manager, alert_level_t level,
                        metric_type_t metric_type, const char *title,
                        const char *message, const char *source,
                        double threshold, double actual_value);

/**
 * 获取告警列表
 * @param manager 监控管理器指针
 * @param alerts 返回的告警数组（需要调用者释放）
 * @param count 返回的告警数量
 * @param status_filter 状态过滤，-1表示不过滤
 * @return 成功返回0，失败返回-1
 */
int monitoring_get_alerts(monitoring_manager_t *manager, alert_t **alerts, 
                         uint32_t *count, alert_status_t status_filter);

/**
 * 确认告警
 * @param manager 监控管理器指针
 * @param alert_id 告警ID
 * @param acknowledged_by 确认人
 * @return 成功返回0，失败返回-1
 */
int monitoring_acknowledge_alert(monitoring_manager_t *manager, uint64_t alert_id,
                                const char *acknowledged_by);

/**
 * 解决告警
 * @param manager 监控管理器指针
 * @param alert_id 告警ID
 * @param resolved_by 解决人
 * @return 成功返回0，失败返回-1
 */
int monitoring_resolve_alert(monitoring_manager_t *manager, uint64_t alert_id,
                            const char *resolved_by);

/**
 * 静音告警
 * @param manager 监控管理器指针
 * @param alert_id 告警ID
 * @param duration_seconds 静音持续时间（秒）
 * @return 成功返回0，失败返回-1
 */
int monitoring_silence_alert(monitoring_manager_t *manager, uint64_t alert_id,
                            uint32_t duration_seconds);

/**
 * 更新指标值
 * @param manager 监控管理器指针
 * @param type 指标类型
 * @param value 指标值
 * @return 成功返回0，失败返回-1
 */
int monitoring_update_metric(monitoring_manager_t *manager, metric_type_t type,
                            double value);

/**
 * 获取指标信息
 * @param manager 监控管理器指针
 * @param type 指标类型
 * @param metric 存储指标信息的位置
 * @return 成功返回0，失败返回-1
 */
int monitoring_get_metric(monitoring_manager_t *manager, metric_type_t type,
                         metric_t *metric);

/**
 * 设置指标阈值
 * @param manager 监控管理器指针
 * @param type 指标类型
 * @param warning 警告阈值
 * @param error 错误阈值
 * @return 成功返回0，失败返回-1
 */
int monitoring_set_threshold(monitoring_manager_t *manager, metric_type_t type,
                           double warning, double error);

/**
 * 注册告警回调函数
 * @param manager 监控管理器指针
 * @param callback 回调函数
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int monitoring_register_alert_callback(monitoring_manager_t *manager,
                                      alert_callback_t callback, void *user_data);

/**
 * 注册指标更新回调函数
 * @param manager 监控管理器指针
 * @param callback 回调函数
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int monitoring_register_metric_callback(monitoring_manager_t *manager,
                                       metric_update_callback_t callback, 
                                       void *user_data);

/**
 * 注册健康检查回调函数
 * @param manager 监控管理器指针
 * @param callback 回调函数
 * @param user_data 用户数据
 * @return 成功返回0，失败返回-1
 */
int monitoring_register_health_check_callback(monitoring_manager_t *manager,
                                             health_check_callback_t callback,
                                             void *user_data);

/**
 * 设置WebSocket广播回调函数
 * @param manager 监控管理器指针
 * @param callback 回调函数
 * @param user_data 用户数据
 */
void monitoring_set_ws_broadcast_callback(monitoring_manager_t *manager,
                                         void (*callback)(const uint8_t *data, size_t length),
                                         void *user_data);

/**
 * 通过WebSocket广播快照
 * @param manager 监控管理器指针
 * @return 成功返回0，失败返回-1
 */
int monitoring_ws_broadcast_snapshot(monitoring_manager_t *manager);

/**
 * 通过WebSocket发送告警
 * @param manager 监控管理器指针
 * @param alert 告警指针
 * @return 成功返回0，失败返回-1
 */
int monitoring_ws_send_alert(monitoring_manager_t *manager, const alert_t *alert);

/**
 * 通过WebSocket发送指标更新
 * @param manager 监控管理器指针
 * @param type 指标类型
 * @param value 指标值
 * @return 成功返回0，失败返回-1
 */
int monitoring_ws_send_metric_update(monitoring_manager_t *manager,
                                    metric_type_t type, double value);

// ==================== 工具函数 ====================

/**
 * 将指标类型转换为字符串
 * @param type 指标类型
 * @return 指标类型字符串
 */
const char* metric_type_to_string(metric_type_t type);

/**
 * 将告警级别转换为字符串
 * @param level 告警级别
 * @return 告警级别字符串
 */
const char* alert_level_to_string(alert_level_t level);

/**
 * 将告警状态转换为字符串
 * @param status 告警状态
 * @return 告警状态字符串
 */
const char* alert_status_to_string(alert_status_t status);

/**
 * 获取系统主机名
 * @return 主机名字符串
 */
const char* get_system_hostname(void);

/**
 * 获取系统运行时间
 * @return 运行时间（秒）
 */
uint64_t get_system_uptime(void);

#ifdef __cplusplus
}
#endif

#endif // SECURE_CHAT_MONITORING_H