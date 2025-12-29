#include "user_manager.h"
#include "websocket_server.h"
#include <string.h>
#include <stdlib.h>
#include <json-c/json.h>

// WebSocket用户管理处理器
static void user_manager_websocket_handler(websocket_client_t* client,
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
    if (!json_object_object_get_ex(json, "type", &type_obj)) {
        json_object_put(json);
        return;
    }
    
    const char* type = json_object_get_string(type_obj);
    json_object* response = json_object_new_object();
    
    if (strcmp(type, "get_user_info") == 0) {
        // 获取用户信息
        json_object* user_id_obj = NULL;
        uint64_t user_id = client->user_id; // 默认获取自己的信息
        
        if (json_object_object_get_ex(json, "user_id", &user_id_obj)) {
            user_id = json_object_get_int64(user_id_obj);
        }
        
        user_info_t user_info;
        user_manager_error_t result = user_manager_get_user(user_id, &user_info);
        
        json_object_object_add(response, "type",
                              json_object_new_string("user_info_response"));
        
        if (result == UM_SUCCESS) {
            json_object* user_obj = json_object_new_object();
            json_object_object_add(user_obj, "id",
                                  json_object_new_int64(user_info.user_id));
            json_object_object_add(user_obj, "username",
                                  json_object_new_string(user_info.username));
            json_object_object_add(user_obj, "nickname",
                                  json_object_new_string(user_info.nickname));
            json_object_object_add(user_obj, "avatar_id",
                                  json_object_new_int(user_info.avatar_id));
            json_object_object_add(user_obj, "status",
                                  json_object_new_int(user_info.status));
            json_object_object_add(user_obj, "status_message",
                                  json_object_new_string(user_info.status_message));
            json_object_object_add(user_obj, "last_seen",
                                  json_object_new_int64(user_info.last_seen));
            json_object_object_add(user_obj, "friend_count",
                                  json_object_new_int(user_info.friend_count));
            json_object_object_add(user_obj, "group_count",
                                  json_object_new_int(user_info.group_count));
            json_object_object_add(user_obj, "is_verified",
                                  json_object_new_boolean(user_info.is_verified));
            json_object_object_add(user_obj, "is_premium",
                                  json_object_new_boolean(user_info.is_premium));
            json_object_object_add(user_obj, "created_at",
                                  json_object_new_int64(user_info.created_at));
            
            json_object_object_add(response, "user", user_obj);
            json_object_object_add(response, "success",
                                  json_object_new_boolean(true));
        } else {
            json_object_object_add(response, "success",
                                  json_object_new_boolean(false));
            json_object_object_add(response, "error",
                                  json_object_new_string(
                                      user_manager_error_to_string(result)));
        }
        
    } else if (strcmp(type, "update_status") == 0) {
        // 更新用户状态
        json_object* status_obj = NULL;
        json_object* message_obj = NULL;
        
        if (json_object_object_get_ex(json, "status", &status_obj)) {
            user_status_t status = json_object_get_int(status_obj);
            const char* message = NULL;
            
            if (json_object_object_get_ex(json, "message", &message_obj)) {
                message = json_object_get_string(message_obj);
            }
            
            user_info_t updates;
            memset(&updates, 0, sizeof(updates));
            updates.status = status;
            
            if (message) {
                strncpy(updates.status_message, message,
                        sizeof(updates.status_message) - 1);
            }
            
            user_manager_error_t result = user_manager_update_user(
                client->user_id, &updates);
            
            json_object_object_add(response, "type",
                                  json_object_new_string("update_status_response"));
            
            if (result == UM_SUCCESS) {
                json_object_object_add(response, "success",
                                      json_object_new_boolean(true));
                
                // 如果是离线状态，同时更新在线状态
                if (status == USER_STATUS_OFFLINE) {
                    user_manager_update_online_status(client->user_id,
                                                     ONLINE_STATE_OFFLINE);
                }
            } else {
                json_object_object_add(response, "success",
                                      json_object_new_boolean(false));
                json_object_object_add(response, "error",
                                      json_object_new_string(
                                          user_manager_error_to_string(result)));
            }
        }
        
    } else if (strcmp(type, "search_users") == 0) {
        // 搜索用户
        json_object* query_obj = NULL;
        
        if (json_object_object_get_ex(json, "query", &query_obj)) {
            const char* query = json_object_get_string(query_obj);
            uint64_t* user_ids = NULL;
            size_t user_count = 0;
            
            user_manager_error_t result = user_manager_search_users(
                query, &user_ids, &user_count);
            
            json_object_object_add(response, "type",
                                  json_object_new_string("search_users_response"));
            
            if (result == UM_SUCCESS) {
                json_object_object_add(response, "success",
                                      json_object_new_boolean(true));
                
                json_object* users_array = json_object_new_array();
                for (size_t i = 0; i < user_count && i < 50; i++) {
                    user_info_t user_info;
                    if (user_manager_get_user(user_ids[i], &user_info) == UM_SUCCESS) {
                        json_object* user_obj = json_object_new_object();
                        json_object_object_add(user_obj, "id",
                                              json_object_new_int64(user_info.user_id));
                        json_object_object_add(user_obj, "username",
                                              json_object_new_string(user_info.username));
                        json_object_object_add(user_obj, "nickname",
                                              json_object_new_string(user_info.nickname));
                        json_object_object_add(user_obj, "avatar_id",
                                              json_object_new_int(user_info.avatar_id));
                        json_object_object_add(user_obj, "status",
                                              json_object_new_int(user_info.status));
                        
                        json_object_array_add(users_array, user_obj);
                    }
                }
                
                json_object_object_add(response, "users", users_array);
                json_object_object_add(response, "total_count",
                                      json_object_new_int(user_count));
                
                free(user_ids);
            } else {
                json_object_object_add(response, "success",
                                      json_object_new_boolean(false));
                json_object_object_add(response, "error",
                                      json_object_new_string(
                                          user_manager_error_to_string(result)));
            }
        }
        
    } else if (strcmp(type, "send_friend_request") == 0) {
        // 发送好友请求
        json_object* to_user_id_obj = NULL;
        json_object* message_obj = NULL;
        
        if (json_object_object_get_ex(json, "to_user_id", &to_user_id_obj)) {
            uint64_t to_user_id = json_object_get_int64(to_user_id_obj);
            const char* message = NULL;
            
            if (json_object_object_get_ex(json, "message", &message_obj)) {
                message = json_object_get_string(message_obj);
            }
            
            user_manager_error_t result = user_manager_send_friend_request(
                client->user_id, to_user_id, message);
            
            json_object_object_add(response, "type",
                                  json_object_new_string("send_friend_request_response"));
            
            if (result == UM_SUCCESS) {
                json_object_object_add(response, "success",
                                      json_object_new_boolean(true));
            } else {
                json_object_object_add(response, "success",
                                      json_object_new_boolean(false));
                json_object_object_add(response, "error",
                                      json_object_new_string(
                                          user_manager_error_to_string(result)));
            }
        }
        
    } else if (strcmp(type, "get_friend_requests") == 0) {
        // 获取好友请求列表
        friend_request_t* requests = NULL;
        size_t request_count = 0;
        
        user_manager_error_t result = user_manager_get_friend_requests(
            client->user_id, &requests, &request_count);
        
        json_object_object_add(response, "type",
                              json_object_new_string("friend_requests_response"));
        
        if (result == UM_SUCCESS) {
            json_object_object_add(response, "success",
                                  json_object_new_boolean(true));
            
            json_object* requests_array = json_object_new_array();
            for (size_t i = 0; i < request_count; i++) {
                json_object* request_obj = json_object_new_object();
                json_object_object_add(request_obj, "id",
                                      json_object_new_int64(requests[i].request_id));
                json_object_object_add(request_obj, "from_user_id",
                                      json_object_new_int64(requests[i].from_user_id));
                json_object_object_add(request_obj, "message",
                                      json_object_new_string(requests[i].message));
                json_object_object_add(request_obj, "created_at",
                                      json_object_new_int64(requests[i].created_at));
                json_object_object_add(request_obj, "status",
                                      json_object_new_int(requests[i].status));
                
                // 获取发送者的信息
                user_info_t from_user;
                if (user_manager_get_user(requests[i].from_user_id, &from_user) == UM_SUCCESS) {
                    json_object_object_add(request_obj, "from_username",
                                          json_object_new_string(from_user.username));
                    json_object_object_add(request_obj, "from_nickname",
                                          json_object_new_string(from_user.nickname));
                }
                
                json_object_array_add(requests_array, request_obj);
            }
            
            json_object_object_add(response, "requests", requests_array);
            
            // 释放内存
            for (size_t i = 0; i < request_count; i++) {
                // 注意：实际结构中如果有动态分配的字段需要释放
            }
            free(requests);
        } else {
            json_object_object_add(response, "success",
                                  json_object_new_boolean(false));
            json_object_object_add(response, "error",
                                  json_object_new_string(
                                      user_manager_error_to_string(result)));
        }
        
    } else if (strcmp(type, "get_friends") == 0) {
        // 获取好友列表
        uint64_t* friend_ids = NULL;
        size_t friend_count = 0;
        
        user_manager_error_t result = user_manager_get_friends(
            client->user_id, &friend_ids, &friend_count);
        
        json_object_object_add(response, "type",
                              json_object_new_string("friends_list_response"));
        
        if (result == UM_SUCCESS) {
            json_object_object_add(response, "success",
                                  json_object_new_boolean(true));
            
            json_object* friends_array = json_object_new_array();
            for (size_t i = 0; i < friend_count; i++) {
                user_info_t friend_info;
                if (user_manager_get_user(friend_ids[i], &friend_info) == UM_SUCCESS) {
                    json_object* friend_obj = json_object_new_object();
                    json_object_object_add(friend_obj, "id",
                                          json_object_new_int64(friend_info.user_id));
                    json_object_object_add(friend_obj, "username",
                                          json_object_new_string(friend_info.username));
                    json_object_object_add(friend_obj, "nickname",
                                          json_object_new_string(friend_info.nickname));
                    json_object_object_add(friend_obj, "avatar_id",
                                          json_object_new_int(friend_info.avatar_id));
                    json_object_object_add(friend_obj, "status",
                                          json_object_new_int(friend_info.status));
                    json_object_object_add(friend_obj, "status_message",
                                          json_object_new_string(friend_info.status_message));
                    json_object_object_add(friend_obj, "last_seen",
                                          json_object_new_int64(friend_info.last_seen));
                    
                    // 获取在线状态
                    online_state_t online_state;
                    if (user_manager_get_online_status(friend_ids[i], &online_state) == UM_SUCCESS) {
                        json_object_object_add(friend_obj, "online_state",
                                              json_object_new_int(online_state));
                    }
                    
                    json_object_array_add(friends_array, friend_obj);
                }
            }
            
            json_object_object_add(response, "friends", friends_array);
            free(friend_ids);
        } else {
            json_object_object_add(response, "success",
                                  json_object_new_boolean(false));
            json_object_object_add(response, "error",
                                  json_object_new_string(
                                      user_manager_error_to_string(result)));
        }
        
    } else if (strcmp(type, "create_group") == 0) {
        // 创建群组
        json_object* name_obj = NULL;
        json_object* desc_obj = NULL;
        json_object* max_members_obj = NULL;
        
        if (json_object_object_get_ex(json, "name", &name_obj)) {
            const char* name = json_object_get_string(name_obj);
            const char* description = NULL;
            uint32_t max_members = 500;
            
            if (json_object_object_get_ex(json, "description", &desc_obj)) {
                description = json_object_get_string(desc_obj);
            }
            
            if (json_object_object_get_ex(json, "max_members", &max_members_obj)) {
                max_members = json_object_get_int(max_members_obj);
            }
            
            uint64_t group_id = 0;
            user_manager_error_t result = user_manager_create_group(
                client->user_id, name, description, max_members, &group_id);
            
            json_object_object_add(response, "type",
                                  json_object_new_string("create_group_response"));
            
            if (result == UM_SUCCESS) {
                json_object_object_add(response, "success",
                                      json_object_new_boolean(true));
                json_object_object_add(response, "group_id",
                                      json_object_new_int64(group_id));
            } else {
                json_object_object_add(response, "success",
                                      json_object_new_boolean(false));
                json_object_object_add(response, "error",
                                      json_object_new_string(
                                          user_manager_error_to_string(result)));
            }
        }
    }
    
    // 发送响应
    if (json_object_object_length(response) > 0) {
        const char* response_str = json_object_to_json_string(response);
        websocket_send_text(client, response_str, strlen(response_str));
    }
    
    json_object_put(response);
    json_object_put(json);
}

// 状态广播线程
static void* status_broadcast_thread(void* arg) {
    websocket_server_t* server = (websocket_server_t*)arg;
    
    while (1) {
        // 定期广播在线用户状态
        uint64_t* online_user_ids = NULL;
        size_t online_count = 0;
        
        if (user_manager_get_online_users(&online_user_ids, &online_count) == UM_SUCCESS) {
            // 创建广播消息
            json_object* broadcast = json_object_new_object();
            json_object_object_add(broadcast, "type",
                                  json_object_new_string("online_users_update"));
            
            json_object* users_array = json_object_new_array();
            for (size_t i = 0; i < online_count; i++) {
                json_object* user_obj = json_object_new_object();
                json_object_object_add(user_obj, "user_id",
                                      json_object_new_int64(online_user_ids[i]));
                
                // 获取在线状态
                online_state_t state;
                if (user_manager_get_online_status(online_user_ids[i], &state) == UM_SUCCESS) {
                    json_object_object_add(user_obj, "online_state",
                                          json_object_new_int(state));
                }
                
                json_object_array_add(users_array, user_obj);
            }
            
            json_object_object_add(broadcast, "online_users", users_array);
            json_object_object_add(broadcast, "count",
                                  json_object_new_int(online_count));
            
            const char* broadcast_str = json_object_to_json_string(broadcast);
            
            // 广播给所有连接的客户端
            websocket_broadcast_text(server, broadcast_str, strlen(broadcast_str));
            
            json_object_put(broadcast);
            free(online_user_ids);
        }
        
        sleep(10); // 每10秒广播一次
    }
    
    return NULL;
}

// 注册WebSocket处理器
void user_manager_register_websocket_handlers(websocket_server_t* server) {
    // 注册用户管理处理器
    websocket_register_handler("/user", user_manager_websocket_handler);
    
    // 启动状态广播线程
    pthread_t broadcast_thread;
    pthread_create(&broadcast_thread, NULL, status_broadcast_thread, server);
}