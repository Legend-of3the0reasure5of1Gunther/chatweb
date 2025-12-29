#include "monitoring.h"
#include "protocol.h"
#include "threadpool.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <jansson.h>

// ==================== 内部宏定义 ====================
#define MONITORING_LOCK(mutex) pthread_mutex_lock(&(mutex))
#define MONITORING_UNLOCK(mutex) pthread_mutex_unlock(&(mutex))
#define MONITORING_COND_WAIT(cond, mutex) pthread_cond_wait(&(cond), &(mutex))
#define MONITORING_COND_SIGNAL(cond) pthread_cond_signal(&(cond))

#define MONITORING_MIN(a, b) ((a) < (b) ? (a) : (b))
#define MONITORING_MAX(a, b) ((a) > (b) ? (a) : (b))

#define MONITORING_CHECK_NULL(ptr) if ((ptr) == NULL) return -1
#define MONITORING_CHECK_MANAGER(manager) if ((manager) == NULL) return -1

#define MONITORING_LOG_INFO(fmt, ...) \
    printf("[Monitoring Info] " fmt "\n", ##__VA_ARGS__)
#define MONITORING_LOG_ERROR(fmt, ...) \
    fprintf(stderr, "[Monitoring Error] %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#define MONITORING_LOG_WARNING(fmt, ...) \
    printf("[Monitoring Warning] " fmt "\n", ##__VA_ARGS__)

// ==================== 内部结构定义 ====================
typedef struct {
    metric_data_point_t *points;    // 数据点数组
    uint32_t capacity;              // 数组容量
    uint32_t count;                 // 当前数量
    uint32_t start_index;           // 起始索引
    uint32_t end_index;             // 结束索引
} metric_history_t;

typedef struct {
    alert_t *alerts;                // 告警数组
    uint32_t capacity;              // 数组容量
    uint32_t count;                 // 当前数量
    uint64_t next_alert_id;         // 下一个告警ID
} alert_store_t;

typedef struct alert_callback_entry {
    alert_callback_t callback;      // 回调函数
    void *user_data;                // 用户数据
    struct alert_callback_entry *next;
} alert_callback_entry_t;

typedef struct metric_callback_entry {
    metric_update_callback_t callback; // 回调函数
    void *user_data;                  // 用户数据
    struct metric_callback_entry *next;
} metric_callback_entry_t;

typedef struct health_callback_entry {
    health_check_callback_t callback; // 回调函数
    void *user_data;                  // 用户数据
    struct health_callback_entry *next;
} health_callback_entry_t;

struct monitoring_manager {
    // 配置
    monitoring_config_t config;      // 监控配置
    
    // 状态
    volatile bool running;           // 运行状态
    volatile bool shutdown_requested; // 关闭请求
    
    // 数据存储
    metric_t metrics[MONITORING_MAX_METRICS]; // 指标数组
    metric_history_t *history[MONITORING_MAX_METRICS]; // 历史数据
    alert_store_t alert_store;       // 告警存储
    monitoring_snapshot_t *snapshots; // 快照数组
    uint32_t snapshot_capacity;      // 快照容量
    uint32_t snapshot_count;         // 快照数量
    uint32_t snapshot_index;         // 快照索引
    
    // 回调函数
    alert_callback_entry_t *alert_callbacks;
    metric_callback_entry_t *metric_callbacks;
    health_callback_entry_t *health_callbacks;
    
    // 线程管理
    pthread_t collection_thread;     // 数据收集线程
    pthread_t alert_thread;          // 告警处理线程
    pthread_mutex_t data_mutex;      // 数据互斥锁
    pthread_mutex_t alert_mutex;     // 告警互斥锁
    pthread_cond_t data_cond;        // 数据条件变量
    pthread_cond_t alert_cond;       // 告警条件变量
    
    // 统计信息
    uint64_t total_collections;      // 总收集次数
    uint64_t total_alerts_triggered; // 总触发告警数
    uint64_t start_time;             // 启动时间
    
    // WebSocket集成
    void (*ws_broadcast_callback)(const uint8_t *data, size_t length); // WebSocket广播回调
    void *ws_user_data;              // WebSocket用户数据
    
    // 文件句柄
    FILE *log_file;                  // 日志文件
};

// ==================== 静态函数声明 ====================
static void* collection_thread_function(void *arg);
static void* alert_thread_function(void *arg);
static int collect_system_resources(system_resources_t *resources);
static int collect_network_stats(uint64_t *rx_bytes, uint64_t *tx_bytes);
static int update_metric_history(monitoring_manager_t *manager, 
                                metric_type_t type, double value);
static int check_metric_thresholds(monitoring_manager_t *manager, 
                                  metric_type_t type, double value);
static int add_snapshot(monitoring_manager_t *manager, 
                       const monitoring_snapshot_t *snapshot);
static void trigger_alert_callbacks(monitoring_manager_t *manager, 
                                   const alert_t *alert);
static void trigger_metric_callbacks(monitoring_manager_t *manager,
                                    metric_type_t type, double value);
static void trigger_health_callbacks(monitoring_manager_t *manager,
                                    bool healthy, const char *message);
static int write_log_entry(monitoring_manager_t *manager, 
                          const char *level, const char *message);

// ==================== 工具函数实现 ====================

/**
 * 获取当前时间戳 (毫秒)
 */
static uint64_t get_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/**
 * 获取系统主机名
 */
const char* get_system_hostname(void) {
    static char hostname[256] = {0};
    if (hostname[0] == '\0') {
        gethostname(hostname, sizeof(hostname));
    }
    return hostname;
}

/**
 * 获取系统运行时间
 */
uint64_t get_system_uptime(void) {
    struct sysinfo info;
    if (sysinfo(&info) == 0) {
        return info.uptime;
    }
    return 0;
}

/**
 * 将指标类型转换为字符串
 */
const char* metric_type_to_string(metric_type_t type) {
    static const char* names[] = {
        "CPU_USAGE",
        "MEMORY_USAGE",
        "DISK_USAGE",
        "NETWORK_RX",
        "NETWORK_TX",
        "CONNECTIONS",
        "MESSAGES_PER_SECOND",
        "ACTIVE_THREADS",
        "QUEUE_LENGTH",
        "RESPONSE_TIME",
        "ERROR_RATE",
        "THROUGHPUT",
        "UPTIME",
        "FILE_TRANSFERS",
        "SSL_HANDSHAKES",
        "CACHE_HITS"
    };
    
    if (type < 0 || type >= sizeof(names) / sizeof(names[0])) {
        return "UNKNOWN";
    }
    return names[type];
}

/**
 * 将告警级别转换为字符串
 */
const char* alert_level_to_string(alert_level_t level) {
    static const char* names[] = {
        "INFO",
        "WARNING",
        "ERROR",
        "CRITICAL",
        "EMERGENCY"
    };
    
    if (level < 0 || level >= sizeof(names) / sizeof(names[0])) {
        return "UNKNOWN";
    }
    return names[level];
}

/**
 * 将告警状态转换为字符串
 */
const char* alert_status_to_string(alert_status_t status) {
    static const char* names[] = {
        "ACTIVE",
        "ACKNOWLEDGED",
        "RESOLVED",
        "SILENCED"
    };
    
    if (status < 0 || status >= sizeof(names) / sizeof(names[0])) {
        return "UNKNOWN";
    }
    return names[status];
}

/**
 * 写入日志条目
 */
static int write_log_entry(monitoring_manager_t *manager, 
                          const char *level, const char *message) {
    if (!manager->config.enable_logging || !manager->log_file) {
        return 0;
    }
    
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_now);
    
    fprintf(manager->log_file, "[%s] %s: %s\n", timestamp, level, message);
    fflush(manager->log_file);
    
    return 0;
}

// ==================== 监控管理器实现 ====================

/**
 * 获取默认配置
 */
monitoring_config_t monitoring_get_default_config(void) {
    monitoring_config_t config = {
        .collection_interval_ms = MONITORING_INTERVAL_MS,
        .history_retention_seconds = MONITORING_HISTORY_SIZE,
        .enable_system_monitoring = true,
        .enable_network_monitoring = true,
        .enable_performance_monitoring = true,
        .enable_business_monitoring = true,
        .enable_alerting = true,
        .alert_email = "",
        .alert_webhook = "",
        .warning_threshold_percent = 80.0,
        .error_threshold_percent = 90.0,
        .alert_cooldown_seconds = 300, // 5分钟冷却时间
        .enable_logging = true,
        .log_file_path = "/var/log/secure_chat_monitoring.log"
    };
    return config;
}

/**
 * 创建监控管理器
 */
monitoring_manager_t* monitoring_manager_create(const monitoring_config_t *config) {
    monitoring_manager_t *manager = (monitoring_manager_t*)calloc(1, sizeof(monitoring_manager_t));
    if (!manager) {
        MONITORING_LOG_ERROR("Failed to allocate memory for monitoring manager");
        return NULL;
    }
    
    // 设置配置
    if (config) {
        manager->config = *config;
    } else {
        manager->config = monitoring_get_default_config();
    }
    
    // 初始化互斥锁和条件变量
    if (pthread_mutex_init(&manager->data_mutex, NULL) != 0 ||
        pthread_mutex_init(&manager->alert_mutex, NULL) != 0 ||
        pthread_cond_init(&manager->data_cond, NULL) != 0 ||
        pthread_cond_init(&manager->alert_cond, NULL) != 0) {
        MONITORING_LOG_ERROR("Failed to initialize mutexes and condition variables");
        free(manager);
        return NULL;
    }
    
    // 初始化告警存储
    manager->alert_store.capacity = MONITORING_MAX_ALERTS;
    manager->alert_store.count = 0;
    manager->alert_store.next_alert_id = 1;
    manager->alert_store.alerts = (alert_t*)calloc(MONITORING_MAX_ALERTS, sizeof(alert_t));
    if (!manager->alert_store.alerts) {
        MONITORING_LOG_ERROR("Failed to allocate memory for alerts");
        monitoring_manager_destroy(manager);
        return NULL;
    }
    
    // 初始化快照存储
    manager->snapshot_capacity = MONITORING_HISTORY_SIZE;
    manager->snapshot_count = 0;
    manager->snapshot_index = 0;
    manager->snapshots = (monitoring_snapshot_t*)calloc(
        manager->snapshot_capacity, sizeof(monitoring_snapshot_t));
    if (!manager->snapshots) {
        MONITORING_LOG_ERROR("Failed to allocate memory for snapshots");
        monitoring_manager_destroy(manager);
        return NULL;
    }
    
    // 初始化历史数据存储
    for (int i = 0; i < MONITORING_MAX_METRICS; i++) {
        manager->history[i] = (metric_history_t*)calloc(1, sizeof(metric_history_t));
        if (!manager->history[i]) {
            MONITORING_LOG_ERROR("Failed to allocate memory for metric history");
            monitoring_manager_destroy(manager);
            return NULL;
        }
        
        manager->history[i]->capacity = MONITORING_HISTORY_SIZE;
        manager->history[i]->count = 0;
        manager->history[i]->start_index = 0;
        manager->history[i]->end_index = 0;
        manager->history[i]->points = (metric_data_point_t*)calloc(
            MONITORING_HISTORY_SIZE, sizeof(metric_data_point_t));
        
        if (!manager->history[i]->points) {
            MONITORING_LOG_ERROR("Failed to allocate memory for metric points");
            monitoring_manager_destroy(manager);
            return NULL;
        }
    }
    
    // 初始化指标
    for (int i = 0; i < MONITORING_MAX_METRICS; i++) {
        manager->metrics[i].type = i;
        snprintf(manager->metrics[i].name, sizeof(manager->metrics[i].name),
                "%s", metric_type_to_string(i));
        manager->metrics[i].enabled = true;
        manager->metrics[i].warning_threshold = manager->config.warning_threshold_percent;
        manager->metrics[i].error_threshold = manager->config.error_threshold_percent;
        
        // 设置单位和描述
        switch (i) {
            case METRIC_CPU_USAGE:
                snprintf(manager->metrics[i].unit, sizeof(manager->metrics[i].unit), "%");
                snprintf(manager->metrics[i].description, sizeof(manager->metrics[i].description),
                        "CPU使用率");
                break;
            case METRIC_MEMORY_USAGE:
                snprintf(manager->metrics[i].unit, sizeof(manager->metrics[i].unit), "%");
                snprintf(manager->metrics[i].description, sizeof(manager->metrics[i].description),
                        "内存使用率");
                break;
            case METRIC_DISK_USAGE:
                snprintf(manager->metrics[i].unit, sizeof(manager->metrics[i].unit), "%");
                snprintf(manager->metrics[i].description, sizeof(manager->metrics[i].description),
                        "磁盘使用率");
                break;
            case METRIC_NETWORK_RX:
                snprintf(manager->metrics[i].unit, sizeof(manager->metrics[i].unit), "B/s");
                snprintf(manager->metrics[i].description, sizeof(manager->metrics[i].description),
                        "网络接收速率");
                break;
            case METRIC_NETWORK_TX:
                snprintf(manager->metrics[i].unit, sizeof(manager->metrics[i].unit), "B/s");
                snprintf(manager->metrics[i].description, sizeof(manager->metrics[i].description),
                        "网络发送速率");
                break;
            case METRIC_CONNECTIONS:
                snprintf(manager->metrics[i].unit, sizeof(manager->metrics[i].unit), "");
                snprintf(manager->metrics[i].description, sizeof(manager->metrics[i].description),
                        "活跃连接数");
                break;
            case METRIC_MESSAGES_PER_SECOND:
                snprintf(manager->metrics[i].unit, sizeof(manager->metrics[i].unit), "msg/s");
                snprintf(manager->metrics[i].description, sizeof(manager->metrics[i].description),
                        "每秒消息数");
                break;
            case METRIC_ACTIVE_THREADS:
                snprintf(manager->metrics[i].unit, sizeof(manager->metrics[i].unit), "");
                snprintf(manager->metrics[i].description, sizeof(manager->metrics[i].description),
                        "活跃线程数");
                break;
            case METRIC_QUEUE_LENGTH:
                snprintf(manager->metrics[i].unit, sizeof(manager->metrics[i].unit), "");
                snprintf(manager->metrics[i].description, sizeof(manager->metrics[i].description),
                        "队列长度");
                break;
            case METRIC_RESPONSE_TIME:
                snprintf(manager->metrics[i].unit, sizeof(manager->metrics[i].unit), "ms");
                snprintf(manager->metrics[i].description, sizeof(manager->metrics[i].description),
                        "响应时间");
                break;
            case METRIC_ERROR_RATE:
                snprintf(manager->metrics[i].unit, sizeof(manager->metrics[i].unit), "%");
                snprintf(manager->metrics[i].description, sizeof(manager->metrics[i].description),
                        "错误率");
                break;
            case METRIC_THROUGHPUT:
                snprintf(manager->metrics[i].unit, sizeof(manager->metrics[i].unit), "req/s");
                snprintf(manager->metrics[i].description, sizeof(manager->metrics[i].description),
                        "吞吐量");
                break;
            case METRIC_UPTIME:
                snprintf(manager->metrics[i].unit, sizeof(manager->metrics[i].unit), "s");
                snprintf(manager->metrics[i].description, sizeof(manager->metrics[i].description),
                        "运行时间");
                break;
            case METRIC_FILE_TRANSFERS:
                snprintf(manager->metrics[i].unit, sizeof(manager->metrics[i].unit), "");
                snprintf(manager->metrics[i].description, sizeof(manager->metrics[i].description),
                        "文件传输数");
                break;
            case METRIC_SSL_HANDSHAKES:
                snprintf(manager->metrics[i].unit, sizeof(manager->metrics[i].unit), "");
                snprintf(manager->metrics[i].description, sizeof(manager->metrics[i].description),
                        "SSL握手数");
                break;
            case METRIC_CACHE_HITS:
                snprintf(manager->metrics[i].unit, sizeof(manager->metrics[i].unit), "");
                snprintf(manager->metrics[i].description, sizeof(manager->metrics[i].description),
                        "缓存命中数");
                break;
        }
    }
    
    // 打开日志文件
    if (manager->config.enable_logging) {
        manager->log_file = fopen(manager->config.log_file_path, "a");
        if (!manager->log_file) {
            MONITORING_LOG_WARNING("Failed to open log file: %s", manager->config.log_file_path);
        }
    }
    
    manager->running = false;
    manager->shutdown_requested = false;
    manager->start_time = get_timestamp_ms();
    manager->total_collections = 0;
    manager->total_alerts_triggered = 0;
    
    MONITORING_LOG_INFO("Monitoring manager created successfully");
    write_log_entry(manager, "INFO", "Monitoring manager initialized");
    
    return manager;
}

/**
 * 销毁监控管理器
 */
void monitoring_manager_destroy(monitoring_manager_t *manager) {
    if (!manager) return;
    
    // 停止监控
    monitoring_stop(manager);
    
    // 等待线程结束
    if (manager->collection_thread) {
        pthread_join(manager->collection_thread, NULL);
    }
    if (manager->alert_thread) {
        pthread_join(manager->alert_thread, NULL);
    }
    
    // 清理回调函数
    alert_callback_entry_t *alert_entry = manager->alert_callbacks;
    while (alert_entry) {
        alert_callback_entry_t *next = alert_entry->next;
        free(alert_entry);
        alert_entry = next;
    }
    
    metric_callback_entry_t *metric_entry = manager->metric_callbacks;
    while (metric_entry) {
        metric_callback_entry_t *next = metric_entry->next;
        free(metric_entry);
        metric_entry = next;
    }
    
    health_callback_entry_t *health_entry = manager->health_callbacks;
    while (health_entry) {
        health_callback_entry_t *next = health_entry->next;
        free(health_entry);
        health_entry = next;
    }
    
    // 清理历史数据
    for (int i = 0; i < MONITORING_MAX_METRICS; i++) {
        if (manager->history[i]) {
            if (manager->history[i]->points) {
                free(manager->history[i]->points);
            }
            free(manager->history[i]);
        }
    }
    
    // 清理告警存储
    if (manager->alert_store.alerts) {
        free(manager->alert_store.alerts);
    }
    
    // 清理快照存储
    if (manager->snapshots) {
        free(manager->snapshots);
    }
    
    // 关闭日志文件
    if (manager->log_file) {
        fclose(manager->log_file);
    }
    
    // 销毁互斥锁和条件变量
    pthread_mutex_destroy(&manager->data_mutex);
    pthread_mutex_destroy(&manager->alert_mutex);
    pthread_cond_destroy(&manager->data_cond);
    pthread_cond_destroy(&manager->alert_cond);
    
    // 释放管理器
    write_log_entry(manager, "INFO", "Monitoring manager destroyed");
    free(manager);
}

/**
 * 启动监控
 */
int monitoring_start(monitoring_manager_t *manager) {
    MONITORING_CHECK_MANAGER(manager);
    
    if (manager->running) {
        MONITORING_LOG_WARNING("Monitoring is already running");
        return -1;
    }
    
    manager->running = true;
    manager->shutdown_requested = false;
    
    // 创建数据收集线程
    if (pthread_create(&manager->collection_thread, NULL, 
                      collection_thread_function, manager) != 0) {
        MONITORING_LOG_ERROR("Failed to create collection thread");
        manager->running = false;
        return -1;
    }
    
    // 创建告警处理线程
    if (pthread_create(&manager->alert_thread, NULL,
                      alert_thread_function, manager) != 0) {
        MONITORING_LOG_ERROR("Failed to create alert thread");
        manager->running = false;
        pthread_cancel(manager->collection_thread);
        return -1;
    }
    
    MONITORING_LOG_INFO("Monitoring started successfully");
    write_log_entry(manager, "INFO", "Monitoring started");
    
    return 0;
}

/**
 * 停止监控
 */
int monitoring_stop(monitoring_manager_t *manager) {
    MONITORING_CHECK_MANAGER(manager);
    
    if (!manager->running) {
        return 0;
    }
    
    manager->shutdown_requested = true;
    manager->running = false;
    
    // 通知线程退出
    MONITORING_COND_SIGNAL(manager->data_cond);
    MONITORING_COND_SIGNAL(manager->alert_cond);
    
    MONITORING_LOG_INFO("Monitoring stopped");
    write_log_entry(manager, "INFO", "Monitoring stopped");
    
    return 0;
}

/**
 * 检查监控是否运行
 */
bool monitoring_is_running(const monitoring_manager_t *manager) {
    if (!manager) return false;
    return manager->running;
}

// ==================== 数据收集线程 ====================

/**
 * 收集系统资源信息
 */
static int collect_system_resources(system_resources_t *resources) {
    if (!resources) return -1;
    
    memset(resources, 0, sizeof(system_resources_t));
    
    // 获取系统运行时间
    resources->uptime = get_system_uptime();
    
    // 获取内存信息
    struct sysinfo info;
    if (sysinfo(&info) == 0) {
        resources->memory_total = info.totalram * info.mem_unit;
        resources->memory_free = info.freeram * info.mem_unit;
        resources->memory_used = resources->memory_total - resources->memory_free;
        
        if (resources->memory_total > 0) {
            resources->memory_usage = ((double)resources->memory_used / 
                                       resources->memory_total) * 100.0;
        }
        
        resources->memory_cached = info.bufferram * info.mem_unit;
    }
    
    // 获取CPU使用率 (简单实现)
    static uint64_t last_total = 0;
    static uint64_t last_idle = 0;
    
    FILE *stat = fopen("/proc/stat", "r");
    if (stat) {
        char line[256];
        fgets(line, sizeof(line), stat);
        fclose(stat);
        
        uint64_t user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
        sscanf(line, "cpu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal, &guest, &guest_nice);
        
        uint64_t total = user + nice + system + idle + iowait + irq + softirq + steal;
        uint64_t idle_time = idle;
        
        if (last_total > 0 && last_idle > 0) {
            uint64_t total_diff = total - last_total;
            uint64_t idle_diff = idle_time - last_idle;
            
            if (total_diff > 0) {
                resources->cpu_usage = ((double)(total_diff - idle_diff) / total_diff) * 100.0;
            }
        }
        
        last_total = total;
        last_idle = idle_time;
    }
    
    // 获取磁盘信息
    struct statvfs disk_info;
    if (statvfs("/", &disk_info) == 0) {
        resources->disk_total = disk_info.f_blocks * disk_info.f_frsize;
        resources->disk_free = disk_info.f_bfree * disk_info.f_frsize;
        resources->disk_used = resources->disk_total - resources->disk_free;
        
        if (resources->disk_total > 0) {
            resources->disk_usage = ((double)resources->disk_used / 
                                     resources->disk_total) * 100.0;
        }
    }
    
    // 获取打开文件数和进程数
    resources->open_files = 0;
    resources->processes = 0;
    
    // 统计进程数
    DIR *proc_dir = opendir("/proc");
    if (proc_dir) {
        struct dirent *entry;
        while ((entry = readdir(proc_dir)) != NULL) {
            if (entry->d_type == DT_DIR) {
                char *endptr;
                long pid = strtol(entry->d_name, &endptr, 10);
                if (*endptr == '\0' && pid > 0) {
                    resources->processes++;
                }
            }
        }
        closedir(proc_dir);
    }
    
    // 获取线程数 (简化实现)
    FILE *status = fopen("/proc/self/status", "r");
    if (status) {
        char line[256];
        while (fgets(line, sizeof(line), status)) {
            if (strncmp(line, "Threads:", 8) == 0) {
                sscanf(line, "Threads: %u", &resources->threads);
                break;
            }
        }
        fclose(status);
    }
    
    return 0;
}

/**
 * 收集网络统计信息
 */
static int collect_network_stats(uint64_t *rx_bytes, uint64_t *tx_bytes) {
    static uint64_t last_rx_bytes = 0;
    static uint64_t last_tx_bytes = 0;
    static uint64_t last_collection_time = 0;
    
    uint64_t current_rx_bytes = 0;
    uint64_t current_tx_bytes = 0;
    
    // 读取/proc/net/dev文件获取网络统计
    FILE *net_dev = fopen("/proc/net/dev", "r");
    if (!net_dev) {
        return -1;
    }
    
    char line[512];
    // 跳过前两行标题
    fgets(line, sizeof(line), net_dev);
    fgets(line, sizeof(line), net_dev);
    
    while (fgets(line, sizeof(line), net_dev)) {
        char interface[32];
        uint64_t rx, tx;
        
        // 解析网络接口统计
        sscanf(line, "%s %lu %*u %*u %*u %*u %*u %*u %*u %lu", 
               interface, &rx, &tx);
        
        // 跳过回环接口
        if (strcmp(interface, "lo:") != 0) {
            current_rx_bytes += rx;
            current_tx_bytes += tx;
        }
    }
    
    fclose(net_dev);
    
    *rx_bytes = current_rx_bytes;
    *tx_bytes = current_tx_bytes;
    
    // 计算速率
    uint64_t current_time = get_timestamp_ms();
    if (last_collection_time > 0) {
        double time_diff = (current_time - last_collection_time) / 1000.0; // 转换为秒
        if (time_diff > 0) {
            // 速率计算在外部处理
        }
    }
    
    last_rx_bytes = current_rx_bytes;
    last_tx_bytes = current_tx_bytes;
    last_collection_time = current_time;
    
    return 0;
}

/**
 * 更新指标历史数据
 */
static int update_metric_history(monitoring_manager_t *manager, 
                                metric_type_t type, double value) {
    if (type < 0 || type >= MONITORING_MAX_METRICS) {
        return -1;
    }
    
    MONITORING_LOCK(manager->data_mutex);
    
    metric_history_t *history = manager->history[type];
    if (!history) {
        MONITORING_UNLOCK(manager->data_mutex);
        return -1;
    }
    
    uint64_t timestamp = get_timestamp_ms();
    
    // 找到相同时间戳的数据点或创建新的
    metric_data_point_t *point = NULL;
    
    // 检查是否有相同时间戳的数据点 (在1秒容差内)
    for (uint32_t i = 0; i < history->count; i++) {
        uint32_t idx = (history->start_index + i) % history->capacity;
        if (abs((int64_t)(timestamp - history->points[idx].timestamp)) < 1000) {
            point = &history->points[idx];
            break;
        }
    }
    
    if (!point) {
        // 创建新数据点
        if (history->count >= history->capacity) {
            // 覆盖最旧的数据
            history->start_index = (history->start_index + 1) % history->capacity;
            history->count--;
        }
        
        uint32_t idx = (history->start_index + history->count) % history->capacity;
        point = &history->points[idx];
        point->timestamp = timestamp;
        point->value = value;
        point->min_value = value;
        point->max_value = value;
        point->avg_value = value;
        point->sample_count = 1;
        
        history->count++;
    } else {
        // 更新现有数据点
        point->min_value = MONITORING_MIN(point->min_value, value);
        point->max_value = MONITORING_MAX(point->max_value, value);
        
        // 更新平均值
        double total = point->avg_value * point->sample_count;
        point->sample_count++;
        point->avg_value = (total + value) / point->sample_count;
        
        // 更新当前值
        point->value = value;
    }
    
    // 更新指标统计
    metric_t *metric = &manager->metrics[type];
    metric->last_value = metric->current_value;
    metric->current_value = value;
    
    if (metric->update_count == 0) {
        metric->min_value = value;
        metric->max_value = value;
        metric->avg_value = value;
    } else {
        metric->min_value = MONITORING_MIN(metric->min_value, value);
        metric->max_value = MONITORING_MAX(metric->max_value, value);
        
        // 更新滚动平均值
        metric->avg_value = (metric->avg_value * metric->update_count + value) / 
                           (metric->update_count + 1);
    }
    
    metric->update_count++;
    metric->last_update = timestamp;
    
    // 判断趋势
    if (metric->update_count > 1) {
        if (value > metric->last_value + (metric->last_value * 0.05)) {
            metric->trend = 1; // 上升
        } else if (value < metric->last_value - (metric->last_value * 0.05)) {
            metric->trend = 2; // 下降
        } else {
            metric->trend = 0; // 稳定
        }
    }
    
    MONITORING_UNLOCK(manager->data_mutex);
    
    return 0;
}

/**
 * 检查指标阈值并触发告警
 */
static int check_metric_thresholds(monitoring_manager_t *manager, 
                                  metric_type_t type, double value) {
    if (!manager->config.enable_alerting) {
        return 0;
    }
    
    metric_t *metric = &manager->metrics[type];
    
    alert_level_t level = ALERT_LEVEL_INFO;
    bool should_alert = false;
    char title[128];
    char message[512];
    
    // 检查错误阈值
    if (metric->error_threshold > 0 && value >= metric->error_threshold) {
        level = ALERT_LEVEL_ERROR;
        should_alert = true;
        snprintf(title, sizeof(title), "%s超过错误阈值", metric->name);
        snprintf(message, sizeof(message), 
                "%s当前值: %.2f%s, 错误阈值: %.2f%s",
                metric->description, value, metric->unit,
                metric->error_threshold, metric->unit);
    }
    // 检查警告阈值
    else if (metric->warning_threshold > 0 && value >= metric->warning_threshold) {
        level = ALERT_LEVEL_WARNING;
        should_alert = true;
        snprintf(title, sizeof(title), "%s超过警告阈值", metric->name);
        snprintf(message, sizeof(message),
                "%s当前值: %.2f%s, 警告阈值: %.2f%s",
                metric->description, value, metric->unit,
                metric->warning_threshold, metric->unit);
    }
    // 检查过低的值 (某些指标)
    else if (value <= 0 && type != METRIC_ERROR_RATE) {
        level = ALERT_LEVEL_WARNING;
        should_alert = true;
        snprintf(title, sizeof(title), "%s异常低", metric->name);
        snprintf(message, sizeof(message),
                "%s当前值: %.2f%s, 预期正常范围",
                metric->description, value, metric->unit);
    }
    
    if (should_alert) {
        // 检查是否已经有相同类型的活跃告警
        bool has_active_alert = false;
        
        MONITORING_LOCK(manager->alert_mutex);
        for (uint32_t i = 0; i < manager->alert_store.count; i++) {
            alert_t *alert = &manager->alert_store.alerts[i];
            if (alert->metric_type == type && 
                alert->status == ALERT_STATUS_ACTIVE &&
                alert->level == level) {
                // 更新现有告警
                alert->actual_value = value;
                alert->trigger_time = get_timestamp_ms();
                has_active_alert = true;
                break;
            }
        }
        
        if (!has_active_alert) {
            // 添加新告警
            monitoring_add_alert(manager, level, type, title, message,
                               get_system_hostname(), 
                               (level == ALERT_LEVEL_ERROR) ? 
                               metric->error_threshold : metric->warning_threshold,
                               value);
        }
        MONITORING_UNLOCK(manager->alert_mutex);
    }
    
    return 0;
}

/**
 * 添加快照
 */
static int add_snapshot(monitoring_manager_t *manager, 
                       const monitoring_snapshot_t *snapshot) {
    if (!manager || !snapshot) return -1;
    
    MONITORING_LOCK(manager->data_mutex);
    
    uint32_t index = manager->snapshot_index;
    manager->snapshots[index] = *snapshot;
    manager->snapshot_index = (index + 1) % manager->snapshot_capacity;
    
    if (manager->snapshot_count < manager->snapshot_capacity) {
        manager->snapshot_count++;
    }
    
    MONITORING_UNLOCK(manager->data_mutex);
    
    return 0;
}

/**
 * 数据收集线程函数
 */
static void* collection_thread_function(void *arg) {
    monitoring_manager_t *manager = (monitoring_manager_t*)arg;
    
    MONITORING_LOG_INFO("Collection thread started");
    write_log_entry(manager, "INFO", "Collection thread started");
    
    uint64_t last_collection_time = get_timestamp_ms();
    uint64_t last_network_rx = 0;
    uint64_t last_network_tx = 0;
    
    while (manager->running && !manager->shutdown_requested) {
        uint64_t current_time = get_timestamp_ms();
        uint64_t elapsed = current_time - last_collection_time;
        
        if (elapsed < manager->config.collection_interval_ms) {
            // 等待下一个收集周期
            usleep((manager->config.collection_interval_ms - elapsed) * 1000);
            continue;
        }
        
        last_collection_time = current_time;
        
        // 收集系统资源
        system_resources_t resources;
        if (manager->config.enable_system_monitoring) {
            if (collect_system_resources(&resources) == 0) {
                // 更新CPU使用率指标
                update_metric_history(manager, METRIC_CPU_USAGE, resources.cpu_usage);
                check_metric_thresholds(manager, METRIC_CPU_USAGE, resources.cpu_usage);
                
                // 更新内存使用率指标
                update_metric_history(manager, METRIC_MEMORY_USAGE, resources.memory_usage);
                check_metric_thresholds(manager, METRIC_MEMORY_USAGE, resources.memory_usage);
                
                // 更新磁盘使用率指标
                update_metric_history(manager, METRIC_DISK_USAGE, resources.disk_usage);
                check_metric_thresholds(manager, METRIC_DISK_USAGE, resources.disk_usage);
                
                // 更新运行时间指标
                update_metric_history(manager, METRIC_UPTIME, (double)resources.uptime);
            }
        }
        
        // 收集网络统计
        if (manager->config.enable_network_monitoring) {
            uint64_t rx_bytes, tx_bytes;
            if (collect_network_stats(&rx_bytes, &tx_bytes) == 0) {
                // 计算网络速率
                double rx_rate = 0, tx_rate = 0;
                
                if (last_network_rx > 0 && last_network_tx > 0) {
                    double time_diff = elapsed / 1000.0; // 转换为秒
                    if (time_diff > 0) {
                        rx_rate = (rx_bytes - last_network_rx) / time_diff;
                        tx_rate = (tx_bytes - last_network_tx) / time_diff;
                    }
                }
                
                // 更新网络指标
                update_metric_history(manager, METRIC_NETWORK_RX, rx_rate);
                update_metric_history(manager, METRIC_NETWORK_TX, tx_rate);
                
                last_network_rx = rx_bytes;
                last_network_tx = tx_bytes;
            }
        }
        
        // 更新收集次数
        manager->total_collections++;
        
        // 触发指标更新回调
        for (int i = 0; i < MONITORING_MAX_METRICS; i++) {
            if (manager->metrics[i].enabled) {
                trigger_metric_callbacks(manager, i, manager->metrics[i].current_value);
            }
        }
        
        // 定期创建快照 (每分钟)
        static uint64_t last_snapshot_time = 0;
        if (current_time - last_snapshot_time >= 60000) {
            monitoring_snapshot_t snapshot;
            snapshot.timestamp = current_time;
            snapshot.resources = resources;
            
            // 这里可以添加其他数据的收集
            // snapshot.connections = ...;
            // snapshot.performance = ...;
            // snapshot.business = ...;
            
            add_snapshot(manager, &snapshot);
            
            // 通过WebSocket广播快照
            if (manager->ws_broadcast_callback) {
                monitoring_ws_broadcast_snapshot(manager);
            }
            
            last_snapshot_time = current_time;
        }
    }
    
    MONITORING_LOG_INFO("Collection thread stopped");
    write_log_entry(manager, "INFO", "Collection thread stopped");
    
    return NULL;
}

// ==================== 告警处理线程 ====================

/**
 * 触发告警回调
 */
static void trigger_alert_callbacks(monitoring_manager_t *manager, 
                                   const alert_t *alert) {
    alert_callback_entry_t *entry = manager->alert_callbacks;
    while (entry) {
        if (entry->callback) {
            entry->callback(alert, entry->user_data);
        }
        entry = entry->next;
    }
}

/**
 * 触发指标更新回调
 */
static void trigger_metric_callbacks(monitoring_manager_t *manager,
                                    metric_type_t type, double value) {
    metric_callback_entry_t *entry = manager->metric_callbacks;
    while (entry) {
        if (entry->callback) {
            entry->callback(type, value, entry->user_data);
        }
        entry = entry->next;
    }
}

/**
 * 触发健康检查回调
 */
static void trigger_health_callbacks(monitoring_manager_t *manager,
                                    bool healthy, const char *message) {
    health_callback_entry_t *entry = manager->health_callbacks;
    while (entry) {
        if (entry->callback) {
            entry->callback(healthy, message, entry->user_data);
        }
        entry = entry->next;
    }
}

/**
 * 告警处理线程函数
 */
static void* alert_thread_function(void *arg) {
    monitoring_manager_t *manager = (monitoring_manager_t*)arg;
    
    MONITORING_LOG_INFO("Alert thread started");
    write_log_entry(manager, "INFO", "Alert thread started");
    
    while (manager->running && !manager->shutdown_requested) {
        // 检查是否有需要处理的告警
        // 这里可以实现告警升级、通知等功能
        
        // 简单实现：每秒检查一次
        sleep(1);
        
        // 发送健康检查结果
        static uint64_t last_health_check = 0;
        uint64_t current_time = get_timestamp_ms();
        
        if (current_time - last_health_check >= 30000) { // 每30秒一次
            bool healthy = true;
            char health_message[256] = "系统运行正常";
            
            // 检查关键指标
            for (int i = 0; i < 5; i++) { // 检查前5个关键指标
                metric_t *metric = &manager->metrics[i];
                if (metric->current_value >= metric->error_threshold) {
                    healthy = false;
                    snprintf(health_message, sizeof(health_message),
                            "%s超过错误阈值", metric->name);
                    break;
                }
            }
            
            trigger_health_callbacks(manager, healthy, health_message);
            last_health_check = current_time;
        }
    }
    
    MONITORING_LOG_INFO("Alert thread stopped");
    write_log_entry(manager, "INFO", "Alert thread stopped");
    
    return NULL;
}

// ==================== 公共API实现 ====================

/**
 * 设置监控配置
 */
int monitoring_set_config(monitoring_manager_t *manager, const monitoring_config_t *config) {
    MONITORING_CHECK_MANAGER(manager);
    if (!config) return -1;
    
    MONITORING_LOCK(manager->data_mutex);
    manager->config = *config;
    MONITORING_UNLOCK(manager->data_mutex);
    
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "Monitoring configuration updated");
    write_log_entry(manager, "INFO", log_msg);
    
    return 0;
}

/**
 * 获取监控配置
 */
int monitoring_get_config(const monitoring_manager_t *manager, monitoring_config_t *config) {
    MONITORING_CHECK_MANAGER(manager);
    if (!config) return -1;
    
    MONITORING_LOCK(manager->data_mutex);
    *config = manager->config;
    MONITORING_UNLOCK(manager->data_mutex);
    
    return 0;
}

/**
 * 收集快照
 */
int monitoring_collect_snapshot(monitoring_manager_t *manager, monitoring_snapshot_t *snapshot) {
    MONITORING_CHECK_MANAGER(manager);
    if (!snapshot) return -1;
    
    snapshot->timestamp = get_timestamp_ms();
    
    // 收集系统资源
    if (collect_system_resources(&snapshot->resources) != 0) {
        return -1;
    }
    
    // 收集网络统计
    uint64_t rx_bytes, tx_bytes;
    if (collect_network_stats(&rx_bytes, &tx_bytes) == 0) {
        // 计算网络速率 (简化)
        snapshot->resources.network_rx_bytes = rx_bytes;
        snapshot->resources.network_tx_bytes = tx_bytes;
    }
    
    // 设置默认值
    snapshot->alert_count = manager->alert_store.count;
    
    // 查找最高级别的活跃告警
    alert_level_t highest = ALERT_LEVEL_INFO;
    for (uint32_t i = 0; i < manager->alert_store.count; i++) {
        if (manager->alert_store.alerts[i].status == ALERT_STATUS_ACTIVE &&
            manager->alert_store.alerts[i].level > highest) {
            highest = manager->alert_store.alerts[i].level;
        }
    }
    snapshot->highest_alert = highest;
    
    return 0;
}

/**
 * 获取历史数据
 */
int monitoring_get_history(monitoring_manager_t *manager, metric_type_t type, 
                          uint64_t start_time, uint64_t end_time,
                          metric_data_point_t **points, uint32_t *count) {
    MONITORING_CHECK_MANAGER(manager);
    if (!points || !count) return -1;
    if (type < 0 || type >= MONITORING_MAX_METRICS) return -1;
    
    MONITORING_LOCK(manager->data_mutex);
    
    metric_history_t *history = manager->history[type];
    if (!history || history->count == 0) {
        MONITORING_UNLOCK(manager->data_mutex);
        *points = NULL;
        *count = 0;
        return 0;
    }
    
    // 计算符合条件的点数
    uint32_t match_count = 0;
    for (uint32_t i = 0; i < history->count; i++) {
        uint32_t idx = (history->start_index + i) % history->capacity;
        metric_data_point_t *point = &history->points[idx];
        
        if (point->timestamp >= start_time && point->timestamp <= end_time) {
            match_count++;
        }
    }
    
    if (match_count == 0) {
        MONITORING_UNLOCK(manager->data_mutex);
        *points = NULL;
        *count = 0;
        return 0;
    }
    
    // 分配内存
    *points = (metric_data_point_t*)malloc(match_count * sizeof(metric_data_point_t));
    if (!*points) {
        MONITORING_UNLOCK(manager->data_mutex);
        return -1;
    }
    
    // 复制数据
    uint32_t dest_index = 0;
    for (uint32_t i = 0; i < history->count; i++) {
        uint32_t idx = (history->start_index + i) % history->capacity;
        metric_data_point_t *point = &history->points[idx];
        
        if (point->timestamp >= start_time && point->timestamp <= end_time) {
            (*points)[dest_index++] = *point;
        }
    }
    
    *count = match_count;
    
    MONITORING_UNLOCK(manager->data_mutex);
    
    return 0;
}

/**
 * 添加告警
 */
int monitoring_add_alert(monitoring_manager_t *manager, alert_level_t level,
                        metric_type_t metric_type, const char *title,
                        const char *message, const char *source,
                        double threshold, double actual_value) {
    MONITORING_CHECK_MANAGER(manager);
    if (!title || !message || !source) return -1;
    
    MONITORING_LOCK(manager->alert_mutex);
    
    // 检查冷却时间
    static uint64_t last_alert_time[MONITORING_MAX_METRICS] = {0};
    uint64_t current_time = get_timestamp_ms();
    
    if (last_alert_time[metric_type] > 0) {
        uint64_t elapsed = current_time - last_alert_time[metric_type];
        if (elapsed < manager->config.alert_cooldown_seconds * 1000) {
            MONITORING_UNLOCK(manager->alert_mutex);
            return -2; // 冷却时间内
        }
    }
    
    // 检查是否达到最大告警数
    if (manager->alert_store.count >= manager->alert_store.capacity) {
        // 删除最旧的告警
        for (uint32_t i = 1; i < manager->alert_store.count; i++) {
            manager->alert_store.alerts[i-1] = manager->alert_store.alerts[i];
        }
        manager->alert_store.count--;
    }
    
    // 添加新告警
    alert_t *alert = &manager->alert_store.alerts[manager->alert_store.count];
    alert->alert_id = manager->alert_store.next_alert_id++;
    alert->level = level;
    alert->status = ALERT_STATUS_ACTIVE;
    alert->metric_type = metric_type;
    strncpy(alert->title, title, sizeof(alert->title) - 1);
    strncpy(alert->message, message, sizeof(alert->message) - 1);
    strncpy(alert->source, source, sizeof(alert->source) - 1);
    alert->threshold = threshold;
    alert->actual_value = actual_value;
    alert->trigger_time = current_time;
    alert->acknowledge_time = 0;
    alert->resolve_time = 0;
    alert->acknowledged_by[0] = '\0';
    alert->resolve_by[0] = '\0';
    
    manager->alert_store.count++;
    manager->total_alerts_triggered++;
    
    // 更新最后告警时间
    last_alert_time[metric_type] = current_time;
    
    MONITORING_UNLOCK(manager->alert_mutex);
    
    // 记录日志
    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "Alert triggered: %s (Level: %s, Value: %.2f)",
             title, alert_level_to_string(level), actual_value);
    write_log_entry(manager, alert_level_to_string(level), log_msg);
    
    // 触发回调
    trigger_alert_callbacks(manager, alert);
    
    // 通过WebSocket发送告警
    if (manager->ws_broadcast_callback) {
        monitoring_ws_send_alert(manager, alert);
    }
    
    MONITORING_LOG_WARNING("Alert triggered: %s (Level: %s)", 
                          title, alert_level_to_string(level));
    
    return 0;
}

/**
 * 获取告警列表
 */
int monitoring_get_alerts(monitoring_manager_t *manager, alert_t **alerts, 
                         uint32_t *count, alert_status_t status_filter) {
    MONITORING_CHECK_MANAGER(manager);
    if (!alerts || !count) return -1;
    
    MONITORING_LOCK(manager->alert_mutex);
    
    // 计算符合条件的告警数
    uint32_t match_count = 0;
    for (uint32_t i = 0; i < manager->alert_store.count; i++) {
        if (status_filter == (uint32_t)-1 || 
            manager->alert_store.alerts[i].status == status_filter) {
            match_count++;
        }
    }
    
    if (match_count == 0) {
        MONITORING_UNLOCK(manager->alert_mutex);
        *alerts = NULL;
        *count = 0;
        return 0;
    }
    
    // 分配内存
    *alerts = (alert_t*)malloc(match_count * sizeof(alert_t));
    if (!*alerts) {
        MONITORING_UNLOCK(manager->alert_mutex);
        return -1;
    }
    
    // 复制数据
    uint32_t dest_index = 0;
    for (uint32_t i = 0; i < manager->alert_store.count; i++) {
        if (status_filter == (uint32_t)-1 || 
            manager->alert_store.alerts[i].status == status_filter) {
            (*alerts)[dest_index++] = manager->alert_store.alerts[i];
        }
    }
    
    *count = match_count;
    
    MONITORING_UNLOCK(manager->alert_mutex);
    
    return 0;
}

/**
 * 确认告警
 */
int monitoring_acknowledge_alert(monitoring_manager_t *manager, uint64_t alert_id,
                                const char *acknowledged_by) {
    MONITORING_CHECK_MANAGER(manager);
    if (!acknowledged_by) return -1;
    
    MONITORING_LOCK(manager->alert_mutex);
    
    for (uint32_t i = 0; i < manager->alert_store.count; i++) {
        if (manager->alert_store.alerts[i].alert_id == alert_id) {
            manager->alert_store.alerts[i].status = ALERT_STATUS_ACKNOWLEDGED;
            manager->alert_store.alerts[i].acknowledge_time = get_timestamp_ms();
            strncpy(manager->alert_store.alerts[i].acknowledged_by, 
                   acknowledged_by, 
                   sizeof(manager->alert_store.alerts[i].acknowledged_by) - 1);
            
            MONITORING_UNLOCK(manager->alert_mutex);
            
            char log_msg[256];
            snprintf(log_msg, sizeof(log_msg), 
                    "Alert %lu acknowledged by %s", alert_id, acknowledged_by);
            write_log_entry(manager, "INFO", log_msg);
            
            return 0;
        }
    }
    
    MONITORING_UNLOCK(manager->alert_mutex);
    
    return -1; // 未找到告警
}

/**
 * 解决告警
 */
int monitoring_resolve_alert(monitoring_manager_t *manager, uint64_t alert_id,
                            const char *resolved_by) {
    MONITORING_CHECK_MANAGER(manager);
    if (!resolved_by) return -1;
    
    MONITORING_LOCK(manager->alert_mutex);
    
    for (uint32_t i = 0; i < manager->alert_store.count; i++) {
        if (manager->alert_store.alerts[i].alert_id == alert_id) {
            manager->alert_store.alerts[i].status = ALERT_STATUS_RESOLVED;
            manager->alert_store.alerts[i].resolve_time = get_timestamp_ms();
            strncpy(manager->alert_store.alerts[i].resolve_by, 
                   resolved_by, 
                   sizeof(manager->alert_store.alerts[i].resolve_by) - 1);
            
            MONITORING_UNLOCK(manager->alert_mutex);
            
            char log_msg[256];
            snprintf(log_msg, sizeof(log_msg), 
                    "Alert %lu resolved by %s", alert_id, resolved_by);
            write_log_entry(manager, "INFO", log_msg);
            
            return 0;
        }
    }
    
    MONITORING_UNLOCK(manager->alert_mutex);
    
    return -1; // 未找到告警
}

/**
 * 静音告警
 */
int monitoring_silence_alert(monitoring_manager_t *manager, uint64_t alert_id,
                            uint32_t duration_seconds) {
    MONITORING_CHECK_MANAGER(manager);
    
    MONITORING_LOCK(manager->alert_mutex);
    
    for (uint32_t i = 0; i < manager->alert_store.count; i++) {
        if (manager->alert_store.alerts[i].alert_id == alert_id) {
            manager->alert_store.alerts[i].status = ALERT_STATUS_SILENCED;
            
            // 这里可以添加静音到期时间的处理
            
            MONITORING_UNLOCK(manager->alert_mutex);
            
            char log_msg[256];
            snprintf(log_msg, sizeof(log_msg), 
                    "Alert %lu silenced for %u seconds", alert_id, duration_seconds);
            write_log_entry(manager, "INFO", log_msg);
            
            return 0;
        }
    }
    
    MONITORING_UNLOCK(manager->alert_mutex);
    
    return -1; // 未找到告警
}

/**
 * 更新指标值
 */
int monitoring_update_metric(monitoring_manager_t *manager, metric_type_t type,
                            double value) {
    MONITORING_CHECK_MANAGER(manager);
    if (type < 0 || type >= MONITORING_MAX_METRICS) return -1;
    
    int result = update_metric_history(manager, type, value);
    if (result == 0) {
        check_metric_thresholds(manager, type, value);
        
        // 通过WebSocket发送指标更新
        if (manager->ws_broadcast_callback) {
            monitoring_ws_send_metric_update(manager, type, value);
        }
    }
    
    return result;
}

/**
 * 获取指标信息
 */
int monitoring_get_metric(monitoring_manager_t *manager, metric_type_t type,
                         metric_t *metric) {
    MONITORING_CHECK_MANAGER(manager);
    if (!metric) return -1;
    if (type < 0 || type >= MONITORING_MAX_METRICS) return -1;
    
    MONITORING_LOCK(manager->data_mutex);
    *metric = manager->metrics[type];
    MONITORING_UNLOCK(manager->data_mutex);
    
    return 0;
}

/**
 * 设置指标阈值
 */
int monitoring_set_threshold(monitoring_manager_t *manager, metric_type_t type,
                           double warning, double error) {
    MONITORING_CHECK_MANAGER(manager);
    if (type < 0 || type >= MONITORING_MAX_METRICS) return -1;
    
    MONITORING_LOCK(manager->data_mutex);
    
    manager->metrics[type].warning_threshold = warning;
    manager->metrics[type].error_threshold = error;
    
    // 更新配置中的默认阈值
    if (type == METRIC_CPU_USAGE || type == METRIC_MEMORY_USAGE || 
        type == METRIC_DISK_USAGE) {
        manager->config.warning_threshold_percent = warning;
        manager->config.error_threshold_percent = error;
    }
    
    MONITORING_UNLOCK(manager->data_mutex);
    
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), 
            "Metric %s thresholds set: warning=%.2f, error=%.2f",
            metric_type_to_string(type), warning, error);
    write_log_entry(manager, "INFO", log_msg);
    
    return 0;
}

/**
 * 注册告警回调
 */
int monitoring_register_alert_callback(monitoring_manager_t *manager,
                                      alert_callback_t callback, void *user_data) {
    MONITORING_CHECK_MANAGER(manager);
    if (!callback) return -1;
    
    alert_callback_entry_t *entry = (alert_callback_entry_t*)malloc(
        sizeof(alert_callback_entry_t));
    if (!entry) return -1;
    
    entry->callback = callback;
    entry->user_data = user_data;
    entry->next = manager->alert_callbacks;
    manager->alert_callbacks = entry;
    
    return 0;
}

/**
 * 注册指标更新回调
 */
int monitoring_register_metric_callback(monitoring_manager_t *manager,
                                       metric_update_callback_t callback, 
                                       void *user_data) {
    MONITORING_CHECK_MANAGER(manager);
    if (!callback) return -1;
    
    metric_callback_entry_t *entry = (metric_callback_entry_t*)malloc(
        sizeof(metric_callback_entry_t));
    if (!entry) return -1;
    
    entry->callback = callback;
    entry->user_data = user_data;
    entry->next = manager->metric_callbacks;
    manager->metric_callbacks = entry;
    
    return 0;
}

/**
 * 注册健康检查回调
 */
int monitoring_register_health_check_callback(monitoring_manager_t *manager,
                                             health_check_callback_t callback,
                                             void *user_data) {
    MONITORING_CHECK_MANAGER(manager);
    if (!callback) return -1;
    
    health_callback_entry_t *entry = (health_callback_entry_t*)malloc(
        sizeof(health_callback_entry_t));
    if (!entry) return -1;
    
    entry->callback = callback;
    entry->user_data = user_data;
    entry->next = manager->health_callbacks;
    manager->health_callbacks = entry;
    
    return 0;
}

/**
 * 通过WebSocket广播快照
 */
int monitoring_ws_broadcast_snapshot(monitoring_manager_t *manager) {
    MONITORING_CHECK_MANAGER(manager);
    
    if (!manager->ws_broadcast_callback) {
        return 0; // 没有WebSocket回调
    }
    
    monitoring_snapshot_t snapshot;
    if (monitoring_collect_snapshot(manager, &snapshot) != 0) {
        return -1;
    }
    
    // 将快照转换为JSON
    json_t *root = json_object();
    json_object_set_new(root, "type", json_integer(MONITORING_MSG_SNAPSHOT));
    json_object_set_new(root, "timestamp", json_integer(snapshot.timestamp));
    
    // 添加资源信息
    json_t *resources = json_object();
    json_object_set_new(resources, "cpu_usage", json_real(snapshot.resources.cpu_usage));
    json_object_set_new(resources, "memory_usage", json_real(snapshot.resources.memory_usage));
    json_object_set_new(resources, "memory_total", json_integer(snapshot.resources.memory_total));
    json_object_set_new(resources, "memory_used", json_integer(snapshot.resources.memory_used));
    json_object_set_new(resources, "memory_free", json_integer(snapshot.resources.memory_free));
    json_object_set_new(resources, "disk_usage", json_real(snapshot.resources.disk_usage));
    json_object_set_new(resources, "disk_total", json_integer(snapshot.resources.disk_total));
    json_object_set_new(resources, "disk_used", json_integer(snapshot.resources.disk_used));
    json_object_set_new(resources, "disk_free", json_integer(snapshot.resources.disk_free));
    json_object_set_new(resources, "network_rx_rate", json_real(snapshot.resources.network_rx_rate));
    json_object_set_new(resources, "network_tx_rate", json_real(snapshot.resources.network_tx_rate));
    json_object_set_new(resources, "uptime", json_integer(snapshot.resources.uptime));
    json_object_set_new(resources, "processes", json_integer(snapshot.resources.processes));
    json_object_set_new(resources, "threads", json_integer(snapshot.resources.threads));
    
    json_object_set_new(root, "resources", resources);
    
    // 添加告警信息
    json_object_set_new(root, "alert_count", json_integer(snapshot.alert_count));
    json_object_set_new(root, "highest_alert", json_integer(snapshot.highest_alert));
    
    // 序列化为JSON字符串
    char *json_str = json_dumps(root, JSON_COMPACT);
    if (!json_str) {
        json_decref(root);
        return -1;
    }
    
    // 通过WebSocket发送
    if (manager->ws_broadcast_callback) {
        manager->ws_broadcast_callback((const uint8_t*)json_str, strlen(json_str));
    }
    
    // 清理
    free(json_str);
    json_decref(root);
    
    return 0;
}

/**
 * 通过WebSocket发送告警
 */
int monitoring_ws_send_alert(monitoring_manager_t *manager, const alert_t *alert) {
    MONITORING_CHECK_MANAGER(manager);
    if (!alert || !manager->ws_broadcast_callback) return -1;
    
    // 将告警转换为JSON
    json_t *root = json_object();
    json_object_set_new(root, "type", json_integer(MONITORING_MSG_ALERT));
    json_object_set_new(root, "timestamp", json_integer(get_timestamp_ms()));
    
    json_t *alert_obj = json_object();
    json_object_set_new(alert_obj, "id", json_integer(alert->alert_id));
    json_object_set_new(alert_obj, "level", json_integer(alert->level));
    json_object_set_new(alert_obj, "status", json_integer(alert->status));
    json_object_set_new(alert_obj, "metric_type", json_integer(alert->metric_type));
    json_object_set_new(alert_obj, "title", json_string(alert->title));
    json_object_set_new(alert_obj, "message", json_string(alert->message));
    json_object_set_new(alert_obj, "source", json_string(alert->source));
    json_object_set_new(alert_obj, "threshold", json_real(alert->threshold));
    json_object_set_new(alert_obj, "actual_value", json_real(alert->actual_value));
    json_object_set_new(alert_obj, "trigger_time", json_integer(alert->trigger_time));
    json_object_set_new(alert_obj, "acknowledge_time", json_integer(alert->acknowledge_time));
    json_object_set_new(alert_obj, "resolve_time", json_integer(alert->resolve_time));
    json_object_set_new(alert_obj, "acknowledged_by", json_string(alert->acknowledged_by));
    json_object_set_new(alert_obj, "resolve_by", json_string(alert->resolve_by));
    
    json_object_set_new(root, "alert", alert_obj);
    
    // 序列化为JSON字符串
    char *json_str = json_dumps(root, JSON_COMPACT);
    if (!json_str) {
        json_decref(root);
        return -1;
    }
    
    // 通过WebSocket发送
    manager->ws_broadcast_callback((const uint8_t*)json_str, strlen(json_str));
    
    // 清理
    free(json_str);
    json_decref(root);
    
    return 0;
}

/**
 * 通过WebSocket发送指标更新
 */
int monitoring_ws_send_metric_update(monitoring_manager_t *manager,
                                    metric_type_t type, double value) {
    MONITORING_CHECK_MANAGER(manager);
    if (!manager->ws_broadcast_callback) return -1;
    
    // 将指标更新转换为JSON
    json_t *root = json_object();
    json_object_set_new(root, "type", json_integer(MONITORING_MSG_METRIC_UPDATE));
    json_object_set_new(root, "timestamp", json_integer(get_timestamp_ms()));
    json_object_set_new(root, "metric_type", json_integer(type));
    json_object_set_new(root, "value", json_real(value));
    
    // 添加指标名称
    const char *metric_name = metric_type_to_string(type);
    json_object_set_new(root, "metric_name", json_string(metric_name));
    
    // 获取指标信息
    metric_t metric;
    if (monitoring_get_metric(manager, type, &metric) == 0) {
        json_object_set_new(root, "unit", json_string(metric.unit));
        json_object_set_new(root, "warning_threshold", json_real(metric.warning_threshold));
        json_object_set_new(root, "error_threshold", json_real(metric.error_threshold));
    }
    
    // 序列化为JSON字符串
    char *json_str = json_dumps(root, JSON_COMPACT);
    if (!json_str) {
        json_decref(root);
        return -1;
    }
    
    // 通过WebSocket发送
    manager->ws_broadcast_callback((const uint8_t*)json_str, strlen(json_str));
    
    // 清理
    free(json_str);
    json_decref(root);
    
    return 0;
}