#include "websocket_server.h"
#include "protocol.h"
#include "threadpool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <openssl/sha.h>
#include <zlib.h>

// ==================== 文件传输结构定义 ====================
typedef struct {
    uint64_t transfer_id;
    uint64_t client_id;
    char filename[MAX_FILENAME_LEN];
    char filepath[512];
    uint64_t file_size;
    uint64_t bytes_transferred;
    uint32_t chunk_size;
    uint32_t total_chunks;
    uint32_t current_chunk;
    uint8_t encryption_key[32];
    bool encrypted;
    bool upload; // true=上传, false=下载
    time_t start_time;
    time_t last_update;
    FILE *file_handle;
} file_transfer_context_t;

typedef struct {
    file_transfer_context_t **transfers;
    uint32_t max_transfers;
    uint32_t active_transfers;
    pthread_mutex_t mutex;
    threadpool_t *thread_pool;
} file_transfer_manager_t;

// ==================== 静态变量 ====================
static file_transfer_manager_t g_transfer_manager = {0};

// ==================== 内部函数声明 ====================
static file_transfer_context_t* create_transfer_context(uint64_t client_id, 
                                                       const char *filename,
                                                       uint64_t file_size,
                                                       bool upload);
static void destroy_transfer_context(file_transfer_context_t *ctx);
static void* process_file_upload(void *arg);
static void* process_file_download(void *arg);
static int send_progress_update(uint64_t client_id, uint64_t transfer_id, 
                               float progress, uint64_t bytes_transferred);
static int send_transfer_complete(uint64_t client_id, uint64_t transfer_id,
                                 bool success, const char *message);
static int encrypt_data(uint8_t *data, size_t length, const uint8_t *key);
static int decrypt_data(uint8_t *data, size_t length, const uint8_t *key);
static char* calculate_file_hash(const char *filepath);

// ==================== 文件传输管理器初始化 ====================

/**
 * 初始化文件传输管理器
 */
int file_transfer_manager_init(uint32_t max_transfers, uint32_t thread_pool_size) {
    if (g_transfer_manager.transfers) {
        return -1; // 已经初始化
    }
    
    g_transfer_manager.max_transfers = max_transfers;
    g_transfer_manager.active_transfers = 0;
    
    // 分配传输上下文数组
    g_transfer_manager.transfers = (file_transfer_context_t**)calloc(
        max_transfers, sizeof(file_transfer_context_t*));
    if (!g_transfer_manager.transfers) {
        return -1;
    }
    
    // 初始化互斥锁
    if (pthread_mutex_init(&g_transfer_manager.mutex, NULL) != 0) {
        free(g_transfer_manager.transfers);
        return -1;
    }
    
    // 创建线程池
    threadpool_config_t pool_config = threadpool_get_default_config();
    pool_config.default_threads = thread_pool_size;
    pool_config.queue_size = 64;
    
    g_transfer_manager.thread_pool = threadpool_create(&pool_config, "file-transfer");
    if (!g_transfer_manager.thread_pool) {
        pthread_mutex_destroy(&g_transfer_manager.mutex);
        free(g_transfer_manager.transfers);
        return -1;
    }
    
    // 创建文件传输目录
    mkdir("uploads", 0755);
    mkdir("downloads", 0755);
    
    return 0;
}

/**
 * 销毁文件传输管理器
 */
void file_transfer_manager_destroy(void) {
    if (!g_transfer_manager.transfers) return;
    
    pthread_mutex_lock(&g_transfer_manager.mutex);
    
    // 清理所有传输
    for (uint32_t i = 0; i < g_transfer_manager.max_transfers; i++) {
        if (g_transfer_manager.transfers[i]) {
            destroy_transfer_context(g_transfer_manager.transfers[i]);
            g_transfer_manager.transfers[i] = NULL;
        }
    }
    
    free(g_transfer_manager.transfers);
    g_transfer_manager.transfers = NULL;
    g_transfer_manager.active_transfers = 0;
    
    pthread_mutex_unlock(&g_transfer_manager.mutex);
    pthread_mutex_destroy(&g_transfer_manager.mutex);
    
    // 销毁线程池
    if (g_transfer_manager.thread_pool) {
        threadpool_destroy(g_transfer_manager.thread_pool, true);
        g_transfer_manager.thread_pool = NULL;
    }
}

// ==================== 文件传输处理 ====================

/**
 * 处理文件上传请求
 */
int handle_file_upload_request(websocket_server_t *server, uint64_t client_id,
                              const char *filename, uint64_t file_size,
                              bool encrypted, uint32_t chunk_size) {
    // 检查文件大小限制
    if (file_size > MAX_FILE_SIZE) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg),
                "文件大小超过限制: %.2f MB > %.2f MB",
                file_size / (1024.0 * 1024.0),
                MAX_FILE_SIZE / (1024.0 * 1024.0));
        
        // 发送错误消息给客户端
        char json_error[512];
        snprintf(json_error, sizeof(json_error),
                "{\"type\":\"file_upload_error\",\"data\":{\"filename\":\"%s\",\"error\":\"%s\"}}",
                filename, error_msg);
        websocket_send_text(server, client_id, json_error);
        
        return -1;
    }
    
    // 创建传输上下文
    file_transfer_context_t *ctx = create_transfer_context(client_id, filename, 
                                                          file_size, true);
    if (!ctx) {
        return -1;
    }
    
    ctx->encrypted = encrypted;
    ctx->chunk_size = chunk_size;
    ctx->total_chunks = (file_size + chunk_size - 1) / chunk_size;
    
    // 生成加密密钥
    if (encrypted) {
        // 生成随机加密密钥
        srand(time(NULL));
        for (int i = 0; i < sizeof(ctx->encryption_key); i++) {
            ctx->encryption_key[i] = rand() % 256;
        }
    }
    
    // 保存传输上下文
    pthread_mutex_lock(&g_transfer_manager.mutex);
    
    uint32_t slot = UINT32_MAX;
    for (uint32_t i = 0; i < g_transfer_manager.max_transfers; i++) {
        if (g_transfer_manager.transfers[i] == NULL) {
            g_transfer_manager.transfers[i] = ctx;
            g_transfer_manager.active_transfers++;
            slot = i;
            break;
        }
    }
    
    pthread_mutex_unlock(&g_transfer_manager.mutex);
    
    if (slot == UINT32_MAX) {
        destroy_transfer_context(ctx);
        return -1;
    }
    
    // 创建上传目录
    char upload_dir[256];
    snprintf(upload_dir, sizeof(upload_dir), "uploads/client_%lu", client_id);
    mkdir(upload_dir, 0755);
    
    // 创建文件路径
    snprintf(ctx->filepath, sizeof(ctx->filepath),
            "%s/%s", upload_dir, filename);
    
    // 打开文件用于写入
    ctx->file_handle = fopen(ctx->filepath, "wb");
    if (!ctx->file_handle) {
        pthread_mutex_lock(&g_transfer_manager.mutex);
        g_transfer_manager.transfers[slot] = NULL;
        g_transfer_manager.active_transfers--;
        pthread_mutex_unlock(&g_transfer_manager.mutex);
        
        destroy_transfer_context(ctx);
        return -1;
    }
    
    // 发送传输开始确认
    char response[512];
    snprintf(response, sizeof(response),
            "{\"type\":\"file_upload_started\",\"data\":{\"transfer_id\":%lu,"
            "\"filename\":\"%s\",\"file_size\":%lu,\"total_chunks\":%u,"
            "\"chunk_size\":%u,\"encrypted\":%s}}",
            ctx->transfer_id, filename, file_size, ctx->total_chunks,
            chunk_size, encrypted ? "true" : "false");
    
    websocket_send_text(server, client_id, response);
    
    // 如果使用了加密，发送加密密钥（在实际应用中应该使用安全的方式）
    if (encrypted) {
        char key_hex[65];
        for (int i = 0; i < 32; i++) {
            sprintf(key_hex + (i * 2), "%02x", ctx->encryption_key[i]);
        }
        key_hex[64] = '\0';
        
        char key_msg[256];
        snprintf(key_msg, sizeof(key_msg),
                "{\"type\":\"file_encryption_key\",\"data\":{\"transfer_id\":%lu,\"key\":\"%s\"}}",
                ctx->transfer_id, key_hex);
        websocket_send_text(server, client_id, key_msg);
    }
    
    return 0;
}

/**
 * 处理文件上传数据块
 */
int handle_file_upload_chunk(websocket_server_t *server, uint64_t client_id,
                            uint64_t transfer_id, uint32_t chunk_index,
                            const uint8_t *chunk_data, uint32_t chunk_size) {
    // 查找传输上下文
    pthread_mutex_lock(&g_transfer_manager.mutex);
    
    file_transfer_context_t *ctx = NULL;
    uint32_t slot = UINT32_MAX;
    
    for (uint32_t i = 0; i < g_transfer_manager.max_transfers; i++) {
        if (g_transfer_manager.transfers[i] && 
            g_transfer_manager.transfers[i]->transfer_id == transfer_id &&
            g_transfer_manager.transfers[i]->client_id == client_id) {
            ctx = g_transfer_manager.transfers[i];
            slot = i;
            break;
        }
    }
    
    pthread_mutex_unlock(&g_transfer_manager.mutex);
    
    if (!ctx) {
        return -1;
    }
    
    // 检查分片索引
    if (chunk_index != ctx->current_chunk) {
        return -2; // 分片顺序错误
    }
    
    // 如果是加密数据，先解密
    uint8_t *decrypted_data = (uint8_t*)malloc(chunk_size);
    if (!decrypted_data) {
        return -1;
    }
    
    memcpy(decrypted_data, chunk_data, chunk_size);
    
    if (ctx->encrypted) {
        if (decrypt_data(decrypted_data, chunk_size, ctx->encryption_key) != 0) {
            free(decrypted_data);
            return -1;
        }
    }
    
    // 写入文件
    size_t written = fwrite(decrypted_data, 1, chunk_size, ctx->file_handle);
    free(decrypted_data);
    
    if (written != chunk_size) {
        return -1;
    }
    
    // 更新传输状态
    ctx->bytes_transferred += chunk_size;
    ctx->current_chunk++;
    ctx->last_update = time(NULL);
    
    // 计算进度
    float progress = (float)ctx->bytes_transferred / ctx->file_size * 100.0f;
    
    // 发送进度更新
    send_progress_update(client_id, transfer_id, progress, ctx->bytes_transferred);
    
    // 检查是否完成
    if (ctx->bytes_transferred >= ctx->file_size) {
        // 文件上传完成
        fclose(ctx->file_handle);
        ctx->file_handle = NULL;
        
        // 计算文件哈希
        char *file_hash = calculate_file_hash(ctx->filepath);
        
        // 发送完成消息
        char complete_msg[512];
        if (file_hash) {
            snprintf(complete_msg, sizeof(complete_msg),
                    "{\"type\":\"file_upload_complete\",\"data\":{\"transfer_id\":%lu,"
                    "\"filename\":\"%s\",\"file_size\":%lu,\"file_hash\":\"%s\"}}",
                    transfer_id, ctx->filename, ctx->file_size, file_hash);
            free(file_hash);
        } else {
            snprintf(complete_msg, sizeof(complete_msg),
                    "{\"type\":\"file_upload_complete\",\"data\":{\"transfer_id\":%lu,"
                    "\"filename\":\"%s\",\"file_size\":%lu}}",
                    transfer_id, ctx->filename, ctx->file_size);
        }
        
        websocket_send_text(server, client_id, complete_msg);
        
        // 清理传输上下文
        pthread_mutex_lock(&g_transfer_manager.mutex);
        if (slot != UINT32_MAX) {
            destroy_transfer_context(ctx);
            g_transfer_manager.transfers[slot] = NULL;
            g_transfer_manager.active_transfers--;
        }
        pthread_mutex_unlock(&g_transfer_manager.mutex);
    }
    
    return 0;
}

/**
 * 处理文件下载请求
 */
int handle_file_download_request(websocket_server_t *server, uint64_t client_id,
                                const char *filename, bool encrypted) {
    // 构建文件路径
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "uploads/%s", filename);
    
    // 检查文件是否存在
    struct stat st;
    if (stat(filepath, &st) != 0) {
        // 发送错误消息
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg),
                "{\"type\":\"file_download_error\",\"data\":{\"filename\":\"%s\",\"error\":\"文件不存在\"}}",
                filename);
        websocket_send_text(server, client_id, error_msg);
        return -1;
    }
    
    uint64_t file_size = st.st_size;
    
    // 创建传输上下文
    file_transfer_context_t *ctx = create_transfer_context(client_id, filename,
                                                          file_size, false);
    if (!ctx) {
        return -1;
    }
    
    ctx->encrypted = encrypted;
    ctx->chunk_size = FILE_CHUNK_SIZE;
    ctx->total_chunks = (file_size + FILE_CHUNK_SIZE - 1) / FILE_CHUNK_SIZE;
    
    // 生成加密密钥
    if (encrypted) {
        srand(time(NULL));
        for (int i = 0; i < sizeof(ctx->encryption_key); i++) {
            ctx->encryption_key[i] = rand() % 256;
        }
    }
    
    // 打开文件用于读取
    ctx->file_handle = fopen(filepath, "rb");
    if (!ctx->file_handle) {
        destroy_transfer_context(ctx);
        return -1;
    }
    
    strcpy(ctx->filepath, filepath);
    
    // 保存传输上下文
    pthread_mutex_lock(&g_transfer_manager.mutex);
    
    uint32_t slot = UINT32_MAX;
    for (uint32_t i = 0; i < g_transfer_manager.max_transfers; i++) {
        if (g_transfer_manager.transfers[i] == NULL) {
            g_transfer_manager.transfers[i] = ctx;
            g_transfer_manager.active_transfers++;
            slot = i;
            break;
        }
    }
    
    pthread_mutex_unlock(&g_transfer_manager.mutex);
    
    if (slot == UINT32_MAX) {
        fclose(ctx->file_handle);
        destroy_transfer_context(ctx);
        return -1;
    }
    
    // 发送下载开始确认
    char response[512];
    snprintf(response, sizeof(response),
            "{\"type\":\"file_download_started\",\"data\":{\"transfer_id\":%lu,"
            "\"filename\":\"%s\",\"file_size\":%lu,\"total_chunks\":%u,"
            "\"chunk_size\":%u,\"encrypted\":%s}}",
            ctx->transfer_id, filename, file_size, ctx->total_chunks,
            FILE_CHUNK_SIZE, encrypted ? "true" : "false");
    
    websocket_send_text(server, client_id, response);
    
    // 如果使用了加密，发送加密密钥
    if (encrypted) {
        char key_hex[65];
        for (int i = 0; i < 32; i++) {
            sprintf(key_hex + (i * 2), "%02x", ctx->encryption_key[i]);
        }
        key_hex[64] = '\0';
        
        char key_msg[256];
        snprintf(key_msg, sizeof(key_msg),
                "{\"type\":\"file_encryption_key\",\"data\":{\"transfer_id\":%lu,\"key\":\"%s\"}}",
                ctx->transfer_id, key_hex);
        websocket_send_text(server, client_id, key_msg);
    }
    
    // 将下载任务添加到线程池
    threadpool_add_task(g_transfer_manager.thread_pool,
                       process_file_download,
                       ctx,
                       NULL,
                       TASK_PRIORITY_NORMAL,
                       0,
                       NULL);
    
    return 0;
}

// ==================== 工具函数 ====================

/**
 * 创建传输上下文
 */
static file_transfer_context_t* create_transfer_context(uint64_t client_id,
                                                       const char *filename,
                                                       uint64_t file_size,
                                                       bool upload) {
    static uint64_t next_transfer_id = 1;
    
    file_transfer_context_t *ctx = (file_transfer_context_t*)calloc(1, sizeof(file_transfer_context_t));
    if (!ctx) return NULL;
    
    ctx->transfer_id = next_transfer_id++;
    ctx->client_id = client_id;
    strncpy(ctx->filename, filename, MAX_FILENAME_LEN - 1);
    ctx->file_size = file_size;
    ctx->bytes_transferred = 0;
    ctx->chunk_size = FILE_CHUNK_SIZE;
    ctx->current_chunk = 0;
    ctx->encrypted = false;
    ctx->upload = upload;
    ctx->start_time = time(NULL);
    ctx->last_update = ctx->start_time;
    ctx->file_handle = NULL;
    
    return ctx;
}

/**
 * 销毁传输上下文
 */
static void destroy_transfer_context(file_transfer_context_t *ctx) {
    if (!ctx) return;
    
    if (ctx->file_handle) {
        fclose(ctx->file_handle);
    }
    
    free(ctx);
}

/**
 * 处理文件上传
 */
static void* process_file_upload(void *arg) {
    file_transfer_context_t *ctx = (file_transfer_context_t *)arg;
    // 实际的上传处理逻辑
    // 这里可以添加更多的处理，如文件校验、病毒扫描等
    return NULL;
}

/**
 * 处理文件下载
 */
static void* process_file_download(void *arg) {
    file_transfer_context_t *ctx = (file_transfer_context_t *)arg;
    
    // 模拟文件下载过程
    uint8_t buffer[FILE_CHUNK_SIZE];
    size_t bytes_read;
    uint32_t chunk_index = 0;
    
    while ((bytes_read = fread(buffer, 1, FILE_CHUNK_SIZE, ctx->file_handle)) > 0) {
        // 加密数据（如果需要）
        uint8_t *chunk_data = buffer;
        size_t chunk_size = bytes_read;
        
        if (ctx->encrypted) {
            if (encrypt_data(buffer, bytes_read, ctx->encryption_key) != 0) {
                // 发送错误消息
                break;
            }
        }
        
        // 在实际应用中，这里应该通过WebSocket发送数据
        // 为了简化，我们只模拟进度更新
        ctx->bytes_transferred += bytes_read;
        chunk_index++;
        
        // 计算并发送进度
        float progress = (float)ctx->bytes_transferred / ctx->file_size * 100.0f;
        send_progress_update(ctx->client_id, ctx->transfer_id, progress, ctx->bytes_transferred);
        
        // 模拟网络延迟
        usleep(10000); // 10ms
    }
    
    fclose(ctx->file_handle);
    ctx->file_handle = NULL;
    
    // 发送下载完成消息
    char complete_msg[512];
    snprintf(complete_msg, sizeof(complete_msg),
            "{\"type\":\"file_download_complete\",\"data\":{\"transfer_id\":%lu,"
            "\"filename\":\"%s\",\"file_size\":%lu}}",
            ctx->transfer_id, ctx->filename, ctx->file_size);
    
    // 在实际应用中，这里应该通过WebSocket发送消息
    // 为了简化，我们只打印消息
    printf("File download complete: %s\n", complete_msg);
    
    return NULL;
}

/**
 * 发送进度更新
 */
static int send_progress_update(uint64_t client_id, uint64_t transfer_id,
                               float progress, uint64_t bytes_transferred) {
    // 在实际应用中，这里应该通过WebSocket发送消息
    // 为了简化，我们只打印进度
    printf("Transfer %lu progress: %.1f%% (%lu bytes)\n",
           transfer_id, progress, bytes_transferred);
    return 0;
}

/**
 * 加密数据
 */
static int encrypt_data(uint8_t *data, size_t length, const uint8_t *key) {
    // 简单的XOR加密（在实际应用中应使用更安全的加密算法）
    for (size_t i = 0; i < length; i++) {
        data[i] ^= key[i % 32];
    }
    return 0;
}

/**
 * 解密数据
 */
static int decrypt_data(uint8_t *data, size_t length, const uint8_t *key) {
    // 与加密相同（XOR加密解密是相同的操作）
    return encrypt_data(data, length, key);
}

/**
 * 计算文件哈希
 */
static char* calculate_file_hash(const char *filepath) {
    FILE *file = fopen(filepath, "rb");
    if (!file) return NULL;
    
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    
    uint8_t buffer[8192];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        SHA256_Update(&sha256, buffer, bytes_read);
    }
    
    fclose(file);
    
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &sha256);
    
    char *hash_str = (char*)malloc(65); // 64个字符 + null终止符
    if (!hash_str) return NULL;
    
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(hash_str + (i * 2), "%02x", hash[i]);
    }
    hash_str[64] = '\0';
    
    return hash_str;
}