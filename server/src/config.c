#include "config.h"
#include "protocol.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <dirent.h>
#include <jansson.h>

// ==================== 内部宏定义 ====================
#define CONFIG_DEFAULT_SECTION_CAPACITY 16
#define CONFIG_DEFAULT_ITEM_CAPACITY 32

// ==================== 静态变量 ====================
static pthread_mutex_t g_config_mutex = PTHREAD_MUTEX_INITIALIZER;

// ==================== 内部函数声明 ====================
static config_section_t* find_or_create_section(config_manager_t *manager, const char *section_name);
static config_item_t* find_item_in_section(config_section_t *section, const char *key);
static int parse_config_file(config_manager_t *manager, const char *file_path);
static int write_config_file(config_manager_t *manager, const char *file_path);
static void free_config_item(config_item_t *item);
static void free_config_section(config_section_t *section);
static bool validate_config_value(config_item_t *item, const char *value);
static int convert_string_to_value(config_item_t *item, const char *string_value);

// ==================== 内部函数实现 ====================

// ==================== config.c（续）====================

// 查找或创建配置段（续）
static config_section_t* find_or_create_section(config_manager_t *manager, const char *section_name) {
    if (!manager || !section_name) return NULL;
    
    // 查找现有段
    for (size_t i = 0; i < manager->section_count; i++) {
        if (strcmp(manager->sections[i]->name, section_name) == 0) {
            return manager->sections[i];
        }
    }
    
    // 创建新段
    if (manager->section_count >= manager->section_capacity) {
        size_t new_capacity = manager->section_capacity * 2;
        config_section_t **new_sections = realloc(manager->sections, 
                                                  new_capacity * sizeof(config_section_t*));
        if (!new_sections) return NULL;
        
        manager->sections = new_sections;
        manager->section_capacity = new_capacity;
    }
    
    config_section_t *section = calloc(1, sizeof(config_section_t));
    if (!section) return NULL;
    
    strncpy(section->name, section_name, CONFIG_MAX_SECTION_LEN - 1);
    section->item_capacity = CONFIG_DEFAULT_ITEM_CAPACITY;
    section->items = calloc(section->item_capacity, sizeof(config_item_t*));
    if (!section->items) {
        free(section);
        return NULL;
    }
    
    manager->sections[manager->section_count] = section;
    manager->section_count++;
    
    return section;
}

/**
 * 在配置段中查找配置项
 */
static config_item_t* find_item_in_section(config_section_t *section, const char *key) {
    if (!section || !key) return NULL;
    
    for (size_t i = 0; i < section->item_count; i++) {
        if (strcmp(section->items[i]->key, key) == 0) {
            return section->items[i];
        }
    }
    
    return NULL;
}

/**
 * 解析配置文件
 */
static int parse_config_file(config_manager_t *manager, const char *file_path) {
    if (!manager || !file_path) return -1;
    
    FILE *file = fopen(file_path, "r");
    if (!file) {
        return -2; // 文件不存在
    }
    
    char line[CONFIG_MAX_VALUE_LEN];
    char current_section[CONFIG_MAX_SECTION_LEN] = "default";
    int line_number = 0;
    
    while (fgets(line, sizeof(line), file)) {
        line_number++;
        
        // 去除换行符和空白字符
        char *trimmed = utils_trim(line);
        if (!trimmed || trimmed[0] == '\0' || trimmed[0] == ';' || trimmed[0] == '#') {
            continue;
        }
        
        // 检查是否是段定义
        if (trimmed[0] == '[' && trimmed[strlen(trimmed)-1] == ']') {
            // 提取段名
            char section_name[CONFIG_MAX_SECTION_LEN];
            strncpy(section_name, trimmed + 1, strlen(trimmed) - 2);
            section_name[strlen(trimmed) - 2] = '\0';
            utils_trim(section_name);
            
            if (strlen(section_name) > 0) {
                strncpy(current_section, section_name, CONFIG_MAX_SECTION_LEN - 1);
            }
            continue;
        }
        
        // 解析键值对
        char *equal_sign = strchr(trimmed, '=');
        if (!equal_sign) {
            continue; // 无效行
        }
        
        *equal_sign = '\0';
        char *key = utils_trim(trimmed);
        char *value = utils_trim(equal_sign + 1);
        
        if (strlen(key) == 0) {
            continue; // 无效键
        }
        
        // 查找对应的配置项
        config_item_t *item = config_find_item(manager, current_section, key);
        if (item) {
            // 更新配置值
            if (validate_config_value(item, value)) {
                convert_string_to_value(item, value);
                item->last_modified = time(NULL);
            }
        }
    }
    
    fclose(file);
    return 0;
}

/**
 * 写入配置文件
 */
static int write_config_file(config_manager_t *manager, const char *file_path) {
    if (!manager || !file_path) return -1;
    
    // 创建目录（如果不存在）
    char dir_path[CONFIG_MAX_PATH_LEN];
    strncpy(dir_path, file_path, CONFIG_MAX_PATH_LEN - 1);
    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkdir(dir_path, 0755);
    }
    
    FILE *file = fopen(file_path, "w");
    if (!file) return -2;
    
    fprintf(file, "; Secure Chat Configuration File\n");
    fprintf(file, "; Generated at: %s\n", ctime(&manager->last_save_time));
    fprintf(file, "\n");
    
    for (size_t i = 0; i < manager->section_count; i++) {
        config_section_t *section = manager->sections[i];
        
        if (section->item_count == 0) continue;
        
        fprintf(file, "[%s]\n", section->name);
        
        for (size_t j = 0; j < section->item_count; j++) {
            config_item_t *item = section->items[j];
            
            if (item->description) {
                fprintf(file, "; %s\n", item->description);
            }
            
            fprintf(file, "%s = ", item->key);
            
            switch (item->type) {
                case CONFIG_TYPE_STRING:
                    fprintf(file, "%s\n", item->string_value ? item->string_value : "");
                    break;
                case CONFIG_TYPE_INT:
                    fprintf(file, "%d\n", item->int_value);
                    break;
                case CONFIG_TYPE_BOOL:
                    fprintf(file, "%s\n", item->bool_value ? "true" : "false");
                    break;
                case CONFIG_TYPE_FLOAT:
                    fprintf(file, "%f\n", item->float_value);
                    break;
                case CONFIG_TYPE_STRING_ARRAY:
                    if (item->string_array && item->array_size > 0) {
                        for (size_t k = 0; k < item->array_size; k++) {
                            if (k > 0) fprintf(file, ",");
                            fprintf(file, "%s", item->string_array[k]);
                        }
                        fprintf(file, "\n");
                    }
                    break;
                case CONFIG_TYPE_INT_ARRAY:
                    if (item->int_array && item->array_size > 0) {
                        for (size_t k = 0; k < item->array_size; k++) {
                            if (k > 0) fprintf(file, ",");
                            fprintf(file, "%d", item->int_array[k]);
                        }
                        fprintf(file, "\n");
                    }
                    break;
            }
            
            fprintf(file, "\n");
        }
        
        fprintf(file, "\n");
    }
    
    fclose(file);
    return 0;
}

/**
 * 释放配置项内存
 */
static void free_config_item(config_item_t *item) {
    if (!item) return;
    
    if (item->string_value) free(item->string_value);
    if (item->default_value) free(item->default_value);
    if (item->description) free(item->description);
    
    if (item->type == CONFIG_TYPE_STRING_ARRAY && item->string_array) {
        for (size_t i = 0; i < item->array_size; i++) {
            if (item->string_array[i]) free(item->string_array[i]);
        }
        free(item->string_array);
    }
    
    if (item->type == CONFIG_TYPE_INT_ARRAY && item->int_array) {
        free(item->int_array);
    }
    
    free(item);
}

/**
 * 释放配置段内存
 */
static void free_config_section(config_section_t *section) {
    if (!section) return;
    
    for (size_t i = 0; i < section->item_count; i++) {
        free_config_item(section->items[i]);
    }
    
    free(section->items);
    free(section);
}

/**
 * 验证配置值
 */
static bool validate_config_value(config_item_t *item, const char *value) {
    if (!item || !value) return false;
    
    // 调用验证回调
    if (item->validate_callback) {
        return item->validate_callback(value, item->validate_user_data);
    }
    
    // 默认验证
    switch (item->type) {
        case CONFIG_TYPE_INT:
            {
                char *endptr;
                long val = strtol(value, &endptr, 10);
                if (*endptr != '\0') return false;
                return true;
            }
        case CONFIG_TYPE_BOOL:
            {
                if (strcasecmp(value, "true") == 0 || 
                    strcasecmp(value, "false") == 0 ||
                    strcasecmp(value, "yes") == 0 || 
                    strcasecmp(value, "no") == 0 ||
                    strcasecmp(value, "1") == 0 || 
                    strcasecmp(value, "0") == 0) {
                    return true;
                }
                return false;
            }
        case CONFIG_TYPE_FLOAT:
            {
                char *endptr;
                float val = strtof(value, &endptr);
                if (*endptr != '\0') return false;
                return true;
            }
        default:
            return true;
    }
}

/**
 * 转换字符串到配置值
 */
static int convert_string_to_value(config_item_t *item, const char *string_value) {
    if (!item || !string_value) return -1;
    
    switch (item->type) {
        case CONFIG_TYPE_STRING:
            {
                free(item->string_value);
                item->string_value = strdup(string_value);
                if (!item->string_value) return -2;
                break;
            }
        case CONFIG_TYPE_INT:
            {
                char *endptr;
                item->int_value = strtol(string_value, &endptr, 10);
                break;
            }
        case CONFIG_TYPE_BOOL:
            {
                if (strcasecmp(string_value, "true") == 0 || 
                    strcasecmp(string_value, "yes") == 0 ||
                    strcasecmp(string_value, "1") == 0) {
                    item->bool_value = true;
                } else {
                    item->bool_value = false;
                }
                break;
            }
        case CONFIG_TYPE_FLOAT:
            {
                char *endptr;
                item->float_value = strtof(string_value, &endptr);
                break;
            }
        case CONFIG_TYPE_STRING_ARRAY:
            {
                // 分割逗号分隔的字符串
                char *copy = strdup(string_value);
                if (!copy) return -2;
                
                char *token = strtok(copy, ",");
                size_t count = 0;
                
                // 计算元素数量
                while (token) {
                    utils_trim(token);
                    count++;
                    token = strtok(NULL, ",");
                }
                
                // 分配内存
                free(copy);
                copy = strdup(string_value);
                token = strtok(copy, ",");
                
                char **new_array = calloc(count, sizeof(char*));
                if (!new_array) {
                    free(copy);
                    return -2;
                }
                
                // 填充数组
                for (size_t i = 0; i < count && token; i++) {
                    utils_trim(token);
                    new_array[i] = strdup(token);
                    token = strtok(NULL, ",");
                }
                
                // 释放旧数组
                if (item->string_array) {
                    for (size_t i = 0; i < item->array_size; i++) {
                        free(item->string_array[i]);
                    }
                    free(item->string_array);
                }
                
                item->string_array = new_array;
                item->array_size = count;
                free(copy);
                break;
            }
        case CONFIG_TYPE_INT_ARRAY:
            {
                // 分割逗号分隔的整数
                char *copy = strdup(string_value);
                if (!copy) return -2;
                
                char *token = strtok(copy, ",");
                size_t count = 0;
                
                // 计算元素数量
                while (token) {
                    utils_trim(token);
                    count++;
                    token = strtok(NULL, ",");
                }
                
                // 分配内存
                free(copy);
                copy = strdup(string_value);
                token = strtok(copy, ",");
                
                int *new_array = calloc(count, sizeof(int));
                if (!new_array) {
                    free(copy);
                    return -2;
                }
                
                // 填充数组
                for (size_t i = 0; i < count && token; i++) {
                    utils_trim(token);
                    char *endptr;
                    new_array[i] = strtol(token, &endptr, 10);
                    token = strtok(NULL, ",");
                }
                
                // 释放旧数组
                if (item->int_array) {
                    free(item->int_array);
                }
                
                item->int_array = new_array;
                item->array_size = count;
                free(copy);
                break;
            }
    }
    
    item->modified = true;
    item->last_modified = time(NULL);
    
    return 0;
}

// ==================== 公共函数实现 ====================

/**
 * 创建配置管理器
 */
config_manager_t* config_manager_create(const char *config_path) {
    if (!config_path) return NULL;
    
    config_manager_t *manager = calloc(1, sizeof(config_manager_t));
    if (!manager) return NULL;
    
    strncpy(manager->config_path, config_path, CONFIG_MAX_PATH_LEN - 1);
    
    manager->section_capacity = CONFIG_DEFAULT_SECTION_CAPACITY;
    manager->sections = calloc(manager->section_capacity, sizeof(config_section_t*));
    if (!manager->sections) {
        free(manager);
        return NULL;
    }
    
    if (pthread_mutex_init(&manager->mutex, NULL) != 0) {
        free(manager->sections);
        free(manager);
        return NULL;
    }
    
    return manager;
}

/**
 * 销毁配置管理器
 */
void config_manager_destroy(config_manager_t *manager) {
    if (!manager) return;
    
    pthread_mutex_lock(&manager->mutex);
    
    for (size_t i = 0; i < manager->section_count; i++) {
        free_config_section(manager->sections[i]);
    }
    
    free(manager->sections);
    pthread_mutex_destroy(&manager->mutex);
    free(manager);
}

/**
 * 注册字符串配置项
 */
int config_register_string(config_manager_t *manager, const char *section, const char *key,
                          const char *default_value, const char *description,
                          config_validate_callback_t validate, void *user_data) {
    if (!manager || !section || !key) return -1;
    
    pthread_mutex_lock(&manager->mutex);
    
    config_section_t *section_ptr = find_or_create_section(manager, section);
    if (!section_ptr) {
        pthread_mutex_unlock(&manager->mutex);
        return -2;
    }
    
    // 检查是否已存在
    if (find_item_in_section(section_ptr, key)) {
        pthread_mutex_unlock(&manager->mutex);
        return -3;
    }
    
    // 扩展容量
    if (section_ptr->item_count >= section_ptr->item_capacity) {
        size_t new_capacity = section_ptr->item_capacity * 2;
        config_item_t **new_items = realloc(section_ptr->items, 
                                           new_capacity * sizeof(config_item_t*));
        if (!new_items) {
            pthread_mutex_unlock(&manager->mutex);
            return -4;
        }
        section_ptr->items = new_items;
        section_ptr->item_capacity = new_capacity;
    }
    
    config_item_t *item = calloc(1, sizeof(config_item_t));
    if (!item) {
        pthread_mutex_unlock(&manager->mutex);
        return -4;
    }
    
    strncpy(item->section, section, CONFIG_MAX_SECTION_LEN - 1);
    strncpy(item->key, key, CONFIG_MAX_KEY_LEN - 1);
    item->type = CONFIG_TYPE_STRING;
    
    if (default_value) {
        item->default_value = strdup(default_value);
        item->string_value = strdup(default_value);
    }
    
    if (description) {
        item->description = strdup(description);
    }
    
    item->validate_callback = validate;
    item->validate_user_data = user_data;
    item->last_modified = time(NULL);
    
    section_ptr->items[section_ptr->item_count] = item;
    section_ptr->item_count++;
    
    pthread_mutex_unlock(&manager->mutex);
    return 0;
}

/**
 * 获取字符串配置值
 */
const char* config_get_string(config_manager_t *manager, const char *section, const char *key) {
    if (!manager || !section || !key) return NULL;
    
    pthread_mutex_lock(&manager->mutex);
    
    config_item_t *item = config_find_item(manager, section, key);
    if (!item || item->type != CONFIG_TYPE_STRING) {
        pthread_mutex_unlock(&manager->mutex);
        return NULL;
    }
    
    const char *result = item->string_value;
    pthread_mutex_unlock(&manager->mutex);
    return result;
}

// ==================== utils.c ====================
#ifndef SECURE_CHAT_UTILS_H
#define SECURE_CHAT_UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <openssl/sha.h>
#include <openssl/rand.h>

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 字符串工具 ====================
char* utils_trim(char *str);
char* utils_strdup(const char *src);
char* utils_strndup(const char *src, size_t n);
int utils_strcasecmp(const char *s1, const char *s2);
char* utils_base64_encode(const unsigned char *data, size_t input_length, size_t *output_length);
unsigned char* utils_base64_decode(const char *data, size_t input_length, size_t *output_length);

// ==================== 安全工具 ====================
void utils_secure_zero(void *ptr, size_t len);
int utils_constant_time_compare(const void *a, const void *b, size_t len);
void utils_random_bytes(void *buffer, size_t len);

// ==================== 哈希工具 ====================
void utils_sha256(const void *data, size_t len, unsigned char hash[SHA256_DIGEST_LENGTH]);
char* utils_sha256_hex(const void *data, size_t len);

// ==================== 时间工具 ====================
uint64_t utils_get_timestamp_ms(void);
uint64_t utils_get_timestamp_ns(void);
char* utils_format_timestamp(uint64_t timestamp_ms, const char *format);

// ==================== 文件工具 ====================
int utils_file_exists(const char *path);
int utils_directory_exists(const char *path);
int utils_create_directory(const char *path);
int64_t utils_file_size(const char *path);
int utils_read_file(const char *path, void **buffer, size_t *size);
int utils_write_file(const char *path, const void *data, size_t size);
int utils_append_file(const char *path, const void *data, size_t size);

// ==================== 网络工具 ====================
int utils_is_valid_ipv4(const char *ip);
int utils_is_valid_ipv6(const char *ip);
int utils_is_valid_port(int port);
char* utils_get_local_ip(void);
int utils_parse_url(const char *url, char *protocol, char *host, char *port, char *path);

// ==================== 错误处理 ====================
const char* utils_strerror(int errnum);
void utils_perror(const char *msg);
void utils_print_backtrace(void);

// ==================== 内存工具 ====================
void* utils_malloc(size_t size, const char *purpose);
void* utils_calloc(size_t count, size_t size, const char *purpose);
void utils_free(void **ptr);

#ifdef __cplusplus
}
#endif

#endif // SECURE_CHAT_UTILS_H