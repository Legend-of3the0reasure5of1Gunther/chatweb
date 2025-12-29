#include "message_handler.h"
#include "../common/protocol.h"
#include "../common/security.h"
#include "../common/logger.h"
#include "../common/buffer.h"
#include "../common/utils.h"
#include "database.h"
#include "file_transfer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sqlite3.h>

// ==================== 内部函数声明 ====================

static message_handler_entry_t* find_handler(message_handler_manager_t *mgr, 
                                            uint32_t message_type);
static message_handler_entry_t* create_handler_entry(uint32_t message_type,
                                                    message_handler_func_t handler,
                                                    bool require_auth,
                                                    bool require_encryption);
static void free_handler_entry(message_handler_entry_t *entry);
static void register_default_handlers(message_handler_manager_t *mgr);

static bool validate_message_auth(message_handler_manager_t *mgr,
                                 server_instance_t *server,
                                 client_connection_t *conn,
                                 message_handler_entry_t *handler);
static bool validate_message_encryption(message_handler_manager_t *mgr,
                                       server_instance_t *server,
                                       client_connection_t *conn,
                                       message_handler_entry_t *handler);

static int process_auth_messages(server_instance_t *server, 
                                client_connection_t *conn,
                                uint32_t message_type, 
                                const void *data, size_t len);
static int process_chat_messages(server_instance_t *server, 
                                client_connection_t *conn,
                                uint32_t message_type, 
                                const void *data, size_t len);
static int process_file_messages(server_instance_t *server, 
                                client_connection_t *conn,
                                uint32_t message_type, 
                                const void *data, size_t len);
static int process_user_messages(server_instance_t *server, 
                                client_connection_t *conn,
                                uint32_t message_type, 
                                const void *data, size_t len);
static int process_system_messages(server_instance_t *server, 
                                  client_connection_t *conn,
                                  uint32_t message_type, 
                                  const void *data, size_t len);

static void update_statistics(message_handler_manager_t *mgr, bool success,
                             bool auth_failure, bool encryption_failure);

// ==================== 消息处理器管理器实现 ====================

message_handler_manager_t* message_handler_manager_create(void) {
    message_handler_manager_t *mgr = (message_handler_manager_t*)calloc(1, sizeof(message_handler_manager_t));
    if (!mgr) {
        LOG_ERROR("消息处理器管理器内存分配失败");
        return NULL;
    }
    
    // 初始化互斥锁
    if (pthread_mutex_init(&mgr->lock, NULL) != 0) {
        LOG_ERROR("消息处理器管理器互斥锁初始化失败");
        free(mgr);
        return NULL;
    }
    
    // 初始化统计信息
    memset(&mgr->stats, 0, sizeof(mgr->stats));
    
    // 注册默认处理器
    register_default_handlers(mgr);
    
    LOG_INFO("消息处理器管理器创建成功");
    return mgr;
}

void message_handler_manager_destroy(message_handler_manager_t *mgr) {
    if (!mgr) {
        return;
    }
    
    LOG_DEBUG("销毁消息处理器管理器...");
    
    // 释放所有处理器
    pthread_mutex_lock(&mgr->lock);
    
    message_handler_entry_t *entry = mgr->handlers;
    while (entry) {
        message_handler_entry_t *next = entry->next;
        free_handler_entry(entry);
        entry = next;
    }
    
    mgr->handlers = NULL;
    
    pthread_mutex_unlock(&mgr->lock);
    
    // 销毁互斥锁
    pthread_mutex_destroy(&mgr->lock);
    
    // 释放管理器
    free(mgr);
    
    LOG_DEBUG("消息处理器管理器已销毁");
}

static message_handler_entry_t* find_handler(message_handler_manager_t *mgr, 
                                            uint32_t message_type) {
    message_handler_entry_t *entry = mgr->handlers;
    
    while (entry) {
        if (entry->message_type == message_type) {
            return entry;
        }
        entry = entry->next;
    }
    
    return NULL;
}

static message_handler_entry_t* create_handler_entry(uint32_t message_type,
                                                    message_handler_func_t handler,
                                                    bool require_auth,
                                                    bool require_encryption) {
    message_handler_entry_t *entry = (message_handler_entry_t*)malloc(sizeof(message_handler_entry_t));
    if (!entry) {
        return NULL;
    }
    
    entry->message_type = message_type;
    entry->handler = handler;
    entry->require_auth = require_auth;
    entry->require_encryption = require_encryption;
    entry->next = NULL;
    
    return entry;
}

static void free_handler_entry(message_handler_entry_t *entry) {
    if (!entry) {
        return;
    }
    
    free(entry);
}

int message_handler_register(message_handler_manager_t *mgr, uint32_t message_type,
                            message_handler_func_t handler, bool require_auth,
                            bool require_encryption) {
    if (!mgr || !handler) {
        return -1;
    }
    
    pthread_mutex_lock(&mgr->lock);
    
    // 检查是否已注册
    message_handler_entry_t *existing = find_handler(mgr, message_type);
    if (existing) {
        LOG_WARNING("消息处理器已注册: 0x%08x", message_type);
        pthread_mutex_unlock(&mgr->lock);
        return -2;
    }
    
    // 创建新的处理器条目
    message_handler_entry_t *entry = create_handler_entry(message_type, handler,
                                                         require_auth, require_encryption);
    if (!entry) {
        LOG_ERROR("处理器条目创建失败");
        pthread_mutex_unlock(&mgr->lock);
        return -3;
    }
    
    // 添加到链表头部
    entry->next = mgr->handlers;
    mgr->handlers = entry;
    
    pthread_mutex_unlock(&mgr->lock);
    
    LOG_DEBUG("消息处理器注册成功: 类型=0x%08x, 需要认证=%s, 需要加密=%s",
              message_type, require_auth ? "是" : "否", 
              require_encryption ? "是" : "否");
    return 0;
}

int message_handler_unregister(message_handler_manager_t *mgr, uint32_t message_type,
                              message_handler_func_t handler) {
    if (!mgr || !handler) {
        return -1;
    }
    
    pthread_mutex_lock(&mgr->lock);
    
    message_handler_entry_t *prev = NULL;
    message_handler_entry_t *current = mgr->handlers;
    
    while (current) {
        if (current->message_type == message_type && current->handler == handler) {
            // 找到要删除的条目
            if (prev) {
                prev->next = current->next;
            } else {
                mgr->handlers = current->next;
            }
            
            free_handler_entry(current);
            
            pthread_mutex_unlock(&mgr->lock);
            
            LOG_DEBUG("消息处理器注销成功: 类型=0x%08x", message_type);
            return 0;
        }
        
        prev = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&mgr->lock);
    
    LOG_WARNING("未找到消息处理器: 类型=0x%08x", message_type);
    return -2;
}

static void register_default_handlers(message_handler_manager_t *mgr) {
    // 注册认证消息处理器
    message_handler_register(mgr, MSG_AUTH_LOGIN, handle_auth_login, false, false);
    message_handler_register(mgr, MSG_AUTH_LOGOUT, handle_auth_logout, true, false);
    message_handler_register(mgr, MSG_AUTH_REGISTER, handle_auth_register, false, false);
    message_handler_register(mgr, MSG_AUTH_TOKEN_REFRESH, handle_auth_login, true, true);
    
    // 注册聊天消息处理器
    message_handler_register(mgr, MSG_CHAT_TEXT_SEND, handle_chat_text_send, true, true);
    message_handler_register(mgr, MSG_CHAT_TEXT_RECEIVE, handle_chat_text_receive, true, true);
    message_handler_register(mgr, MSG_CHAT_GROUP_CREATE, handle_chat_group_create, true, true);
    message_handler_register(mgr, MSG_CHAT_GROUP_JOIN, handle_chat_group_join, true, true);
    message_handler_register(mgr, MSG_CHAT_GROUP_LEAVE, handle_chat_group_leave, true, true);
    
    // 注册文件传输处理器
    message_handler_register(mgr, MSG_FILE_UPLOAD_REQUEST, handle_file_upload_request, true, true);
    message_handler_register(mgr, MSG_FILE_DOWNLOAD_REQUEST, handle_file_download_request, true, true);
    message_handler_register(mgr, MSG_FILE_TRANSFER_CANCEL, handle_file_transfer_cancel, true, false);
    message_handler_register(mgr, MSG_FILE_TRANSFER_STATUS, handle_file_transfer_status, true, false);
    message_handler_register(mgr, MSG_FILE_PROGRESS_UPDATE, handle_file_transfer_status, true, true);
    
    // 注册用户管理处理器
    message_handler_register(mgr, MSG_USER_LIST_GET, handle_user_list_request, true, false);
    message_handler_register(mgr, MSG_USER_STATUS_UPDATE, handle_user_status_update, true, true);
    message_handler_register(mgr, MSG_USER_PROFILE_GET, handle_user_profile_get, true, true);
    message_handler_register(mgr, MSG_USER_PROFILE_UPDATE, handle_user_profile_update, true, true);
    
    // 注册系统消息处理器
    message_handler_register(mgr, MSG_SYSTEM_HEARTBEAT, handle_system_heartbeat, false, false);
    message_handler_register(mgr, MSG_SYSTEM_NOTIFICATION, handle_system_notification, true, false);
    message_handler_register(mgr, MSG_SYSTEM_ERROR, handle_system_error, false, false);
    message_handler_register(mgr, MSG_SYSTEM_SHUTDOWN, handle_system_shutdown, true, true);
    message_handler_register(mgr, MSG_SYSTEM_INFO, handle_system_heartbeat, true, false);
    
    LOG_DEBUG("默认消息处理器注册完成");
}

static bool validate_message_auth(message_handler_manager_t *mgr,
                                 server_instance_t *server,
                                 client_connection_t *conn,
                                 message_handler_entry_t *handler) {
    if (!handler->require_auth) {
        return true;
    }
    
    // 检查连接是否已认证
    if (conn->state != CONNECTION_STATE_AUTHENTICATED) {
        LOG_WARNING("连接未认证: 连接ID=%llu", (unsigned long long)conn->id);
        
        // 更新统计信息
        update_statistics(mgr, false, true, false);
        
        return false;
    }
    
    return true;
}

static bool validate_message_encryption(message_handler_manager_t *mgr,
                                       server_instance_t *server,
                                       client_connection_t *conn,
                                       message_handler_entry_t *handler) {
    if (!handler->require_encryption) {
        return true;
    }
    
    // 检查连接是否启用加密
    if (!conn->encryption_enabled) {
        LOG_WARNING("连接未加密: 连接ID=%llu", (unsigned long long)conn->id);
        
        // 更新统计信息
        update_statistics(mgr, false, false, true);
        
        return false;
    }
    
    return true;
}

static void update_statistics(message_handler_manager_t *mgr, bool success,
                             bool auth_failure, bool encryption_failure) {
    pthread_mutex_lock(&mgr->lock);
    
    if (success) {
        mgr->stats.messages_processed++;
    } else {
        mgr->stats.messages_failed++;
        
        if (auth_failure) {
            mgr->stats.authentication_failures++;
        }
        
        if (encryption_failure) {
            mgr->stats.encryption_failures++;
        }
    }
    
    pthread_mutex_unlock(&mgr->lock);
}

int message_handler_process(message_handler_manager_t *mgr, server_instance_t *server,
                           client_connection_t *conn, uint32_t message_type,
                           const void *data, size_t len,
                           message_processing_result_t *result) {
    if (!mgr || !server || !conn || !data || len == 0) {
        return -1;
    }
    
    // 查找消息处理器
    pthread_mutex_lock(&mgr->lock);
    message_handler_entry_t *handler = find_handler(mgr, message_type);
    pthread_mutex_unlock(&mgr->lock);
    
    if (!handler) {
        LOG_WARNING("未找到消息处理器: 类型=0x%08x", message_type);
        
        // 更新统计信息
        update_statistics(mgr, false, false, false);
        
        return -2;
    }
    
    // 验证消息完整性
    if (!message_validate_integrity(data, len, message_type)) {
        LOG_WARNING("消息完整性验证失败: 类型=0x%08x", message_type);
        
        pthread_mutex_lock(&mgr->lock);
        mgr->stats.invalid_messages++;
        pthread_mutex_unlock(&mgr->lock);
        
        return -3;
    }
    
    // 验证认证要求
    if (!validate_message_auth(mgr, server, conn, handler)) {
        return -4;
    }
    
    // 验证加密要求
    if (!validate_message_encryption(mgr, server, conn, handler)) {
        return -5;
    }
    
    // 记录审计日志
    message_log_audit(server, conn, message_type, "接收", "");
    
    // 处理消息
    int process_result = handler->handler(server, conn, message_type, data, len);
    
    // 更新统计信息
    update_statistics(mgr, (process_result == 0), false, false);
    
    // 记录处理结果
    if (process_result == 0) {
        message_log_audit(server, conn, message_type, "处理成功", "");
    } else {
        message_log_audit(server, conn, message_type, "处理失败", "");
    }
    
    return process_result;
}

int message_handler_get_statistics(message_handler_manager_t *mgr,
                                  message_handler_stats_t *stats) {
    if (!mgr || !stats) {
        return -1;
    }
    
    pthread_mutex_lock(&mgr->lock);
    memcpy(stats, &mgr->stats, sizeof(message_handler_stats_t));
    pthread_mutex_unlock(&mgr->lock);
    
    return 0;
}

int message_handler_reset_statistics(message_handler_manager_t *mgr) {
    if (!mgr) {
        return -1;
    }
    
    pthread_mutex_lock(&mgr->lock);
    memset(&mgr->stats, 0, sizeof(mgr->stats));
    pthread_mutex_unlock(&mgr->lock);
    
    LOG_DEBUG("消息处理器统计信息已重置");
    return 0;
}

// ==================== 内置消息处理器实现 ====================

int handle_auth_login(server_instance_t *server, client_connection_t *conn,
                     uint32_t message_type, const void *data, size_t len) {
    if (!server || !conn || !data || len < sizeof(login_msg_t)) {
        return -1;
    }
    
    login_msg_t login_msg;
    memcpy(&login_msg, data, sizeof(login_msg_t));
    
    // 验证用户名和密码
    user_info_t user_info;
    int auth_result = user_manager_authenticate(server->user_manager, 
                                               login_msg.username, 
                                               (const char*)login_msg.password,
                                               &user_info);
    
    // 准备响应
    response_t response;
    memset(&response, 0, sizeof(response_t));
    
    if (auth_result == 0) {
        // 认证成功
        response.status_code = 0;
        strncpy(response.status_message, "登录成功", sizeof(response.status_message) - 1);
        
        // 设置连接的用户信息
        connection_set_user_info(conn, user_info.user_id, 
                                user_info.username, user_info.nickname);
        
        // 更新用户状态
        user_manager_update_status(server->user_manager, user_info.user_id, 
                                  USER_STATUS_ONLINE, "在线");
        
        // 创建会话
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &conn->client_addr.sin_addr, client_ip, sizeof(client_ip));
        
        session_info_t session_info;
        auth_manager_create_session(server->auth_manager, user_info.user_id, 
                                   client_ip, "SecureChat Client", &session_info);
        
        // 将会话信息添加到响应
        memcpy(response.data, &session_info, sizeof(session_info_t));
        response.data_length = sizeof(session_info_t);
        
        LOG_INFO("用户登录成功: %s (ID: %llu)", login_msg.username, 
                 (unsigned long long)user_info.user_id);
    } else {
        // 认证失败
        response.status_code = auth_result;
        
        switch (auth_result) {
            case -2:
                strncpy(response.status_message, "用户不存在", sizeof(response.status_message) - 1);
                break;
            case -3:
                strncpy(response.status_message, "账户被锁定", sizeof(response.status_message) - 1);
                break;
            case -4:
                strncpy(response.status_message, "账户未激活", sizeof(response.status_message) - 1);
                break;
            case -7:
                strncpy(response.status_message, "密码错误", sizeof(response.status_message) - 1);
                break;
            default:
                strncpy(response.status_message, "认证失败", sizeof(response.status_message) - 1);
                break;
        }
        
        LOG_WARNING("用户登录失败: %s, 错误码: %d", login_msg.username, auth_result);
    }
    
    // 发送响应
    uint32_t response_type = (message_type == MSG_AUTH_LOGIN) ? 
                            MSG_AUTH_LOGIN_RESPONSE : MSG_AUTH_TOKEN_REFRESH_RESPONSE;
    
    return message_send_response(conn, response_type, &response, sizeof(response_t));
}

int handle_auth_logout(server_instance_t *server, client_connection_t *conn,
                      uint32_t message_type, const void *data, size_t len) {
    if (!server || !conn) {
        return -1;
    }
    
    // 清除连接的用户信息
    connection_clear_user_info(conn);
    
    // 更新用户状态
    if (conn->user_id > 0) {
        user_manager_update_status(server->user_manager, conn->user_id, 
                                  USER_STATUS_OFFLINE, "离线");
    }
    
    // 准备响应
    response_t response;
    memset(&response, 0, sizeof(response_t));
    response.status_code = 0;
    strncpy(response.status_message, "登出成功", sizeof(response.status_message) - 1);
    
    // 发送响应
    return message_send_response(conn, MSG_AUTH_LOGOUT_RESPONSE, 
                                &response, sizeof(response_t));
}

int handle_auth_register(server_instance_t *server, client_connection_t *conn,
                        uint32_t message_type, const void *data, size_t len) {
    if (!server || !conn || !data || len < sizeof(register_msg_t)) {
        return -1;
    }
    
    register_msg_t register_msg;
    memcpy(&register_msg, data, sizeof(register_msg_t));
    
    // 创建用户
    user_info_t user_info;
    int register_result = user_manager_create_user(server->user_manager,
                                                  register_msg.username,
                                                  (const char*)register_msg.password,
                                                  register_msg.email,
                                                  register_msg.nickname,
                                                  &user_info);
    
    // 准备响应
    response_t response;
    memset(&response, 0, sizeof(response_t));
    
    if (register_result == 0) {
        response.status_code = 0;
        strncpy(response.status_message, "注册成功", sizeof(response.status_message) - 1);
        
        // 将用户信息添加到响应
        memcpy(response.data, &user_info, sizeof(user_info_t));
        response.data_length = sizeof(user_info_t);
        
        LOG_INFO("用户注册成功: %s (ID: %llu)", register_msg.username, 
                 (unsigned long long)user_info.user_id);
    } else {
        response.status_code = register_result;
        
        switch (register_result) {
            case -2:
                strncpy(response.status_message, "用户名无效", sizeof(response.status_message) - 1);
                break;
            case -3:
                strncpy(response.status_message, "密码强度不足", sizeof(response.status_message) - 1);
                break;
            case -4:
                strncpy(response.status_message, "邮箱地址无效", sizeof(response.status_message) - 1);
                break;
            case -6:
                strncpy(response.status_message, "用户名已存在", sizeof(response.status_message) - 1);
                break;
            case -8:
                strncpy(response.status_message, "邮箱地址已存在", sizeof(response.status_message) - 1);
                break;
            default:
                strncpy(response.status_message, "注册失败", sizeof(response.status_message) - 1);
                break;
        }
        
        LOG_WARNING("用户注册失败: %s, 错误码: %d", register_msg.username, register_result);
    }
    
    // 发送响应
    return message_send_response(conn, MSG_AUTH_REGISTER_RESPONSE, 
                                &response, sizeof(response_t));
}

int handle_chat_text_send(server_instance_t *server, client_connection_t *conn,
                         uint32_t message_type, const void *data, size_t len) {
    if (!server || !conn || !data || len < sizeof(message_content_t)) {
        return -1;
    }
    
    message_content_t *msg = (message_content_t*)data;
    
    // 验证发送者权限
    if (msg->sender_id != conn->user_id) {
        LOG_WARNING("发送者ID不匹配: 期望=%llu, 实际=%llu", 
                   (unsigned long long)conn->user_id, 
                   (unsigned long long)msg->sender_id);
        return -2;
    }
    
    // 检查消息大小
    if (msg->content_length > MAX_MESSAGE_SIZE) {
        LOG_WARNING("消息太大: %u > %u", msg->content_length, MAX_MESSAGE_SIZE);
        return -3;
    }
    
    // 验证接收者
    if (msg->receiver_id == 0) {
        // 广播消息
        // 获取在线用户列表
        user_info_t *users = NULL;
        size_t user_count = 0;
        
        if (user_manager_get_online_users(server->user_manager, &users, &user_count) == 0) {
            // 准备用户ID数组
            uint64_t *user_ids = (uint64_t*)malloc(user_count * sizeof(uint64_t));
            if (user_ids) {
                for (size_t i = 0; i < user_count; i++) {
                    // 排除发送者自己
                    if (users[i].user_id != conn->user_id) {
                        user_ids[i] = users[i].user_id;
                    }
                }
                
                // 广播消息
                message_broadcast_to_users(server, user_ids, user_count - 1,
                                          MSG_CHAT_TEXT_RECEIVE, data, len);
                
                free(user_ids);
            }
            
            user_manager_free_user_list(users, user_count);
        }
    } else {
        // 私聊消息
        // 检查接收者是否在线
        if (user_manager_is_online(server->user_manager, msg->receiver_id)) {
            // 发送给接收者
            server_send_to_user(server, msg->receiver_id, 
                               MSG_CHAT_TEXT_RECEIVE, data, len);
        } else {
            // 存储离线消息
            // 这里可以保存到数据库，等用户上线时再发送
            LOG_DEBUG("用户离线，消息已存储: 接收者ID=%llu", 
                     (unsigned long long)msg->receiver_id);
        }
    }
    
    // 将消息保存到数据库
    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO messages (sender_id, receiver_id, message_type, "
                     "content, content_hash, timestamp, status) "
                     "VALUES (?, ?, ?, ?, ?, ?, ?)";
    
    pthread_mutex_lock(&server->user_manager->db_mutex);
    
    if (sqlite3_prepare_v2(server->db_conn, sql, -1, &stmt, NULL) == SQLITE_OK) {
        uint64_t now = get_current_timestamp_ms();
        uint32_t content_hash = calculate_message_checksum(msg->content, msg->content_length);
        
        sqlite3_bind_int64(stmt, 1, msg->sender_id);
        sqlite3_bind_int64(stmt, 2, msg->receiver_id);
        sqlite3_bind_int(stmt, 3, msg->message_type);
        sqlite3_bind_blob(stmt, 4, msg->content, msg->content_length, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, "hash", -1, SQLITE_STATIC); // 简化处理
        sqlite3_bind_int64(stmt, 6, now);
        sqlite3_bind_int(stmt, 7, 1); // 已发送
        
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    
    pthread_mutex_unlock(&server->user_manager->db_mutex);
    
    // 发送发送成功的响应
    response_t response;
    memset(&response, 0, sizeof(response_t));
    response.status_code = 0;
    strncpy(response.status_message, "消息发送成功", sizeof(response.status_message) - 1);
    
    return message_send_response(conn, MSG_CHAT_TEXT_SEND_RESPONSE, 
                                &response, sizeof(response_t));
}

int handle_chat_text_receive(server_instance_t *server, client_connection_t *conn,
                           uint32_t message_type, const void *data, size_t len) {
    // 这个消息类型通常由服务器发送给客户端，客户端不需要处理
    // 这里只记录日志
    LOG_DEBUG("聊天消息接收处理器: 连接ID=%llu", (unsigned long long)conn->id);
    return 0;
}

int handle_chat_group_create(server_instance_t *server, client_connection_t *conn,
                           uint32_t message_type, const void *data, size_t len) {
    if (!server || !conn || !data) {
        return -1;
    }
    
    // 解析群组创建请求
    group_create_msg_t *group_msg = (group_create_msg_t*)data;
    
    // 验证权限
    if (!message_check_permission(server, conn->user_id, "group.create")) {
        LOG_WARNING("用户无创建群组权限: 用户ID=%llu", (unsigned long long)conn->user_id);
        return -2;
    }
    
    // 创建群组
    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO groups (name, description, creator_id, created_at) "
                     "VALUES (?, ?, ?, ?)";
    
    pthread_mutex_lock(&server->user_manager->db_mutex);
    
    int group_id = -1;
    if (sqlite3_prepare_v2(server->db_conn, sql, -1, &stmt, NULL) == SQLITE_OK) {
        uint64_t now = get_current_timestamp_ms();
        
        sqlite3_bind_text(stmt, 1, group_msg->name, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, group_msg->description, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 3, conn->user_id);
        sqlite3_bind_int64(stmt, 4, now);
        
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            group_id = sqlite3_last_insert_rowid(server->db_conn);
        }
        
        sqlite3_finalize(stmt);
    }
    
    pthread_mutex_unlock(&server->user_manager->db_mutex);
    
    // 准备响应
    response_t response;
    memset(&response, 0, sizeof(response_t));
    
    if (group_id > 0) {
        response.status_code = 0;
        strncpy(response.status_message, "群组创建成功", sizeof(response.status_message) - 1);
        
        // 将群组ID添加到响应
        memcpy(response.data, &group_id, sizeof(int));
        response.data_length = sizeof(int);
        
        LOG_INFO("群组创建成功: %s (ID: %d)", group_msg->name, group_id);
    } else {
        response.status_code = -1;
        strncpy(response.status_message, "群组创建失败", sizeof(response.status_message) - 1);
        
        LOG_WARNING("群组创建失败: %s", group_msg->name);
    }
    
    // 发送响应
    return message_send_response(conn, MSG_CHAT_GROUP_CREATE_RESPONSE, 
                                &response, sizeof(response_t));
}

int handle_chat_group_join(server_instance_t *server, client_connection_t *conn,
                          uint32_t message_type, const void *data, size_t len) {
    if (!server || !conn || !data || len < sizeof(group_join_msg_t)) {
        return -1;
    }
    
    group_join_msg_t *join_msg = (group_join_msg_t*)data;
    
    // 检查用户是否已经在群组中
    sqlite3_stmt *stmt;
    const char *sql = "SELECT COUNT(*) FROM group_members WHERE group_id = ? AND user_id = ?";
    
    pthread_mutex_lock(&server->user_manager->db_mutex);
    
    bool already_member = false;
    if (sqlite3_prepare_v2(server->db_conn, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, join_msg->group_id);
        sqlite3_bind_int64(stmt, 2, conn->user_id);
        
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int count = sqlite3_column_int(stmt, 0);
            already_member = (count > 0);
        }
        
        sqlite3_finalize(stmt);
    }
    
    pthread_mutex_unlock(&server->user_manager->db_mutex);
    
    // 准备响应
    response_t response;
    memset(&response, 0, sizeof(response_t));
    
    if (already_member) {
        response.status_code = 0;
        strncpy(response.status_message, "用户已在群组中", sizeof(response.status_message) - 1);
        
        LOG_DEBUG("用户已在群组中: 用户ID=%llu, 群组ID=%d", 
                 (unsigned long long)conn->user_id, join_msg->group_id);
    } else {
        // 加入群组
        sql = "INSERT INTO group_members (group_id, user_id, joined_at) VALUES (?, ?, ?)";
        
        pthread_mutex_lock(&server->user_manager->db_mutex);
        
        bool success = false;
        if (sqlite3_prepare_v2(server->db_conn, sql, -1, &stmt, NULL) == SQLITE_OK) {
            uint64_t now = get_current_timestamp_ms();
            
            sqlite3_bind_int(stmt, 1, join_msg->group_id);
            sqlite3_bind_int64(stmt, 2, conn->user_id);
            sqlite3_bind_int64(stmt, 3, now);
            
            success = (sqlite3_step(stmt) == SQLITE_DONE);
            
            sqlite3_finalize(stmt);
        }
        
        pthread_mutex_unlock(&server->user_manager->db_mutex);
        
        if (success) {
            response.status_code = 0;
            strncpy(response.status_message, "加入群组成功", sizeof(response.status_message) - 1);
            
            LOG_INFO("用户加入群组: 用户ID=%llu, 群组ID=%d", 
                    (unsigned long long)conn->user_id, join_msg->group_id);
        } else {
            response.status_code = -1;
            strncpy(response.status_message, "加入群组失败", sizeof(response.status_message) - 1);
            
            LOG_WARNING("用户加入群组失败: 用户ID=%llu, 群组ID=%d", 
                       (unsigned long long)conn->user_id, join_msg->group_id);
        }
    }
    
    // 发送响应
    return message_send_response(conn, MSG_CHAT_GROUP_JOIN_RESPONSE, 
                                &response, sizeof(response_t));
}

int handle_chat_group_leave(server_instance_t *server, client_connection_t *conn,
                           uint32_t message_type, const void *data, size_t len) {
    if (!server || !conn || !data || len < sizeof(group_leave_msg_t)) {
        return -1;
    }
    
    group_leave_msg_t *leave_msg = (group_leave_msg_t*)data;
    
    // 离开群组
    sqlite3_stmt *stmt;
    const char *sql = "DELETE FROM group_members WHERE group_id = ? AND user_id = ?";
    
    pthread_mutex_lock(&server->user_manager->db_mutex);
    
    bool success = false;
    if (sqlite3_prepare_v2(server->db_conn, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, leave_msg->group_id);
        sqlite3_bind_int64(stmt, 2, conn->user_id);
        
        success = (sqlite3_step(stmt) == SQLITE_DONE);
        
        sqlite3_finalize(stmt);
    }
    
    pthread_mutex_unlock(&server->user_manager->db_mutex);
    
    // 准备响应
    response_t response;
    memset(&response, 0, sizeof(response_t));
    
    if (success) {
        response.status_code = 0;
        strncpy(response.status_message, "离开群组成功", sizeof(response.status_message) - 1);
        
        LOG_INFO("用户离开群组: 用户ID=%llu, 群组ID=%d", 
                (unsigned long long)conn->user_id, leave_msg->group_id);
    } else {
        response.status_code = -1;
        strncpy(response.status_message, "离开群组失败", sizeof(response.status_message) - 1);
        
        LOG_WARNING("用户离开群组失败: 用户ID=%llu, 群组ID=%d", 
                   (unsigned long long)conn->user_id, leave_msg->group_id);
    }
    
    // 发送响应
    return message_send_response(conn, MSG_CHAT_GROUP_LEAVE_RESPONSE, 
                                &response, sizeof(response_t));
}

int handle_file_upload_request(server_instance_t *server, client_connection_t *conn,
                              uint32_t message_type, const void *data, size_t len) {
    if (!server || !conn || !data || len < sizeof(file_transfer_request_t)) {
        return -1;
    }
    
    file_transfer_request_t *request = (file_transfer_request_t*)data;
    
    // 验证发送者
    if (request->sender_id != conn->user_id) {
        LOG_WARNING("文件上传请求发送者ID不匹配");
        return -2;
    }
    
    // 检查文件大小限制
    if (request->metadata.file_size > server->config.max_file_size) {
        LOG_WARNING("文件太大: %llu > %llu", 
                   (unsigned long long)request->metadata.file_size,
                   (unsigned long long)server->config.max_file_size);
        
        // 发送错误响应
        response_t response;
        memset(&response, 0, sizeof(response_t));
        response.status_code = -1;
        snprintf(response.status_message, sizeof(response.status_message),
                "文件太大，最大支持 %llu 字节", 
                (unsigned long long)server->config.max_file_size);
        
        return message_send_response(conn, MSG_FILE_UPLOAD_REQUEST_RESPONSE,
                                    &response, sizeof(response_t));
    }
    
    // 创建文件传输任务
    uint64_t transfer_id;
    int result = file_transfer_create_upload(server->file_transfer_mgr,
                                            request->sender_id,
                                            request->receiver_id,
                                            &request->metadata,
                                            &transfer_id);
    
    // 准备响应
    response_t response;
    memset(&response, 0, sizeof(response_t));
    
    if (result == 0) {
        response.status_code = 0;
        strncpy(response.status_message, "文件上传请求已接受", 
                sizeof(response.status_message) - 1);
        
        // 将传输ID添加到响应
        memcpy(response.data, &transfer_id, sizeof(transfer_id));
        response.data_length = sizeof(transfer_id);
        
        LOG_INFO("文件上传请求已接受: 传输ID=%llu, 发送者=%llu, 接收者=%llu, 文件=%s, 大小=%llu",
                (unsigned long long)transfer_id,
                (unsigned long long)request->sender_id,
                (unsigned long long)request->receiver_id,
                request->metadata.filename,
                (unsigned long long)request->metadata.file_size);
    } else {
        response.status_code = result;
        strncpy(response.status_message, "文件上传请求失败", 
                sizeof(response.status_message) - 1);
        
        LOG_WARNING("文件上传请求失败: 错误码=%d", result);
    }
    
    // 发送响应
    return message_send_response(conn, MSG_FILE_UPLOAD_REQUEST_RESPONSE,
                                &response, sizeof(response_t));
}

int handle_file_download_request(server_instance_t *server, client_connection_t *conn,
                                uint32_t message_type, const void *data, size_t len) {
    if (!server || !conn || !data || len < sizeof(file_download_request_t)) {
        return -1;
    }
    
    file_download_request_t *request = (file_download_request_t*)data;
    
    // 验证用户权限
    if (!message_check_permission(server, conn->user_id, "file.download")) {
        LOG_WARNING("用户无文件下载权限: 用户ID=%llu", (unsigned long long)conn->user_id);
        return -2;
    }
    
    // 检查文件是否存在
    sqlite3_stmt *stmt;
    const char *sql = "SELECT file_path, file_size FROM file_transfers WHERE transfer_id = ?";
    
    pthread_mutex_lock(&server->user_manager->db_mutex);
    
    char file_path[512] = {0};
    uint64_t file_size = 0;
    bool file_exists = false;
    
    if (sqlite3_prepare_v2(server->db_conn, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, request->transfer_id);
        
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *path = (const char*)sqlite3_column_text(stmt, 0);
            if (path) {
                strncpy(file_path, path, sizeof(file_path) - 1);
                file_size = sqlite3_column_int64(stmt, 1);
                file_exists = true;
            }
        }
        
        sqlite3_finalize(stmt);
    }
    
    pthread_mutex_unlock(&server->user_manager->db_mutex);
    
    // 准备响应
    response_t response;
    memset(&response, 0, sizeof(response_t));
    
    if (file_exists) {
        // 检查文件是否实际存在
        struct stat st;
        if (stat(file_path, &st) == 0 && S_ISREG(st.st_mode)) {
            // 创建文件下载传输
            uint64_t new_transfer_id;
            file_transfer_metadata_t metadata;
            memset(&metadata, 0, sizeof(metadata));
            
            strncpy(metadata.filename, strrchr(file_path, '/') ? 
                   strrchr(file_path, '/') + 1 : file_path, 
                   sizeof(metadata.filename) - 1);
            metadata.file_size = file_size;
            metadata.uploaded_at = get_current_timestamp_ms();
            
            int result = file_transfer_create_download(server->file_transfer_mgr,
                                                      conn->user_id,
                                                      request->receiver_id,
                                                      &metadata,
                                                      file_path,
                                                      &new_transfer_id);
            
            if (result == 0) {
                response.status_code = 0;
                strncpy(response.status_message, "文件下载请求已接受",
                        sizeof(response.status_message) - 1);
                
                // 将传输ID添加到响应
                memcpy(response.data, &new_transfer_id, sizeof(new_transfer_id));
                response.data_length = sizeof(new_transfer_id);
                
                LOG_INFO("文件下载请求已接受: 传输ID=%llu, 文件=%s, 大小=%llu",
                        (unsigned long long)new_transfer_id,
                        metadata.filename,
                        (unsigned long long)file_size);
            } else {
                response.status_code = result;
                strncpy(response.status_message, "文件下载请求失败",
                        sizeof(response.status_message) - 1);
                
                LOG_WARNING("文件下载请求失败: 错误码=%d", result);
            }
        } else {
            response.status_code = -3;
            strncpy(response.status_message, "文件不存在",
                    sizeof(response.status_message) - 1);
            
            LOG_WARNING("文件不存在: %s", file_path);
        }
    } else {
        response.status_code = -2;
        strncpy(response.status_message, "传输记录不存在",
                sizeof(response.status_message) - 1);
        
        LOG_WARNING("传输记录不存在: 传输ID=%llu", 
                   (unsigned long long)request->transfer_id);
    }
    
    // 发送响应
    return message_send_response(conn, MSG_FILE_DOWNLOAD_REQUEST_RESPONSE,
                                &response, sizeof(response_t));
}

int handle_file_transfer_cancel(server_instance_t *server, client_connection_t *conn,
                               uint32_t message_type, const void *data, size_t len) {
    if (!server || !conn || !data || len < sizeof(file_transfer_cancel_t)) {
        return -1;
    }
    
    file_transfer_cancel_t *cancel_request = (file_transfer_cancel_t*)data;
    
    // 取消文件传输
    int result = file_transfer_cancel(server->file_transfer_mgr,
                                     cancel_request->transfer_id);
    
    // 准备响应
    response_t response;
    memset(&response, 0, sizeof(response_t));
    
    if (result == 0) {
        response.status_code = 0;
        strncpy(response.status_message, "文件传输已取消",
                sizeof(response.status_message) - 1);
        
        LOG_INFO("文件传输已取消: 传输ID=%llu, 用户ID=%llu",
                (unsigned long long)cancel_request->transfer_id,
                (unsigned long long)conn->user_id);
    } else {
        response.status_code = result;
        strncpy(response.status_message, "文件传输取消失败",
                sizeof(response.status_message) - 1);
        
        LOG_WARNING("文件传输取消失败: 传输ID=%llu, 错误码=%d",
                   (unsigned long long)cancel_request->transfer_id, result);
    }
    
    // 发送响应
    return message_send_response(conn, MSG_FILE_TRANSFER_CANCEL_RESPONSE,
                                &response, sizeof(response_t));
}

int handle_file_transfer_status(server_instance_t *server, client_connection_t *conn,
                               uint32_t message_type, const void *data, size_t len) {
    if (!server || !conn) {
        return -1;
    }
    
    if (message_type == MSG_FILE_PROGRESS_UPDATE) {
        // 处理进度更新
        if (len < sizeof(file_transfer_status_t)) {
            return -1;
        }
        
        file_transfer_status_t *status = (file_transfer_status_t*)data;
        
        // 更新传输状态
        int result = file_transfer_update_status(server->file_transfer_mgr,
                                                status->transfer_id,
                                                status->progress,
                                                status->status);
        
        if (result == 0) {
            // 如果是完成状态，更新数据库
            if (status->status == 2) { // 完成
                sqlite3_stmt *stmt;
                const char *sql = "UPDATE file_transfers SET status = ?, progress = ?, "
                                 "end_time = ? WHERE transfer_id = ?";
                
                pthread_mutex_lock(&server->user_manager->db_mutex);
                
                if (sqlite3_prepare_v2(server->db_conn, sql, -1, &stmt, NULL) == SQLITE_OK) {
                    uint64_t now = get_current_timestamp_ms();
                    
                    sqlite3_bind_int(stmt, 1, status->status);
                    sqlite3_bind_int(stmt, 2, status->progress);
                    sqlite3_bind_int64(stmt, 3, now);
                    sqlite3_bind_int64(stmt, 4, status->transfer_id);
                    
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                }
                
                pthread_mutex_unlock(&server->user_manager->db_mutex);
                
                LOG_INFO("文件传输完成: 传输ID=%llu, 进度=%u%%",
                        (unsigned long long)status->transfer_id,
                        status->progress);
            }
            
            return 0;
        } else {
            LOG_WARNING("文件传输状态更新失败: 传输ID=%llu, 错误码=%d",
                       (unsigned long long)status->transfer_id, result);
            return result;
        }
    } else {
        // 请求传输状态
        if (len < sizeof(uint64_t)) {
            return -1;
        }
        
        uint64_t transfer_id = *(uint64_t*)data;
        
        // 获取传输状态
        file_transfer_status_t status;
        int result = file_transfer_get_status(server->file_transfer_mgr,
                                             transfer_id, &status);
        
        // 准备响应
        response_t response;
        memset(&response, 0, sizeof(response_t));
        
        if (result == 0) {
            response.status_code = 0;
            strncpy(response.status_message, "传输状态获取成功",
                    sizeof(response.status_message) - 1);
            
            // 将状态信息添加到响应
            memcpy(response.data, &status, sizeof(status));
            response.data_length = sizeof(status);
        } else {
            response.status_code = result;
            strncpy(response.status_message, "传输状态获取失败",
                    sizeof(response.status_message) - 1);
        }
        
        // 发送响应
        return message_send_response(conn, MSG_FILE_TRANSFER_STATUS_RESPONSE,
                                    &response, sizeof(response_t));
    }
}

int handle_user_list_request(server_instance_t *server, client_connection_t *conn,
                            uint32_t message_type, const void *data, size_t len) {
    if (!server || !conn) {
        return -1;
    }
    
    user_list_request_t *request = (user_list_request_t*)data;
    
    // 获取用户列表
    user_info_t *users = NULL;
    size_t user_count = 0;
    
    int result = user_manager_get_users(server->user_manager, 
                                       request->online_only,
                                       request->min_user_id,
                                       request->max_user_id,
                                       request->limit,
                                       &users, &user_count);
    
    // 准备响应
    response_t response;
    memset(&response, 0, sizeof(response_t));
    
    if (result == 0 && user_count > 0) {
        response.status_code = 0;
        strncpy(response.status_message, "用户列表获取成功",
                sizeof(response.status_message) - 1);
        
        // 将用户列表添加到响应
        response.data_length = user_count * sizeof(user_info_t);
        response.data = malloc(response.data_length);
        if (response.data) {
            memcpy(response.data, users, response.data_length);
        } else {
            response.status_code = -1;
            response.data_length = 0;
            strncpy(response.status_message, "内存分配失败",
                    sizeof(response.status_message) - 1);
        }
        
        LOG_DEBUG("用户列表获取成功: 数量=%zu", user_count);
    } else if (user_count == 0) {
        response.status_code = 0;
        strncpy(response.status_message, "用户列表为空",
                sizeof(response.status_message) - 1);
        response.data_length = 0;
    } else {
        response.status_code = result;
        strncpy(response.status_message, "用户列表获取失败",
                sizeof(response.status_message) - 1);
        response.data_length = 0;
    }
    
    // 发送响应
    int send_result = message_send_response(conn, MSG_USER_LIST_GET_RESPONSE,
                                           &response, sizeof(response_t));
    
    // 释放用户列表
    if (users) {
        user_manager_free_user_list(users, user_count);
    }
    
    // 释放响应数据
    if (response.data) {
        free(response.data);
    }
    
    return send_result;
}

int handle_user_status_update(server_instance_t *server, client_connection_t *conn,
                             uint32_t message_type, const void *data, size_t len) {
    if (!server || !conn || !data || len < sizeof(user_status_update_t)) {
        return -1;
    }
    
    user_status_update_t *status_update = (user_status_update_t*)data;
    
    // 验证用户权限
    if (status_update->user_id != conn->user_id) {
        if (!message_check_permission(server, conn->user_id, "user.status.update")) {
            LOG_WARNING("用户无更新他人状态权限: 请求者=%llu, 目标=%llu",
                       (unsigned long long)conn->user_id,
                       (unsigned long long)status_update->user_id);
            return -2;
        }
    }
    
    // 更新用户状态
    int result = user_manager_update_status(server->user_manager,
                                           status_update->user_id,
                                           status_update->status,
                                           status_update->status_message);
    
    // 准备响应
    response_t response;
    memset(&response, 0, sizeof(response_t));
    
    if (result == 0) {
        response.status_code = 0;
        strncpy(response.status_message, "用户状态更新成功",
                sizeof(response.status_message) - 1);
        
        // 如果状态改变，广播给其他用户
        if (status_update->user_id == conn->user_id) {
            // 广播状态更新
            user_info_t user_info;
            if (user_manager_get_user_info(server->user_manager,
                                          status_update->user_id,
                                          &user_info) == 0) {
                server_broadcast_message(server, MSG_USER_STATUS_UPDATE,
                                        &user_info, sizeof(user_info_t),
                                        status_update->user_id);
            }
        }
        
        LOG_INFO("用户状态更新: 用户ID=%llu, 状态=%d, 消息=%s",
                (unsigned long long)status_update->user_id,
                status_update->status,
                status_update->status_message ? status_update->status_message : "");
    } else {
        response.status_code = result;
        strncpy(response.status_message, "用户状态更新失败",
                sizeof(response.status_message) - 1);
        
        LOG_WARNING("用户状态更新失败: 用户ID=%llu, 错误码=%d",
                   (unsigned long long)status_update->user_id, result);
    }
    
    // 发送响应
    return message_send_response(conn, MSG_USER_STATUS_UPDATE_RESPONSE,
                                &response, sizeof(response_t));
}

int handle_user_profile_get(server_instance_t *server, client_connection_t *conn,
                           uint32_t message_type, const void *data, size_t len) {
    if (!server || !conn || !data || len < sizeof(uint64_t)) {
        return -1;
    }
    
    uint64_t target_user_id = *(uint64_t*)data;
    
    // 获取用户信息
    user_info_t user_info;
    int result = user_manager_get_user_info(server->user_manager,
                                           target_user_id, &user_info);
    
    // 准备响应
    response_t response;
    memset(&response, 0, sizeof(response_t));
    
    if (result == 0) {
        response.status_code = 0;
        strncpy(response.status_message, "用户信息获取成功",
                sizeof(response.status_message) - 1);
        
        // 将用户信息添加到响应
        memcpy(response.data, &user_info, sizeof(user_info));
        response.data_length = sizeof(user_info);
    } else {
        response.status_code = result;
        strncpy(response.status_message, "用户信息获取失败",
                sizeof(response.status_message) - 1);
        response.data_length = 0;
    }
    
    // 发送响应
    return message_send_response(conn, MSG_USER_PROFILE_GET_RESPONSE,
                                &response, sizeof(response_t));
}

int handle_user_profile_update(server_instance_t *server, client_connection_t *conn,
                              uint32_t message_type, const void *data, size_t len) {
    if (!server || !conn || !data || len < sizeof(user_profile_update_t)) {
        return -1;
    }
    
    user_profile_update_t *profile_update = (user_profile_update_t*)data;
    
    // 验证用户权限
    if (profile_update->user_id != conn->user_id) {
        if (!message_check_permission(server, conn->user_id, "user.profile.update")) {
            LOG_WARNING("用户无更新他人资料权限: 请求者=%llu, 目标=%llu",
                       (unsigned long long)conn->user_id,
                       (unsigned long long)profile_update->user_id);
            return -2;
        }
    }
    
    // 更新用户资料
    int result = user_manager_update_profile(server->user_manager,
                                            profile_update->user_id,
                                            profile_update->nickname,
                                            profile_update->email,
                                            profile_update->avatar);
    
    // 准备响应
    response_t response;
    memset(&response, 0, sizeof(response_t));
    
    if (result == 0) {
        response.status_code = 0;
        strncpy(response.status_message, "用户资料更新成功",
                sizeof(response.status_message) - 1);
        
        LOG_INFO("用户资料更新: 用户ID=%llu", 
                (unsigned long long)profile_update->user_id);
    } else {
        response.status_code = result;
        strncpy(response.status_message, "用户资料更新失败",
                sizeof(response.status_message) - 1);
        
        LOG_WARNING("用户资料更新失败: 用户ID=%llu, 错误码=%d",
                   (unsigned long long)profile_update->user_id, result);
    }
    
    // 发送响应
    return message_send_response(conn, MSG_USER_PROFILE_UPDATE_RESPONSE,
                                &response, sizeof(response_t));
}

int handle_system_heartbeat(server_instance_t *server, client_connection_t *conn,
                           uint32_t message_type, const void *data, size_t len) {
    if (!server || !conn) {
        return -1;
    }
    
    uint64_t timestamp = 0;
    if (len >= sizeof(uint64_t)) {
        memcpy(&timestamp, data, sizeof(uint64_t));
    }
    
    // 更新连接的最后活动时间
    connection_update_activity(conn);
    
    // 更新心跳时间
    conn->last_heartbeat = get_current_timestamp_ms();
    
    // 发送心跳响应
    uint64_t response_timestamp = get_current_timestamp_ms();
    
    if (message_type == MSG_SYSTEM_HEARTBEAT) {
        return message_send_response(conn, MSG_SYSTEM_HEARTBEAT_RESPONSE,
                                    &response_timestamp, sizeof(response_timestamp));
    } else {
        // 对于INFO消息，需要返回更多系统信息
        system_info_t system_info;
        memset(&system_info, 0, sizeof(system_info));
        
        system_info.timestamp = response_timestamp;
        system_info.server_version = PROTOCOL_VERSION;
        system_info.uptime = response_timestamp - server->stats.startup_time;
        system_info.active_connections = server->stats.active_connections;
        system_info.total_connections = server->stats.total_connections;
        
        return message_send_response(conn, MSG_SYSTEM_INFO_RESPONSE,
                                    &system_info, sizeof(system_info));
    }
}

int handle_system_notification(server_instance_t *server, client_connection_t *conn,
                              uint32_t message_type, const void *data, size_t len) {
    if (!server || !conn || !data) {
        return -1;
    }
    
    // 处理系统通知
    const char *notification = (const char*)data;
    size_t notification_len = len > 255 ? 255 : len;
    
    LOG_INFO("系统通知: %.*s", (int)notification_len, notification);
    
    // 这里可以广播通知给所有用户
    server_broadcast_message(server, MSG_SYSTEM_NOTIFICATION,
                            data, len, conn->user_id);
    
    return 0;
}

int handle_system_error(server_instance_t *server, client_connection_t *conn,
                       uint32_t message_type, const void *data, size_t len) {
    if (!server || !conn || !data || len < sizeof(error_info_t)) {
        return -1;
    }
    
    error_info_t *error_info = (error_info_t*)data;
    
    // 记录错误日志
    LOG_ERROR("系统错误: 代码=%d, 消息=%s", 
              error_info->error_code, error_info->error_message);
    
    // 这里可以处理错误，比如关闭连接、发送错误报告等
    if (error_info->error_code >= 1000) { // 严重错误
        connection_close(conn, false);
        return -1;
    }
    
    return 0;
}

int handle_system_shutdown(server_instance_t *server, client_connection_t *conn,
                          uint32_t message_type, const void *data, size_t len) {
    if (!server || !conn) {
        return -1;
    }
    
    // 验证管理员权限
    if (!message_check_permission(server, conn->user_id, "system.shutdown")) {
        LOG_WARNING("用户无关机权限: 用户ID=%llu", (unsigned long long)conn->user_id);
        return -2;
    }
    
    LOG_INFO("收到关机请求，用户ID=%llu", (unsigned long long)conn->user_id);
    
    // 发送关机响应
    response_t response;
    memset(&response, 0, sizeof(response_t));
    response.status_code = 0;
    strncpy(response.status_message, "服务器正在关闭...",
            sizeof(response.status_message) - 1);
    
    int result = message_send_response(conn, MSG_SYSTEM_SHUTDOWN_RESPONSE,
                                      &response, sizeof(response_t));
    
    if (result == 0) {
        // 延迟执行关机
        pthread_t shutdown_thread;
        pthread_create(&shutdown_thread, NULL, (void* (*)(void*))server_stop, server);
        pthread_detach(shutdown_thread);
    }
    
    return result;
}

// ==================== 消息处理工具函数实现 ====================

bool message_validate_integrity(const void *data, size_t len, uint32_t message_type) {
    // 基本验证
    if (!data || len == 0) {
        return false;
    }
    
    // 根据消息类型进行验证
    switch (message_type) {
        case MSG_AUTH_LOGIN:
            return (len >= sizeof(login_msg_t));
        case MSG_AUTH_REGISTER:
            return (len >= sizeof(register_msg_t));
        case MSG_CHAT_TEXT_SEND:
            return (len >= sizeof(message_content_t));
        case MSG_FILE_UPLOAD_REQUEST:
            return (len >= sizeof(file_transfer_request_t));
        case MSG_FILE_DOWNLOAD_REQUEST:
            return (len >= sizeof(file_download_request_t));
        case MSG_FILE_TRANSFER_CANCEL:
            return (len >= sizeof(file_transfer_cancel_t));
        case MSG_FILE_PROGRESS_UPDATE:
            return (len >= sizeof(file_transfer_status_t));
        case MSG_USER_LIST_GET:
            return (len >= sizeof(user_list_request_t));
        case MSG_USER_STATUS_UPDATE:
            return (len >= sizeof(user_status_update_t));
        case MSG_USER_PROFILE_UPDATE:
            return (len >= sizeof(user_profile_update_t));
        case MSG_SYSTEM_HEARTBEAT:
            return (len >= sizeof(uint64_t));
        case MSG_SYSTEM_ERROR:
            return (len >= sizeof(error_info_t));
        default:
            return true; // 未知消息类型，由处理器决定
    }
}

int message_parse_data(const void *data, size_t len, uint32_t message_type,
                      void **parsed_data) {
    if (!data || len == 0 || !parsed_data) {
        return -1;
    }
    
    // 根据消息类型解析数据
    switch (message_type) {
        case MSG_AUTH_LOGIN: {
            if (len < sizeof(login_msg_t)) {
                return -2;
            }
            *parsed_data = malloc(sizeof(login_msg_t));
            if (!*parsed_data) {
                return -3;
            }
            memcpy(*parsed_data, data, sizeof(login_msg_t));
            break;
        }
        case MSG_CHAT_TEXT_SEND: {
            if (len < sizeof(message_content_t)) {
                return -2;
            }
            message_content_t *msg = (message_content_t*)data;
            if (len < sizeof(message_content_t) + msg->content_length) {
                return -3;
            }
            *parsed_data = malloc(sizeof(message_content_t) + msg->content_length);
            if (!*parsed_data) {
                return -4;
            }
            memcpy(*parsed_data, data, sizeof(message_content_t) + msg->content_length);
            break;
        }
        default:
            // 默认复制整个数据
            *parsed_data = malloc(len);
            if (!*parsed_data) {
                return -5;
            }
            memcpy(*parsed_data, data, len);
            break;
    }
    
    return 0;
}

int message_create_response(uint32_t status_code, const char *status_message,
                           const void *data, size_t data_len,
                           void **response_data, size_t *response_len) {
    if (!response_data || !response_len) {
        return -1;
    }
    
    // 计算响应大小
    size_t message_len = status_message ? strlen(status_message) : 0;
    size_t total_len = sizeof(response_t) + data_len;
    
    // 分配内存
    response_t *response = (response_t*)malloc(total_len);
    if (!response) {
        return -2;
    }
    
    // 填充响应
    memset(response, 0, total_len);
    response->status_code = status_code;
    
    if (status_message) {
        strncpy(response->status_message, status_message, 
                sizeof(response->status_message) - 1);
        response->status_message[sizeof(response->status_message) - 1] = '\0';
    }
    
    response->data_length = data_len;
    if (data_len > 0 && data) {
        memcpy(response->data, data, data_len);
    }
    
    *response_data = response;
    *response_len = total_len;
    
    return 0;
}

int message_send_response(client_connection_t *conn, uint32_t response_type,
                         const void *response_data, size_t response_len) {
    if (!conn || !response_data) {
        return -1;
    }
    
    return connection_send_message(conn, response_type, response_data, response_len);
}

size_t message_broadcast_to_users(server_instance_t *server, uint64_t *user_ids,
                                 size_t user_count, uint32_t message_type,
                                 const void *data, size_t len) {
    if (!server || !user_ids || user_count == 0 || !data) {
        return 0;
    }
    
    size_t sent_count = 0;
    
    for (size_t i = 0; i < user_count; i++) {
        if (server_send_to_user(server, user_ids[i], message_type, data, len) == 0) {
            sent_count++;
        }
    }
    
    return sent_count;
}

bool message_check_permission(server_instance_t *server, uint64_t user_id,
                             const char *permission) {
    if (!server || !permission) {
        return false;
    }
    
    // 这里实现权限检查逻辑
    // 实际项目中可以使用RBAC或ABAC等权限模型
    
    // 简单实现：检查用户角色
    user_info_t user_info;
    if (user_manager_get_user_info(server->user_manager, user_id, &user_info) != 0) {
        return false;
    }
    
    // 管理员有所有权限
    if (user_info.role == USER_ROLE_ADMIN) {
        return true;
    }
    
    // 检查特定权限
    if (strcmp(permission, "user.status.update") == 0) {
        // 普通用户可以更新自己的状态
        return true;
    }
    
    if (strcmp(permission, "user.profile.update") == 0) {
        // 普通用户可以更新自己的资料
        return true;
    }
    
    if (strcmp(permission, "group.create") == 0) {
        // 需要特定角色
        return (user_info.role >= USER_ROLE_MODERATOR);
    }
    
    if (strcmp(permission, "file.download") == 0) {
        // 所有登录用户都可以下载文件
        return true;
    }
    
    if (strcmp(permission, "system.shutdown") == 0) {
        // 只有管理员可以关机
        return (user_info.role == USER_ROLE_ADMIN);
    }
    
    return false;
}

void message_log_audit(server_instance_t *server, client_connection_t *conn,
                      uint32_t message_type, const char *status,
                      const char *details) {
    if (!server || !server->config.enable_audit) {
        return;
    }
    
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm);
    
    char ip_address[INET_ADDRSTRLEN] = {0};
    if (conn) {
        inet_ntop(AF_INET, &conn->client_addr.sin_addr, ip_address, sizeof(ip_address));
    }
    
    // 记录到审计日志文件
    pthread_mutex_lock(&server->audit_mutex);
    
    if (server->audit_log) {
        fprintf(server->audit_log, "[%s] MSG=0x%08x USER=%llu CONN=%llu IP=%s STATUS=%s DETAILS=%s\n",
                timestamp, message_type,
                conn ? (unsigned long long)conn->user_id : 0,
                conn ? (unsigned long long)conn->id : 0,
                ip_address, status, details ? details : "");
        fflush(server->audit_log);
    }
    
    // 同时记录到系统日志
    LOG_DEBUG("审计日志: 消息=0x%08x, 用户=%llu, 连接=%llu, 状态=%s, 详情=%s",
              message_type,
              conn ? (unsigned long long)conn->user_id : 0,
              conn ? (unsigned long long)conn->id : 0,
              status, details ? details : "");
    
    pthread_mutex_unlock(&server->audit_mutex);
    
    // 记录到数据库
    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO audit_log (event_type, user_id, connection_id, "
                     "ip_address, description, details, timestamp) "
                     "VALUES (?, ?, ?, ?, ?, ?, ?)";
    
    pthread_mutex_lock(&server->user_manager->db_mutex);
    
    if (sqlite3_prepare_v2(server->db_conn, sql, -1, &stmt, NULL) == SQLITE_OK) {
        uint64_t timestamp_ms = get_current_timestamp_ms();
        
        sqlite3_bind_int(stmt, 1, 1); // 消息事件
        sqlite3_bind_int64(stmt, 2, conn ? conn->user_id : 0);
        sqlite3_bind_int64(stmt, 3, conn ? conn->id : 0);
        sqlite3_bind_text(stmt, 4, ip_address, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, status, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 6, details ? details : "", -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 7, timestamp_ms);
        
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    
    pthread_mutex_unlock(&server->user_manager->db_mutex);
}