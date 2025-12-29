// 文件：server/src/database_migration.c
#include "database.h"
#include "../common/logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 迁移脚本结构
typedef struct {
    int version_from;
    int version_to;
    const char *description;
    const char *sql;
} migration_script_t;

// 迁移脚本定义
static migration_script_t migrations[] = {
    {
        1, 2,
        "添加消息索引优化",
        "CREATE INDEX IF NOT EXISTS idx_messages_composite "
        "ON messages(sender_id, receiver_id, timestamp);\n"
        "CREATE INDEX IF NOT EXISTS idx_messages_status "
        "ON messages(status, timestamp);\n"
        "CREATE INDEX IF NOT EXISTS idx_group_messages_composite "
        "ON group_messages(group_id, timestamp);"
    },
    {
        2, 3,
        "添加文件访问控制",
        "ALTER TABLE files ADD COLUMN access_level INTEGER DEFAULT 0;\n"
        "ALTER TABLE files ADD COLUMN access_password TEXT;\n"
        "ALTER TABLE files ADD COLUMN access_expiry INTEGER DEFAULT 0;\n"
        "CREATE INDEX IF NOT EXISTS idx_files_access "
        "ON files(access_level, access_expiry);"
    },
    {
        3, 4,
        "添加消息加密元数据",
        "ALTER TABLE messages ADD COLUMN encryption_algorithm TEXT DEFAULT 'AES-256-GCM';\n"
        "ALTER TABLE messages ADD COLUMN encryption_key_id TEXT;\n"
        "ALTER TABLE messages ADD COLUMN signature TEXT;\n"
        "CREATE INDEX IF NOT EXISTS idx_messages_encryption "
        "ON messages(encryption_key_id, timestamp);"
    },
    {0, 0, NULL, NULL} // 结束标记
};

// 获取当前数据库版本
static int get_current_version(database_pool_t *pool) {
    const char *sql = "SELECT config_value FROM system_config WHERE config_key = 'schema_version';";
    query_result_t *result = database_execute_query(pool, sql, NULL, 0);
    
    if (!result || result->row_count == 0) {
        // 如果版本表不存在，可能是旧版本数据库
        database_free_result(result);
        return 1;
    }
    
    int version = atoi(result->rows[0][0]);
    database_free_result(result);
    
    return version;
}

// 设置数据库版本
static bool set_current_version(database_pool_t *pool, int version) {
    char version_str[32];
    snprintf(version_str, sizeof(version_str), "%d", version);
    
    const char *sql = 
        "INSERT OR REPLACE INTO system_config (config_key, config_value, updated_at) "
        "VALUES ('schema_version', ?, unix_timestamp());";
    
    const char *params[] = {version_str};
    
    return database_execute_update(pool, sql, params, 1);
}

// 执行迁移
bool database_migrate(database_pool_t *pool) {
    int current_version = get_current_version(pool);
    int target_version = 4; // 当前目标版本
    
    LOG_INFO("当前数据库版本: %d, 目标版本: %d", current_version, target_version);
    
    if (current_version >= target_version) {
        LOG_INFO("数据库已经是最新版本");
        return true;
    }
    
    // 开启事务
    const char *begin_sql = "BEGIN TRANSACTION;";
    if (!database_execute_update(pool, begin_sql, NULL, 0)) {
        LOG_ERROR("无法开始迁移事务");
        return false;
    }
    
    bool success = true;
    
    // 执行需要的迁移脚本
    for (int i = 0; migrations[i].sql != NULL; i++) {
        if (current_version < migrations[i].version_to && 
            migrations[i].version_to <= target_version) {
            
            LOG_INFO("执行迁移: %s (v%d -> v%d)", 
                    migrations[i].description,
                    migrations[i].version_from,
                    migrations[i].version_to);
            
            if (!database_execute_update(pool, migrations[i].sql, NULL, 0)) {
                LOG_ERROR("迁移脚本执行失败: %s", migrations[i].description);
                success = false;
                break;
            }
            
            // 更新版本
            if (!set_current_version(pool, migrations[i].version_to)) {
                LOG_ERROR("无法更新数据库版本");
                success = false;
                break;
            }
        }
    }
    
    // 提交或回滚事务
    const char *end_sql = success ? "COMMIT;" : "ROLLBACK;";
    database_execute_update(pool, end_sql, NULL, 0);
    
    if (success) {
        LOG_INFO("数据库迁移完成，当前版本: %d", target_version);
    } else {
        LOG_ERROR("数据库迁移失败");
    }
    
    return success;
}

// 验证数据库架构
bool database_validate_schema(database_pool_t *pool) {
    const char *tables[] = {
        "users", "sessions", "friends", "groups", "group_members",
        "messages", "group_messages", "files", "file_shares",
        "message_read_status", "user_settings", "security_logs",
        "system_config", NULL
    };
    
    LOG_INFO("验证数据库架构...");
    
    for (int i = 0; tables[i] != NULL; i++) {
        const char *sql = "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?;";
        const char *params[] = {tables[i]};
        
        query_result_t *result = database_execute_query(pool, sql, params, 1);
        if (!result || result->row_count == 0 || atoi(result->rows[0][0]) == 0) {
            LOG_ERROR("缺少表: %s", tables[i]);
            database_free_result(result);
            return false;
        }
        
        database_free_result(result);
        LOG_DEBUG("表 %s 存在", tables[i]);
    }
    
    // 检查必要的索引
    const char *indices[] = {
        "idx_users_username", "idx_users_email",
        "idx_sessions_access_token", "idx_messages_sender_id",
        "idx_messages_receiver_id", "idx_messages_timestamp",
        NULL
    };
    
    for (int i = 0; indices[i] != NULL; i++) {
        const char *sql = "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name=?;";
        const char *params[] = {indices[i]};
        
        query_result_t *result = database_execute_query(pool, sql, params, 1);
        if (!result || result->row_count == 0 || atoi(result->rows[0][0]) == 0) {
            LOG_WARN("缺少索引: %s", indices[i]);
        }
        
        database_free_result(result);
    }
    
    LOG_INFO("数据库架构验证通过");
    return true;
}