#ifndef SECURE_CHAT_CONFIG_H
#define SECURE_CHAT_CONFIG_H

#include "protocol.h"
#include "utils.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 配置常量 ====================
#define CONFIG_MAX_PATH_LEN 256
#define CONFIG_MAX_SECTION_LEN 64
#define CONFIG_MAX_KEY_LEN 64
#define CONFIG_MAX_VALUE_LEN 1024

// ==================== 配置项类型 ====================
typedef enum {
    CONFIG_TYPE_STRING = 0,
    CONFIG_TYPE_INT = 1,
    CONFIG_TYPE_BOOL = 2,
    CONFIG_TYPE_FLOAT = 3,
    CONFIG_TYPE_STRING_ARRAY = 4,
    CONFIG_TYPE_INT_ARRAY = 5
} config_value_type_t;

// ==================== 配置项验证回调 ====================
typedef bool (*config_validate_callback_t)(const char *value, void *user_data);

// ==================== 配置项结构 ====================
typedef struct {
    char section[CONFIG_MAX_SECTION_LEN];
    char key[CONFIG_MAX_KEY_LEN];
    config_value_type_t type;
    char *string_value;
    int int_value;
    bool bool_value;
    float float_value;
    char **string_array;
    int *int_array;
    size_t array_size;
    
    char *default_value;
    char *description;
    config_validate_callback_t validate_callback;
    void *validate_user_data;
    
    bool required;
    bool modified;
    time_t last_modified;
} config_item_t;

// ==================== 配置段结构 ====================
typedef struct {
    char name[CONFIG_MAX_SECTION_LEN];
    config_item_t **items;
    size_t item_count;
    size_t item_capacity;
} config_section_t;

// ==================== 配置管理器结构 ====================
typedef struct {
    char config_path[CONFIG_MAX_PATH_LEN];
    config_section_t **sections;
    size_t section_count;
    size_t section_capacity;
    
    // 回调函数
    void (*on_config_changed)(const char *section, const char *key, void *user_data);
    void *callback_user_data;
    
    // 锁
    pthread_mutex_t mutex;
    
    // 统计
    uint64_t load_count;
    uint64_t save_count;
    uint64_t reload_count;
    time_t last_load_time;
    time_t last_save_time;
} config_manager_t;

// ==================== 默认配置 ====================
typedef struct {
    // 服务器配置
    uint16_t server_port;
    uint16_t websocket_port;
    uint32_t max_clients;
    uint32_t max_message_size;
    uint32_t max_file_size;
    uint32_t connection_timeout;
    
    // 数据库配置
    char database_path[CONFIG_MAX_PATH_LEN];
    uint32_t database_cache_size;
    uint32_t database_max_connections;
    
    // 安全配置
    bool enable_ssl;
    char ssl_cert_path[CONFIG_MAX_PATH_LEN];
    char ssl_key_path[CONFIG_MAX_PATH_LEN];
    bool require_authentication;
    uint32_t password_min_length;
    uint32_t max_login_attempts;
    uint32_t lockout_duration;
    
    // 文件传输配置
    char upload_dir[CONFIG_MAX_PATH_LEN];
    char download_dir[CONFIG_MAX_PATH_LEN];
    uint32_t max_upload_size;
    uint32_t chunk_size;
    bool enable_resume;
    uint32_t max_concurrent_transfers;
    
    // 日志配置
    char log_file[CONFIG_MAX_PATH_LEN];
    uint32_t log_level;
    uint32_t log_max_size;
    uint32_t log_max_files;
    bool log_to_console;
    bool log_to_file;
    
    // 监控配置
    uint32_t monitoring_interval;
    uint32_t monitoring_history_size;
    bool enable_monitoring;
    float cpu_warning_threshold;
    float cpu_critical_threshold;
    float memory_warning_threshold;
    float memory_critical_threshold;
    
    // 性能配置
    uint32_t thread_pool_size;
    uint32_t thread_pool_max_size;
    uint32_t thread_pool_queue_size;
    uint32_t cache_size;
    uint32_t max_cache_entries;
    uint32_t cache_ttl;
} default_config_t;

// ==================== 函数声明 ====================

// 配置管理器创建和销毁
config_manager_t* config_manager_create(const char *config_path);
void config_manager_destroy(config_manager_t *manager);

// 配置加载和保存
int config_load(config_manager_t *manager);
int config_save(config_manager_t *manager);
int config_reload(config_manager_t *manager);
int config_reset_to_defaults(config_manager_t *manager);

// 配置项注册
int config_register_string(config_manager_t *manager, const char *section, const char *key,
                          const char *default_value, const char *description,
                          config_validate_callback_t validate, void *user_data);
int config_register_int(config_manager_t *manager, const char *section, const char *key,
                       int default_value, const char *description,
                       config_validate_callback_t validate, void *user_data);
int config_register_bool(config_manager_t *manager, const char *section, const char *key,
                        bool default_value, const char *description,
                        config_validate_callback_t validate, void *user_data);
int config_register_float(config_manager_t *manager, const char *section, const char *key,
                         float default_value, const char *description,
                         config_validate_callback_t validate, void *user_data);

// 配置值获取
const char* config_get_string(config_manager_t *manager, const char *section, const char *key);
int config_get_int(config_manager_t *manager, const char *section, const char *key);
bool config_get_bool(config_manager_t *manager, const char *section, const char *key);
float config_get_float(config_manager_t *manager, const char *section, const char *key);

// 配置值设置
int config_set_string(config_manager_t *manager, const char *section, const char *key,
                     const char *value);
int config_set_int(config_manager_t *manager, const char *section, const char *key,
                  int value);
int config_set_bool(config_manager_t *manager, const char *section, const char *key,
                   bool value);
int config_set_float(config_manager_t *manager, const char *section, const char *key,
                    float value);

// 配置验证
int config_validate(config_manager_t *manager, char **error_message);
bool config_is_modified(config_manager_t *manager);

// 回调设置
void config_set_change_callback(config_manager_t *manager,
                               void (*callback)(const char *section, const char *key, void *user_data),
                               void *user_data);

// 工具函数
config_item_t* config_find_item(config_manager_t *manager, const char *section, const char *key);
bool config_section_exists(config_manager_t *manager, const char *section);
bool config_key_exists(config_manager_t *manager, const char *section, const char *key);
int config_remove_key(config_manager_t *manager, const char *section, const char *key);
int config_remove_section(config_manager_t *manager, const char *section);

// 默认配置获取
default_config_t config_get_defaults(void);
int config_apply_defaults(config_manager_t *manager);

// 批量操作
int config_import_from_json(config_manager_t *manager, const char *json_data);
char* config_export_to_json(config_manager_t *manager);

// 环境变量支持
int config_load_from_environment(config_manager_t *manager, const char *prefix);

// 热重载
int config_enable_auto_reload(config_manager_t *manager, uint32_t interval_seconds);
int config_disable_auto_reload(config_manager_t *manager);

// 统计信息
void config_get_statistics(config_manager_t *manager, uint64_t *load_count,
                          uint64_t *save_count, uint64_t *reload_count,
                          time_t *last_load_time, time_t *last_save_time);

// 验证函数
bool config_validate_port(const char *value, void *user_data);
bool config_validate_path(const char *value, void *user_data);
bool config_validate_ip_address(const char *value, void *user_data);
bool config_validate_email(const char *value, void *user_data);
bool config_validate_url(const char *value, void *user_data);

#ifdef __cplusplus
}
#endif

#endif // SECURE_CHAT_CONFIG_H