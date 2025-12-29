#ifndef SECURE_CHAT_MESSAGE_HANDLER_H
#define SECURE_CHAT_MESSAGE_HANDLER_H

#include <stdbool.h>
#include <stdint.h>
#include <sqlite3.h>
#include "connection.h"
#include "auth.h"
#include "server.h"

// 消息处理器类型定义
typedef int (*message_handler_func_t)(server_instance_t *server, 
                                     client_connection_t *conn,
                                     uint32_t message_type, 
                                     const void *data, size_t len);

// 消息处理器注册项
typedef struct message_handler_entry_t {
    uint32_t message_type;
    message_handler_func_t handler;
    bool require_auth;
    bool require_encryption;
    struct message_handler_entry_t *next;
} message_handler_entry_t;

// 消息处理器管理器
typedef struct message_handler_manager_t {
    message_handler_entry_t *handlers;
    pthread_mutex_t lock;
    
    // 统计信息
    struct {
        uint64_t messages_processed;
        uint64_t messages_failed;
        uint64_t authentication_failures;
        uint64_t encryption_failures;
        uint64_t invalid_messages;
    } stats;
} message_handler_manager_t;

// 消息处理结果
typedef struct {
    bool success;
    bool response_required;
    uint32_t response_type;
    void *response_data;
    size_t response_len;
    char error_message[256];
} message_processing_result_t;

// ==================== 消息处理器管理器API ====================

/**
 * @brief 创建消息处理器管理器
 * @return 消息处理器管理器指针，失败返回NULL
 */
message_handler_manager_t* message_handler_manager_create(void);

/**
 * @brief 销毁消息处理器管理器
 * @param mgr 消息处理器管理器指针
 */
void message_handler_manager_destroy(message_handler_manager_t *mgr);

/**
 * @brief 注册消息处理器
 * @param mgr 消息处理器管理器指针
 * @param message_type 消息类型
 * @param handler 处理器函数
 * @param require_auth 是否需要认证
 * @param require_encryption 是否需要加密
 * @return 0表示成功，负数表示错误
 */
int message_handler_register(message_handler_manager_t *mgr, uint32_t message_type,
                            message_handler_func_t handler, bool require_auth,
                            bool require_encryption);

/**
 * @brief 注销消息处理器
 * @param mgr 消息处理器管理器指针
 * @param message_type 消息类型
 * @param handler 处理器函数
 * @return 0表示成功，负数表示错误
 */
int message_handler_unregister(message_handler_manager_t *mgr, uint32_t message_type,
                              message_handler_func_t handler);

/**
 * @brief 处理消息
 * @param mgr 消息处理器管理器指针
 * @param server 服务器实例指针
 * @param conn 客户端连接指针
 * @param message_type 消息类型
 * @param data 消息数据
 * @param len 数据长度
 * @param result 处理结果（可选）
 * @return 0表示成功，负数表示错误
 */
int message_handler_process(message_handler_manager_t *mgr, server_instance_t *server,
                           client_connection_t *conn, uint32_t message_type,
                           const void *data, size_t len,
                           message_processing_result_t *result);

/**
 * @brief 获取消息处理器统计信息
 * @param mgr 消息处理器管理器指针
 * @param stats 输出统计信息
 * @return 0表示成功，负数表示错误
 */
int message_handler_get_statistics(message_handler_manager_t *mgr,
                                  message_handler_stats_t *stats);

/**
 * @brief 重置消息处理器统计信息
 * @param mgr 消息处理器管理器指针
 * @return 0表示成功，负数表示错误
 */
int message_handler_reset_statistics(message_handler_manager_t *mgr);

// ==================== 内置消息处理器 ====================

// 认证消息处理器
int handle_auth_login(server_instance_t *server, client_connection_t *conn,
                     uint32_t message_type, const void *data, size_t len);
int handle_auth_logout(server_instance_t *server, client_connection_t *conn,
                      uint32_t message_type, const void *data, size_t len);
int handle_auth_register(server_instance_t *server, client_connection_t *conn,
                        uint32_t message_type, const void *data, size_t len);

// 聊天消息处理器
int handle_chat_text_send(server_instance_t *server, client_connection_t *conn,
                         uint32_t message_type, const void *data, size_t len);
int handle_chat_text_receive(server_instance_t *server, client_connection_t *conn,
                           uint32_t message_type, const void *data, size_t len);
int handle_chat_group_create(server_instance_t *server, client_connection_t *conn,
                           uint32_t message_type, const void *data, size_t len);
int handle_chat_group_join(server_instance_t *server, client_connection_t *conn,
                          uint32_t message_type, const void *data, size_t len);
int handle_chat_group_leave(server_instance_t *server, client_connection_t *conn,
                           uint32_t message_type, const void *data, size_t len);

// 文件传输处理器
int handle_file_upload_request(server_instance_t *server, client_connection_t *conn,
                              uint32_t message_type, const void *data, size_t len);
int handle_file_download_request(server_instance_t *server, client_connection_t *conn,
                                uint32_t message_type, const void *data, size_t len);
int handle_file_transfer_cancel(server_instance_t *server, client_connection_t *conn,
                               uint32_t message_type, const void *data, size_t len);
int handle_file_transfer_status(server_instance_t *server, client_connection_t *conn,
                               uint32_t message_type, const void *data, size_t len);

// 用户管理处理器
int handle_user_list_request(server_instance_t *server, client_connection_t *conn,
                            uint32_t message_type, const void *data, size_t len);
int handle_user_status_update(server_instance_t *server, client_connection_t *conn,
                             uint32_t message_type, const void *data, size_t len);
int handle_user_profile_get(server_instance_t *server, client_connection_t *conn,
                           uint32_t message_type, const void *data, size_t len);
int handle_user_profile_update(server_instance_t *server, client_connection_t *conn,
                              uint32_t message_type, const void *data, size_t len);

// 系统消息处理器
int handle_system_heartbeat(server_instance_t *server, client_connection_t *conn,
                           uint32_t message_type, const void *data, size_t len);
int handle_system_notification(server_instance_t *server, client_connection_t *conn,
                              uint32_t message_type, const void *data, size_t len);
int handle_system_error(server_instance_t *server, client_connection_t *conn,
                       uint32_t message_type, const void *data, size_t len);
int handle_system_shutdown(server_instance_t *server, client_connection_t *conn,
                          uint32_t message_type, const void *data, size_t len);

// ==================== 消息处理工具函数 ====================

/**
 * @brief 验证消息完整性
 * @param data 消息数据
 * @param len 数据长度
 * @param message_type 消息类型
 * @return true表示消息完整，false表示不完整
 */
bool message_validate_integrity(const void *data, size_t len, uint32_t message_type);

/**
 * @brief 解析消息数据
 * @param data 消息数据
 * @param len 数据长度
 * @param message_type 消息类型
 * @param parsed_data 输出解析后的数据
 * @return 0表示成功，负数表示错误
 */
int message_parse_data(const void *data, size_t len, uint32_t message_type,
                      void **parsed_data);

/**
 * @brief 创建响应消息
 * @param status_code 状态码
 * @param status_message 状态消息
 * @param data 响应数据
 * @param data_len 数据长度
 * @param response_data 输出响应数据
 * @param response_len 输出响应长度
 * @return 0表示成功，负数表示错误
 */
int message_create_response(uint32_t status_code, const char *status_message,
                           const void *data, size_t data_len,
                           void **response_data, size_t *response_len);

/**
 * @brief 发送响应消息
 * @param conn 客户端连接指针
 * @param response_type 响应消息类型
 * @param response_data 响应数据
 * @param response_len 响应长度
 * @return 0表示成功，负数表示错误
 */
int message_send_response(client_connection_t *conn, uint32_t response_type,
                         const void *response_data, size_t response_len);

/**
 * @brief 广播消息给多个用户
 * @param server 服务器实例指针
 * @param user_ids 用户ID数组
 * @param user_count 用户数量
 * @param message_type 消息类型
 * @param data 消息数据
 * @param len 数据长度
 * @return 成功发送的消息数
 */
size_t message_broadcast_to_users(server_instance_t *server, uint64_t *user_ids,
                                 size_t user_count, uint32_t message_type,
                                 const void *data, size_t len);

/**
 * @brief 验证用户权限
 * @param server 服务器实例指针
 * @param user_id 用户ID
 * @param permission 权限标识
 * @return true表示有权限，false表示无权限
 */
bool message_check_permission(server_instance_t *server, uint64_t user_id,
                             const char *permission);

/**
 * @brief 记录消息审计日志
 * @param server 服务器实例指针
 * @param conn 客户端连接指针
 * @param message_type 消息类型
 * @param status 处理状态
 * @param details 详细信息
 */
void message_log_audit(server_instance_t *server, client_connection_t *conn,
                      uint32_t message_type, const char *status,
                      const char *details);

// ==================== 消息类型定义 ====================

// 认证消息类型
#define MSG_AUTH_LOGIN                  0x00010001
#define MSG_AUTH_LOGIN_RESPONSE         0x00010002
#define MSG_AUTH_LOGOUT                 0x00010003
#define MSG_AUTH_LOGOUT_RESPONSE        0x00010004
#define MSG_AUTH_REGISTER               0x00010005
#define MSG_AUTH_REGISTER_RESPONSE      0x00010006
#define MSG_AUTH_TOKEN_REFRESH          0x00010007
#define MSG_AUTH_TOKEN_REFRESH_RESPONSE 0x00010008

// 聊天消息类型
#define MSG_CHAT_TEXT_SEND              0x00020001
#define MSG_CHAT_TEXT_SEND_RESPONSE     0x00020002
#define MSG_CHAT_TEXT_RECEIVE           0x00020003
#define MSG_CHAT_GROUP_CREATE           0x00020004
#define MSG_CHAT_GROUP_CREATE_RESPONSE  0x00020005
#define MSG_CHAT_GROUP_JOIN             0x00020006
#define MSG_CHAT_GROUP_JOIN_RESPONSE    0x00020007
#define MSG_CHAT_GROUP_LEAVE            0x00020008
#define MSG_CHAT_GROUP_LEAVE_RESPONSE   0x00020009
#define MSG_CHAT_GROUP_LIST             0x0002000A
#define MSG_CHAT_GROUP_LIST_RESPONSE    0x0002000B

// 文件传输消息类型
#define MSG_FILE_UPLOAD_REQUEST         0x00030001
#define MSG_FILE_UPLOAD_REQUEST_RESPONSE 0x00030002
#define MSG_FILE_DOWNLOAD_REQUEST       0x00030003
#define MSG_FILE_DOWNLOAD_REQUEST_RESPONSE 0x00030004
#define MSG_FILE_TRANSFER_CANCEL        0x00030005
#define MSG_FILE_TRANSFER_CANCEL_RESPONSE 0x00030006
#define MSG_FILE_TRANSFER_STATUS        0x00030007
#define MSG_FILE_TRANSFER_STATUS_RESPONSE 0x00030008
#define MSG_FILE_PROGRESS_UPDATE        0x00030009

// 用户管理消息类型
#define MSG_USER_LIST_GET               0x00040001
#define MSG_USER_LIST_GET_RESPONSE      0x00040002
#define MSG_USER_STATUS_UPDATE          0x00040003
#define MSG_USER_STATUS_UPDATE_RESPONSE 0x00040004
#define MSG_USER_PROFILE_GET            0x00040005
#define MSG_USER_PROFILE_GET_RESPONSE   0x00040006
#define MSG_USER_PROFILE_UPDATE         0x00040007
#define MSG_USER_PROFILE_UPDATE_RESPONSE 0x00040008

// 系统消息类型
#define MSG_SYSTEM_HEARTBEAT            0x00050001
#define MSG_SYSTEM_HEARTBEAT_RESPONSE   0x00050002
#define MSG_SYSTEM_NOTIFICATION         0x00050003
#define MSG_SYSTEM_ERROR                0x00050004
#define MSG_SYSTEM_SHUTDOWN             0x00050005
#define MSG_SYSTEM_SHUTDOWN_RESPONSE    0x00050006
#define MSG_SYSTEM_INFO                 0x00050007
#define MSG_SYSTEM_INFO_RESPONSE        0x00050008

#endif // SECURE_CHAT_MESSAGE_HANDLER_H