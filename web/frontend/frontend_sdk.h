// 文件：frontend_sdk.h
#ifndef FRONTEND_SDK_H
#define FRONTEND_SDK_H

#include "protocol.h"
#include "client.h"
#include <stdbool.h>
#include <stdint.h>

// UI事件类型
typedef enum {
    UI_EVENT_CONNECTION_STATE_CHANGED,
    UI_EVENT_MESSAGE_RECEIVED,
    UI_EVENT_FILE_PROGRESS,
    UI_EVENT_USER_STATUS_CHANGED,
    UI_EVENT_GROUP_UPDATE,
    UI_EVENT_SYSTEM_NOTIFICATION,
    UI_EVENT_ERROR
} ui_event_type_t;

// UI事件结构
typedef struct {
    ui_event_type_t type;
    uint64_t timestamp;
    union {
        struct {
            client_state_t old_state;
            client_state_t new_state;
        } connection;
        struct {
            uint64_t sender_id;
            char sender_name[64];
            uint32_t message_type;
            char content[4096];
            uint8_t encrypted;
        } message;
        struct {
            uint64_t transfer_id;
            uint32_t progress;
            uint64_t bytes_transferred;
            uint64_t total_bytes;
            char filename[256];
        } file;
        struct {
            uint64_t user_id;
            user_status_t old_status;
            user_status_t new_status;
            char status_message[128];
        } user_status;
        struct {
            uint64_t group_id;
            uint64_t user_id;
            char event_type[32];
            char details[256];
        } group;
        struct {
            error_code_t error_code;
            char error_message[256];
            char debug_info[512];
        } error;
    } data;
} ui_event_t;

// UI回调函数类型
typedef void (*ui_event_callback_t)(const ui_event_t *event, void *user_data);

// 前端SDK主结构
typedef struct {
    secure_chat_client_t *client;
    ui_event_callback_t event_callback;
    void *callback_user_data;
    
    // 消息路由
    struct {
        void (*route_message)(uint32_t msg_type, const void *data, size_t len);
        void (*broadcast_event)(const ui_event_t *event);
    } router;
    
    // 缓存
    struct {
        user_info_t *user_cache;
        size_t user_cache_size;
        pthread_mutex_t cache_mutex;
    } cache;
    
    // 状态
    struct {
        bool is_initialized;
        bool is_connected;
        bool is_authenticated;
        char current_user[64];
        uint64_t current_user_id;
    } state;
    
    // 统计
    struct {
        uint64_t ui_events_processed;
        uint64_t messages_displayed;
        uint64_t errors_handled;
    } stats;
} frontend_sdk_t;

// ==================== 核心API ====================
frontend_sdk_t* frontend_sdk_create(void);
void frontend_sdk_destroy(frontend_sdk_t *sdk);

// 初始化
int frontend_sdk_init(frontend_sdk_t *sdk, const client_config_t *config);
int frontend_sdk_connect(frontend_sdk_t *sdk);
int frontend_sdk_disconnect(frontend_sdk_t *sdk);

// 认证
int frontend_sdk_login(frontend_sdk_t *sdk, const char *username, 
                       const char *password, char *error_msg, size_t error_len);
int frontend_sdk_logout(frontend_sdk_t *sdk);
int frontend_sdk_register(frontend_sdk_t *sdk, const user_info_t *user_info,
                         const char *password, char *error_msg, size_t error_len);

// 消息发送
int frontend_sdk_send_text(frontend_sdk_t *sdk, uint64_t receiver_id, 
                          const char *text, uint64_t *message_id);
int frontend_sdk_send_file(frontend_sdk_t *sdk, uint64_t receiver_id,
                          const char *filepath, uint64_t *transfer_id);
int frontend_sdk_send_group_message(frontend_sdk_t *sdk, uint64_t group_id,
                                   const char *text, uint64_t *message_id);

// 消息接收
int frontend_sdk_fetch_messages(frontend_sdk_t *sdk, uint64_t conversation_id,
                               time_t since, message_content_t **messages,
                               size_t *count);
int frontend_sdk_mark_message_read(frontend_sdk_t *sdk, uint64_t message_id);

// 用户管理
int frontend_sdk_get_user_info(frontend_sdk_t *sdk, uint64_t user_id,
                              user_info_t *user_info);
int frontend_sdk_update_user_info(frontend_sdk_t *sdk, const user_info_t *updates);
int frontend_sdk_search_users(frontend_sdk_t *sdk, const char *query,
                             user_info_t **results, size_t *count);

// 群组管理
int frontend_sdk_create_group(frontend_sdk_t *sdk, const char *group_name,
                             const char *description, uint64_t *group_id);
int frontend_sdk_join_group(frontend_sdk_t *sdk, uint64_t group_id,
                           const char *invitation_code);
int frontend_sdk_get_group_info(frontend_sdk_t *sdk, uint64_t group_id,
                               group_info_t *group_info);

// 文件传输
int frontend_sdk_get_file_transfers(frontend_sdk_t *sdk,
                                   file_transfer_status_t **transfers,
                                   size_t *count);
int frontend_sdk_cancel_transfer(frontend_sdk_t *sdk, uint64_t transfer_id);

// UI集成
int frontend_sdk_set_event_callback(frontend_sdk_t *sdk,
                                   ui_event_callback_t callback,
                                   void *user_data);
int frontend_sdk_process_ui_event(frontend_sdk_t *sdk, const ui_event_t *event);
int frontend_sdk_get_state(frontend_sdk_t *sdk, frontend_state_t *state);

// 缓存管理
int frontend_sdk_prefetch_user(frontend_sdk_t *sdk, uint64_t user_id);
int frontend_sdk_clear_cache(frontend_sdk_t *sdk);

// 工具函数
const char* frontend_sdk_get_version(void);
int frontend_sdk_get_statistics(frontend_sdk_t *sdk, frontend_stats_t *stats);
bool frontend_sdk_validate_input(const char *input, uint32_t input_type);

#endif // FRONTEND_SDK_H