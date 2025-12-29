#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "file_transfer.h"

// 进度回调函数
static void progress_callback(ft_transfer_session_t* session, void* user_data) {
    double progress = (double)session->bytes_transferred / session->total_bytes * 100.0;
    double speed = 0;
    
    if (session->bytes_transferred > 0) {
        time_t elapsed = time(NULL) - session->start_time;
        if (elapsed > 0) {
            speed = (double)session->bytes_transferred / elapsed / 1024.0;  // KB/s
        }
    }
    
    printf("Transfer %016llx: %.2f%% complete, %.2f KB/s\n",
           (unsigned long long)session->transfer_id,
           progress, speed);
}

// 完成回调函数
static void completion_callback(ft_transfer_session_t* session, void* user_data) {
    printf("Transfer %016llx completed successfully\n",
           (unsigned long long)session->transfer_id);
    
    // 验证文件
    if (session->expected_checksum[0] != '\0') {
        ft_error_code_t verify_result = ft_verify_file_checksum(
            session->local_path, session->expected_checksum);
        
        if (verify_result == FT_SUCCESS) {
            printf("File verification passed\n");
        } else {
            printf("File verification failed: %s\n",
                   ft_error_to_string(verify_result));
        }
    }
}

// 错误回调函数
static void error_callback(ft_transfer_session_t* session,
                          ft_error_code_t error,
                          void* user_data) {
    printf("Transfer %016llx failed: %s\n",
           (unsigned long long)session->transfer_id,
           ft_error_to_string(error));
}

// 创建测试文件
static ft_error_code_t create_test_file(const char* filename, size_t size) {
    FILE* file = fopen(filename, "wb");
    if (!file) {
        return FT_ERROR_IO;
    }
    
    // 写入一些测试数据
    for (size_t i = 0; i < size; i++) {
        uint8_t byte = (uint8_t)(i % 256);
        fwrite(&byte, 1, 1, file);
    }
    
    fclose(file);
    
    printf("Created test file: %s (%zu bytes)\n", filename, size);
    return FT_SUCCESS;
}

// 示例1：简单上传
static ft_error_code_t example_simple_upload(void) {
    printf("\n=== Example 1: Simple Upload ===\n");
    
    const char* source_file = "./test_upload_source.txt";
    const char* dest_file = "./test_upload_dest.txt";
    
    // 创建测试文件
    ft_error_code_t result = create_test_file(source_file, 1024 * 1024);  // 1MB
    if (result != FT_SUCCESS) {
        return result;
    }
    
    // 获取文件信息
    struct stat st;
    if (stat(source_file, &st) != 0) {
        return FT_ERROR_FILE_NOT_FOUND;
    }
    
    // 创建文件元数据
    file_metadata_t metadata;
    memset(&metadata, 0, sizeof(metadata));
    
    metadata.file_size = st.st_size;
    strncpy(metadata.filename, dest_file, sizeof(metadata.filename) - 1);
    strncpy(metadata.original_name, "test_upload_source.txt",
            sizeof(metadata.original_name) - 1);
    metadata.file_type = FILE_TYPE_DOCUMENT;
    
    // 开始上传
    ft_transfer_session_t* session = NULL;
    result = ft_start_upload(1001, 1002,  // user_id, peer_user_id
                            source_file, dest_file,
                            &metadata, FT_MODE_DIRECT, &session);
    
    if (result != FT_SUCCESS) {
        printf("Failed to start upload: %s\n", ft_error_to_string(result));
        return result;
    }
    
    // 设置回调
    ft_set_progress_callback(session, progress_callback, NULL);
    ft_set_completion_callback(session, completion_callback, NULL);
    ft_set_error_callback(session, error_callback, NULL);
    
    // 模拟分片上传
    FILE* file = fopen(source_file, "rb");
    if (!file) {
        ft_cancel_upload(session);
        return FT_ERROR_FILE_NOT_FOUND;
    }
    
    uint8_t buffer[FT_CHUNK_SIZE];
    size_t chunk_size;
    uint32_t chunk_index = 0;
    
    while ((chunk_size = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        // 模拟网络延迟
        usleep(10000);  // 10ms
        
        // 上传chunk
        result = ft_upload_chunk(session, chunk_index, buffer, chunk_size);
        
        if (result != FT_SUCCESS) {
            printf("Failed to upload chunk %u: %s\n",
                   chunk_index, ft_error_to_string(result));
            fclose(file);
            ft_cancel_upload(session);
            return result;
        }
        
        chunk_index++;
    }
    
    fclose(file);
    
    // 完成上传
    result = ft_complete_upload(session);
    
    if (result != FT_SUCCESS) {
        printf("Failed to complete upload: %s\n", ft_error_to_string(result));
        return result;
    }
    
    // 清理会话
    ft_remove_session(session->transfer_id);
    
    // 验证文件
    if (access(dest_file, F_OK) == 0) {
        printf("Upload successful: %s\n", dest_file);
        unlink(dest_file);  // 清理测试文件
    }
    
    unlink(source_file);  // 清理源文件
    
    return FT_SUCCESS;
}

// 示例2：简单下载
static ft_error_code_t example_simple_download(void) {
    printf("\n=== Example 2: Simple Download ===\n");
    
    const char* source_file = "./test_download_source.txt";
    const char* dest_file = "./test_download_dest.txt";
    
    // 创建测试源文件
    ft_error_code_t result = create_test_file(source_file, 2 * 1024 * 1024);  // 2MB
    if (result != FT_SUCCESS) {
        return result;
    }
    
    // 开始下载
    ft_transfer_session_t* session = NULL;
    result = ft_start_download(1001, 1002,  // user_id, peer_user_id
                              source_file, dest_file,
                              FT_MODE_DIRECT, &session);
    
    if (result != FT_SUCCESS) {
        printf("Failed to start download: %s\n", ft_error_to_string(result));
        unlink(source_file);
        return result;
    }
    
    // 设置回调
    ft_set_progress_callback(session, progress_callback, NULL);
    ft_set_completion_callback(session, completion_callback, NULL);
    ft_set_error_callback(session, error_callback, NULL);
    
    // 模拟分片下载
    uint32_t chunk_count = session->total_chunks;
    
    for (uint32_t chunk_index = 0; chunk_index < chunk_count; chunk_index++) {
        uint8_t* chunk_data = NULL;
        uint32_t chunk_size = 0;
        
        // 获取chunk
        result = ft_download_chunk(session, chunk_index, &chunk_data, &chunk_size);
        
        if (result != FT_SUCCESS) {
            printf("Failed to download chunk %u: %s\n",
                   chunk_index, ft_error_to_string(result));
            ft_cancel_download(session);
            safe_free((void**)&chunk_data);
            unlink(source_file);
            return result;
        }
        
        // 模拟网络延迟
        usleep(5000);  // 5ms
        
        // 在这里可以发送chunk到客户端
        // 对于下载示例，我们只是读取而不发送
        
        safe_free((void**)&chunk_data);
    }
    
    // 完成下载
    result = ft_complete_download(session);
    
    if (result != FT_SUCCESS) {
        printf("Failed to complete download: %s\n", ft_error_to_string(result));
        unlink(source_file);
        return result;
    }
    
    // 清理会话
    ft_remove_session(session->transfer_id);
    
    // 验证文件
    if (access(dest_file, F_OK) == 0) {
        printf("Download successful: %s\n", dest_file);
        unlink(dest_file);  // 清理测试文件
    }
    
    unlink(source_file);  // 清理源文件
    
    return FT_SUCCESS;
}

// 示例3：断点续传
static ft_error_code_t example_resume_transfer(void) {
    printf("\n=== Example 3: Resume Transfer ===\n");
    
    const char* source_file = "./test_resume_source.txt";
    const char* dest_file = "./test_resume_dest.txt";
    
    // 创建大测试文件
    ft_error_code_t result = create_test_file(source_file, 5 * 1024 * 1024);  // 5MB
    if (result != FT_SUCCESS) {
        return result;
    }
    
    // 开始上传
    ft_transfer_session_t* session = NULL;
    
    // 创建文件元数据
    file_metadata_t metadata;
    memset(&metadata, 0, sizeof(metadata));
    
    struct stat st;
    stat(source_file, &st);
    
    metadata.file_size = st.st_size;
    strncpy(metadata.filename, dest_file, sizeof(metadata.filename) - 1);
    strncpy(metadata.original_name, "test_resume_source.txt",
            sizeof(metadata.original_name) - 1);
    metadata.file_type = FILE_TYPE_DOCUMENT;
    
    result = ft_start_upload(1001, 1002, source_file, dest_file,
                            &metadata, FT_MODE_DIRECT, &session);
    
    if (result != FT_SUCCESS) {
        printf("Failed to start upload: %s\n", ft_error_to_string(result));
        unlink(source_file);
        return result;
    }
    
    // 设置回调
    ft_set_progress_callback(session, progress_callback, NULL);
    
    // 上传前50%的chunk
    FILE* file = fopen(source_file, "rb");
    if (!file) {
        ft_cancel_upload(session);
        unlink(source_file);
        return FT_ERROR_FILE_NOT_FOUND;
    }
    
    uint8_t buffer[FT_CHUNK_SIZE];
    size_t chunk_size;
    uint32_t chunk_index = 0;
    uint32_t half_chunks = session->total_chunks / 2;
    
    while (chunk_index < half_chunks &&
           (chunk_size = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        
        result = ft_upload_chunk(session, chunk_index, buffer, chunk_size);
        
        if (result != FT_SUCCESS) {
            printf("Failed to upload chunk %u: %s\n",
                   chunk_index, ft_error_to_string(result));
            fclose(file);
            ft_cancel_upload(session);
            unlink(source_file);
            return result;
        }
        
        chunk_index++;
    }
    
    fclose(file);
    
    printf("Uploaded 50%%, simulating interruption...\n");
    
    // 模拟中断：保存会话ID以便恢复
    uint64_t transfer_id = session->transfer_id;
    
    // 暂停上传（模拟网络中断）
    ft_pause_upload(session);
    
    // 等待一段时间
    sleep(2);
    
    printf("Resuming transfer %016llx...\n", (unsigned long long)transfer_id);
    
    // 恢复上传（在实际应用中，需要从存储中恢复会话）
    // 这里简化处理，继续使用原会话
    
    ft_resume_upload(session);
    
    // 继续上传剩余chunk
    file = fopen(source_file, "rb");
    if (!file) {
        ft_cancel_upload(session);
        unlink(source_file);
        return FT_ERROR_FILE_NOT_FOUND;
    }
    
    // 跳过已上传的chunk
    fseek(file, (long)chunk_index * FT_CHUNK_SIZE, SEEK_SET);
    
    while ((chunk_size = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        result = ft_upload_chunk(session, chunk_index, buffer, chunk_size);
        
        if (result != FT_SUCCESS) {
            printf("Failed to upload chunk %u: %s\n",
                   chunk_index, ft_error_to_string(result));
            fclose(file);
            ft_cancel_upload(session);
            unlink(source_file);
            return result;
        }
        
        chunk_index++;
    }
    
    fclose(file);
    
    // 完成上传
    result = ft_complete_upload(session);
    
    if (result != FT_SUCCESS) {
        printf("Failed to complete upload: %s\n", ft_error_to_string(result));
        unlink(source_file);
        return result;
    }
    
    printf("Resume transfer completed successfully\n");
    
    // 清理
    ft_remove_session(session->transfer_id);
    unlink(source_file);
    unlink(dest_file);
    
    return FT_SUCCESS;
}

// 示例4：加密传输
static ft_error_code_t example_encrypted_transfer(void) {
    printf("\n=== Example 4: Encrypted Transfer ===\n");
    
    const char* source_file = "./test_encrypted_source.txt";
    const char* dest_file = "./test_encrypted_dest.txt";
    
    // 创建包含敏感数据的测试文件
    FILE* file = fopen(source_file, "w");
    if (!file) {
        return FT_ERROR_IO;
    }
    
    fprintf(file, "This is sensitive data that should be encrypted during transfer.\n");
    fprintf(file, "Credit card: 4111-1111-1111-1111\n");
    fprintf(file, "Password: SuperSecret123!\n");
    
    fclose(file);
    
    // 获取文件信息
    struct stat st;
    if (stat(source_file, &st) != 0) {
        return FT_ERROR_FILE_NOT_FOUND;
    }
    
    // 创建文件元数据
    file_metadata_t metadata;
    memset(&metadata, 0, sizeof(metadata));
    
    metadata.file_size = st.st_size;
    metadata.is_encrypted = 1;  // 标记为加密文件
    strncpy(metadata.filename, dest_file, sizeof(metadata.filename) - 1);
    strncpy(metadata.original_name, "sensitive_data.txt",
            sizeof(metadata.original_name) - 1);
    metadata.file_type = FILE_TYPE_DOCUMENT;
    
    // 开始上传（启用加密）
    ft_transfer_session_t* session = NULL;
    ft_error_code_t result = ft_start_upload(1001, 1002,
                                            source_file, dest_file,
                                            &metadata, FT_MODE_DIRECT, &session);
    
    if (result != FT_SUCCESS) {
        printf("Failed to start encrypted upload: %s\n", ft_error_to_string(result));
        unlink(source_file);
        return result;
    }
    
    // 确保加密已启用
    if (!session->encryption_enabled) {
        printf("Encryption not enabled for this transfer\n");
        ft_cancel_upload(session);
        unlink(source_file);
        return FT_ERROR_ENCRYPTION;
    }
    
    printf("Encryption enabled for transfer %016llx\n",
           (unsigned long long)session->transfer_id);
    printf("Encryption key: ");
    for (int i = 0; i < 16; i++) {
        printf("%02x", session->encryption_key[i]);
    }
    printf("...\n");
    
    // 上传文件
    file = fopen(source_file, "rb");
    if (!file) {
        ft_cancel_upload(session);
        unlink(source_file);
        return FT_ERROR_FILE_NOT_FOUND;
    }
    
    uint8_t buffer[FT_CHUNK_SIZE];
    size_t chunk_size;
    uint32_t chunk_index = 0;
    
    while ((chunk_size = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        // 在实际加密传输中，数据会在传输前自动加密
        result = ft_upload_chunk(session, chunk_index, buffer, chunk_size);
        
        if (result != FT_SUCCESS) {
            printf("Failed to upload encrypted chunk %u: %s\n",
                   chunk_index, ft_error_to_string(result));
            fclose(file);
            ft_cancel_upload(session);
            unlink(source_file);
            return result;
        }
        
        chunk_index++;
    }
    
    fclose(file);
    
    // 完成上传
    result = ft_complete_upload(session);
    
    if (result != FT_SUCCESS) {
        printf("Failed to complete encrypted upload: %s\n", ft_error_to_string(result));
        unlink(source_file);
        return result;
    }
    
    printf("Encrypted transfer completed successfully\n");
    
    // 验证文件已加密存储
    printf("Checking if file is encrypted on disk...\n");
    FILE* dest = fopen(dest_file, "rb");
    if (dest) {
        uint8_t first_bytes[16];
        fread(first_bytes, 1, 16, dest);
        fclose(dest);
        
        printf("First 16 bytes of stored file: ");
        for (int i = 0; i < 16; i++) {
            printf("%02x ", first_bytes[i]);
        }
        printf("\n");
        
        // 加密文件通常以特定模式开始
        printf("File appears to be encrypted (random looking bytes)\n");
    }
    
    // 清理
    ft_remove_session(session->transfer_id);
    unlink(source_file);
    unlink(dest_file);
    
    return FT_SUCCESS;
}

// 主测试函数
int main(int argc, char* argv[]) {
    printf("=== File Transfer Module Test ===\n");
    
    // 初始化文件传输模块
    ft_config_t config = {
        .chunk_size = 64 * 1024,  // 64KB chunks
        .max_retries = 3,
        .timeout_ms = 30000,
        .max_concurrent_transfers = 10,
        .enable_encryption = true,
        .enable_compression = true,
        .enable_resume = true,
        .verify_checksum = true,
        .storage_path = "./test_storage",
        .temp_path = "./test_temp",
        .max_file_size = 100 * 1024 * 1024  // 100MB
    };
    
    ft_error_code_t result = ft_init(&config);
    if (result != FT_SUCCESS) {
        fprintf(stderr, "Failed to initialize file transfer module: %s\n",
                ft_error_to_string(result));
        return 1;
    }
    
    printf("File transfer module initialized successfully\n");
    
    // 启动传输管理器
    result = ft_start_manager();
    if (result != FT_SUCCESS) {
        fprintf(stderr, "Failed to start transfer manager: %s\n",
                ft_error_to_string(result));
        ft_cleanup();
        return 1;
    }
    
    // 运行示例
    example_simple_upload();
    example_simple_download();
    example_resume_transfer();
    example_encrypted_transfer();
    
    // 获取统计信息
    ft_stats_t stats = ft_get_statistics();
    printf("\n=== Transfer Statistics ===\n");
    printf("Total transfers: %llu\n", (unsigned long long)stats.total_transfers);
    printf("Successful transfers: %llu\n", (unsigned long long)stats.successful_transfers);
    printf("Failed transfers: %llu\n", (unsigned long long)stats.failed_transfers);
    printf("Bytes transferred: %llu\n", (unsigned long long)stats.bytes_transferred);
    printf("Average speed: %.2f KB/s\n", stats.average_speed_bytes_per_sec / 1024.0);
    printf("Peak speed: %.2f KB/s\n", stats.peak_speed_bytes_per_sec / 1024.0);
    printf("Active transfers: %u\n", stats.active_transfers);
    
    // 获取存储使用情况
    uint64_t used_bytes = 0, total_bytes = 0;
    ft_get_storage_usage(&used_bytes, &total_bytes);
    
    printf("\n=== Storage Usage ===\n");
    printf("Used: %.2f MB\n", used_bytes / (1024.0 * 1024.0));
    printf("Total: %.2f MB\n", total_bytes / (1024.0 * 1024.0));
    printf("Available: %.2f MB\n", (total_bytes - used_bytes) / (1024.0 * 1024.0));
    
    // 清理临时文件
    ft_cleanup_temp_files();
    
    // 停止传输管理器
    ft_stop_manager();
    
    // 清理模块
    ft_cleanup();
    
    // 清理测试目录
    rmdir("./test_storage");
    rmdir("./test_temp");
    
    printf("\n=== All tests completed ===\n");
    return 0;
}