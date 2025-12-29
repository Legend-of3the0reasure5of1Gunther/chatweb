#include "file_transfer.h"
#include "websocket_server.h"
#include <string.h>
#include <stdlib.h>
#include <json-c/json.h>

// WebSocket文件传输处理器
static void ft_websocket_upload_handler(websocket_client_t* client,
                                       const uint8_t* data,
                                       size_t data_len,
                                       int opcode) {
    if (!client || !data) {
        return;
    }
    
    ft_websocket_context_t* context = (ft_websocket_context_t*)client->user_data;
    
    if (opcode == WS_OPCODE_TEXT) {
        // 处理JSON消息
        json_object* json = json_tokener_parse((const char*)data);
        if (!json) {
            return;
        }
        
        json_object* type_obj = NULL;
        if (json_object_object_get_ex(json, "type", &type_obj)) {
            const char* type = json_object_get_string(type_obj);
            
            if (strcmp(type, "upload_start") == 0) {
                // 开始上传
                json_object* metadata_obj = NULL;
                if (json_object_object_get_ex(json, "metadata", &metadata_obj)) {
                    file_metadata_t metadata;
                    memset(&metadata, 0, sizeof(metadata));
                    
                    // 解析元数据
                    json_object* filename_obj = NULL;
                    if (json_object_object_get_ex(metadata_obj, "filename", &filename_obj)) {
                        strncpy(metadata.filename,
                               json_object_get_string(filename_obj),
                               sizeof(metadata.filename) - 1);
                    }
                    
                    json_object* size_obj = NULL;
                    if (json_object_object_get_ex(metadata_obj, "size", &size_obj)) {
                        metadata.file_size = json_object_get_int64(size_obj);
                    }
                    
                    json_object* type_obj = NULL;
                    if (json_object_object_get_ex(metadata_obj, "type", &type_obj)) {
                        metadata.file_type = json_object_get_int(type_obj);
                    }
                    
                    // 开始上传
                    ft_error_code_t result = ft_websocket_start_upload(
                        client->fd, client->user_id, &metadata, &context);
                    
                    json_object* response = json_object_new_object();
                    json_object_object_add(response, "type",
                                          json_object_new_string("upload_start_response"));
                    
                    if (result == FT_SUCCESS) {
                        json_object_object_add(response, "success",
                                              json_object_new_boolean(true));
                        json_object_object_add(response, "transfer_id",
                                              json_object_new_int64(context->transfer_id));
                        json_object_object_add(response, "chunk_size",
                                              json_object_new_int(FT_CHUNK_SIZE));
                    } else {
                        json_object_object_add(response, "success",
                                              json_object_new_boolean(false));
                        json_object_object_add(response, "error",
                                              json_object_new_string(ft_error_to_string(result)));
                    }
                    
                    const char* response_str = json_object_to_json_string(response);
                    websocket_send_text(client, response_str, strlen(response_str));
                    
                    json_object_put(response);
                }
            } else if (strcmp(type, "upload_complete") == 0) {
                // 完成上传
                if (context) {
                    ft_error_code_t result = ft_websocket_complete_upload(context);
                    
                    json_object* response = json_object_new_object();
                    json_object_object_add(response, "type",
                                          json_object_new_string("upload_complete_response"));
                    
                    if (result == FT_SUCCESS) {
                        json_object_object_add(response, "success",
                                              json_object_new_boolean(true));
                        
                        // 发送文件信息
                        json_object* file_info = json_object_new_object();
                        json_object_object_add(file_info, "id",
                                              json_object_new_int64(context->session->file_id));
                        json_object_object_add(file_info, "size",
                                              json_object_new_int64(context->session->total_bytes));
                        json_object_object_add(file_info, "checksum",
                                              json_object_new_string(context->session->calculated_checksum));
                        
                        json_object_object_add(response, "file_info", file_info);
                    } else {
                        json_object_object_add(response, "success",
                                              json_object_new_boolean(false));
                        json_object_object_add(response, "error",
                                              json_object_new_string(ft_error_to_string(result)));
                    }
                    
                    const char* response_str = json_object_to_json_string(response);
                    websocket_send_text(client, response_str, strlen(response_str));
                    
                    json_object_put(response);
                    
                    // 清理上下文
                    safe_free((void**)&context);
                    client->user_data = NULL;
                }
            } else if (strcmp(type, "upload_cancel") == 0) {
                // 取消上传
                if (context && context->session) {
                    ft_cancel_upload(context->session);
                    
                    json_object* response = json_object_new_object();
                    json_object_object_add(response, "type",
                                          json_object_new_string("upload_cancel_response"));
                    json_object_object_add(response, "success",
                                          json_object_new_boolean(true));
                    
                    const char* response_str = json_object_to_json_string(response);
                    websocket_send_text(client, response_str, strlen(response_str));
                    
                    json_object_put(response);
                    
                    safe_free((void**)&context);
                    client->user_data = NULL;
                }
            }
        }
        
        json_object_put(json);
    } else if (opcode == WS_OPCODE_BINARY) {
        // 处理二进制数据（文件chunk）
        if (context && context->session) {
            // 解析chunk头部
            if (data_len >= 8) {
                uint32_t chunk_index = *(uint32_t*)data;
                uint32_t chunk_size = *(uint32_t*)(data + 4);
                
                if (chunk_size > 0 && 8 + chunk_size <= data_len) {
                    const uint8_t* chunk_data = data + 8;
                    
                    ft_error_code_t result = ft_websocket_handle_chunk(
                        context, chunk_data, chunk_size);
                    
                    if (result == FT_SUCCESS) {
                        // 发送进度更新
                        ft_progress_info_t progress;
                        ft_get_progress(context->transfer_id, &progress);
                        
                        json_object* progress_update = json_object_new_object();
                        json_object_object_add(progress_update, "type",
                                              json_object_new_string("upload_progress"));
                        json_object_object_add(progress_update, "transfer_id",
                                              json_object_new_int64(context->transfer_id));
                        json_object_object_add(progress_update, "bytes_transferred",
                                              json_object_new_int64(progress.bytes_transferred));
                        json_object_object_add(progress_update, "total_bytes",
                                              json_object_new_int64(progress.total_bytes));
                        json_object_object_add(progress_update, "progress_percent",
                                              json_object_new_double(progress.progress_percent));
                        json_object_object_add(progress_update, "speed",
                                              json_object_new_double(progress.speed_bytes_per_sec));
                        
                        const char* progress_str = json_object_to_json_string(progress_update);
                        websocket_send_text(client, progress_str, strlen(progress_str));
                        
                        json_object_put(progress_update);
                    } else {
                        // 发送错误
                        json_object* error_response = json_object_new_object();
                        json_object_object_add(error_response, "type",
                                              json_object_new_string("upload_error"));
                        json_object_object_add(error_response, "transfer_id",
                                              json_object_new_int64(context->transfer_id));
                        json_object_object_add(error_response, "error",
                                              json_object_new_string(ft_error_to_string(result)));
                        json_object_object_add(error_response, "chunk_index",
                                              json_object_new_int(chunk_index));
                        
                        const char* error_str = json_object_to_json_string(error_response);
                        websocket_send_text(client, error_str, strlen(error_str));
                        
                        json_object_put(error_response);
                    }
                }
            }
        }
    }
}

static void ft_websocket_download_handler(websocket_client_t* client,
                                         const uint8_t* data,
                                         size_t data_len,
                                         int opcode) {
    if (!client || !data || opcode != WS_OPCODE_TEXT) {
        return;
    }
    
    json_object* json = json_tokener_parse((const char*)data);
    if (!json) {
        return;
    }
    
    json_object* type_obj = NULL;
    if (json_object_object_get_ex(json, "type", &type_obj)) {
        const char* type = json_object_get_string(type_obj);
        
        if (strcmp(type, "download_start") == 0) {
            // 开始下载
            json_object* file_id_obj = NULL;
            uint64_t file_id = 0;
            
            if (json_object_object_get_ex(json, "file_id", &file_id_obj)) {
                file_id = json_object_get_int64(file_id_obj);
            }
            
            ft_websocket_context_t* context = NULL;
            ft_error_code_t result = ft_websocket_start_download(
                client->fd, client->user_id, file_id, &context);
            
            json_object* response = json_object_new_object();
            json_object_object_add(response, "type",
                                  json_object_new_string("download_start_response"));
            
            if (result == FT_SUCCESS) {
                json_object_object_add(response, "success",
                                      json_object_new_boolean(true));
                json_object_object_add(response, "transfer_id",
                                      json_object_new_int64(context->transfer_id));
                
                // 发送文件信息
                json_object* file_info = json_object_new_object();
                json_object_object_add(file_info, "filename",
                                      json_object_new_string(context->session->metadata.filename));
                json_object_object_add(file_info, "size",
                                      json_object_new_int64(context->session->total_bytes));
                json_object_object_add(file_info, "chunks",
                                      json_object_new_int(context->session->total_chunks));
                
                json_object_object_add(response, "file_info", file_info);
                
                // 设置用户数据
                client->user_data = context;
                
                // 开始发送chunk
                ft_websocket_send_chunk(context);
            } else {
                json_object_object_add(response, "success",
                                      json_object_new_boolean(false));
                json_object_object_add(response, "error",
                                      json_object_new_string(ft_error_to_string(result)));
            }
            
            const char* response_str = json_object_to_json_string(response);
            websocket_send_text(client, response_str, strlen(response_str));
            
            json_object_put(response);
        } else if (strcmp(type, "download_chunk_ack") == 0) {
            // 确认收到chunk，发送下一个
            ft_websocket_context_t* context = (ft_websocket_context_t*)client->user_data;
            if (context) {
                ft_websocket_send_chunk(context);
            }
        }
    }
    
    json_object_put(json);
}

// 注册WebSocket处理器
void ft_register_websocket_handlers(void) {
    // 注册上传处理器
    websocket_register_handler("/file/upload", ft_websocket_upload_handler);
    
    // 注册下载处理器
    websocket_register_handler("/file/download", ft_websocket_download_handler);
}