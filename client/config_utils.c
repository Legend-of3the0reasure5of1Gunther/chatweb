#include "client.h"
#include "../common/logger.h"
#include "../common/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// 查找配置文件
char* client_config_find_default_location(void) {
    const char *locations[] = {
        "~/.securechat/config.json",
        "~/.config/securechat/config.json",
        "/etc/securechat/client-config.json",
        "./config/client-config.json",
        "./securechat-config.json",
        NULL
    };
    
    for (int i = 0; locations[i]; i++) {
        char *expanded = expand_path(locations[i]);
        if (!expanded) continue;
        
        struct stat st;
        if (stat(expanded, &st) == 0 && S_ISREG(st.st_mode)) {
            return expanded;
        }
        
        free(expanded);
    }
    
    // 没有找到，返回默认位置
    return expand_path("~/.securechat/config.json");
}

// 列出可用的profiles
char** client_config_list_profiles(size_t *count) {
    if (!config_ctx.root || !count) {
        return NULL;
    }
    
    json_t *profiles = json_object_get(config_ctx.root, "profiles");
    if (!profiles || !json_is_array(profiles)) {
        *count = 0;
        return NULL;
    }
    
    size_t profile_count = json_array_size(profiles);
    if (profile_count == 0) {
        *count = 0;
        return NULL;
    }
    
    char **profile_names = malloc(profile_count * sizeof(char*));
    if (!profile_names) {
        *count = 0;
        return NULL;
    }
    
    size_t index;
    json_t *profile;
    size_t valid_count = 0;
    
    json_array_foreach(profiles, index, profile) {
        json_t *name = json_object_get(profile, "name");
        if (json_is_string(name)) {
            profile_names[valid_count] = strdup(json_string_value(name));
            if (profile_names[valid_count]) {
                valid_count++;
            }
        }
    }
    
    if (valid_count == 0) {
        free(profile_names);
        *count = 0;
        return NULL;
    }
    
    *count = valid_count;
    return profile_names;
}

// 获取当前活跃的profile
const char* client_config_get_current_profile(void) {
    if (!config_ctx.root) {
        return NULL;
    }
    
    json_t *metadata = json_object_get(config_ctx.root, "metadata");
    if (!metadata) {
        return NULL;
    }
    
    json_t *profile = json_object_get(metadata, "last_used_profile");
    if (!json_is_string(profile)) {
        return NULL;
    }
    
    return json_string_value(profile);
}

// 导入配置
int client_config_import(const char *source_file, const char *target_file, bool overwrite) {
    if (!source_file) {
        LOG_ERROR("源文件未指定");
        return -1;
    }
    
    char *expanded_source = expand_path(source_file);
    if (!expanded_source) {
        LOG_ERROR("源文件路径扩展失败");
        return -1;
    }
    
    struct stat st;
    if (stat(expanded_source, &st) != 0) {
        LOG_ERROR("源文件不存在: %s", expanded_source);
        free(expanded_source);
        return -1;
    }
    
    const char *target = target_file;
    char *default_target = NULL;
    if (!target) {
        default_target = client_config_find_default_location();
        target = default_target;
    }
    
    if (!target) {
        LOG_ERROR("无法确定目标文件");
        free(expanded_source);
        return -1;
    }
    
    char *expanded_target = expand_path(target);
    if (!expanded_target) {
        LOG_ERROR("目标文件路径扩展失败");
        free(expanded_source);
        if (default_target) free(default_target);
        return -1;
    }
    
    // 检查目标文件是否存在
    if (stat(expanded_target, &st) == 0) {
        if (!overwrite) {
            LOG_ERROR("目标文件已存在，使用--overwrite参数覆盖");
            free(expanded_source);
            free(expanded_target);
            if (default_target) free(default_target);
            return -1;
        }
        
        // 备份原文件
        char backup[1024];
        snprintf(backup, sizeof(backup), "%s.backup", expanded_target);
        
        if (rename(expanded_target, backup) != 0) {
            LOG_WARNING("无法备份原文件: %s", strerror(errno));
        } else {
            LOG_INFO("已备份原文件到: %s", backup);
        }
    }
    
    // 复制文件
    FILE *src = fopen(expanded_source, "rb");
    if (!src) {
        LOG_ERROR("无法打开源文件: %s", strerror(errno));
        free(expanded_source);
        free(expanded_target);
        if (default_target) free(default_target);
        return -1;
    }
    
    FILE *dst = fopen(expanded_target, "wb");
    if (!dst) {
        LOG_ERROR("无法创建目标文件: %s", strerror(errno));
        fclose(src);
        free(expanded_source);
        free(expanded_target);
        if (default_target) free(default_target);
        return -1;
    }
    
    char buffer[8192];
    size_t bytes;
    size_t total = 0;
    
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, bytes, dst) != bytes) {
            LOG_ERROR("写入目标文件失败");
            fclose(src);
            fclose(dst);
            free(expanded_source);
            free(expanded_target);
            if (default_target) free(default_target);
            return -1;
        }
        total += bytes;
    }
    
    fclose(src);
    fclose(dst);
    
    LOG_INFO("配置文件导入成功: %s -> %s (%zu 字节)", 
             expanded_source, expanded_target, total);
    
    free(expanded_source);
    free(expanded_target);
    if (default_target) free(default_target);
    
    return 0;
}

// 导出配置
int client_config_export(const char *source_file, const char *target_file) {
    const char *source = source_file;
    char *default_source = NULL;
    
    if (!source) {
        if (config_ctx.filename) {
            source = config_ctx.filename;
        } else {
            default_source = client_config_find_default_location();
            source = default_source;
        }
    }
    
    if (!source) {
        LOG_ERROR("无法确定源文件");
        return -1;
    }
    
    if (!target_file) {
        LOG_ERROR("目标文件未指定");
        if (default_source) free(default_source);
        return -1;
    }
    
    char *expanded_source = expand_path(source);
    if (!expanded_source) {
        LOG_ERROR("源文件路径扩展失败");
        if (default_source) free(default_source);
        return -1;
    }
    
    char *expanded_target = expand_path(target_file);
    if (!expanded_target) {
        LOG_ERROR("目标文件路径扩展失败");
        free(expanded_source);
        if (default_source) free(default_source);
        return -1;
    }
    
    // 检查源文件是否存在
    struct stat st;
    if (stat(expanded_source, &st) != 0) {
        LOG_ERROR("源文件不存在: %s", expanded_source);
        free(expanded_source);
        free(expanded_target);
        if (default_source) free(default_source);
        return -1;
    }
    
    // 复制文件
    FILE *src = fopen(expanded_source, "rb");
    if (!src) {
        LOG_ERROR("无法打开源文件: %s", strerror(errno));
        free(expanded_source);
        free(expanded_target);
        if (default_source) free(default_source);
        return -1;
    }
    
    FILE *dst = fopen(expanded_target, "wb");
    if (!dst) {
        LOG_ERROR("无法创建目标文件: %s", strerror(errno));
        fclose(src);
        free(expanded_source);
        free(expanded_target);
        if (default_source) free(default_source);
        return -1;
    }
    
    char buffer[8192];
    size_t bytes;
    size_t total = 0;
    
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, bytes, dst) != bytes) {
            LOG_ERROR("写入目标文件失败");
            fclose(src);
            fclose(dst);
            free(expanded_source);
            free(expanded_target);
            if (default_source) free(default_source);
            return -1;
        }
        total += bytes;
    }
    
    fclose(src);
    fclose(dst);
    
    LOG_INFO("配置文件导出成功: %s -> %s (%zu 字节)", 
             expanded_source, expanded_target, total);
    
    free(expanded_source);
    free(expanded_target);
    if (default_source) free(default_source);
    
    return 0;
}

// 验证配置文件的完整性
int client_config_validate_file(const char *filename) {
    if (!filename) {
        LOG_ERROR("文件名未指定");
        return -1;
    }
    
    char *expanded = expand_path(filename);
    if (!expanded) {
        LOG_ERROR("路径扩展失败");
        return -1;
    }
    
    json_error_t error;
    json_t *root = json_load_file(expanded, 0, &error);
    if (!root) {
        LOG_ERROR("JSON解析失败 (行 %d, 列 %d): %s", 
                 error.line, error.column, error.text);
        free(expanded);
        return -1;
    }
    
    // 基本验证
    if (!validate_config_structure(root)) {
        LOG_ERROR("配置文件结构验证失败");
        json_decref(root);
        free(expanded);
        return -1;
    }
    
    // 验证必需字段
    json_t *client = json_object_get(root, "client");
    json_t *server = json_object_get(client, "server");
    
    const char *host = json_string_value(json_object_get(server, "host"));
    int port = json_integer_value(json_object_get(server, "tcp_port"));
    
    if (!host || strlen(host) == 0) {
        LOG_ERROR("服务器地址无效");
        json_decref(root);
        free(expanded);
        return -1;
    }
    
    if (port <= 0 || port > 65535) {
        LOG_ERROR("端口号无效: %d", port);
        json_decref(root);
        free(expanded);
        return -1;
    }
    
    // 验证profiles（如果有）
    json_t *profiles = json_object_get(root, "profiles");
    if (profiles && json_is_array(profiles)) {
        size_t index;
        json_t *profile;
        json_array_foreach(profiles, index, profile) {
            json_t *name = json_object_get(profile, "name");
            if (!json_is_string(name) || strlen(json_string_value(name)) == 0) {
                LOG_ERROR("profile %zu 缺少名称", index);
                json_decref(root);
                free(expanded);
                return -1;
            }
        }
    }
    
    json_decref(root);
    
    LOG_INFO("配置文件验证成功: %s", expanded);
    free(expanded);
    
    return 0;
}

// 配置迁移（旧版本到新版本）
int client_config_migrate(const char *old_file, const char *new_file) {
    LOG_INFO("开始配置迁移: %s -> %s", old_file, new_file);
    
    // 读取旧配置文件
    char *expanded_old = expand_path(old_file);
    if (!expanded_old) {
        LOG_ERROR("旧文件路径扩展失败");
        return -1;
    }
    
    json_error_t error;
    json_t *old_root = json_load_file(expanded_old, 0, &error);
    if (!old_root) {
        LOG_ERROR("无法读取旧配置文件: %s", error.text);
        free(expanded_old);
        return -1;
    }
    
    // 检查旧版本
    json_t *old_version = json_object_get(old_root, "version");
    if (!json_is_string(old_version)) {
        LOG_ERROR("旧配置文件缺少版本信息");
        json_decref(old_root);
        free(expanded_old);
        return -1;
    }
    
    const char *version_str = json_string_value(old_version);
    LOG_INFO("旧配置文件版本: %s", version_str);
    
    // 创建新配置文件
    json_t *new_root = json_object();
    json_object_set_new(new_root, "version", json_string("3.0.0"));
    json_object_set_new(new_root, "description", 
                       json_string("迁移自 SecureChat v2.x"));
    
    // 迁移client部分
    json_t *old_client = json_object_get(old_root, "client");
    if (old_client) {
        json_t *new_client = json_object();
        
        // 迁移server配置
        json_t *old_server = json_object_get(old_client, "server");
        if (old_server) {
            json_t *new_server = json_object();
            
            const char *host = json_string_value(json_object_get(old_server, "host"));
            if (host) json_object_set_new(new_server, "host", json_string(host));
            
            json_t *port = json_object_get(old_server, "port");
            if (port) {
                json_object_set_new(new_server, "tcp_port", json_integer(json_integer_value(port)));
                json_object_set_new(new_server, "websocket_port", json_integer(json_integer_value(port) + 1));
            }
            
            json_t *ssl = json_object_get(old_server, "ssl");
            if (ssl) json_object_set_new(new_server, "enable_ssl", json_boolean(json_boolean_value(ssl)));
            
            json_object_set_new(new_client, "server", new_server);
        }
        
        // 迁移authentication配置
        json_t *old_auth = json_object_get(old_client, "auth");
        if (old_auth) {
            json_t *new_auth = json_object();
            
            const char *username = json_string_value(json_object_get(old_auth, "username"));
            if (username) json_object_set_new(new_auth, "username", json_string(username));
            
            const char *password = json_string_value(json_object_get(old_auth, "password"));
            if (password) json_object_set_new(new_auth, "password", json_string(password));
            
            json_object_set_new(new_client, "authentication", new_auth);
        }
        
        json_object_set_new(new_root, "client", new_client);
    }
    
    // 添加metadata
    json_t *metadata = json_object();
    time_t now = time(NULL);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    
    json_object_set_new(metadata, "created_at", json_string(timestamp));
    json_object_set_new(metadata, "migrated_from", json_string(version_str));
    json_object_set_new(metadata, "migration_timestamp", json_string(timestamp));
    
    json_object_set_new(new_root, "metadata", metadata);
    
    // 保存新配置文件
    char *expanded_new = expand_path(new_file);
    if (!expanded_new) {
        LOG_ERROR("新文件路径扩展失败");
        json_decref(old_root);
        json_decref(new_root);
        free(expanded_old);
        return -1;
    }
    
    if (json_dump_file(new_root, expanded_new, JSON_INDENT(2) | JSON_SORT_KEYS) != 0) {
        LOG_ERROR("保存新配置文件失败");
        json_decref(old_root);
        json_decref(new_root);
        free(expanded_old);
        free(expanded_new);
        return -1;
    }
    
    LOG_INFO("配置迁移完成: %s -> %s", expanded_old, expanded_new);
    
    // 备份旧配置文件
    char backup[1024];
    snprintf(backup, sizeof(backup), "%s.v2.backup", expanded_old);
    if (rename(expanded_old, backup) == 0) {
        LOG_INFO("旧配置文件已备份到: %s", backup);
    }
    
    json_decref(old_root);
    json_decref(new_root);
    free(expanded_old);
    free(expanded_new);
    
    return 0;
}