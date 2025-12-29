#include "threadpool.h"
#include "protocol.h"  // 使用协议中的时间函数
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>

// ==================== 内部宏定义 ====================
#define LOCK(mutex) pthread_mutex_lock(&(mutex))
#define UNLOCK(mutex) pthread_mutex_unlock(&(mutex))
#define SIGNAL(cond) pthread_cond_signal(&(cond))
#define BROADCAST(cond) pthread_cond_broadcast(&(cond))
#define WAIT(cond, mutex) pthread_cond_wait(&(cond), &(mutex))
#define TIMEDWAIT(cond, mutex, timeout) \
    pthread_cond_timedwait(&(cond), &(mutex), &(timeout))

#define THREADPOOL_ASSERT(expr, error_code) \
    do { \
        if (!(expr)) { \
            return (error_code); \
        } \
    } while (0)

#define THREADPOOL_CHECK_NULL(ptr) \
    THREADPOOL_ASSERT((ptr) != NULL, THREADPOOL_ERROR_INVALID_ARG)

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// ==================== 内部结构 ====================
typedef struct {
    threadpool_t *pool;
    uint32_t thread_index;
} worker_thread_arg_t;

// ==================== 静态函数声明 ====================
static void* worker_thread_function(void *arg);
static threadpool_task_t* create_task(
    task_function_t function,
    void *argument,
    cleanup_function_t cleanup,
    task_priority_t priority,
    uint64_t timeout_ms,
    uint64_t task_id
);
static void destroy_task(threadpool_task_t *task);
static threadpool_task_t* get_next_task(threadpool_t *pool);
static void update_thread_statistics(threadpool_t *pool, uint32_t thread_index,
                                   uint64_t task_id, uint64_t execution_time);
static void check_and_adjust_pool_size(threadpool_t *pool);
static uint32_t calculate_optimal_threads(const threadpool_t *pool);
static void set_thread_name(const char *name);

// ==================== 工具函数 ====================

/**
 * 获取当前时间（微秒）
 */
static uint64_t get_current_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/**
 * 创建任务结构
 */
static threadpool_task_t* create_task(
    task_function_t function,
    void *argument,
    cleanup_function_t cleanup,
    task_priority_t priority,
    uint64_t timeout_ms,
    uint64_t task_id
) {
    threadpool_task_t *task = (threadpool_task_t*)malloc(sizeof(threadpool_task_t));
    if (!task) return NULL;
    
    task->function = function;
    task->argument = argument;
    task->cleanup = cleanup;
    task->priority = priority;
    task->task_id = task_id;
    task->created_time = get_current_time_us() / 1000; // 转换为毫秒
    task->timeout_ms = timeout_ms;
    task->next = NULL;
    
    return task;
}

/**
 * 销毁任务结构
 */
static void destroy_task(threadpool_task_t *task) {
    if (!task) return;
    
    // 如果有清理函数，调用它
    if (task->cleanup && task->argument) {
        task->cleanup(task->argument);
    }
    
    free(task);
}

/**
 * 设置线程名称
 */
static void set_thread_name(const char *name) {
#if defined(__linux__) || defined(__APPLE__)
    char thread_name[16];
    snprintf(thread_name, sizeof(thread_name), "%s", name);
    pthread_setname_np(pthread_self(), thread_name);
#endif
}

// ==================== 主要函数实现 ====================

/**
 * 获取默认配置
 */
threadpool_config_t threadpool_get_default_config(void) {
    threadpool_config_t config = {
        .min_threads = MIN_THREADS,
        .max_threads = MAX_THREADS,
        .default_threads = DEFAULT_THREADS,
        .idle_timeout_sec = 300,        // 5分钟空闲超时
        .queue_size = 1024,             // 默认队列大小
        .stack_size = DEFAULT_STACK_SIZE,
        .enable_statistics = true,
        .enable_dynamic_scaling = true,
        .scaling_check_interval_ms = 5000, // 5秒检查一次
        .load_factor_threshold = 0.8f   // 80%负载阈值
    };
    return config;
}

/**
 * 创建线程池
 */
threadpool_t* threadpool_create(const threadpool_config_t *config, const char *name) {
    threadpool_t *pool = (threadpool_t*)calloc(1, sizeof(threadpool_t));
    if (!pool) return NULL;
    
    // 设置配置
    if (config) {
        pool->config = *config;
    } else {
        pool->config = threadpool_get_default_config();
    }
    
    // 验证配置
    if (pool->config.min_threads < MIN_THREADS) {
        pool->config.min_threads = MIN_THREADS;
    }
    if (pool->config.max_threads > MAX_THREADS) {
        pool->config.max_threads = MAX_THREADS;
    }
    if (pool->config.default_threads < pool->config.min_threads) {
        pool->config.default_threads = pool->config.min_threads;
    }
    if (pool->config.default_threads > pool->config.max_threads) {
        pool->config.default_threads = pool->config.max_threads;
    }
    
    // 设置名称
    if (name) {
        snprintf(pool->name, sizeof(pool->name), "%s", name);
    } else {
        snprintf(pool->name, sizeof(pool->name), "threadpool-%lx", (uintptr_t)pool);
    }
    
    // 初始化互斥锁和条件变量
    if (pthread_mutex_init(&pool->queue_mutex, NULL) != 0 ||
        pthread_mutex_init(&pool->stat_mutex, NULL) != 0 ||
        pthread_cond_init(&pool->queue_not_empty, NULL) != 0 ||
        pthread_cond_init(&pool->queue_not_full, NULL) != 0 ||
        pthread_cond_init(&pool->all_tasks_done, NULL) != 0) {
        free(pool);
        return NULL;
    }
    
    // 初始化线程数组
    pool->thread_count = pool->config.default_threads;
    pool->threads = (pthread_t*)malloc(sizeof(pthread_t) * pool->thread_count);
    if (!pool->threads) {
        threadpool_destroy(pool, false);
        return NULL;
    }
    
    // 初始化线程统计
    pool->thread_stats = (thread_statistics_t*)calloc(pool->thread_count, 
                                                     sizeof(thread_statistics_t));
    if (!pool->thread_stats) {
        threadpool_destroy(pool, false);
        return NULL;
    }
    
    // 设置状态
    pool->state = THREADPOOL_CREATED;
    pool->shutdown_requested = false;
    pool->immediate_shutdown = false;
    pool->task_counter = 1;  // 从1开始
    
    // 启动工作线程
    pool->state = THREADPOOL_RUNNING;
    for (uint32_t i = 0; i < pool->thread_count; i++) {
        worker_thread_arg_t *arg = (worker_thread_arg_t*)malloc(sizeof(worker_thread_arg_t));
        if (!arg) {
            // 部分线程已启动，需要优雅关闭
            pool->shutdown_requested = true;
            threadpool_destroy(pool, true);
            return NULL;
        }
        
        arg->pool = pool;
        arg->thread_index = i;
        
        // 设置线程属性（栈大小）
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, pool->config.stack_size);
        
        if (pthread_create(&pool->threads[i], &attr, 
                          worker_thread_function, arg) != 0) {
            free(arg);
            pool->shutdown_requested = true;
            threadpool_destroy(pool, true);
            return NULL;
        }
        
        pthread_attr_destroy(&attr);
        
        // 初始化线程统计
        pool->thread_stats[i].thread_id = (uint64_t)pool->threads[i];
        snprintf(pool->thread_stats[i].name, THREAD_NAME_LEN, 
                "%s-%d", pool->name, i);
    }
    
    return pool;
}

/**
 * 工作线程函数
 */
static void* worker_thread_function(void *arg) {
    worker_thread_arg_t *thread_arg = (worker_thread_arg_t*)arg;
    threadpool_t *pool = thread_arg->pool;
    uint32_t thread_index = thread_arg->thread_index;
    
    free(thread_arg);
    
    // 设置线程名称
    char thread_name[THREAD_NAME_LEN];
    snprintf(thread_name, sizeof(thread_name), "%s-%d", pool->name, thread_index);
    set_thread_name(thread_name);
    
    while (1) {
        LOCK(pool->queue_mutex);
        
        // 等待任务或关闭信号
        while (!pool->shutdown_requested && 
               pool->task_queue_high == NULL &&
               pool->task_queue_normal == NULL &&
               pool->task_queue_low == NULL) {
            // 更新线程状态
            pool->thread_stats[thread_index].is_busy = false;
            pool->active_threads--;
            
            // 等待任务
            WAIT(pool->queue_not_empty, pool->queue_mutex);
            
            // 恢复线程状态
            pool->active_threads++;
            pool->thread_stats[thread_index].is_busy = true;
        }
        
        // 检查是否应该退出
        if (pool->shutdown_requested && 
            pool->task_queue_high == NULL &&
            pool->task_queue_normal == NULL &&
            pool->task_queue_low == NULL) {
            UNLOCK(pool->queue_mutex);
            break;
        }
        
        // 获取下一个任务
        threadpool_task_t *task = get_next_task(pool);
        
        // 如果有等待队列非满的条件，通知它
        if (pool->config.queue_size > 0) {
            uint32_t queue_len = pool->stats.queue_length;
            if (queue_len >= pool->config.queue_size) {
                SIGNAL(pool->queue_not_full);
            }
        }
        
        UNLOCK(pool->queue_mutex);
        
        if (task) {
            // 更新统计信息
            LOCK(pool->stat_mutex);
            pool->thread_stats[thread_index].current_task_id = task->task_id;
            pool->thread_stats[thread_index].last_task_time = get_current_time_us();
            UNLOCK(pool->stat_mutex);
            
            // 执行任务
            uint64_t start_time = get_current_time_us();
            
            if (task->function) {
                task->function(task->argument);
            }
            
            uint64_t end_time = get_current_time_us();
            uint64_t execution_time = end_time - start_time;
            
            // 更新统计
            update_thread_statistics(pool, thread_index, task->task_id, execution_time);
            
            // 销毁任务
            destroy_task(task);
            
            // 动态调整检查
            if (pool->config.enable_dynamic_scaling) {
                uint64_t current_time = get_current_time_us() / 1000;
                if (current_time - pool->last_scaling_check > 
                    pool->config.scaling_check_interval_ms) {
                    check_and_adjust_pool_size(pool);
                    pool->last_scaling_check = current_time;
                }
            }
        }
    }
    
    // 线程退出
    LOCK(pool->stat_mutex);
    pool->thread_stats[thread_index].is_busy = false;
    pool->active_threads--;
    UNLOCK(pool->stat_mutex);
    
    pthread_exit(NULL);
    return NULL;
}

/**
 * 获取下一个任务（按优先级）
 */
static threadpool_task_t* get_next_task(threadpool_t *pool) {
    threadpool_task_t *task = NULL;
    
    // 按优先级选择任务
    if (pool->task_queue_high) {
        task = pool->task_queue_high;
        pool->task_queue_high = task->next;
    } else if (pool->task_queue_normal) {
        task = pool->task_queue_normal;
        pool->task_queue_normal = task->next;
    } else if (pool->task_queue_low) {
        task = pool->task_queue_low;
        pool->task_queue_low = task->next;
    }
    
    if (task) {
        // 更新队列尾指针
        if (pool->task_queue_tail == task) {
            pool->task_queue_tail = NULL;
        }
        
        // 更新统计
        pool->stats.queue_length--;
        pool->stats.total_tasks_completed++;
        
        // 计算等待时间
        uint64_t wait_time = (get_current_time_us() / 1000) - task->created_time;
        pool->stats.total_execution_time += wait_time;
        
        if (pool->stats.total_tasks_completed > 0) {
            pool->stats.average_wait_time = 
                pool->stats.total_execution_time / pool->stats.total_tasks_completed;
        }
        
        // 检查是否所有任务都完成了
        if (pool->stats.queue_length == 0 && pool->state == THREADPOOL_STOPPING) {
            BROADCAST(pool->all_tasks_done);
        }
    }
    
    return task;
}

/**
 * 添加任务到线程池
 */
threadpool_error_t threadpool_add_task(
    threadpool_t *pool,
    task_function_t function,
    void *argument,
    cleanup_function_t cleanup,
    task_priority_t priority,
    uint64_t timeout_ms,
    uint64_t *task_id
) {
    THREADPOOL_CHECK_NULL(pool);
    THREADPOOL_CHECK_NULL(function);
    
    if (pool->state != THREADPOOL_RUNNING) {
        return THREADPOOL_ERROR_SHUTDOWN;
    }
    
    LOCK(pool->queue_mutex);
    
    // 检查队列是否已满
    if (pool->config.queue_size > 0 && 
        pool->stats.queue_length >= pool->config.queue_size) {
        UNLOCK(pool->queue_mutex);
        return THREADPOOL_ERROR_QUEUE_FULL;
    }
    
    // 创建任务
    uint64_t current_task_id = pool->task_counter++;
    threadpool_task_t *task = create_task(
        function, argument, cleanup, priority, timeout_ms, current_task_id
    );
    
    if (!task) {
        UNLOCK(pool->queue_mutex);
        return THREADPOOL_ERROR_MEMORY;
    }
    
    // 添加到相应优先级的队列
    threadpool_task_t **queue_head = NULL;
    switch (priority) {
        case TASK_PRIORITY_LOW:
            queue_head = &pool->task_queue_low;
            break;
        case TASK_PRIORITY_NORMAL:
            queue_head = &pool->task_queue_normal;
            break;
        case TASK_PRIORITY_HIGH:
        case TASK_PRIORITY_CRITICAL:
            queue_head = &pool->task_queue_high;
            break;
    }
    
    if (*queue_head == NULL) {
        *queue_head = task;
        pool->task_queue_tail = task;
    } else {
        // 添加到队列尾部
        threadpool_task_t *tail = pool->task_queue_tail;
        tail->next = task;
        pool->task_queue_tail = task;
    }
    
    // 更新统计
    pool->stats.queue_length++;
    pool->stats.total_tasks_submitted++;
    if (pool->stats.queue_length > pool->stats.max_queue_length) {
        pool->stats.max_queue_length = pool->stats.queue_length;
    }
    
    // 通知等待的线程
    SIGNAL(pool->queue_not_empty);
    
    UNLOCK(pool->queue_mutex);
    
    if (task_id) {
        *task_id = current_task_id;
    }
    
    return THREADPOOL_SUCCESS;
}

/**
 * 添加紧急任务（插队到队列头部）
 */
threadpool_error_t threadpool_add_urgent_task(
    threadpool_t *pool,
    task_function_t function,
    void *argument,
    cleanup_function_t cleanup
) {
    THREADPOOL_CHECK_NULL(pool);
    THREADPOOL_CHECK_NULL(function);
    
    if (pool->state != THREADPOOL_RUNNING) {
        return THREADPOOL_ERROR_SHUTDOWN;
    }
    
    LOCK(pool->queue_mutex);
    
    // 创建任务
    uint64_t current_task_id = pool->task_counter++;
    threadpool_task_t *task = create_task(
        function, argument, cleanup, TASK_PRIORITY_CRITICAL, 0, current_task_id
    );
    
    if (!task) {
        UNLOCK(pool->queue_mutex);
        return THREADPOOL_ERROR_MEMORY;
    }
    
    // 紧急任务总是添加到高优先级队列头部
    task->next = pool->task_queue_high;
    pool->task_queue_high = task;
    
    if (pool->task_queue_tail == NULL) {
        pool->task_queue_tail = task;
    }
    
    // 更新统计
    pool->stats.queue_length++;
    pool->stats.total_tasks_submitted++;
    if (pool->stats.queue_length > pool->stats.max_queue_length) {
        pool->stats.max_queue_length = pool->stats.queue_length;
    }
    
    // 通知等待的线程
    SIGNAL(pool->queue_not_empty);
    
    UNLOCK(pool->queue_mutex);
    
    return THREADPOOL_SUCCESS;
}

/**
 * 销毁线程池
 */
threadpool_error_t threadpool_destroy(threadpool_t *pool, bool wait) {
    THREADPOOL_CHECK_NULL(pool);
    
    LOCK(pool->queue_mutex);
    
    if (pool->state == THREADPOOL_STOPPED || pool->state == THREADPOOL_ERROR) {
        UNLOCK(pool->queue_mutex);
        return THREADPOOL_SUCCESS;
    }
    
    pool->shutdown_requested = true;
    pool->immediate_shutdown = !wait;
    pool->state = THREADPOOL_STOPPING;
    
    // 通知所有等待的线程
    BROADCAST(pool->queue_not_empty);
    BROADCAST(pool->queue_not_full);
    
    // 如果等待完成，等待所有任务完成
    if (wait && pool->stats.queue_length > 0) {
        while (pool->stats.queue_length > 0) {
            WAIT(pool->all_tasks_done, pool->queue_mutex);
        }
    }
    
    UNLOCK(pool->queue_mutex);
    
    // 等待所有线程退出
    for (uint32_t i = 0; i < pool->thread_count; i++) {
        if (pool->threads[i] != 0) {
            pthread_join(pool->threads[i], NULL);
        }
    }
    
    // 清理剩余的任务
    LOCK(pool->queue_mutex);
    
    // 清理各个优先级的队列
    threadpool_task_t *task;
    while ((task = pool->task_queue_high) != NULL) {
        pool->task_queue_high = task->next;
        destroy_task(task);
        pool->stats.total_tasks_failed++;
    }
    
    while ((task = pool->task_queue_normal) != NULL) {
        pool->task_queue_normal = task->next;
        destroy_task(task);
        pool->stats.total_tasks_failed++;
    }
    
    while ((task = pool->task_queue_low) != NULL) {
        pool->task_queue_low = task->next;
        destroy_task(task);
        pool->stats.total_tasks_failed++;
    }
    
    pool->task_queue_tail = NULL;
    pool->stats.queue_length = 0;
    
    UNLOCK(pool->queue_mutex);
    
    // 清理资源
    pthread_mutex_destroy(&pool->queue_mutex);
    pthread_mutex_destroy(&pool->stat_mutex);
    pthread_cond_destroy(&pool->queue_not_empty);
    pthread_cond_destroy(&pool->queue_not_full);
    pthread_cond_destroy(&pool->all_tasks_done);
    
    if (pool->threads) free(pool->threads);
    if (pool->thread_stats) free(pool->thread_stats);
    
    pool->state = THREADPOOL_STOPPED;
    free(pool);
    
    return THREADPOOL_SUCCESS;
}

/**
 * 更新线程统计信息
 */
static void update_thread_statistics(threadpool_t *pool, uint32_t thread_index,
                                   uint64_t task_id, uint64_t execution_time) {
    LOCK(pool->stat_mutex);
    
    thread_statistics_t *stats = &pool->thread_stats[thread_index];
    stats->tasks_executed++;
    stats->total_execution_time += execution_time;
    stats->current_task_id = 0;
    
    UNLOCK(pool->stat_mutex);
}

/**
 * 等待所有任务完成
 */
threadpool_error_t threadpool_wait(threadpool_t *pool, uint64_t timeout_ms) {
    THREADPOOL_CHECK_NULL(pool);
    
    if (pool->state != THREADPOOL_RUNNING) {
        return THREADPOOL_ERROR_SHUTDOWN;
    }
    
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    
    // 计算超时时间
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    
    LOCK(pool->queue_mutex);
    
    while (pool->stats.queue_length > 0) {
        if (timeout_ms == 0) {
            // 无限等待
            WAIT(pool->all_tasks_done, pool->queue_mutex);
        } else {
            if (TIMEDWAIT(pool->all_tasks_done, pool->queue_mutex, ts) == ETIMEDOUT) {
                UNLOCK(pool->queue_mutex);
                return THREADPOOL_ERROR_TIMEOUT;
            }
        }
    }
    
    UNLOCK(pool->queue_mutex);
    
    return THREADPOOL_SUCCESS;
}

/**
 * 调整线程池大小
 */
threadpool_error_t threadpool_resize(threadpool_t *pool, uint32_t num_threads) {
    THREADPOOL_CHECK_NULL(pool);
    
    if (num_threads < pool->config.min_threads || 
        num_threads > pool->config.max_threads) {
        return THREADPOOL_ERROR_INVALID_ARG;
    }
    
    if (pool->state != THREADPOOL_RUNNING) {
        return THREADPOOL_ERROR_SHUTDOWN;
    }
    
    LOCK(pool->queue_mutex);
    
    if (num_threads == pool->thread_count) {
        UNLOCK(pool->queue_mutex);
        return THREADPOOL_SUCCESS;
    }
    
    if (num_threads > pool->thread_count) {
        // 增加线程
        uint32_t threads_to_add = num_threads - pool->thread_count;
        pthread_t *new_threads = (pthread_t*)realloc(pool->threads, 
                                                    sizeof(pthread_t) * num_threads);
        if (!new_threads) {
            UNLOCK(pool->queue_mutex);
            return THREADPOOL_ERROR_MEMORY;
        }
        
        thread_statistics_t *new_stats = (thread_statistics_t*)realloc(pool->thread_stats,
                                                                      sizeof(thread_statistics_t) * num_threads);
        if (!new_stats) {
            UNLOCK(pool->queue_mutex);
            return THREADPOOL_ERROR_MEMORY;
        }
        
        pool->threads = new_threads;
        pool->thread_stats = new_stats;
        
        // 初始化新线程统计
        for (uint32_t i = pool->thread_count; i < num_threads; i++) {
            memset(&pool->thread_stats[i], 0, sizeof(thread_statistics_t));
            snprintf(pool->thread_stats[i].name, THREAD_NAME_LEN, 
                    "%s-%d", pool->name, i);
        }
        
        // 创建新线程
        for (uint32_t i = pool->thread_count; i < num_threads; i++) {
            worker_thread_arg_t *arg = (worker_thread_arg_t*)malloc(sizeof(worker_thread_arg_t));
            if (!arg) {
                // 部分线程已启动，继续处理
                break;
            }
            
            arg->pool = pool;
            arg->thread_index = i;
            
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, pool->config.stack_size);
            
            if (pthread_create(&pool->threads[i], &attr, 
                              worker_thread_function, arg) != 0) {
                free(arg);
                break;
            }
            
            pthread_attr_destroy(&attr);
            threads_to_add--;
        }
        
        pool->thread_count = num_threads - threads_to_add;
        
    } else {
        // 减少线程 - 通过自然退出
        // 设置较小的线程数，新线程会自然退出
        pool->config.max_threads = num_threads;
        
        // 通知所有线程检查是否应该退出
        BROADCAST(pool->queue_not_empty);
    }
    
    UNLOCK(pool->queue_mutex);
    
    return THREADPOOL_SUCCESS;
}

/**
 * 获取线程池统计信息
 */
threadpool_error_t threadpool_get_statistics(
    threadpool_t *pool,
    threadpool_statistics_t *stats
) {
    THREADPOOL_CHECK_NULL(pool);
    THREADPOOL_CHECK_NULL(stats);
    
    LOCK(pool->stat_mutex);
    *stats = pool->stats;
    stats->active_threads = pool->active_threads;
    UNLOCK(pool->stat_mutex);
    
    return THREADPOOL_SUCCESS;
}

/**
 * 检查并调整线程池大小（动态缩放）
 */
static void check_and_adjust_pool_size(threadpool_t *pool) {
    if (!pool->config.enable_dynamic_scaling) return;
    
    uint32_t optimal_threads = calculate_optimal_threads(pool);
    
    if (optimal_threads != pool->thread_count) {
        threadpool_resize(pool, optimal_threads);
    }
}

/**
 * 计算最优线程数
 */
static uint32_t calculate_optimal_threads(const threadpool_t *pool) {
    // 简单的负载因子计算
    float load_factor = (float)pool->stats.queue_length / 
                       (float)pool->config.queue_size;
    
    if (load_factor > pool->config.load_factor_threshold) {
        // 高负载，增加线程
        return MIN(pool->thread_count * 2, pool->config.max_threads);
    } else if (load_factor < 0.2f) {
        // 低负载，减少线程
        return MAX(pool->thread_count / 2, pool->config.min_threads);
    }
    
    return pool->thread_count;
}

/**
 * 获取错误信息字符串
 */
const char* threadpool_error_string(threadpool_error_t error) {
    static const char* error_strings[] = {
        "Success",
        "Invalid argument",
        "Memory allocation failed",
        "Thread pool is busy",
        "Thread pool is shutting down",
        "Operation timed out",
        "Thread creation failed",
        "Task queue is full",
        "Internal error"
    };
    
    if (error < THREADPOOL_SUCCESS || error > THREADPOOL_ERROR_INTERNAL) {
        return "Unknown error";
    }
    
    return error_strings[error];
}

// 其他辅助函数的实现...

void threadpool_pause(threadpool_t *pool) {
    THREADPOOL_CHECK_NULL(pool);
    pool->state = THREADPOOL_STOPPING;
}

void threadpool_resume(threadpool_t *pool) {
    THREADPOOL_CHECK_NULL(pool);
    pool->state = THREADPOOL_RUNNING;
    SIGNAL(pool->queue_not_empty);
}

threadpool_state_t threadpool_get_state(const threadpool_t *pool) {
    THREADPOOL_CHECK_NULL(pool);
    return pool->state;
}

void threadpool_get_config(const threadpool_t *pool, threadpool_config_t *config) {
    THREADPOOL_CHECK_NULL(pool);
    THREADPOOL_CHECK_NULL(config);
    *config = pool->config;
}

const char* threadpool_get_name(const threadpool_t *pool) {
    THREADPOOL_CHECK_NULL(pool);
    return pool->name;
}

uint32_t threadpool_get_task_count(const threadpool_t *pool) {
    THREADPOOL_CHECK_NULL(pool);
    return pool->stats.queue_length;
}

uint32_t threadpool_get_active_threads(const threadpool_t *pool) {
    THREADPOOL_CHECK_NULL(pool);
    return pool->active_threads;
}