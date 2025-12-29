#ifndef SECURE_CHAT_THREADPOOL_H
#define SECURE_CHAT_THREADPOOL_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 配置常量 ====================
#define MAX_THREADS 128
#define MIN_THREADS 1
#define DEFAULT_THREADS 16
#define MAX_TASK_QUEUE_SIZE 65536
#define THREAD_NAME_LEN 16
#define DEFAULT_STACK_SIZE (2 * 1024 * 1024) // 2MB

// ==================== 线程池状态 ====================
typedef enum {
    THREADPOOL_CREATED = 0,
    THREADPOOL_RUNNING = 1,
    THREADPOOL_STOPPING = 2,
    THREADPOOL_STOPPED = 3,
    THREADPOOL_ERROR = 4
} threadpool_state_t;

// ==================== 任务优先级 ====================
typedef enum {
    TASK_PRIORITY_LOW = 0,
    TASK_PRIORITY_NORMAL = 1,
    TASK_PRIORITY_HIGH = 2,
    TASK_PRIORITY_CRITICAL = 3
} task_priority_t;

// ==================== 错误码 ====================
typedef enum {
    THREADPOOL_SUCCESS = 0,
    THREADPOOL_ERROR_INVALID_ARG = 1,
    THREADPOOL_ERROR_MEMORY = 2,
    THREADPOOL_ERROR_BUSY = 3,
    THREADPOOL_ERROR_SHUTDOWN = 4,
    THREADPOOL_ERROR_TIMEOUT = 5,
    THREADPOOL_ERROR_THREAD = 6,
    THREADPOOL_ERROR_QUEUE_FULL = 7,
    THREADPOOL_ERROR_INTERNAL = 8
} threadpool_error_t;

// ==================== 回调函数类型 ====================
typedef void (*task_function_t)(void *arg);
typedef void (*cleanup_function_t)(void *arg);

// ==================== 任务结构 ====================
typedef struct threadpool_task {
    task_function_t function;       // 任务函数
    void *argument;                // 任务参数
    cleanup_function_t cleanup;    // 清理函数
    task_priority_t priority;      // 任务优先级
    uint64_t task_id;              // 任务ID
    uint64_t created_time;         // 创建时间戳
    uint64_t timeout_ms;           // 超时时间 (0=无超时)
    struct threadpool_task *next;  // 下一个任务
} threadpool_task_t;

// ==================== 线程统计信息 ====================
typedef struct {
    uint64_t thread_id;            // 线程ID
    char name[THREAD_NAME_LEN];    // 线程名称
    uint64_t tasks_executed;       // 执行的任务数
    uint64_t total_execution_time; // 总执行时间 (微秒)
    uint64_t last_task_time;       // 最后任务执行时间
    bool is_busy;                  // 是否繁忙
    uint64_t current_task_id;      // 当前执行的任务ID
} thread_statistics_t;

// ==================== 线程池统计信息 ====================
typedef struct {
    uint64_t total_tasks_submitted;    // 总提交任务数
    uint64_t total_tasks_completed;    // 总完成任务数
    uint64_t total_tasks_failed;       // 总失败任务数
    uint64_t total_tasks_timeout;      // 总超时任务数
    uint64_t queue_length;             // 当前队列长度
    uint64_t active_threads;           // 活跃线程数
    uint64_t max_queue_length;         // 最大队列长度
    uint64_t total_execution_time;     // 总执行时间 (微秒)
    uint64_t average_wait_time;        // 平均等待时间 (微秒)
} threadpool_statistics_t;

// ==================== 线程池配置 ====================
typedef struct {
    uint32_t min_threads;           // 最小线程数
    uint32_t max_threads;           // 最大线程数
    uint32_t default_threads;       // 默认线程数
    uint32_t idle_timeout_sec;      // 空闲超时秒数 (0=不超时)
    uint32_t queue_size;            // 任务队列大小
    size_t stack_size;              // 线程栈大小 (字节)
    bool enable_statistics;         // 启用统计
    bool enable_dynamic_scaling;    // 启用动态缩放
    uint32_t scaling_check_interval_ms; // 缩放检查间隔
    float load_factor_threshold;    // 负载因子阈值 (0.0-1.0)
} threadpool_config_t;

// ==================== 线程池主结构 ====================
typedef struct threadpool {
    // 线程管理
    pthread_t *threads;                    // 线程数组
    thread_statistics_t *thread_stats;     // 线程统计数组
    uint32_t thread_count;                 // 当前线程数
    uint32_t active_threads;               // 活跃线程数
    
    // 任务队列
    threadpool_task_t *task_queue_high;    // 高优先级队列头
    threadpool_task_t *task_queue_normal;  // 普通优先级队列头
    threadpool_task_t *task_queue_low;     // 低优先级队列头
    threadpool_task_t *task_queue_tail;    // 队列尾
    
    // 同步原语
    pthread_mutex_t queue_mutex;           // 队列互斥锁
    pthread_mutex_t stat_mutex;            // 统计互斥锁
    pthread_cond_t queue_not_empty;        // 队列非空条件变量
    pthread_cond_t queue_not_full;         // 队列非满条件变量
    pthread_cond_t all_tasks_done;         // 所有任务完成条件变量
    
    // 控制标志
    volatile threadpool_state_t state;     // 线程池状态
    volatile bool shutdown_requested;      // 关闭请求
    volatile bool immediate_shutdown;      // 立即关闭
    
    // 统计信息
    threadpool_statistics_t stats;         // 统计信息
    threadpool_config_t config;            // 配置
    
    // 内部管理
    uint64_t task_counter;                 // 任务计数器
    uint64_t last_scaling_check;           // 最后缩放检查时间
    char name[32];                         // 线程池名称
} threadpool_t;

// ==================== 函数声明 ====================

/**
 * 创建线程池
 * @param config 线程池配置 (可为NULL，使用默认配置)
 * @param name 线程池名称
 * @return 线程池指针，失败返回NULL
 */
threadpool_t* threadpool_create(const threadpool_config_t *config, const char *name);

/**
 * 销毁线程池
 * @param pool 线程池指针
 * @param wait 是否等待所有任务完成
 * @return 成功返回THREADPOOL_SUCCESS
 */
threadpool_error_t threadpool_destroy(threadpool_t *pool, bool wait);

/**
 * 添加任务到线程池
 * @param pool 线程池指针
 * @param function 任务函数
 * @param argument 任务参数
 * @param cleanup 清理函数
 * @param priority 任务优先级
 * @param timeout_ms 超时时间 (毫秒)
 * @param task_id 返回的任务ID
 * @return 成功返回THREADPOOL_SUCCESS
 */
threadpool_error_t threadpool_add_task(
    threadpool_t *pool,
    task_function_t function,
    void *argument,
    cleanup_function_t cleanup,
    task_priority_t priority,
    uint64_t timeout_ms,
    uint64_t *task_id
);

/**
 * 添加紧急任务（插队）
 * @param pool 线程池指针
 * @param function 任务函数
 * @param argument 任务参数
 * @param cleanup 清理函数
 * @return 成功返回THREADPOOL_SUCCESS
 */
threadpool_error_t threadpool_add_urgent_task(
    threadpool_t *pool,
    task_function_t function,
    void *argument,
    cleanup_function_t cleanup
);

/**
 * 等待所有任务完成
 * @param pool 线程池指针
 * @param timeout_ms 超时时间 (毫秒)
 * @return 成功返回THREADPOOL_SUCCESS
 */
threadpool_error_t threadpool_wait(threadpool_t *pool, uint64_t timeout_ms);

/**
 * 调整线程池大小
 * @param pool 线程池指针
 * @param num_threads 目标线程数
 * @return 成功返回THREADPOOL_SUCCESS
 */
threadpool_error_t threadpool_resize(threadpool_t *pool, uint32_t num_threads);

/**
 * 暂停线程池（不接受新任务）
 * @param pool 线程池指针
 */
void threadpool_pause(threadpool_t *pool);

/**
 * 恢复线程池
 * @param pool 线程池指针
 */
void threadpool_resume(threadpool_t *pool);

/**
 * 获取线程池状态
 * @param pool 线程池指针
 * @return 线程池状态
 */
threadpool_state_t threadpool_get_state(const threadpool_t *pool);

/**
 * 获取线程池统计信息
 * @param pool 线程池指针
 * @param stats 返回的统计信息
 * @return 成功返回THREADPOOL_SUCCESS
 */
threadpool_error_t threadpool_get_statistics(
    threadpool_t *pool,
    threadpool_statistics_t *stats
);

/**
 * 获取线程统计信息
 * @param pool 线程池指针
 * @param thread_stats 返回的线程统计数组
 * @param count 数组大小/返回的实际数量
 * @return 成功返回THREADPOOL_SUCCESS
 */
threadpool_error_t threadpool_get_thread_statistics(
    threadpool_t *pool,
    thread_statistics_t *thread_stats,
    uint32_t *count
);

/**
 * 设置线程池配置
 * @param pool 线程池指针
 * @param config 新配置
 * @return 成功返回THREADPOOL_SUCCESS
 */
threadpool_error_t threadpool_set_config(
    threadpool_t *pool,
    const threadpool_config_t *config
);

/**
 * 获取线程池配置
 * @param pool 线程池指针
 * @param config 返回的配置
 */
void threadpool_get_config(const threadpool_t *pool, threadpool_config_t *config);

/**
 * 获取默认配置
 * @return 默认配置
 */
threadpool_config_t threadpool_get_default_config(void);

/**
 * 获取错误信息
 * @param error 错误码
 * @return 错误信息字符串
 */
const char* threadpool_error_string(threadpool_error_t error);

/**
 * 获取线程池名称
 * @param pool 线程池指针
 * @return 线程池名称
 */
const char* threadpool_get_name(const threadpool_t *pool);

/**
 * 获取当前线程池中的任务数
 * @param pool 线程池指针
 * @return 任务数
 */
uint32_t threadpool_get_task_count(const threadpool_t *pool);

/**
 * 获取活跃线程数
 * @param pool 线程池指针
 * @return 活跃线程数
 */
uint32_t threadpool_get_active_threads(const threadpool_t *pool);

#ifdef __cplusplus
}
#endif

#endif // SECURE_CHAT_THREADPOOL_H