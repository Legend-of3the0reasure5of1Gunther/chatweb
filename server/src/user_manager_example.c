#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "user_manager.h"
#include "database.h"

// 回调函数示例
static void user_status_changed(uint64_t user_id,
                               online_state_t old_state,
                               online_state_t new_state,
                               void* user_data) {
    printf("User %llu changed status: %s -> %s\n",
           (unsigned long long)user_id,
           online_state_to_string(old_state),
           online_state_to_string(new_state));
}

static void friend_request_received(uint64_t from_user_id,
                                   uint64_t to_user_id,
                                   const char* message,
                                   void* user_data) {
    printf("Friend request from %llu to %llu: %s\n",
           (unsigned long long)from_user_id,
           (unsigned long long)to_user_id,
           message ? message : "");
}

static void group_event_occurred(uint64_t group_id,
                                uint64_t user_id,
                                const char* event_type,
                                void* user_data) {
    printf("Group event: %s in group %llu by user %llu\n",
           event_type,
           (unsigned long long)group_id,
           (unsigned long long)user_id);
}

// 示例1：创建和管理用户
static user_manager_error_t example_user_management(void) {
    printf("\n=== Example 1: User Management ===\n");
    
    // 创建测试用户
    user_info_t user1 = {
        .user_id = 0, // 将由数据库分配
        .status = USER_STATUS_ONLINE
    };
    
    strcpy(user1.username, "testuser1");
    strcpy(user1.nickname, "测试用户1");
    strcpy(user1.email, "test1@example.com");
    strcpy(user1.status_message, "Hello World!");
    user1.created_at = time(NULL);
    
    // 在实际应用中，密码哈希和盐值应该由认证模块生成
    const char* password_hash = "hashed_password_here";
    const char* password_salt = "random_salt_here";
    
    user_manager_error_t result = user_manager_create_user(
        &user1, password_hash, password_salt);
    
    if (result != UM_SUCCESS) {
        printf("Failed to create user: %s\n", user_manager_error_to_string(result));
        return result;
    }
    
    printf("User created successfully\n");
    
    // 获取用户信息
    user_info_t retrieved_user;
    result = user_manager_get_user(1, &retrieved_user); // 假设用户ID为1
    
    if (result == UM_SUCCESS) {
        printf("Retrieved user: %s (%s)\n",
               retrieved_user.username,
               retrieved_user.nickname);
    }
    
    // 更新用户信息
    strcpy(retrieved_user.nickname, "更新后的昵称");
    strcpy(retrieved_user.status_message, "状态已更新");
    retrieved_user.status = USER_STATUS_AWAY;
    
    result = user_manager_update_user(retrieved_user.user_id, &retrieved_user);
    
    if (result == UM_SUCCESS) {
        printf("User updated successfully\n");
    }
    
    return UM_SUCCESS;
}

// 示例2：好友关系管理
static user_manager_error_t example_friend_management(void) {
    printf("\n=== Example 2: Friend Management ===\n");
    
    // 发送好友请求（假设用户1向用户2发送请求）
    user_manager_error_t result = user_manager_send_friend_request(
        1, 2, "你好，我们可以成为朋友吗？");
    
    if (result != UM_SUCCESS) {
        printf("Failed to send friend request: %s\n",
               user_manager_error_to_string(result));
        return result;
    }
    
    printf("Friend request sent\n");
    
    // 获取好友请求列表
    friend_request_t* requests = NULL;
    size_t request_count = 0;
    
    result = user_manager_get_friend_requests(2, &requests, &request_count);
    
    if (result == UM_SUCCESS && request_count > 0) {
        printf("User 2 has %zu pending friend requests\n", request_count);
        
        // 接受第一个请求
        result = user_manager_accept_friend_request(requests[0].request_id, 2);
        
        if (result == UM_SUCCESS) {
            printf("Friend request accepted\n");
        }
        
        // 释放内存
        for (size_t i = 0; i < request_count; i++) {
            // 注意：实际结构中如果有动态分配的字段需要释放
        }
        free(requests);
    }
    
    // 获取好友列表
    uint64_t* friend_ids = NULL;
    size_t friend_count = 0;
    
    result = user_manager_get_friends(1, &friend_ids, &friend_count);
    
    if (result == UM_SUCCESS) {
        printf("User 1 has %zu friends\n", friend_count);
        
        // 获取好友的详细信息
        user_info_t* friends = NULL;
        size_t friend_info_count = 0;
        
        // 注意：这个函数需要实现
        // result = user_manager_get_friends_with_info(1, &friends, &friend_info_count);
        
        free(friend_ids);
    }
    
    return UM_SUCCESS;
}

// 示例3：群组管理
static user_manager_error_t example_group_management(void) {
    printf("\n=== Example 3: Group Management ===\n");
    
    // 创建群组
    const char* group_name = "测试群组";
    const char* description = "这是一个测试群组";
    uint64_t group_id = 0;
    
    user_manager_error_t result = user_manager_create_group(
        1, group_name, description, 100, &group_id);
    
    if (result != UM_SUCCESS) {
        printf("Failed to create group: %s\n", user_manager_error_to_string(result));
        return result;
    }
    
    printf("Group created with ID: %llu\n", (unsigned long long)group_id);
    
    // 获取群组信息
    group_info_t group_info;
    result = user_manager_get_group_info(group_id, &group_info);
    
    if (result == UM_SUCCESS) {
        printf("Group info: %s - %s (Owner: %llu)\n",
               group_info.group_name,
               group_info.description,
               (unsigned long long)group_info.owner_id);
    }
    
    // 邀请用户加入群组
    result = user_manager_invite_to_group(1, 2, group_id);
    
    if (result == UM_SUCCESS) {
        printf("User 2 invited to group\n");
    }
    
    // 获取群组成员
    uint64_t* member_ids = NULL;
    size_t member_count = 0;
    
    result = user_manager_get_group_members(group_id, &member_ids, &member_count);
    
    if (result == UM_SUCCESS) {
        printf("Group has %zu members\n", member_count);
        free(member_ids);
    }
    
    return UM_SUCCESS;
}

// 示例4：在线状态管理
static user_manager_error_t example_online_status(void) {
    printf("\n=== Example 4: Online Status Management ===\n");
    
    // 更新用户在线状态
    user_manager_error_t result = user_manager_update_online_status(
        1, ONLINE_STATE_ONLINE);
    
    if (result != UM_SUCCESS) {
        printf("Failed to update online status: %s\n",
               user_manager_error_to_string(result));
        return result;
    }
    
    printf("User 1 is now online\n");
    
    // 获取在线状态
    online_state_t state;
    result = user_manager_get_online_status(1, &state);
    
    if (result == UM_SUCCESS) {
        printf("User 1 status: %s\n", online_state_to_string(state));
    }
    
    // 模拟用户活动
    result = user_manager_ping(1);
    
    if (result == UM_SUCCESS) {
        printf("User activity pinged\n");
    }
    
    // 获取在线用户列表
    uint64_t* online_user_ids = NULL;
    size_t online_count = 0;
    
    result = user_manager_get_online_users(&online_user_ids, &online_count);
    
    if (result == UM_SUCCESS) {
        printf("There are %zu users online\n", online_count);
        
        if (online_user_ids) {
            for (size_t i = 0; i < online_count; i++) {
                printf("  User %llu\n", (unsigned long long)online_user_ids[i]);
            }
            free(online_user_ids);
        }
    }
    
    return UM_SUCCESS;
}

// 示例5：用户搜索
static user_manager_error_t example_user_search(void) {
    printf("\n=== Example 5: User Search ===\n");
    
    // 搜索用户
    uint64_t* user_ids = NULL;
    size_t user_count = 0;
    
    user_manager_error_t result = user_manager_search_users("test", &user_ids, &user_count);
    
    if (result == UM_SUCCESS) {
        printf("Found %zu users matching 'test'\n", user_count);
        
        if (user_ids) {
            for (size_t i = 0; i < user_count; i++) {
                user_info_t user;
                if (user_manager_get_user(user_ids[i], &user) == UM_SUCCESS) {
                    printf("  %s (%s)\n", user.username, user.nickname);
                }
            }
            free(user_ids);
        }
    }
    
    return UM_SUCCESS;
}

// 示例6：缓存管理
static user_manager_error_t example_cache_management(void) {
    printf("\n=== Example 6: Cache Management ===\n");
    
    // 预取用户到缓存
    uint64_t user_ids[] = {1, 2, 3};
    
    user_manager_error_t result = user_manager_prefetch_users(
        user_ids, sizeof(user_ids) / sizeof(user_ids[0]));
    
    if (result == UM_SUCCESS) {
        printf("Users prefetched to cache\n");
    }
    
    // 清除缓存
    result = user_manager_clear_cache();
    
    if (result == UM_SUCCESS) {
        printf("Cache cleared\n");
    }
    
    return UM_SUCCESS;
}

// 主测试函数
int main(int argc, char* argv[]) {
    printf("=== User Manager Module Test ===\n");
    
    // 初始化数据库连接池
    db_config_t db_config = {
        .db_path = "users.db",
        .max_connections = 5,
        .query_timeout_ms = 5000
    };
    
    if (db_init(&db_config) != DB_SUCCESS) {
        fprintf(stderr, "Failed to initialize database\n");
        return 1;
    }
    
    // 初始化用户管理模块
    user_manager_error_t result = user_manager_init("users.db");
    
    if (result != UM_SUCCESS) {
        fprintf(stderr, "Failed to initialize user manager: %s\n",
                user_manager_error_to_string(result));
        db_cleanup();
        return 1;
    }
    
    printf("User manager initialized successfully\n");
    
    // 设置回调函数
    user_manager_set_status_callback(user_status_changed, NULL);
    user_manager_set_friend_request_callback(friend_request_received, NULL);
    user_manager_set_group_event_callback(group_event_occurred, NULL);
    
    // 运行示例
    example_user_management();
    example_friend_management();
    example_group_management();
    example_online_status();
    example_user_search();
    example_cache_management();
    
    // 获取系统统计
    uint64_t total_users, online_users, active_today, new_users_today;
    
    result = user_manager_get_system_stats(&total_users, &online_users,
                                          &active_today, &new_users_today);
    
    if (result == UM_SUCCESS) {
        printf("\n=== System Statistics ===\n");
        printf("Total users: %llu\n", (unsigned long long)total_users);
        printf("Online users: %llu\n", (unsigned long long)online_users);
        printf("Active today: %llu\n", (unsigned long long)active_today);
        printf("New users today: %llu\n", (unsigned long long)new_users_today);
    }
    
    // 清理
    user_manager_cleanup();
    db_cleanup();
    
    // 删除测试数据库文件
    remove("users.db");
    
    printf("\n=== All tests completed ===\n");
    return 0;
}