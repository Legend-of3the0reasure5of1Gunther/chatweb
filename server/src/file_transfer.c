#include "file_transfer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/time.h>
#include <zlib.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

// ==================== 内部数据结构 ====================

// 全局传输管理器
static ft_manager_t* g_ft_manager = NULL;
static pthread_once_t g_ft_init_once = PTHREAD_ONCE_INIT;

// 默认配置
static const ft_config_t DEFAULT_FT_CONFIG = {
    .chunk_size = FT_CHUNK_SIZE,
    .max_retries = FT_MAX_RETRIES,
    .timeout_ms = FT_TIMEOUT_MS,
    .max_concurrent_transfers = FT_MAX_CONCURRENT_TRANSFERS,
    .enable_encryption = true,
    .enable_compression = true,
    .enable_resume = true,
    .verify_checksum = true,
    .storage_path = "./storage",
    .temp_path = "./temp",
    .max_file_size = MAX_FILE_SIZE
};

// 哈希表相关
#define FT_HASH_TABLE_SIZE 101  // 质数，减少冲突

// ==================== 内部函数声明 ====================
static void ft_init_once(void);
static ft_error_code_t ft_init_manager(void);
static ft_error_code_t ft_init_directories(void);
static ft_error_code_t ft_create_session_map(void);
static void ft_destroy_session_map(void);
static ft_error_code_t ft_add_to_session_map(ft_transfer_session_t* session);
static ft_error_code_t ft_remove_from_session_map(uint64_t transfer_id);
static ft_transfer_session_t* ft_find_in_session_map(uint64_t transfer_id);
static uint32_t ft_hash_transfer_id(uint64_t transfer_id);

static ft_error_code_t ft_create_session(ft_transfer_session_t** session,
                                        uint64_t user_id, uint64_t peer_user_id,
                                        const char* local_path, const char* remote_path,
                                        const file_metadata_t* metadata,
                                        ft_transfer_mode_t mode, bool is_upload);
static void ft_destroy_session(ft_transfer_session_t* session);
static ft_error_code_t ft_save_session_to_disk(ft_transfer_session_t* session);
static ft_error_code_t ft_load_session_from_disk(uint64_t transfer_id,
                                                ft_transfer_session_t** session);
static ft_error_code_t ft_delete_session_from_disk(uint64_t transfer_id);

static void* ft_manager_thread_func(void* arg);
static ft_error_code_t ft_process_session(ft_transfer_session_t* session);
static ft_error_code_t ft_update_progress(ft_transfer_session_t* session);
static ft_error_code_t ft_check_timeout(ft_transfer_session_t* session);

static ft_error_code_t ft_prepare_upload(ft_transfer_session_t* session);
static ft_error_code_t ft_prepare_download(ft_transfer_session_t* session);
static ft_error_code_t ft_write_chunk(ft_transfer_session_t* session,
                                     uint32_t chunk_index,
                                     const uint8_t* data,
                                     uint32_t data_length);
static ft_error_code_t ft_read_chunk(ft_transfer_session_t* session,
                                    uint32_t chunk_index,
                                    uint8_t** data,
                                    uint32_t* data_length);

static ft_error_code_t ft_encrypt_data(const uint8_t* plaintext, uint32_t plaintext_len,
                                      const uint8_t* key, const uint8_t* iv,
                                      uint8_t* ciphertext, uint32_t* ciphertext_len);
static ft_error_code_t ft_decrypt_data(const uint8_t* ciphertext, uint32_t ciphertext_len,
                                      const uint8_t* key, const uint8_t* iv,
                                      uint8_t* plaintext, uint32_t* plaintext_len);

// ==================== 初始化/清理函数 ====================

ft_error_code_t ft_init(const ft_config_t* config) {
    pthread_once(&g_ft_init_once, ft_init_once);
    
    if (!g_ft_manager) {
        return FT_ERROR_MEMORY;
    }
    
    // 复制配置
    if (config) {
        memcpy(&g_ft_manager->config, config, sizeof(ft_config_t));
    } else {
        memcpy(&g_ft_manager->config, &DEFAULT_FT_CONFIG, sizeof(ft_config_t));
    }
    
    // 验证配置
    ft_error_code_t validate_result = ft_validate_config(&g_ft_manager->config);
    if (validate_result != FT_SUCCESS) {
        return validate_result;
    }
    
    // 初始化目录
    ft_error_code_t dir_result = ft_init_directories();
    if (dir_result != FT_SUCCESS) {
        return dir_result;
    }
    
    // 初始化会话映射
    ft_error_code_t map_result = ft_create_session_map();
    if (map_result != FT_SUCCESS) {
        return map_result;
    }
    
    // 初始化线程池
    g_ft_manager->thread_pool = thread_pool_create(g_ft_manager->config.max_concurrent_transfers);
    if (!g_ft_manager->thread_pool) {
        return FT_ERROR_MEMORY;
    }
    
    // 初始化统计信息
    memset(&g_ft_manager->stats, 0, sizeof(ft_stats_t));
    g_ft_manager->stats.uptime_seconds = time(NULL);
    
    // 初始化链表
    g_ft_manager->active_sessions = NULL;
    g_ft_manager->completed_sessions = NULL;
    
    printf("File transfer module initialized\n");
    printf("  Storage path: %s\n", g_ft_manager->config.storage_path);
    printf("  Temp path: %s\n", g_ft_manager->config.temp_path);
    printf("  Max concurrent transfers: %u\n", g_ft_manager->config.max_concurrent_transfers);
    printf("  Chunk size: %u bytes\n", g_ft_manager->config.chunk_size);
    
    return FT_SUCCESS;
}

static void ft_init_once(void) {
    // 分配管理器内存
    g_ft_manager = (ft_manager_t*)safe_calloc(1, sizeof(ft_manager_t), "ft_manager");
    if (!g_ft_manager) {
        fprintf(stderr, "Failed to allocate file transfer manager\n");
        return;
    }
    
    // 初始化互斥锁
    if (pthread_mutex_init(&g_ft_manager->sessions_mutex, NULL) != 0) {
        fprintf(stderr, "Failed to initialize sessions mutex\n");
        safe_free((void**)&g_ft_manager);
        return;
    }
    
    g_ft_manager->running = false;
}

ft_error_code_t ft_init_with_path(const char* storage_path, const char* temp_path) {
    ft_config_t config = DEFAULT_FT_CONFIG;
    
    if (storage_path) {
        strncpy(config.storage_path, storage_path, sizeof(config.storage_path) - 1);
    }
    
    if (temp_path) {
        strncpy(config.temp_path, temp_path, sizeof(config.temp_path) - 1);
    }
    
    return ft_init(&config);
}

void ft_cleanup(void) {
    if (!g_ft_manager) {
        return;
    }
    
    // 停止管理器线程
    ft_stop_manager();
    
    // 销毁线程池
    if (g_ft_manager->thread_pool) {
        thread_pool_destroy(g_ft_manager->thread_pool);
        g_ft_manager->thread_pool = NULL;
    }
    
    // 清理所有会话
    pthread_mutex_lock(&g_ft_manager->sessions_mutex);
    
    // 清理活跃会话
    ft_transfer_session_t* current = g_ft_manager->active_sessions;
    while (current) {
        ft_transfer_session_t* next = current->next;
        ft_destroy_session(current);
        current = next;
    }
    g_ft_manager->active_sessions = NULL;
    
    // 清理已完成会话
    current = g_ft_manager->completed_sessions;
    while (current) {
        ft_transfer_session_t* next = current->next;
        ft_destroy_session(current);
        current = next;
    }
    g_ft_manager->completed_sessions = NULL;
    
    pthread_mutex_unlock(&g_ft_manager->sessions_mutex);
    
    // 销毁会话映射
    ft_destroy_session_map();
    
    // 销毁互斥锁
    pthread_mutex_destroy(&g_ft_manager->sessions_mutex);
    
    // 清理临时文件
    ft_cleanup_temp_files();
    
    // 释放管理器
    safe_free((void**)&g_ft_manager);
    
    printf("File transfer module cleaned up\n");
}

static ft_error_code_t ft_init_directories(void) {
    struct stat st = {0};
    
    // 创建存储目录
    if (stat(g_ft_manager->config.storage_path, &st) == -1) {
        if (mkdir(g_ft_manager->config.storage_path, 0755) != 0) {
            fprintf(stderr, "Failed to create storage directory: %s\n", 
                    g_ft_manager->config.storage_path);
            return FT_ERROR_IO;
        }
    }
    
    // 创建临时目录
    if (stat(g_ft_manager->config.temp_path, &st) == -1) {
        if (mkdir(g_ft_manager->config.temp_path, 0755) != 0) {
            fprintf(stderr, "Failed to create temp directory: %s\n",
                    g_ft_manager->config.temp_path);
            return FT_ERROR_IO;
        }
    }
    
    // 创建子目录结构
    char subdir_path[FT_MAX_FILE_PATH];
    
    // 创建上传目录
    snprintf(subdir_path, sizeof(subdir_path), "%s/uploads", g_ft_manager->config.storage_path);
    if (stat(subdir_path, &st) == -1) {
        mkdir(subdir_path, 0755);
    }
    
    // 创建下载目录
    snprintf(subdir_path, sizeof(subdir_path), "%s/downloads", g_ft_manager->config.storage_path);
    if (stat(subdir_path, &st) == -1) {
        mkdir(subdir_path, 0755);
    }
    
    // 创建会话目录
    snprintf(subdir_path, sizeof(subdir_path), "%s/sessions", g_ft_manager->config.temp_path);
    if (stat(subdir_path, &st) == -1) {
        mkdir(subdir_path, 0755);
    }
    
    // 创建chunk目录
    snprintf(subdir_path, sizeof(subdir_path), "%s/chunks", g_ft_manager->config.temp_path);
    if (stat(subdir_path, &st) == -1) {
        mkdir(subdir_path, 0755);
    }
    
    return FT_SUCCESS;
}

// ==================== 哈希表管理 ====================

static ft_error_code_t ft_create_session_map(void) {
    g_ft_manager->session_map.size = FT_HASH_TABLE_SIZE;
    g_ft_manager->session_map.count = 0;
    
    g_ft_manager->session_map.table = (ft_transfer_session_t**)safe_calloc(
        FT_HASH_TABLE_SIZE, sizeof(ft_transfer_session_t*), "session_hash_table");
    
    if (!g_ft_manager->session_map.table) {
        return FT_ERROR_MEMORY;
    }
    
    return FT_SUCCESS;
}

static void ft_destroy_session_map(void) {
    if (g_ft_manager->session_map.table) {
        safe_free((void**)&g_ft_manager->session_map.table);
        g_ft_manager->session_map.size = 0;
        g_ft_manager->session_map.count = 0;
    }
}

static uint32_t ft_hash_transfer_id(uint64_t transfer_id) {
    // 简单的乘法哈希
    return (uint32_t)(transfer_id % FT_HASH_TABLE_SIZE);
}

static ft_error_code_t ft_add_to_session_map(ft_transfer_session_t* session) {
    if (!session) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    uint32_t index = ft_hash_transfer_id(session->transfer_id);
    
    // 添加到链表头部
    session->next = g_ft_manager->session_map.table[index];
    session->prev = NULL;
    
    if (g_ft_manager->session_map.table[index]) {
        g_ft_manager->session_map.table[index]->prev = session;
    }
    
    g_ft_manager->session_map.table[index] = session;
    g_ft_manager->session_map.count++;
    
    return FT_SUCCESS;
}

static ft_error_code_t ft_remove_from_session_map(uint64_t transfer_id) {
    uint32_t index = ft_hash_transfer_id(transfer_id);
    ft_transfer_session_t* current = g_ft_manager->session_map.table[index];
    
    while (current) {
        if (current->transfer_id == transfer_id) {
            // 从链表中移除
            if (current->prev) {
                current->prev->next = current->next;
            } else {
                // 是链表头部
                g_ft_manager->session_map.table[index] = current->next;
            }
            
            if (current->next) {
                current->next->prev = current->prev;
            }
            
            g_ft_manager->session_map.count--;
            return FT_SUCCESS;
        }
        current = current->next;
    }
    
    return FT_ERROR_TRANSFER_NOT_FOUND;
}

static ft_transfer_session_t* ft_find_in_session_map(uint64_t transfer_id) {
    uint32_t index = ft_hash_transfer_id(transfer_id);
    ft_transfer_session_t* current = g_ft_manager->session_map.table[index];
    
    while (current) {
        if (current->transfer_id == transfer_id) {
            return current;
        }
        current = current->next;
    }
    
    return NULL;
}

// ==================== 会话管理 ====================

static ft_error_code_t ft_create_session(ft_transfer_session_t** session,
                                        uint64_t user_id, uint64_t peer_user_id,
                                        const char* local_path, const char* remote_path,
                                        const file_metadata_t* metadata,
                                        ft_transfer_mode_t mode, bool is_upload) {
    if (!session || (!local_path && !remote_path)) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    // 分配会话内存
    ft_transfer_session_t* new_session = (ft_transfer_session_t*)safe_calloc(
        1, sizeof(ft_transfer_session_t), "transfer_session");
    
    if (!new_session) {
        return FT_ERROR_MEMORY;
    }
    
    // 生成传输ID
    new_session->transfer_id = ft_generate_transfer_id();
    new_session->user_id = user_id;
    new_session->peer_user_id = peer_user_id;
    new_session->mode = mode;
    new_session->status = FT_STATUS_PENDING;
    
    // 复制路径
    if (local_path) {
        strncpy(new_session->local_path, local_path, sizeof(new_session->local_path) - 1);
    }
    
    if (remote_path) {
        strncpy(new_session->remote_path, remote_path, sizeof(new_session->remote_path) - 1);
    }
    
    // 生成临时文件路径
    char temp_filename[64];
    snprintf(temp_filename, sizeof(temp_filename), "transfer_%016llx.tmp",
             (unsigned long long)new_session->transfer_id);
    
    snprintf(new_session->temp_path, sizeof(new_session->temp_path), "%s/chunks/%s",
             g_ft_manager->config.temp_path, temp_filename);
    
    // 复制元数据
    if (metadata) {
        memcpy(&new_session->metadata, metadata, sizeof(file_metadata_t));
        new_session->total_bytes = metadata->file_size;
        new_session->total_chunks = (metadata->file_size + g_ft_manager->config.chunk_size - 1) /
                                   g_ft_manager->config.chunk_size;
    }
    
    // 设置加密
    new_session->encryption_enabled = g_ft_manager->config.enable_encryption;
    if (new_session->encryption_enabled) {
        if (!RAND_bytes(new_session->encryption_key, sizeof(new_session->encryption_key)) ||
            !RAND_bytes(new_session->encryption_iv, sizeof(new_session->encryption_iv))) {
            fprintf(stderr, "Failed to generate encryption keys\n");
            safe_free((void**)&new_session);
            return FT_ERROR_ENCRYPTION;
        }
    }
    
    // 设置压缩
    new_session->compression_enabled = g_ft_manager->config.enable_compression;
    new_session->compression_level = 6;  // 默认压缩级别
    
    // 初始化传输状态
    new_session->current_chunk = 0;
    new_session->bytes_transferred = 0;
    new_session->retry_count = 0;
    new_session->error_chunk_count = 0;
    
    // 初始化时间
    new_session->start_time = time(NULL);
    new_session->last_activity_time = new_session->start_time;
    
    // 初始化互斥锁和条件变量
    if (pthread_mutex_init(&new_session->mutex, NULL) != 0) {
        fprintf(stderr, "Failed to initialize session mutex\n");
        safe_free((void**)&new_session);
        return FT_ERROR_MEMORY;
    }
    
    if (pthread_cond_init(&new_session->cond, NULL) != 0) {
        fprintf(stderr, "Failed to initialize session condition variable\n");
        pthread_mutex_destroy(&new_session->mutex);
        safe_free((void**)&new_session);
        return FT_ERROR_MEMORY;
    }
    
    // 添加到会话映射
    ft_error_code_t map_result = ft_add_to_session_map(new_session);
    if (map_result != FT_SUCCESS) {
        pthread_cond_destroy(&new_session->cond);
        pthread_mutex_destroy(&new_session->mutex);
        safe_free((void**)&new_session);
        return map_result;
    }
    
    // 添加到活跃会话链表
    pthread_mutex_lock(&g_ft_manager->sessions_mutex);
    
    new_session->next = g_ft_manager->active_sessions;
    new_session->prev = NULL;
    
    if (g_ft_manager->active_sessions) {
        g_ft_manager->active_sessions->prev = new_session;
    }
    
    g_ft_manager->active_sessions = new_session;
    
    pthread_mutex_unlock(&g_ft_manager->sessions_mutex);
    
    // 更新统计信息
    g_ft_manager->stats.total_transfers++;
    g_ft_manager->stats.active_transfers++;
    
    *session = new_session;
    
    printf("Created transfer session %016llx for user %llu\n",
           (unsigned long long)new_session->transfer_id,
           (unsigned long long)user_id);
    
    return FT_SUCCESS;
}

static void ft_destroy_session(ft_transfer_session_t* session) {
    if (!session) {
        return;
    }
    
    // 清理临时文件
    if (session->temp_path[0] != '\0') {
        unlink(session->temp_path);
    }
    
    // 销毁互斥锁和条件变量
    pthread_cond_destroy(&session->cond);
    pthread_mutex_destroy(&session->mutex);
    
    // 从会话映射中移除
    ft_remove_from_session_map(session->transfer_id);
    
    // 释放内存
    safe_free((void**)&session);
}

// ==================== 传输管理器 ====================

ft_error_code_t ft_start_manager(void) {
    if (!g_ft_manager) {
        return FT_ERROR_INVALID_STATE;
    }
    
    if (g_ft_manager->running) {
        return FT_SUCCESS;  // 已经在运行
    }
    
    g_ft_manager->running = true;
    
    // 创建管理器线程
    if (pthread_create(&g_ft_manager->manager_thread, NULL,
                      ft_manager_thread_func, g_ft_manager) != 0) {
        g_ft_manager->running = false;
        return FT_ERROR_MEMORY;
    }
    
    printf("File transfer manager started\n");
    return FT_SUCCESS;
}

ft_error_code_t ft_stop_manager(void) {
    if (!g_ft_manager || !g_ft_manager->running) {
        return FT_SUCCESS;
    }
    
    g_ft_manager->running = false;
    
    // 等待管理器线程结束
    if (g_ft_manager->manager_thread) {
        pthread_join(g_ft_manager->manager_thread, NULL);
        g_ft_manager->manager_thread = 0;
    }
    
    printf("File transfer manager stopped\n");
    return FT_SUCCESS;
}

static void* ft_manager_thread_func(void* arg) {
    ft_manager_t* manager = (ft_manager_t*)arg;
    
    printf("File transfer manager thread started\n");
    
    while (manager->running) {
        // 处理活跃会话
        pthread_mutex_lock(&manager->sessions_mutex);
        
        ft_transfer_session_t* current = manager->active_sessions;
        while (current && manager->running) {
            ft_transfer_session_t* next = current->next;
            
            // 处理会话
            ft_error_code_t result = ft_process_session(current);
            if (result != FT_SUCCESS) {
                // 处理失败
                if (current->error_callback) {
                    current->error_callback(current, result, current->callback_user_data);
                }
                
                // 更新状态
                current->status = FT_STATUS_FAILED;
                
                // 移动到已完成列表
                ft_transfer_session_t* prev = current->prev;
                ft_transfer_session_t* nxt = current->next;
                
                if (prev) {
                    prev->next = nxt;
                } else {
                    // 是链表头部
                    manager->active_sessions = nxt;
                }
                
                if (nxt) {
                    nxt->prev = prev;
                }
                
                // 添加到已完成列表
                current->next = manager->completed_sessions;
                current->prev = NULL;
                
                if (manager->completed_sessions) {
                    manager->completed_sessions->prev = current;
                }
                
                manager->completed_sessions = current;
                
                // 更新统计信息
                manager->stats.failed_transfers++;
                manager->stats.active_transfers--;
            }
            
            current = next;
        }
        
        pthread_mutex_unlock(&manager->sessions_mutex);
        
        // 检查超时会话
        current = manager->active_sessions;
        while (current) {
            ft_check_timeout(current);
            current = current->next;
        }
        
        // 清理旧的已完成会话
        ft_cleanup_completed_sessions(time(NULL) - 3600);  // 清理1小时前的会话
        
        // 更新统计信息
        manager->stats.uptime_seconds = time(NULL) - manager->stats.uptime_seconds;
        
        // 休眠一段时间
        usleep(100000);  // 100ms
    }
    
    printf("File transfer manager thread exiting\n");
    return NULL;
}

static ft_error_code_t ft_process_session(ft_transfer_session_t* session) {
    if (!session) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    pthread_mutex_lock(&session->mutex);
    
    // 根据状态处理会话
    ft_error_code_t result = FT_SUCCESS;
    
    switch (session->status) {
        case FT_STATUS_PENDING:
            // 准备传输
            if (strstr(session->local_path, "upload") != NULL) {
                result = ft_prepare_upload(session);
            } else {
                result = ft_prepare_download(session);
            }
            
            if (result == FT_SUCCESS) {
                session->status = FT_STATUS_TRANSFERRING;
            }
            break;
            
        case FT_STATUS_TRANSFERRING:
            // 更新进度
            result = ft_update_progress(session);
            break;
            
        case FT_STATUS_PAUSED:
            // 暂停状态，不处理
            break;
            
        case FT_STATUS_VERIFYING:
            // 验证文件完整性
            if (g_ft_manager->config.verify_checksum &&
                session->expected_checksum[0] != '\0') {
                
                result = ft_verify_file_checksum(session->temp_path,
                                                session->expected_checksum);
                
                if (result == FT_SUCCESS) {
                    session->status = FT_STATUS_COMPLETED;
                } else {
                    session->status = FT_STATUS_FAILED;
                }
            } else {
                // 不需要验证，直接完成
                session->status = FT_STATUS_COMPLETED;
            }
            break;
            
        case FT_STATUS_COMPLETED:
            // 完成传输
            if (session->completion_callback) {
                session->completion_callback(session, session->callback_user_data);
            }
            
            // 移动文件到最终位置
            if (rename(session->temp_path, session->local_path) != 0) {
                result = FT_ERROR_IO;
                session->status = FT_STATUS_FAILED;
            } else {
                // 更新统计信息
                g_ft_manager->stats.successful_transfers++;
                g_ft_manager->stats.bytes_transferred += session->total_bytes;
                
                // 计算速度
                time_t elapsed = time(NULL) - session->start_time;
                if (elapsed > 0) {
                    double speed = (double)session->total_bytes / elapsed;
                    g_ft_manager->stats.average_speed_bytes_per_sec = 
                        (g_ft_manager->stats.average_speed_bytes_per_sec * 
                         g_ft_manager->stats.successful_transfers + speed) /
                         (g_ft_manager->stats.successful_transfers + 1);
                    
                    if (speed > g_ft_manager->stats.peak_speed_bytes_per_sec) {
                        g_ft_manager->stats.peak_speed_bytes_per_sec = speed;
                    }
                }
            }
            break;
            
        default:
            // 其他状态不处理
            break;
    }
    
    session->last_activity_time = time(NULL);
    pthread_mutex_unlock(&session->mutex);
    
    return result;
}

static ft_error_code_t ft_update_progress(ft_transfer_session_t* session) {
    if (!session) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    // 检查文件大小
    struct stat st;
    if (stat(session->temp_path, &st) == 0) {
        session->bytes_transferred = st.st_size;
        session->current_chunk = session->bytes_transferred / g_ft_manager->config.chunk_size;
        
        // 计算进度百分比
        if (session->total_bytes > 0) {
            double progress = (double)session->bytes_transferred / session->total_bytes * 100.0;
            
            // 调用进度回调
            if (session->progress_callback) {
                session->progress_callback(session, session->callback_user_data);
            }
            
            // 估计完成时间
            if (session->bytes_transferred > 0) {
                time_t elapsed = time(NULL) - session->start_time;
                if (elapsed > 0) {
                    double speed = (double)session->bytes_transferred / elapsed;
                    double remaining = (session->total_bytes - session->bytes_transferred) / speed;
                    session->estimated_completion_time = time(NULL) + (time_t)remaining;
                }
            }
        }
    }
    
    return FT_SUCCESS;
}

static ft_error_code_t ft_check_timeout(ft_transfer_session_t* session) {
    if (!session) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    time_t now = time(NULL);
    time_t inactive_time = now - session->last_activity_time;
    
    // 检查是否超时
    if (inactive_time > g_ft_manager->config.timeout_ms / 1000) {
        pthread_mutex_lock(&session->mutex);
        
        if (session->status == FT_STATUS_TRANSFERRING ||
            session->status == FT_STATUS_PENDING) {
            
            session->status = FT_STATUS_FAILED;
            
            if (session->error_callback) {
                session->error_callback(session, FT_ERROR_TIMEOUT,
                                       session->callback_user_data);
            }
            
            printf("Transfer %016llx timed out after %ld seconds of inactivity\n",
                   (unsigned long long)session->transfer_id, inactive_time);
        }
        
        pthread_mutex_unlock(&session->mutex);
        
        return FT_ERROR_TIMEOUT;
    }
    
    return FT_SUCCESS;
}

// ==================== 文件上传 ====================

ft_error_code_t ft_start_upload(uint64_t user_id, uint64_t peer_user_id,
                               const char* local_path, const char* remote_path,
                               const file_metadata_t* metadata,
                               ft_transfer_mode_t mode,
                               ft_transfer_session_t** session) {
    if (!local_path || !remote_path || !metadata) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    // 检查文件是否存在
    struct stat st;
    if (stat(local_path, &st) != 0) {
        return FT_ERROR_FILE_NOT_FOUND;
    }
    
    // 检查文件大小
    if ((uint64_t)st.st_size > g_ft_manager->config.max_file_size) {
        return FT_ERROR_FILE_TOO_LARGE;
    }
    
    // 检查磁盘空间
    uint64_t free_space = 0;
    ft_get_storage_usage(NULL, &free_space);
    
    if ((uint64_t)st.st_size > free_space) {
        return FT_ERROR_DISK_FULL;
    }
    
    // 创建会话
    ft_error_code_t result = ft_create_session(session, user_id, peer_user_id,
                                              local_path, remote_path, metadata,
                                              mode, true);
    
    if (result != FT_SUCCESS) {
        return result;
    }
    
    // 准备上传
    result = ft_prepare_upload(*session);
    
    if (result != FT_SUCCESS) {
        ft_remove_session((*session)->transfer_id);
        *session = NULL;
        return result;
    }
    
    // 计算文件校验和
    if (g_ft_manager->config.verify_checksum) {
        result = ft_calculate_file_checksum(local_path, (*session)->expected_checksum);
        if (result != FT_SUCCESS) {
            fprintf(stderr, "Failed to calculate file checksum\n");
            // 继续，校验和不是必需的
        }
    }
    
    printf("Started upload %016llx: %s -> %s (%llu bytes)\n",
           (unsigned long long)(*session)->transfer_id,
           local_path, remote_path, (unsigned long long)st.st_size);
    
    return FT_SUCCESS;
}

static ft_error_code_t ft_prepare_upload(ft_transfer_session_t* session) {
    if (!session) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    pthread_mutex_lock(&session->mutex);
    
    // 创建临时文件
    FILE* temp_file = fopen(session->temp_path, "wb");
    if (!temp_file) {
        pthread_mutex_unlock(&session->mutex);
        return FT_ERROR_IO;
    }
    
    fclose(temp_file);
    
    // 打开源文件
    FILE* source_file = fopen(session->local_path, "rb");
    if (!source_file) {
        pthread_mutex_unlock(&session->mutex);
        return FT_ERROR_FILE_NOT_FOUND;
    }
    
    // 计算文件大小和块数
    fseek(source_file, 0, SEEK_END);
    long file_size = ftell(source_file);
    fseek(source_file, 0, SEEK_SET);
    
    session->total_bytes = file_size;
    session->total_chunks = (file_size + g_ft_manager->config.chunk_size - 1) /
                           g_ft_manager->config.chunk_size;
    
    fclose(source_file);
    
    session->status = FT_STATUS_PREPARING;
    session->last_activity_time = time(NULL);
    
    pthread_mutex_unlock(&session->mutex);
    
    return FT_SUCCESS;
}

ft_error_code_t ft_upload_chunk(ft_transfer_session_t* session,
                               uint32_t chunk_index,
                               const uint8_t* data,
                               uint32_t data_length) {
    if (!session || !data || data_length == 0) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    pthread_mutex_lock(&session->mutex);
    
    // 检查状态
    if (session->status != FT_STATUS_TRANSFERRING &&
        session->status != FT_STATUS_PREPARING) {
        pthread_mutex_unlock(&session->mutex);
        return FT_ERROR_INVALID_STATE;
    }
    
    // 检查chunk索引
    if (chunk_index >= session->total_chunks) {
        pthread_mutex_unlock(&session->mutex);
        return FT_ERROR_INVALID_CHUNK;
    }
    
    // 写入chunk
    ft_error_code_t result = ft_write_chunk(session, chunk_index, data, data_length);
    
    if (result == FT_SUCCESS) {
        // 更新进度
        session->bytes_transferred += data_length;
        session->current_chunk = chunk_index + 1;
        
        // 检查是否完成
        if (session->bytes_transferred >= session->total_bytes) {
            session->status = FT_STATUS_VERIFYING;
        }
        
        session->last_activity_time = time(NULL);
        
        // 调用进度回调
        if (session->progress_callback) {
            session->progress_callback(session, session->callback_user_data);
        }
    } else {
        // 记录错误chunk
        if (session->error_chunk_count < sizeof(session->error_chunks) / sizeof(session->error_chunks[0])) {
            session->error_chunks[session->error_chunk_count++] = chunk_index;
        }
        
        // 检查重试次数
        if (session->retry_count < g_ft_manager->config.max_retries) {
            session->retry_count++;
            printf("Retry %u for chunk %u of transfer %016llx\n",
                   session->retry_count, chunk_index,
                   (unsigned long long)session->transfer_id);
        } else {
            session->status = FT_STATUS_FAILED;
            
            if (session->error_callback) {
                session->error_callback(session, result, session->callback_user_data);
            }
        }
    }
    
    pthread_mutex_unlock(&session->mutex);
    
    return result;
}

static ft_error_code_t ft_write_chunk(ft_transfer_session_t* session,
                                     uint32_t chunk_index,
                                     const uint8_t* data,
                                     uint32_t data_length) {
    if (!session || !data) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    // 打开临时文件
    FILE* file = fopen(session->temp_path, "r+b");
    if (!file) {
        file = fopen(session->temp_path, "wb");
        if (!file) {
            return FT_ERROR_IO;
        }
    }
    
    // 定位到正确位置
    size_t offset = (size_t)chunk_index * g_ft_manager->config.chunk_size;
    if (fseek(file, offset, SEEK_SET) != 0) {
        fclose(file);
        return FT_ERROR_IO;
    }
    
    // 处理数据（加密/压缩）
    uint8_t* processed_data = NULL;
    uint32_t processed_length = data_length;
    
    if (session->encryption_enabled) {
        // 解密数据（假设收到的数据是加密的）
        uint8_t* decrypted_data = (uint8_t*)safe_malloc(data_length, "decrypted_chunk");
        if (!decrypted_data) {
            fclose(file);
            return FT_ERROR_MEMORY;
        }
        
        ft_error_code_t decrypt_result = ft_decrypt_data(
            data, data_length,
            session->encryption_key, session->encryption_iv,
            decrypted_data, &processed_length);
        
        if (decrypt_result != FT_SUCCESS) {
            safe_free((void**)&decrypted_data);
            fclose(file);
            return decrypt_result;
        }
        
        processed_data = decrypted_data;
    } else if (session->compression_enabled) {
        // 解压数据
        uint8_t* decompressed_data = (uint8_t*)safe_malloc(
            g_ft_manager->config.chunk_size, "decompressed_chunk");
        if (!decompressed_data) {
            fclose(file);
            return FT_ERROR_MEMORY;
        }
        
        ft_error_code_t decompress_result = ft_decompress_chunk(
            data, data_length,
            decompressed_data, &processed_length);
        
        if (decompress_result != FT_SUCCESS) {
            safe_free((void**)&decompressed_data);
            fclose(file);
            return decompress_result;
        }
        
        processed_data = decompressed_data;
    } else {
        // 直接使用原始数据
        processed_data = (uint8_t*)data;
    }
    
    // 写入数据
    size_t bytes_written = fwrite(processed_data, 1, processed_length, file);
    
    // 清理
    if (processed_data != data) {
        safe_free((void**)&processed_data);
    }
    
    fclose(file);
    
    if (bytes_written != processed_length) {
        return FT_ERROR_IO;
    }
    
    return FT_SUCCESS;
}

ft_error_code_t ft_complete_upload(ft_transfer_session_t* session) {
    if (!session) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    pthread_mutex_lock(&session->mutex);
    
    // 验证文件完整性
    if (g_ft_manager->config.verify_checksum &&
        session->expected_checksum[0] != '\0') {
        
        ft_error_code_t verify_result = ft_verify_file_checksum(
            session->temp_path, session->expected_checksum);
        
        if (verify_result != FT_SUCCESS) {
            pthread_mutex_unlock(&session->mutex);
            return verify_result;
        }
    }
    
    // 重命名临时文件到最终位置
    if (rename(session->temp_path, session->local_path) != 0) {
        pthread_mutex_unlock(&session->mutex);
        return FT_ERROR_IO;
    }
    
    session->status = FT_STATUS_COMPLETED;
    session->last_activity_time = time(NULL);
    
    // 调用完成回调
    if (session->completion_callback) {
        session->completion_callback(session, session->callback_user_data);
    }
    
    pthread_mutex_unlock(&session->mutex);
    
    printf("Completed upload %016llx\n",
           (unsigned long long)session->transfer_id);
    
    return FT_SUCCESS;
}

// ==================== 文件下载 ====================

ft_error_code_t ft_start_download(uint64_t user_id, uint64_t peer_user_id,
                                 const char* remote_path, const char* local_path,
                                 ft_transfer_mode_t mode,
                                 ft_transfer_session_t** session) {
    if (!remote_path || !local_path) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    // 检查远程文件是否存在
    struct stat st;
    if (stat(remote_path, &st) != 0) {
        return FT_ERROR_FILE_NOT_FOUND;
    }
    
    // 检查磁盘空间
    uint64_t free_space = 0;
    ft_get_storage_usage(NULL, &free_space);
    
    if ((uint64_t)st.st_size > free_space) {
        return FT_ERROR_DISK_FULL;
    }
    
    // 创建文件元数据
    file_metadata_t metadata;
    memset(&metadata, 0, sizeof(metadata));
    
    metadata.file_size = st.st_size;
    strncpy(metadata.filename, remote_path, sizeof(metadata.filename) - 1);
    
    // 提取文件名
    char* filename = strrchr(remote_path, '/');
    if (filename) {
        filename++;
    } else {
        filename = (char*)remote_path;
    }
    
    strncpy(metadata.original_name, filename, sizeof(metadata.original_name) - 1);
    
    // 确定文件类型
    const char* ext = strrchr(filename, '.');
    if (ext) {
        ext++;
        if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0 ||
            strcasecmp(ext, "png") == 0 || strcasecmp(ext, "gif") == 0) {
            metadata.file_type = FILE_TYPE_IMAGE;
        } else if (strcasecmp(ext, "mp3") == 0 || strcasecmp(ext, "wav") == 0 ||
                   strcasecmp(ext, "ogg") == 0) {
            metadata.file_type = FILE_TYPE_AUDIO;
        } else if (strcasecmp(ext, "mp4") == 0 || strcasecmp(ext, "avi") == 0 ||
                   strcasecmp(ext, "mkv") == 0) {
            metadata.file_type = FILE_TYPE_VIDEO;
        } else if (strcasecmp(ext, "pdf") == 0 || strcasecmp(ext, "doc") == 0 ||
                   strcasecmp(ext, "txt") == 0) {
            metadata.file_type = FILE_TYPE_DOCUMENT;
        } else if (strcasecmp(ext, "zip") == 0 || strcasecmp(ext, "tar") == 0 ||
                   strcasecmp(ext, "gz") == 0) {
            metadata.file_type = FILE_TYPE_ARCHIVE;
        } else {
            metadata.file_type = FILE_TYPE_UNKNOWN;
        }
    }
    
    // 创建会话
    ft_error_code_t result = ft_create_session(session, user_id, peer_user_id,
                                              local_path, remote_path, &metadata,
                                              mode, false);
    
    if (result != FT_SUCCESS) {
        return result;
    }
    
    // 准备下载
    result = ft_prepare_download(*session);
    
    if (result != FT_SUCCESS) {
        ft_remove_session((*session)->transfer_id);
        *session = NULL;
        return result;
    }
    
    printf("Started download %016llx: %s -> %s (%llu bytes)\n",
           (unsigned long long)(*session)->transfer_id,
           remote_path, local_path, (unsigned long long)st.st_size);
    
    return FT_SUCCESS;
}

static ft_error_code_t ft_prepare_download(ft_transfer_session_t* session) {
    if (!session) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    pthread_mutex_lock(&session->mutex);
    
    // 打开源文件
    FILE* source_file = fopen(session->remote_path, "rb");
    if (!source_file) {
        pthread_mutex_unlock(&session->mutex);
        return FT_ERROR_FILE_NOT_FOUND;
    }
    
    // 计算文件大小和块数
    fseek(source_file, 0, SEEK_END);
    long file_size = ftell(source_file);
    fseek(source_file, 0, SEEK_SET);
    
    session->total_bytes = file_size;
    session->total_chunks = (file_size + g_ft_manager->config.chunk_size - 1) /
                           g_ft_manager->config.chunk_size;
    
    fclose(source_file);
    
    // 创建临时文件
    FILE* temp_file = fopen(session->temp_path, "wb");
    if (!temp_file) {
        pthread_mutex_unlock(&session->mutex);
        return FT_ERROR_IO;
    }
    
    fclose(temp_file);
    
    session->status = FT_STATUS_PREPARING;
    session->last_activity_time = time(NULL);
    
    pthread_mutex_unlock(&session->mutex);
    
    return FT_SUCCESS;
}

ft_error_code_t ft_download_chunk(ft_transfer_session_t* session,
                                 uint32_t chunk_index,
                                 uint8_t** data,
                                 uint32_t* data_length) {
    if (!session || !data || !data_length) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    pthread_mutex_lock(&session->mutex);
    
    // 检查状态
    if (session->status != FT_STATUS_TRANSFERRING &&
        session->status != FT_STATUS_PREPARING) {
        pthread_mutex_unlock(&session->mutex);
        return FT_ERROR_INVALID_STATE;
    }
    
    // 检查chunk索引
    if (chunk_index >= session->total_chunks) {
        pthread_mutex_unlock(&session->mutex);
        return FT_ERROR_INVALID_CHUNK;
    }
    
    // 读取chunk
    ft_error_code_t result = ft_read_chunk(session, chunk_index, data, data_length);
    
    if (result == FT_SUCCESS) {
        // 更新进度
        session->bytes_transferred += *data_length;
        session->current_chunk = chunk_index + 1;
        
        // 检查是否完成
        if (session->bytes_transferred >= session->total_bytes) {
            session->status = FT_STATUS_VERIFYING;
        }
        
        session->last_activity_time = time(NULL);
        
        // 调用进度回调
        if (session->progress_callback) {
            session->progress_callback(session, session->callback_user_data);
        }
    }
    
    pthread_mutex_unlock(&session->mutex);
    
    return result;
}

static ft_error_code_t ft_read_chunk(ft_transfer_session_t* session,
                                    uint32_t chunk_index,
                                    uint8_t** data,
                                    uint32_t* data_length) {
    if (!session || !data || !data_length) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    // 打开源文件
    FILE* file = fopen(session->remote_path, "rb");
    if (!file) {
        return FT_ERROR_FILE_NOT_FOUND;
    }
    
    // 计算chunk大小
    size_t offset = (size_t)chunk_index * g_ft_manager->config.chunk_size;
    size_t chunk_size = g_ft_manager->config.chunk_size;
    
    // 如果是最后一个chunk，可能小于标准大小
    if (chunk_index == session->total_chunks - 1) {
        chunk_size = session->total_bytes - offset;
    }
    
    // 定位到正确位置
    if (fseek(file, offset, SEEK_SET) != 0) {
        fclose(file);
        return FT_ERROR_IO;
    }
    
    // 读取数据
    uint8_t* chunk_data = (uint8_t*)safe_malloc(chunk_size, "chunk_data");
    if (!chunk_data) {
        fclose(file);
        return FT_ERROR_MEMORY;
    }
    
    size_t bytes_read = fread(chunk_data, 1, chunk_size, file);
    fclose(file);
    
    if (bytes_read != chunk_size) {
        safe_free((void**)&chunk_data);
        return FT_ERROR_IO;
    }
    
    // 处理数据（加密/压缩）
    uint8_t* processed_data = NULL;
    uint32_t processed_length = chunk_size;
    
    if (session->encryption_enabled) {
        // 加密数据
        uint8_t* encrypted_data = (uint8_t*)safe_malloc(
            chunk_size + AES_BLOCK_SIZE, "encrypted_chunk");  // 预留填充空间
        
        if (!encrypted_data) {
            safe_free((void**)&chunk_data);
            return FT_ERROR_MEMORY;
        }
        
        ft_error_code_t encrypt_result = ft_encrypt_data(
            chunk_data, chunk_size,
            session->encryption_key, session->encryption_iv,
            encrypted_data, &processed_length);
        
        safe_free((void**)&chunk_data);
        
        if (encrypt_result != FT_SUCCESS) {
            safe_free((void**)&encrypted_data);
            return encrypt_result;
        }
        
        processed_data = encrypted_data;
    } else if (session->compression_enabled) {
        // 压缩数据
        uint8_t* compressed_data = (uint8_t*)safe_malloc(
            chunk_size * 2, "compressed_chunk");  // 预留额外空间
        
        if (!compressed_data) {
            safe_free((void**)&chunk_data);
            return FT_ERROR_MEMORY;
        }
        
        ft_error_code_t compress_result = ft_compress_chunk(
            chunk_data, chunk_size,
            compressed_data, &processed_length,
            session->compression_level);
        
        safe_free((void**)&chunk_data);
        
        if (compress_result != FT_SUCCESS) {
            safe_free((void**)&compressed_data);
            return compress_result;
        }
        
        processed_data = compressed_data;
    } else {
        // 直接使用原始数据
        processed_data = chunk_data;
    }
    
    // 写入临时文件（用于断点续传）
    FILE* temp_file = fopen(session->temp_path, "r+b");
    if (!temp_file) {
        temp_file = fopen(session->temp_path, "wb");
    }
    
    if (temp_file) {
        fseek(temp_file, offset, SEEK_SET);
        fwrite(processed_data, 1, processed_length, temp_file);
        fclose(temp_file);
    }
    
    *data = processed_data;
    *data_length = processed_length;
    
    return FT_SUCCESS;
}

// ==================== 加密解密 ====================

static ft_error_code_t ft_encrypt_data(const uint8_t* plaintext, uint32_t plaintext_len,
                                      const uint8_t* key, const uint8_t* iv,
                                      uint8_t* ciphertext, uint32_t* ciphertext_len) {
    if (!plaintext || !key || !iv || !ciphertext || !ciphertext_len) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return FT_ERROR_ENCRYPTION;
    }
    
    // 初始化加密上下文
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return FT_ERROR_ENCRYPTION;
    }
    
    int len = 0;
    int ciphertext_len_total = 0;
    
    // 加密数据
    if (EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, plaintext_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return FT_ERROR_ENCRYPTION;
    }
    ciphertext_len_total = len;
    
    // 完成加密
    if (EVP_EncryptFinal_ex(ctx, ciphertext + len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return FT_ERROR_ENCRYPTION;
    }
    ciphertext_len_total += len;
    
    EVP_CIPHER_CTX_free(ctx);
    
    *ciphertext_len = ciphertext_len_total;
    
    return FT_SUCCESS;
}

static ft_error_code_t ft_decrypt_data(const uint8_t* ciphertext, uint32_t ciphertext_len,
                                      const uint8_t* key, const uint8_t* iv,
                                      uint8_t* plaintext, uint32_t* plaintext_len) {
    if (!ciphertext || !key || !iv || !plaintext || !plaintext_len) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return FT_ERROR_DECRYPTION;
    }
    
    // 初始化解密上下文
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return FT_ERROR_DECRYPTION;
    }
    
    int len = 0;
    int plaintext_len_total = 0;
    
    // 解密数据
    if (EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertext_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return FT_ERROR_DECRYPTION;
    }
    plaintext_len_total = len;
    
    // 完成解密
    if (EVP_DecryptFinal_ex(ctx, plaintext + len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return FT_ERROR_DECRYPTION;
    }
    plaintext_len_total += len;
    
    EVP_CIPHER_CTX_free(ctx);
    
    *plaintext_len = plaintext_len_total;
    
    return FT_SUCCESS;
}

// ==================== 压缩解压 ====================

ft_error_code_t ft_compress_chunk(const uint8_t* data, uint32_t data_len,
                                 uint8_t* compressed, uint32_t* compressed_len,
                                 int level) {
    if (!data || !compressed || !compressed_len) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    
    // 初始化zlib流
    if (deflateInit(&stream, level) != Z_OK) {
        return FT_ERROR_COMPRESSION;
    }
    
    stream.next_in = (Bytef*)data;
    stream.avail_in = data_len;
    stream.next_out = (Bytef*)compressed;
    stream.avail_out = *compressed_len;
    
    // 压缩数据
    if (deflate(&stream, Z_FINISH) != Z_STREAM_END) {
        deflateEnd(&stream);
        return FT_ERROR_COMPRESSION;
    }
    
    *compressed_len = stream.total_out;
    
    deflateEnd(&stream);
    
    return FT_SUCCESS;
}

ft_error_code_t ft_decompress_chunk(const uint8_t* compressed, uint32_t compressed_len,
                                   uint8_t* decompressed, uint32_t* decompressed_len) {
    if (!compressed || !decompressed || !decompressed_len) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    
    // 初始化zlib流
    if (inflateInit(&stream) != Z_OK) {
        return FT_ERROR_DECOMPRESSION;
    }
    
    stream.next_in = (Bytef*)compressed;
    stream.avail_in = compressed_len;
    stream.next_out = (Bytef*)decompressed;
    stream.avail_out = *decompressed_len;
    
    // 解压数据
    int ret = inflate(&stream, Z_FINISH);
    if (ret != Z_STREAM_END) {
        inflateEnd(&stream);
        return FT_ERROR_DECOMPRESSION;
    }
    
    *decompressed_len = stream.total_out;
    
    inflateEnd(&stream);
    
    return FT_SUCCESS;
}

// ==================== 校验和验证 ====================

ft_error_code_t ft_calculate_file_checksum(const char* filepath, char* checksum) {
    if (!filepath || !checksum) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    FILE* file = fopen(filepath, "rb");
    if (!file) {
        return FT_ERROR_FILE_NOT_FOUND;
    }
    
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    
    uint8_t buffer[FT_CHECKSUM_CHUNK_SIZE];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        SHA256_Update(&sha256, buffer, bytes_read);
    }
    
    fclose(file);
    
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &sha256);
    
    // 转换为十六进制字符串
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(&checksum[i*2], "%02x", hash[i]);
    }
    checksum[64] = '\0';
    
    return FT_SUCCESS;
}

ft_error_code_t ft_verify_file_checksum(const char* filepath, const char* expected_checksum) {
    if (!filepath || !expected_checksum) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    char calculated_checksum[65];
    ft_error_code_t result = ft_calculate_file_checksum(filepath, calculated_checksum);
    
    if (result != FT_SUCCESS) {
        return result;
    }
    
    if (strcmp(calculated_checksum, expected_checksum) != 0) {
        return FT_ERROR_CHECKSUM_MISMATCH;
    }
    
    return FT_SUCCESS;
}

// ==================== 会话管理 ====================

ft_transfer_session_t* ft_get_session(uint64_t transfer_id) {
    return ft_find_in_session_map(transfer_id);
}

ft_error_code_t ft_remove_session(uint64_t transfer_id) {
    pthread_mutex_lock(&g_ft_manager->sessions_mutex);
    
    // 从映射中移除
    ft_error_code_t map_result = ft_remove_from_session_map(transfer_id);
    if (map_result != FT_SUCCESS) {
        pthread_mutex_unlock(&g_ft_manager->sessions_mutex);
        return map_result;
    }
    
    // 从活跃会话链表中移除
    ft_transfer_session_t* session = NULL;
    ft_transfer_session_t* current = g_ft_manager->active_sessions;
    
    while (current) {
        if (current->transfer_id == transfer_id) {
            session = current;
            break;
        }
        current = current->next;
    }
    
    if (session) {
        // 从链表中移除
        if (session->prev) {
            session->prev->next = session->next;
        } else {
            g_ft_manager->active_sessions = session->next;
        }
        
        if (session->next) {
            session->next->prev = session->prev;
        }
        
        g_ft_manager->stats.active_transfers--;
    } else {
        // 检查已完成会话
        current = g_ft_manager->completed_sessions;
        while (current) {
            if (current->transfer_id == transfer_id) {
                session = current;
                break;
            }
            current = current->next;
        }
        
        if (session) {
            // 从已完成链表中移除
            if (session->prev) {
                session->prev->next = session->next;
            } else {
                g_ft_manager->completed_sessions = session->next;
            }
            
            if (session->next) {
                session->next->prev = session->prev;
            }
        }
    }
    
    pthread_mutex_unlock(&g_ft_manager->sessions_mutex);
    
    // 销毁会话
    if (session) {
        ft_destroy_session(session);
    }
    
    return FT_SUCCESS;
}

ft_error_code_t ft_cleanup_completed_sessions(time_t older_than) {
    pthread_mutex_lock(&g_ft_manager->sessions_mutex);
    
    ft_transfer_session_t* current = g_ft_manager->completed_sessions;
    ft_transfer_session_t* next = NULL;
    
    while (current) {
        next = current->next;
        
        if (current->last_activity_time < older_than) {
            // 从链表中移除
            if (current->prev) {
                current->prev->next = current->next;
            } else {
                g_ft_manager->completed_sessions = current->next;
            }
            
            if (current->next) {
                current->next->prev = current->prev;
            }
            
            // 从映射中移除
            ft_remove_from_session_map(current->transfer_id);
            
            // 销毁会话
            ft_destroy_session(current);
        }
        
        current = next;
    }
    
    pthread_mutex_unlock(&g_ft_manager->sessions_mutex);
    
    return FT_SUCCESS;
}

// ==================== 工具函数 ====================

uint64_t ft_generate_transfer_id(void) {
    static uint64_t counter = 0;
    uint64_t timestamp = (uint64_t)time(NULL) << 32;
    uint64_t random_part = 0;
    
    // 使用随机数增加唯一性
    if (RAND_bytes((unsigned char*)&random_part, sizeof(random_part)) != 1) {
        // 如果RAND_bytes失败，使用简单的方法
        random_part = (uint64_t)rand() << 16 | rand();
    }
    
    return timestamp | (random_part & 0xFFFFFFFF) | (counter++ & 0xFFFF);
}

const char* ft_error_to_string(ft_error_code_t error) {
    switch (error) {
        case FT_SUCCESS: return "Success";
        case FT_ERROR_INVALID_PARAMS: return "Invalid parameters";
        case FT_ERROR_FILE_NOT_FOUND: return "File not found";
        case FT_ERROR_FILE_TOO_LARGE: return "File too large";
        case FT_ERROR_PERMISSION_DENIED: return "Permission denied";
        case FT_ERROR_DISK_FULL: return "Disk full";
        case FT_ERROR_NETWORK: return "Network error";
        case FT_ERROR_TIMEOUT: return "Timeout";
        case FT_ERROR_CHECKSUM_MISMATCH: return "Checksum mismatch";
        case FT_ERROR_ENCRYPTION: return "Encryption error";
        case FT_ERROR_DECRYPTION: return "Decryption error";
        case FT_ERROR_INTERRUPTED: return "Transfer interrupted";
        case FT_ERROR_CONCURRENT_LIMIT: return "Concurrent transfer limit reached";
        case FT_ERROR_INVALID_STATE: return "Invalid transfer state";
        case FT_ERROR_MEMORY: return "Memory allocation error";
        case FT_ERROR_IO: return "I/O error";
        case FT_ERROR_INVALID_CHUNK: return "Invalid chunk";
        case FT_ERROR_TRANSFER_NOT_FOUND: return "Transfer not found";
        case FT_ERROR_RESUME_FAILED: return "Resume failed";
        case FT_ERROR_COMPRESSION: return "Compression error";
        case FT_ERROR_DECOMPRESSION: return "Decompression error";
        default: return "Unknown error";
    }
}

const char* ft_status_to_string(ft_transfer_status_t status) {
    switch (status) {
        case FT_STATUS_PENDING: return "Pending";
        case FT_STATUS_PREPARING: return "Preparing";
        case FT_STATUS_TRANSFERRING: return "Transferring";
        case FT_STATUS_PAUSED: return "Paused";
        case FT_STATUS_COMPLETED: return "Completed";
        case FT_STATUS_FAILED: return "Failed";
        case FT_STATUS_CANCELLED: return "Cancelled";
        case FT_STATUS_VERIFYING: return "Verifying";
        case FT_STATUS_CLEANUP: return "Cleanup";
        default: return "Unknown";
    }
}

ft_error_code_t ft_cleanup_temp_files(void) {
    if (!g_ft_manager) {
        return FT_ERROR_INVALID_STATE;
    }
    
    char temp_dir_path[FT_MAX_FILE_PATH];
    snprintf(temp_dir_path, sizeof(temp_dir_path), "%s/chunks",
             g_ft_manager->config.temp_path);
    
    DIR* dir = opendir(temp_dir_path);
    if (!dir) {
        return FT_SUCCESS;  // 目录不存在
    }
    
    struct dirent* entry;
    int count = 0;
    
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".tmp") != NULL) {
            char filepath[FT_MAX_FILE_PATH];
            snprintf(filepath, sizeof(filepath), "%s/%s",
                     temp_dir_path, entry->d_name);
            
            if (unlink(filepath) == 0) {
                count++;
            }
        }
    }
    
    closedir(dir);
    
    printf("Cleaned up %d temporary files\n", count);
    
    return FT_SUCCESS;
}

ft_error_code_t ft_get_storage_usage(uint64_t* used_bytes, uint64_t* total_bytes) {
    // 简化实现：统计存储目录下的文件大小
    if (!g_ft_manager) {
        return FT_ERROR_INVALID_STATE;
    }
    
    uint64_t total = 0;
    
    // 递归计算目录大小
    char command[512];
    snprintf(command, sizeof(command), "du -sb %s 2>/dev/null | cut -f1",
             g_ft_manager->config.storage_path);
    
    FILE* fp = popen(command, "r");
    if (fp) {
        char buffer[64];
        if (fgets(buffer, sizeof(buffer), fp)) {
            total = strtoull(buffer, NULL, 10);
        }
        pclose(fp);
    }
    
    if (used_bytes) {
        *used_bytes = total;
    }
    
    // 获取磁盘总空间（简化实现）
    if (total_bytes) {
        struct statvfs st;
        if (statvfs(g_ft_manager->config.storage_path, &st) == 0) {
            *total_bytes = (uint64_t)st.f_blocks * st.f_frsize;
        } else {
            *total_bytes = 100 * 1024 * 1024 * 1024ULL;  // 默认100GB
        }
    }
    
    return FT_SUCCESS;
}

// ==================== 统计信息 ====================

ft_stats_t ft_get_statistics(void) {
    ft_stats_t stats;
    
    if (g_ft_manager) {
        memcpy(&stats, &g_ft_manager->stats, sizeof(ft_stats_t));
        stats.uptime_seconds = time(NULL) - stats.uptime_seconds;
    } else {
        memset(&stats, 0, sizeof(ft_stats_t));
    }
    
    return stats;
}

// ==================== 配置管理 ====================

ft_error_code_t ft_get_config(ft_config_t* config) {
    if (!config || !g_ft_manager) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    memcpy(config, &g_ft_manager->config, sizeof(ft_config_t));
    return FT_SUCCESS;
}

ft_error_code_t ft_update_config(const ft_config_t* config) {
    if (!config || !g_ft_manager) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    // 验证配置
    ft_error_code_t validate_result = ft_validate_config(config);
    if (validate_result != FT_SUCCESS) {
        return validate_result;
    }
    
    // 更新配置
    memcpy(&g_ft_manager->config, config, sizeof(ft_config_t));
    
    // 重新初始化目录
    ft_init_directories();
    
    printf("File transfer configuration updated\n");
    return FT_SUCCESS;
}

ft_error_code_t ft_validate_config(const ft_config_t* config) {
    if (!config) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    // 检查chunk大小
    if (config->chunk_size == 0 || config->chunk_size > 1024 * 1024 * 100) {  // 最大100MB
        return FT_ERROR_INVALID_PARAMS;
    }
    
    // 检查最大重试次数
    if (config->max_retries > 10) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    // 检查超时时间
    if (config->timeout_ms == 0 || config->timeout_ms > 300000) {  // 最大5分钟
        return FT_ERROR_INVALID_PARAMS;
    }
    
    // 检查最大并发传输数
    if (config->max_concurrent_transfers == 0 ||
        config->max_concurrent_transfers > 1000) {
        return FT_ERROR_INVALID_PARAMS;
    }
    
    // 检查最大文件大小
    if (config->max_file_size == 0 ||
        config->max_file_size > 10ULL * 1024 * 1024 * 1024 * 1024) {  // 最大10TB
        return FT_ERROR_INVALID_PARAMS;
    }
    
    return FT_SUCCESS;
}