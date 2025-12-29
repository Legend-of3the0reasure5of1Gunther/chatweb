#ifndef FILE_TRANSFER_H
#define FILE_TRANSFER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <pthread.h>
#include <openssl/evp.h>
#include <openssl/aes.h>
#include "protocol.h"
#include "security.h"
#include "thread_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 传输配置 ====================
#define FT_MAX_CONCURRENT_TRANSFERS 100
#define FT_CHUNK_SIZE FILE_CHUNK_SIZE  // 从protocol.h继承
#define FT_MAX_RETRIES 3
#define FT_TIMEOUT_MS 30000           // 30秒超时
#define FT_BUFFER_SIZE 65536          // 64KB缓冲区
#define FT_PROGRESS_INTERVAL_MS 1000  // 进度更新间隔
#define FT_MAX_FILE_PATH 512
#define FT_MAX_TEMP_FILES 1000
#define FT_CHECKSUM_CHUNK_SIZE 8192   // 计算校验和的块大小

// ==================== 传输状态定义 ====================
typedef enum {
    FT_STATUS_PENDING = 0,      // 等待开始
    FT_STATUS_PREPARING = 1,    // 准备中
    FT_STATUS_TRANSFERRING = 2, // 传输中
    FT_STATUS_PAUSED = 3,       // 已暂停
    FT_STATUS_COMPLETED = 4,    // 已完成
    FT_STATUS_FAILED = 5,       // 已失败
    FT_STATUS_CANCELLED = 6,    // 已取消
    FT_STATUS_VERIFYING = 7,    // 验证中
    FT_STATUS_CLEANUP = 8       // 清理中
} ft_transfer_status_t;

// ==================== 传输模式定义 ====================
typedef enum {
    FT_MODE_DIRECT = 0,         // 直接传输
    FT_MODE_RELAY = 1,          // 中继传输
    FT_MODE_P2P = 2             // P2P传输
} ft_transfer_mode_t;

// ==================== 错误码定义 ====================
typedef enum {
    FT_SUCCESS = 0,
    FT_ERROR_INVALID_PARAMS = 1,
    FT_ERROR_FILE_NOT_FOUND = 2,
    FT_ERROR_FILE_TOO_LARGE = 3,
    FT_ERROR_PERMISSION_DENIED = 4,
    FT_ERROR_DISK_FULL = 5,
    FT_ERROR_NETWORK = 6,
    FT_ERROR_TIMEOUT = 7,
    FT_ERROR_CHECKSUM_MISMATCH = 8,
    FT_ERROR_ENCRYPTION = 9,
    FT_ERROR_DECRYPTION = 10,
    FT_ERROR_INTERRUPTED = 11,
    FT_ERROR_CONCURRENT_LIMIT = 12,
    FT_ERROR_INVALID_STATE = 13,
    FT_ERROR_MEMORY = 14,
    FT_ERROR_IO = 15,
    FT_ERROR_INVALID_CHUNK = 16,
    FT_ERROR_TRANSFER_NOT_FOUND = 17,
    FT_ERROR_RESUME_FAILED = 18,
    FT_ERROR_COMPRESSION = 19,
    FT_ERROR_DECOMPRESSION = 20
} ft_error_code_t;

// ==================== 数据结构定义 ====================

// 传输进度信息
typedef struct {
    uint64_t transfer_id;
    uint64_t bytes_transferred;
    uint64_t total_bytes;
    uint32_t chunks_transferred;
    uint32_t total_chunks;
    double progress_percent;
    double speed_bytes_per_sec;
    double estimated_time_sec;
    time_t start_time;
    time_t last_update_time;
} ft_progress_info_t;

// 传输统计信息
typedef struct {
    uint64_t total_transfers;
    uint64_t successful_transfers;
    uint64_t failed_transfers;
    uint64_t bytes_transferred;
    uint64_t average_speed_bytes_per_sec;
    uint64_t peak_speed_bytes_per_sec;
    time_t uptime_seconds;
    uint32_t active_transfers;
    uint32_t queued_transfers;
} ft_stats_t;

// 传输配置
typedef struct {
    uint32_t chunk_size;
    uint32_t max_retries;
    uint32_t timeout_ms;
    uint32_t max_concurrent_transfers;
    bool enable_encryption;
    bool enable_compression;
    bool enable_resume;
    bool verify_checksum;
    char storage_path[FT_MAX_FILE_PATH];
    char temp_path[FT_MAX_FILE_PATH];
    uint64_t max_file_size;
} ft_config_t;

// 传输会话
typedef struct ft_transfer_session ft_transfer_session_t;

struct ft_transfer_session {
    uint64_t transfer_id;
    uint64_t user_id;
    uint64_t peer_user_id;
    uint64_t file_id;
    
    char local_path[FT_MAX_FILE_PATH];
    char remote_path[FT_MAX_FILE_PATH];
    char temp_path[FT_MAX_FILE_PATH];
    
    file_metadata_t metadata;
    ft_transfer_mode_t mode;
    ft_transfer_status_t status;
    
    // 加密相关
    bool encryption_enabled;
    uint8_t encryption_key[AES_BLOCK_SIZE * 2];  // 256-bit key
    uint8_t encryption_iv[AES_BLOCK_SIZE];
    
    // 压缩相关
    bool compression_enabled;
    int compression_level;
    
    // 传输控制
    uint32_t current_chunk;
    uint32_t total_chunks;
    uint64_t bytes_transferred;
    uint64_t total_bytes;
    
    // 重试相关
    uint32_t retry_count;
    uint32_t error_chunks[FT_MAX_RETRIES * 10];  // 记录出错的chunk
    uint32_t error_chunk_count;
    
    // 时间相关
    time_t start_time;
    time_t last_activity_time;
    time_t estimated_completion_time;
    
    // 校验和
    char expected_checksum[65];  // SHA256
    char calculated_checksum[65];
    
    // 互斥锁和条件变量
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    
    // 回调函数
    void (*progress_callback)(ft_transfer_session_t* session, void* user_data);
    void (*completion_callback)(ft_transfer_session_t* session, void* user_data);
    void (*error_callback)(ft_transfer_session_t* session, ft_error_code_t error, void* user_data);
    void* callback_user_data;
    
    // 链表指针
    ft_transfer_session_t* next;
    ft_transfer_session_t* prev;
};

// 传输管理器
typedef struct {
    ft_transfer_session_t* active_sessions;
    ft_transfer_session_t* completed_sessions;
    pthread_mutex_t sessions_mutex;
    
    thread_pool_t* thread_pool;
    ft_config_t config;
    ft_stats_t stats;
    
    bool running;
    pthread_t manager_thread;
    
    // 哈希表用于快速查找
    struct {
        ft_transfer_session_t** table;
        size_t size;
        size_t count;
    } session_map;
} ft_manager_t;

// WebSocket传输上下文
typedef struct {
    uint64_t transfer_id;
    int websocket_fd;
    uint64_t user_id;
    bool is_upload;
    ft_transfer_session_t* session;
} ft_websocket_context_t;

// ==================== 函数声明 ====================

// 初始化/清理
ft_error_code_t ft_init(const ft_config_t* config);
ft_error_code_t ft_init_with_path(const char* storage_path, const char* temp_path);
void ft_cleanup(void);

// 传输管理器
ft_error_code_t ft_start_manager(void);
ft_error_code_t ft_stop_manager(void);
ft_error_code_t ft_pause_manager(void);
ft_error_code_t ft_resume_manager(void);

// 文件上传
ft_error_code_t ft_start_upload(uint64_t user_id, uint64_t peer_user_id,
                               const char* local_path, const char* remote_path,
                               const file_metadata_t* metadata,
                               ft_transfer_mode_t mode,
                               ft_transfer_session_t** session);
ft_error_code_t ft_upload_chunk(ft_transfer_session_t* session,
                               uint32_t chunk_index,
                               const uint8_t* data,
                               uint32_t data_length);
ft_error_code_t ft_complete_upload(ft_transfer_session_t* session);
ft_error_code_t ft_cancel_upload(ft_transfer_session_t* session);
ft_error_code_t ft_pause_upload(ft_transfer_session_t* session);
ft_error_code_t ft_resume_upload(ft_transfer_session_t* session);

// 文件下载
ft_error_code_t ft_start_download(uint64_t user_id, uint64_t peer_user_id,
                                 const char* remote_path, const char* local_path,
                                 ft_transfer_mode_t mode,
                                 ft_transfer_session_t** session);
ft_error_code_t ft_download_chunk(ft_transfer_session_t* session,
                                 uint32_t chunk_index,
                                 uint8_t** data,
                                 uint32_t* data_length);
ft_error_code_t ft_complete_download(ft_transfer_session_t* session);
ft_error_code_t ft_cancel_download(ft_transfer_session_t* session);
ft_error_code_t ft_pause_download(ft_transfer_session_t* session);
ft_error_code_t ft_resume_download(ft_transfer_session_t* session);

// 会话管理
ft_transfer_session_t* ft_get_session(uint64_t transfer_id);
ft_error_code_t ft_remove_session(uint64_t transfer_id);
ft_transfer_session_t* ft_get_user_sessions(uint64_t user_id, size_t* count);
ft_error_code_t ft_cleanup_completed_sessions(time_t older_than);

// 进度和状态
ft_error_code_t ft_get_progress(uint64_t transfer_id, ft_progress_info_t* progress);
ft_error_code_t ft_get_status(uint64_t transfer_id, ft_transfer_status_t* status);
ft_error_code_t ft_set_status(uint64_t transfer_id, ft_transfer_status_t status);

// 断点续传
ft_error_code_t ft_save_resume_point(ft_transfer_session_t* session);
ft_error_code_t ft_load_resume_point(uint64_t transfer_id, ft_transfer_session_t** session);
ft_error_code_t ft_get_resumable_transfers(uint64_t user_id, uint64_t** transfer_ids, size_t* count);

// 加密传输
ft_error_code_t ft_generate_encryption_key(uint64_t transfer_id,
                                          uint8_t* key, size_t key_size,
                                          uint8_t* iv, size_t iv_size);
ft_error_code_t ft_encrypt_chunk(const uint8_t* plaintext, uint32_t plaintext_len,
                                const uint8_t* key, const uint8_t* iv,
                                uint8_t* ciphertext, uint32_t* ciphertext_len);
ft_error_code_t ft_decrypt_chunk(const uint8_t* ciphertext, uint32_t ciphertext_len,
                                const uint8_t* key, const uint8_t* iv,
                                uint8_t* plaintext, uint32_t* plaintext_len);

// 压缩传输
ft_error_code_t ft_compress_chunk(const uint8_t* data, uint32_t data_len,
                                 uint8_t* compressed, uint32_t* compressed_len,
                                 int level);
ft_error_code_t ft_decompress_chunk(const uint8_t* compressed, uint32_t compressed_len,
                                   uint8_t* decompressed, uint32_t* decompressed_len);

// 校验和验证
ft_error_code_t ft_calculate_file_checksum(const char* filepath, char* checksum);
ft_error_code_t ft_verify_file_checksum(const char* filepath, const char* expected_checksum);
ft_error_code_t ft_calculate_chunk_checksum(const uint8_t* data, uint32_t data_len,
                                           char* checksum);

// WebSocket支持
ft_error_code_t ft_websocket_start_upload(int websocket_fd, uint64_t user_id,
                                         const file_metadata_t* metadata,
                                         ft_websocket_context_t** context);
ft_error_code_t ft_websocket_handle_chunk(ft_websocket_context_t* context,
                                         const uint8_t* data, uint32_t data_len);
ft_error_code_t ft_websocket_complete_upload(ft_websocket_context_t* context);
ft_error_code_t ft_websocket_start_download(int websocket_fd, uint64_t user_id,
                                           uint64_t file_id,
                                           ft_websocket_context_t** context);
ft_error_code_t ft_websocket_send_chunk(ft_websocket_context_t* context);

// 回调函数设置
ft_error_code_t ft_set_progress_callback(ft_transfer_session_t* session,
                                        void (*callback)(ft_transfer_session_t*, void*),
                                        void* user_data);
ft_error_code_t ft_set_completion_callback(ft_transfer_session_t* session,
                                          void (*callback)(ft_transfer_session_t*, void*),
                                          void* user_data);
ft_error_code_t ft_set_error_callback(ft_transfer_session_t* session,
                                     void (*callback)(ft_transfer_session_t*, ft_error_code_t, void*),
                                     void* user_data);

// 统计信息
ft_stats_t ft_get_statistics(void);
ft_error_code_t ft_reset_statistics(void);
ft_error_code_t ft_get_active_transfers(uint64_t** transfer_ids, size_t* count);

// 配置管理
ft_error_code_t ft_get_config(ft_config_t* config);
ft_error_code_t ft_update_config(const ft_config_t* config);
ft_error_code_t ft_validate_config(const ft_config_t* config);

// 工具函数
uint64_t ft_generate_transfer_id(void);
const char* ft_error_to_string(ft_error_code_t error);
const char* ft_status_to_string(ft_transfer_status_t status);
ft_error_code_t ft_cleanup_temp_files(void);
ft_error_code_t ft_get_storage_usage(uint64_t* used_bytes, uint64_t* total_bytes);

// 协议消息处理
ft_error_code_t ft_handle_upload_request(const message_header_t* header,
                                        const void* data, size_t data_len,
                                        response_t* response);
ft_error_code_t ft_handle_download_request(const message_header_t* header,
                                          const void* data, size_t data_len,
                                          response_t* response);
ft_error_code_t ft_handle_chunk(const message_header_t* header,
                               const void* data, size_t data_len,
                               response_t* response);
ft_error_code_t ft_handle_progress_update(const message_header_t* header,
                                         const void* data, size_t data_len);

#ifdef __cplusplus
}
#endif

#endif // FILE_TRANSFER_H