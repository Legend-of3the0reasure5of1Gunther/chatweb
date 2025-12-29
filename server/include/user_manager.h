#ifndef USER_MANAGER_H
#define USER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <pthread.h>
#include <sqlite3.h>
#include "protocol.h"
#include "database.h"
#include "auth.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 用户管理配置 ====================
#define UM_MAX_FRIENDS 5000
#define UM_MAX_GROUPS 200
#define UM_MAX_BLOCKED_USERS 1000
#define UM_MAX_FRIEND_REQUESTS 100
#define UM_MAX_SEARCH_RESULTS 100
#define UM_STATUS_UPDATE_INTERVAL 30  // 秒
#define UM_PROFILE_CACHE_SIZE 1000
#define UM_ONLINE_TIMEOUT 300         // 5分钟无活动视为离线

// ==================== 数据结构定义 ====================

// 用户关系类型
typedef enum {
    RELATIONSHIP_NONE = 0,       // 无关系
    RELATIONSHIP_FRIEND = 1,     // 好友
    RELATIONSHIP_CLOSE_FRIEND = 2, // 特别关心
    RELATIONSHIP_BLOCKED = 3,    // 已拉黑
    RELATIONSHIP_PENDING = 4,    // 待确认
    RELATIONSHIP_DECLINED = 5    // 已拒绝
} relationship_type_t;

// 群组成员角色
typedef enum {
    GROUP_ROLE_MEMBER = 0,       // 普通成员
    GROUP_ROLE_ADMIN = 1,        // 管理员
    GROUP_ROLE_OWNER = 2         // 群主
} group_role_t;

// 用户在线状态
typedef enum {
    ONLINE_STATE_OFFLINE = 0,    // 离线
    ONLINE_STATE_ONLINE = 1,     // 在线
    ONLINE_STATE_AWAY = 2,       // 离开
    ONLINE_STATE_BUSY = 3,       // 忙碌
    ONLINE_STATE_INVISIBLE = 4   // 隐身
} online_state_t;

// 用户扩展信息
typedef struct {
    uint64_t user_id;
    char phone_number[32];
    char birthday[16];
    char location[128];
    char biography[512];
    char website[256];
    char gender[16];
    uint32_t age;
    char interests[512];
    char education[256];
    char occupation[128];
    time_t last_profile_update;
} user_extended_info_t;

// 好友关系信息
typedef struct {
    uint64_t relationship_id;
    uint64_t user_id;
    uint64_t friend_id;
    relationship_type_t relationship_type;
    char alias[MAX_NICKNAME_LEN];  // 好友备注
    char category[64];              // 好友分组
    uint32_t intimacy_level;        // 亲密度等级 0-100
    time_t created_at;
    time_t last_interaction;
    uint64_t message_count;
    uint8_t is_favorite;            // 是否星标好友
    uint8_t mute_notifications;     // 是否屏蔽通知
} friendship_t;

// 好友请求信息
typedef struct {
    uint64_t request_id;
    uint64_t from_user_id;
    uint64_t to_user_id;
    char message[256];              // 请求消息
    time_t created_at;
    uint8_t status;                 // 0=等待中, 1=已接受, 2=已拒绝, 3=已过期
    char source[64];                // 来源（如：通过搜索、通过群聊等）
} friend_request_t;

// 群组信息
typedef struct {
    uint64_t group_id;
    char group_name[MAX_GROUP_NAME_LEN];
    char description[512];
    uint64_t owner_id;
    char avatar_hash[65];           // 群头像哈希
    uint32_t member_count;
    uint32_t max_members;
    uint8_t is_public;              // 是否公开群
    uint8_t requires_approval;      // 加群是否需要审批
    uint8_t allow_member_invite;    // 是否允许成员邀请
    time_t created_at;
    time_t updated_at;
    time_t last_activity;
    uint64_t total_messages;
} group_info_t;

// 群组成员信息
typedef struct {
    uint64_t membership_id;
    uint64_t group_id;
    uint64_t user_id;
    group_role_t role;
    char nickname_in_group[MAX_NICKNAME_LEN];  // 群内昵称
    time_t joined_at;
    time_t last_active_in_group;
    uint64_t messages_in_group;
    uint8_t is_muted;               // 是否被禁言
    time_t mute_until;              // 禁言直到
    uint8_t is_banned;              // 是否被踢出
} group_membership_t;

// 黑名单条目
typedef struct {
    uint64_t block_id;
    uint64_t blocker_id;
    uint64_t blocked_id;
    char reason[256];
    time_t blocked_at;
    time_t expires_at;              // 0表示永久拉黑
    uint8_t block_messages;         // 是否屏蔽消息
    uint8_t block_profile_view;     // 是否屏蔽查看资料
} block_entry_t;

// 用户活动记录
typedef struct {
    uint64_t activity_id;
    uint64_t user_id;
    char activity_type[64];         // 如：login, logout, update_profile, add_friend等
    char details[512];
    char ip_address[46];
    char user_agent[256];
    time_t timestamp;
} user_activity_t;

// 用户统计信息
typedef struct {
    uint64_t user_id;
    uint32_t friend_count;
    uint32_t group_count;
    uint64_t total_messages_sent;
    uint64_t total_messages_received;
    uint64_t total_files_sent;
    uint64_t total_files_received;
    uint64_t total_online_time;     // 秒
    time_t account_age;             // 账号创建天数
    time_t last_seen;
    time_t last_message_time;
} user_statistics_t;

// 用户缓存条目
typedef struct user_cache_entry {
    uint64_t user_id;
    user_info_t basic_info;
    user_extended_info_t extended_info;
    user_statistics_t statistics;
    online_state_t online_state;
    time_t last_online_check;
    time_t cache_expiry;
    struct user_cache_entry* next;
    struct user_cache_entry* prev;
} user_cache_entry_t;

// 用户管理器上下文
typedef struct {
    sqlite3* db;
    pthread_mutex_t db_mutex;
    
    // 缓存管理
    user_cache_entry_t* user_cache;
    pthread_mutex_t cache_mutex;
    size_t cache_size;
    size_t max_cache_size;
    
    // 在线用户管理
    struct {
        uint64_t* user_ids;
        online_state_t* states;
        time_t* last_activity;
        size_t count;
        size_t capacity;
        pthread_mutex_t mutex;
    } online_users;
    
    // 统计信息
    struct {
        uint64_t total_users;
        uint64_t online_users;
        uint64_t active_today;
        uint64_t new_users_today;
        time_t last_stat_update;
    } stats;
    
    // 回调函数
    void (*user_status_callback)(uint64_t user_id, online_state_t old_state,
                                online_state_t new_state, void* user_data);
    void (*friend_request_callback)(uint64_t from_user_id, uint64_t to_user_id,
                                   const char* message, void* user_data);
    void (*group_event_callback)(uint64_t group_id, uint64_t user_id,
                                const char* event_type, void* user_data);
    void* callback_user_data;
    
    // 管理器线程
    pthread_t manager_thread;
    bool running;
    
    // 数据库连接池
    db_pool_t* db_pool;
} user_manager_t;

// ==================== 错误码定义 ====================
typedef enum {
    UM_SUCCESS = 0,
    UM_ERROR_DATABASE = 1,
    UM_ERROR_USER_NOT_FOUND = 2,
    UM_ERROR_USER_ALREADY_EXISTS = 3,
    UM_ERROR_FRIEND_LIMIT_EXCEEDED = 4,
    UM_ERROR_GROUP_LIMIT_EXCEEDED = 5,
    UM_ERROR_BLOCK_LIMIT_EXCEEDED = 6,
    UM_ERROR_INVALID_RELATIONSHIP = 7,
    UM_ERROR_PERMISSION_DENIED = 8,
    UM_ERROR_GROUP_NOT_FOUND = 9,
    UM_ERROR_ALREADY_FRIENDS = 10,
    UM_ERROR_ALREADY_BLOCKED = 11,
    UM_ERROR_ALREADY_IN_GROUP = 12,
    UM_ERROR_GROUP_FULL = 13,
    UM_ERROR_INVALID_INVITATION = 14,
    UM_ERROR_REQUEST_NOT_FOUND = 15,
    UM_ERROR_CACHE_FULL = 16,
    UM_ERROR_INVALID_PARAMETERS = 17,
    UM_ERROR_CONCURRENT_MODIFICATION = 18,
    UM_ERROR_RATE_LIMIT_EXCEEDED = 19,
    UM_ERROR_OPERATION_TIMEOUT = 20,
    UM_ERROR_INTERNAL = 21
} user_manager_error_t;

// ==================== 函数声明 ====================

// 初始化/清理
user_manager_error_t user_manager_init(const char* db_path);
user_manager_error_t user_manager_init_with_pool(db_pool_t* db_pool);
void user_manager_cleanup(void);

// 用户基本信息管理
user_manager_error_t user_manager_create_user(const user_info_t* user_info,
                                             const char* password_hash,
                                             const char* password_salt);
user_manager_error_t user_manager_get_user(uint64_t user_id, user_info_t* user_info);
user_manager_error_t user_manager_update_user(uint64_t user_id,
                                             const user_info_t* updates);
user_manager_error_t user_manager_delete_user(uint64_t user_id);
user_manager_error_t user_manager_deactivate_user(uint64_t user_id);
user_manager_error_t user_manager_reactivate_user(uint64_t user_id);

// 用户扩展信息管理
user_manager_error_t user_manager_get_extended_info(uint64_t user_id,
                                                   user_extended_info_t* info);
user_manager_error_t user_manager_update_extended_info(uint64_t user_id,
                                                      const user_extended_info_t* info);
user_manager_error_t user_manager_get_user_statistics(uint64_t user_id,
                                                     user_statistics_t* stats);

// 好友关系管理
user_manager_error_t user_manager_send_friend_request(uint64_t from_user_id,
                                                     uint64_t to_user_id,
                                                     const char* message);
user_manager_error_t user_manager_accept_friend_request(uint64_t request_id,
                                                       uint64_t acceptor_id);
user_manager_error_t user_manager_decline_friend_request(uint64_t request_id,
                                                        uint64_t decliner_id);
user_manager_error_t user_manager_cancel_friend_request(uint64_t request_id,
                                                       uint64_t canceller_id);
user_manager_error_t user_manager_remove_friend(uint64_t user_id,
                                               uint64_t friend_id);
user_manager_error_t user_manager_block_user(uint64_t blocker_id,
                                            uint64_t blocked_id,
                                            const char* reason,
                                            time_t expires_at);
user_manager_error_t user_manager_unblock_user(uint64_t blocker_id,
                                              uint64_t blocked_id);
user_manager_error_t user_manager_update_friend_alias(uint64_t user_id,
                                                     uint64_t friend_id,
                                                     const char* alias);
user_manager_error_t user_manager_set_friend_category(uint64_t user_id,
                                                     uint64_t friend_id,
                                                     const char* category);
user_manager_error_t user_manager_toggle_friend_favorite(uint64_t user_id,
                                                        uint64_t friend_id,
                                                        bool favorite);

// 好友查询
user_manager_error_t user_manager_get_friends(uint64_t user_id,
                                             uint64_t** friend_ids,
                                             size_t* count);
user_manager_error_t user_manager_get_friends_with_info(uint64_t user_id,
                                                       user_info_t** friends,
                                                       size_t* count);
user_manager_error_t user_manager_get_friend_requests(uint64_t user_id,
                                                     friend_request_t** requests,
                                                     size_t* count);
user_manager_error_t user_manager_get_blocked_users(uint64_t user_id,
                                                   uint64_t** blocked_ids,
                                                   size_t* count);
user_manager_error_t user_manager_check_relationship(uint64_t user_id,
                                                    uint64_t other_user_id,
                                                    relationship_type_t* relationship);

// 群组管理
user_manager_error_t user_manager_create_group(uint64_t creator_id,
                                              const char* group_name,
                                              const char* description,
                                              uint32_t max_members,
                                              uint64_t* group_id);
user_manager_error_t user_manager_update_group_info(uint64_t group_id,
                                                   const group_info_t* updates);
user_manager_error_t user_manager_delete_group(uint64_t group_id,
                                              uint64_t requester_id);
user_manager_error_t user_manager_transfer_group_ownership(uint64_t group_id,
                                                          uint64_t current_owner_id,
                                                          uint64_t new_owner_id);

// 群组成员管理
user_manager_error_t user_manager_join_group(uint64_t user_id,
                                            uint64_t group_id,
                                            const char* invitation_code);
user_manager_error_t user_manager_leave_group(uint64_t user_id,
                                             uint64_t group_id);
user_manager_error_t user_manager_invite_to_group(uint64_t inviter_id,
                                                 uint64_t invitee_id,
                                                 uint64_t group_id);
user_manager_error_t user_manager_kick_from_group(uint64_t group_id,
                                                 uint64_t target_user_id,
                                                 uint64_t requester_id,
                                                 const char* reason);
user_manager_error_t user_manager_set_group_role(uint64_t group_id,
                                                uint64_t user_id,
                                                group_role_t role,
                                                uint64_t requester_id);
user_manager_error_t user_manager_mute_group_member(uint64_t group_id,
                                                   uint64_t user_id,
                                                   time_t duration,
                                                   uint64_t requester_id);
user_manager_error_t user_manager_unmute_group_member(uint64_t group_id,
                                                     uint64_t user_id,
                                                     uint64_t requester_id);

// 群组查询
user_manager_error_t user_manager_get_group_info(uint64_t group_id,
                                                group_info_t* group_info);
user_manager_error_t user_manager_get_user_groups(uint64_t user_id,
                                                 uint64_t** group_ids,
                                                 size_t* count);
user_manager_error_t user_manager_get_group_members(uint64_t group_id,
                                                   uint64_t** member_ids,
                                                   size_t* count);
user_manager_error_t user_manager_get_group_members_with_info(uint64_t group_id,
                                                             group_membership_t** members,
                                                             size_t* count);
user_manager_error_t user_manager_search_groups(const char* query,
                                               uint64_t** group_ids,
                                               size_t* count);

// 在线状态管理
user_manager_error_t user_manager_update_online_status(uint64_t user_id,
                                                      online_state_t state);
user_manager_error_t user_manager_get_online_status(uint64_t user_id,
                                                   online_state_t* state);
user_manager_error_t user_manager_get_online_users(uint64_t** user_ids,
                                                  size_t* count);
user_manager_error_t user_manager_set_invisible(uint64_t user_id, bool invisible);
user_manager_error_t user_manager_ping(uint64_t user_id);  // 更新最后活动时间

// 用户搜索
user_manager_error_t user_manager_search_users(const char* query,
                                              uint64_t** user_ids,
                                              size_t* count);
user_manager_error_t user_manager_search_users_by_field(const char* field,
                                                       const char* value,
                                                       uint64_t** user_ids,
                                                       size_t* count);

// 缓存管理
user_manager_error_t user_manager_cache_user(uint64_t user_id);
user_manager_error_t user_manager_remove_from_cache(uint64_t user_id);
user_manager_error_t user_manager_clear_cache(void);
user_manager_error_t user_manager_prefetch_users(const uint64_t* user_ids,
                                                size_t count);

// 活动记录
user_manager_error_t user_manager_log_activity(uint64_t user_id,
                                              const char* activity_type,
                                              const char* details,
                                              const char* ip_address,
                                              const char* user_agent);
user_manager_error_t user_manager_get_recent_activities(uint64_t user_id,
                                                       time_t since,
                                                       user_activity_t** activities,
                                                       size_t* count);

// 统计和监控
user_manager_error_t user_manager_get_system_stats(uint64_t* total_users,
                                                  uint64_t* online_users,
                                                  uint64_t* active_today,
                                                  uint64_t* new_users_today);
user_manager_error_t user_manager_get_user_count_by_status(user_status_t status,
                                                          uint64_t* count);
user_manager_error_t user_manager_get_daily_active_users(time_t date,
                                                        uint64_t** user_ids,
                                                        size_t* count);

// 回调函数设置
user_manager_error_t user_manager_set_status_callback(
    void (*callback)(uint64_t, online_state_t, online_state_t, void*),
    void* user_data);
user_manager_error_t user_manager_set_friend_request_callback(
    void (*callback)(uint64_t, uint64_t, const char*, void*),
    void* user_data);
user_manager_error_t user_manager_set_group_event_callback(
    void (*callback)(uint64_t, uint64_t, const char*, void*),
    void* user_data);

// 工具函数
const char* user_manager_error_to_string(user_manager_error_t error);
const char* relationship_type_to_string(relationship_type_t type);
const char* online_state_to_string(online_state_t state);
const char* group_role_to_string(group_role_t role);

// 验证函数
bool user_manager_validate_username(const char* username);
bool user_manager_validate_nickname(const char* nickname);
bool user_manager_validate_email(const char* email);
bool user_manager_validate_group_name(const char* group_name);

// 批量操作
user_manager_error_t user_manager_batch_update_users(const uint64_t* user_ids,
                                                    size_t count,
                                                    const user_info_t* updates);
user_manager_error_t user_manager_batch_add_friends(uint64_t user_id,
                                                   const uint64_t* friend_ids,
                                                   size_t count);
user_manager_error_t user_manager_batch_remove_friends(uint64_t user_id,
                                                      const uint64_t* friend_ids,
                                                      size_t count);

// 导入/导出
user_manager_error_t user_manager_export_user_data(uint64_t user_id,
                                                  const char* export_path);
user_manager_error_t user_manager_import_user_data(const char* import_path,
                                                  uint64_t* imported_user_id);

// 协议消息处理
user_manager_error_t user_manager_handle_friend_request_message(
    const message_header_t* header,
    const void* data,
    size_t data_len,
    response_t* response);
user_manager_error_t user_manager_handle_group_invite_message(
    const message_header_t* header,
    const void* data,
    size_t data_len,
    response_t* response);
user_manager_error_t user_manager_handle_status_update_message(
    const message_header_t* header,
    const void* data,
    size_t data_len);

#ifdef __cplusplus
}
#endif

#endif // USER_MANAGER_H