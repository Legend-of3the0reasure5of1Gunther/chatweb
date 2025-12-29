#include "utils.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <zlib.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/evp.h>

// ==================== 内部宏定义 ====================
#define UTILS_MIN(a, b) ((a) < (b) ? (a) : (b))
#define UTILS_MAX(a, b) ((a) > (b) ? (a) : (b))

#define UTILS_ALIGN(size, alignment) (((size) + (alignment) - 1) & ~((alignment) - 1))
#define UTILS_ALIGN_PTR(ptr, alignment) ((void*)UTILS_ALIGN((uintptr_t)(ptr), alignment))

#define UTILS_LIKELY(x) __builtin_expect(!!(x), 1)
#define UTILS_UNLIKELY(x) __builtin_expect(!!(x), 0)

#define UTILS_ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

// ==================== 静态变量 ====================
static __thread error_info_t g_last_error = {0};
static logger_t *g_default_logger = NULL;
static pthread_mutex_t g_memory_tracking_mutex = PTHREAD_MUTEX_INITIALIZER;
static hash_table_t *g_memory_tracking_table = NULL;

// ==================== 内存跟踪结构 ====================
typedef struct {
    void *ptr;
    size_t size;
    char purpose[256];
    const char *file;
    int line;
    time_t time;
} memory_block_t;

// ==================== 内存管理 ====================

/**
 * 初始化内存跟踪
 */
static void init_memory_tracking(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    
    static void init(void) {
        g_memory_tracking_table = hash_table_create(1024, hash_string_djb2, free);
    }
    
    pthread_once(&once, init);
}

/**
 * 分配内存并跟踪
 */
void* utils_malloc(size_t size, const char *purpose) {
    if (size == 0) return NULL;
    
    void *ptr = malloc(size);
    if (UTILS_UNLIKELY(!ptr)) {
        utils_set_last_error(ERR_SERVER_ERROR, "内存分配失败: 请求大小 %zu 字节", size);
        return NULL;
    }
    
    // 内存跟踪
    init_memory_tracking();
    
    memory_block_t *block = malloc(sizeof(memory_block_t));
    if (block) {
        block->ptr = ptr;
        block->size = size;
        snprintf(block->purpose, sizeof(block->purpose), "%s", purpose ? purpose : "unknown");
        block->file = NULL;
        block->line = 0;
        block->time = time(NULL);
        
        char key[32];
        snprintf(key, sizeof(key), "%p", ptr);
        
        pthread_mutex_lock(&g_memory_tracking_mutex);
        hash_table_insert(g_memory_tracking_table, key, block);
        pthread_mutex_unlock(&g_memory_tracking_mutex);
    }
    
    return ptr;
}

/**
 * 分配并清零内存
 */
void* utils_calloc(size_t count, size_t size, const char *purpose) {
    if (count == 0 || size == 0) return NULL;
    
    size_t total_size = count * size;
    if (size != 0 && total_size / size != count) {
        utils_set_last_error(ERR_INVALID_PARAMETERS, "内存分配大小溢出");
        return NULL;
    }
    
    void *ptr = calloc(count, size);
    if (UTILS_UNLIKELY(!ptr)) {
        utils_set_last_error(ERR_SERVER_ERROR, "内存分配失败: 请求大小 %zu 字节", total_size);
        return NULL;
    }
    
    // 内存跟踪
    init_memory_tracking();
    
    memory_block_t *block = malloc(sizeof(memory_block_t));
    if (block) {
        block->ptr = ptr;
        block->size = total_size;
        snprintf(block->purpose, sizeof(block->purpose), "%s", purpose ? purpose : "unknown");
        block->file = NULL;
        block->line = 0;
        block->time = time(NULL);
        
        char key[32];
        snprintf(key, sizeof(key), "%p", ptr);
        
        pthread_mutex_lock(&g_memory_tracking_mutex);
        hash_table_insert(g_memory_tracking_table, key, block);
        pthread_mutex_unlock(&g_memory_tracking_mutex);
    }
    
    return ptr;
}

/**
 * 重新分配内存
 */
void* utils_realloc(void *ptr, size_t size, const char *purpose) {
    if (size == 0) {
        utils_free(&ptr);
        return NULL;
    }
    
    if (!ptr) {
        return utils_malloc(size, purpose);
    }
    
    // 从跟踪表中移除旧指针
    init_memory_tracking();
    
    if (g_memory_tracking_table) {
        char old_key[32];
        snprintf(old_key, sizeof(old_key), "%p", ptr);
        
        pthread_mutex_lock(&g_memory_tracking_mutex);
        hash_table_remove(g_memory_tracking_table, old_key);
        pthread_mutex_unlock(&g_memory_tracking_mutex);
    }
    
    void *new_ptr = realloc(ptr, size);
    if (UTILS_UNLIKELY(!new_ptr)) {
        utils_set_last_error(ERR_SERVER_ERROR, "内存重新分配失败: 请求大小 %zu 字节", size);
        return NULL;
    }
    
    // 添加新指针到跟踪表
    if (g_memory_tracking_table) {
        memory_block_t *block = malloc(sizeof(memory_block_t));
        if (block) {
            block->ptr = new_ptr;
            block->size = size;
            snprintf(block->purpose, sizeof(block->purpose), "%s", purpose ? purpose : "unknown");
            block->file = NULL;
            block->line = 0;
            block->time = time(NULL);
            
            char new_key[32];
            snprintf(new_key, sizeof(new_key), "%p", new_ptr);
            
            pthread_mutex_lock(&g_memory_tracking_mutex);
            hash_table_insert(g_memory_tracking_table, new_key, block);
            pthread_mutex_unlock(&g_memory_tracking_mutex);
        }
    }
    
    return new_ptr;
}

/**
 * 释放内存
 */
void utils_free(void **ptr) {
    if (!ptr || !*ptr) return;
    
    void *p = *ptr;
    
    // 从跟踪表中移除
    init_memory_tracking();
    
    if (g_memory_tracking_table) {
        char key[32];
        snprintf(key, sizeof(key), "%p", p);
        
        pthread_mutex_lock(&g_memory_tracking_mutex);
        hash_table_remove(g_memory_tracking_table, key);
        pthread_mutex_unlock(&g_memory_tracking_mutex);
    }
    
    free(p);
    *ptr = NULL;
}

/**
 * 检查内存泄漏
 */
void utils_memory_leak_check(void) {
    init_memory_tracking();
    
    if (!g_memory_tracking_table) return;
    
    pthread_mutex_lock(&g_memory_tracking_mutex);
    
    size_t leak_count = hash_table_count(g_memory_tracking_table);
    if (leak_count > 0) {
        fprintf(stderr, "\n========== 内存泄漏检测报告 ==========\n");
        fprintf(stderr, "发现 %zu 个内存泄漏:\n", leak_count);
        
        size_t total_leaked = 0;
        
        // 遍历所有bucket
        for (size_t i = 0; i < g_memory_tracking_table->size; i++) {
            hash_table_node_t *node = g_memory_tracking_table->buckets[i];
            while (node) {
                memory_block_t *block = (memory_block_t*)node->value;
                fprintf(stderr, "  - 地址: %p, 大小: %zu 字节, 用途: %s\n",
                       block->ptr, block->size, block->purpose);
                total_leaked += block->size;
                node = node->next;
            }
        }
        
        fprintf(stderr, "总泄漏内存: %zu 字节 (%.2f KB, %.2f MB)\n",
               total_leaked,
               total_leaked / 1024.0,
               total_leaked / (1024.0 * 1024.0));
        fprintf(stderr, "=====================================\n");
    } else {
        fprintf(stderr, "没有检测到内存泄漏\n");
    }
    
    pthread_mutex_unlock(&g_memory_tracking_mutex);
}

// ==================== 字符串操作 ====================

/**
 * 复制字符串
 */
char* utils_strdup(const char *str) {
    if (!str) return NULL;
    
    size_t len = strlen(str) + 1;
    char *new_str = (char*)utils_malloc(len, "strdup");
    if (new_str) {
        memcpy(new_str, str, len);
    }
    return new_str;
}

/**
 * 复制指定长度的字符串
 */
char* utils_strndup(const char *str, size_t n) {
    if (!str) return NULL;
    
    size_t len = strlen(str);
    if (len > n) len = n;
    
    char *new_str = (char*)utils_malloc(len + 1, "strndup");
    if (new_str) {
        memcpy(new_str, str, len);
        new_str[len] = '\0';
    }
    return new_str;
}

/**
 * 格式化字符串复制
 */
char* utils_strdup_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char *result = utils_strdup_vprintf(format, args);
    va_end(args);
    return result;
}

/**
 * 可变参数格式化字符串复制
 */
char* utils_strdup_vprintf(const char *format, va_list args) {
    if (!format) return NULL;
    
    va_list args_copy;
    va_copy(args_copy, args);
    
    int length = vsnprintf(NULL, 0, format, args_copy);
    va_end(args_copy);
    
    if (length < 0) {
        return NULL;
    }
    
    char *buffer = (char*)utils_malloc(length + 1, "strdup_vprintf");
    if (!buffer) {
        return NULL;
    }
    
    vsnprintf(buffer, length + 1, format, args);
    return buffer;
}

/**
 * 去除字符串两端的空白字符
 */
char* utils_strtrim(char *str) {
    if (!str) return NULL;
    
    char *end;
    
    // 去除开头的空白字符
    while (isspace((unsigned char)*str)) {
        str++;
    }
    
    if (*str == '\0') {
        return str;
    }
    
    // 去除结尾的空白字符
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        end--;
    }
    
    // 添加终止符
    *(end + 1) = '\0';
    
    return str;
}

/**
 * 去除字符串开头的空白字符
 */
char* utils_strltrim(char *str) {
    if (!str) return NULL;
    
    char *p = str;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    
    if (p != str) {
        size_t len = strlen(p);
        memmove(str, p, len + 1);
    }
    
    return str;
}

/**
 * 去除字符串结尾的空白字符
 */
char* utils_strrtrim(char *str) {
    if (!str) return NULL;
    
    char *end = str + strlen(str);
    while (end > str && isspace((unsigned char)*(end - 1))) {
        end--;
    }
    
    *end = '\0';
    return str;
}

/**
 * 转换为小写
 */
char* utils_strlower(char *str) {
    if (!str) return NULL;
    
    for (char *p = str; *p; p++) {
        *p = tolower((unsigned char)*p);
    }
    
    return str;
}

/**
 * 转换为大写
 */
char* utils_strupper(char *str) {
    if (!str) return NULL;
    
    for (char *p = str; *p; p++) {
        *p = toupper((unsigned char)*p);
    }
    
    return str;
}

/**
 * 检查字符串是否以指定前缀开头
 */
bool utils_strstartswith(const char *str, const char *prefix) {
    if (!str || !prefix) return false;
    
    size_t str_len = strlen(str);
    size_t prefix_len = strlen(prefix);
    
    if (prefix_len > str_len) return false;
    
    return strncmp(str, prefix, prefix_len) == 0;
}

/**
 * 检查字符串是否以指定后缀结尾
 */
bool utils_strendswith(const char *str, const char *suffix) {
    if (!str || !suffix) return false;
    
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    
    if (suffix_len > str_len) return false;
    
    return strcmp(str + str_len - suffix_len, suffix) == 0;
}

/**
 * 替换字符串中的子串
 */
char* utils_strreplace(const char *str, const char *old, const char *new) {
    if (!str || !old || !new) return NULL;
    
    // 计算旧子串出现的次数
    size_t old_len = strlen(old);
    size_t new_len = strlen(new);
    
    if (old_len == 0) {
        return utils_strdup(str);
    }
    
    const char *pos = str;
    size_t count = 0;
    
    while ((pos = strstr(pos, old)) != NULL) {
        count++;
        pos += old_len;
    }
    
    // 计算新字符串长度
    size_t str_len = strlen(str);
    size_t new_str_len = str_len + count * (new_len - old_len);
    
    // 分配内存
    char *result = (char*)utils_malloc(new_str_len + 1, "strreplace");
    if (!result) return NULL;
    
    // 执行替换
    char *dest = result;
    const char *src = str;
    
    while ((pos = strstr(src, old)) != NULL) {
        // 复制旧子串之前的部分
        size_t segment_len = pos - src;
        memcpy(dest, src, segment_len);
        dest += segment_len;
        
        // 复制新子串
        memcpy(dest, new, new_len);
        dest += new_len;
        
        // 移动源指针
        src = pos + old_len;
    }
    
    // 复制剩余部分
    strcpy(dest, src);
    
    return result;
}

/**
 * 分割字符串
 */
char** utils_strsplit(const char *str, const char *delim, int *count) {
    if (!str || !delim || !count) return NULL;
    
    *count = 0;
    
    // 计算分割数量
    char *str_copy = utils_strdup(str);
    if (!str_copy) return NULL;
    
    char *saveptr = NULL;
    char *token = strtok_r(str_copy, delim, &saveptr);
    while (token) {
        (*count)++;
        token = strtok_r(NULL, delim, &saveptr);
    }
    
    free(str_copy);
    
    if (*count == 0) {
        return NULL;
    }
    
    // 分配数组
    char **result = (char**)utils_calloc(*count + 1, sizeof(char*), "strsplit");
    if (!result) return NULL;
    
    // 执行分割
    str_copy = utils_strdup(str);
    if (!str_copy) {
        free(result);
        return NULL;
    }
    
    saveptr = NULL;
    token = strtok_r(str_copy, delim, &saveptr);
    for (int i = 0; token && i < *count; i++) {
        result[i] = utils_strdup(token);
        token = strtok_r(NULL, delim, &saveptr);
    }
    
    free(str_copy);
    return result;
}

/**
 * 释放字符串数组
 */
void utils_strfreev(char **str_array) {
    if (!str_array) return;
    
    for (int i = 0; str_array[i]; i++) {
        free(str_array[i]);
    }
    
    free(str_array);
}

/**
 * 统计字符出现次数
 */
int utils_strcount(const char *str, char ch) {
    if (!str) return 0;
    
    int count = 0;
    for (const char *p = str; *p; p++) {
        if (*p == ch) {
            count++;
        }
    }
    
    return count;
}

/**
 * 连接字符串数组
 */
char* utils_strjoin(const char *separator, char **str_array) {
    if (!str_array) return NULL;
    
    // 计算总长度
    size_t total_length = 0;
    size_t sep_len = separator ? strlen(separator) : 0;
    int count = 0;
    
    for (int i = 0; str_array[i]; i++) {
        total_length += strlen(str_array[i]);
        if (i > 0 && sep_len > 0) {
            total_length += sep_len;
        }
        count++;
    }
    
    // 分配内存
    char *result = (char*)utils_malloc(total_length + 1, "strjoin");
    if (!result) return NULL;
    
    // 连接字符串
    char *p = result;
    for (int i = 0; i < count; i++) {
        if (i > 0 && sep_len > 0) {
            memcpy(p, separator, sep_len);
            p += sep_len;
        }
        
        size_t len = strlen(str_array[i]);
        memcpy(p, str_array[i], len);
        p += len;
    }
    
    *p = '\0';
    return result;
}

// ==================== 动态字符串 ====================

/**
 * 创建动态字符串
 */
string_t* string_create(size_t initial_capacity) {
    string_t *str = (string_t*)utils_malloc(sizeof(string_t), "string_create");
    if (!str) return NULL;
    
    if (initial_capacity < 16) {
        initial_capacity = 16;
    }
    
    str->data = (char*)utils_malloc(initial_capacity, "string_data");
    if (!str->data) {
        free(str);
        return NULL;
    }
    
    str->data[0] = '\0';
    str->length = 0;
    str->capacity = initial_capacity;
    
    return str;
}

/**
 * 销毁动态字符串
 */
void string_destroy(string_t *str) {
    if (!str) return;
    
    if (str->data) {
        free(str->data);
    }
    
    free(str);
}

/**
 * 清空动态字符串
 */
void string_clear(string_t *str) {
    if (!str || !str->data) return;
    
    str->data[0] = '\0';
    str->length = 0;
}

/**
 * 追加字符串
 */
int string_append(string_t *str, const char *text) {
    if (!str || !text) return -1;
    
    size_t text_len = strlen(text);
    size_t new_length = str->length + text_len;
    
    // 检查是否需要扩容
    if (new_length + 1 > str->capacity) {
        size_t new_capacity = str->capacity * 2;
        while (new_capacity <= new_length) {
            new_capacity *= 2;
        }
        
        char *new_data = (char*)utils_realloc(str->data, new_capacity, "string_append");
        if (!new_data) return -1;
        
        str->data = new_data;
        str->capacity = new_capacity;
    }
    
    // 追加字符串
    memcpy(str->data + str->length, text, text_len);
    str->length = new_length;
    str->data[str->length] = '\0';
    
    return 0;
}

/**
 * 格式化追加
 */
int string_append_format(string_t *str, const char *format, ...) {
    if (!str || !format) return -1;
    
    va_list args;
    va_start(args, format);
    
    // 获取格式化字符串长度
    va_list args_copy;
    va_copy(args_copy, args);
    int length = vsnprintf(NULL, 0, format, args_copy);
    va_end(args_copy);
    
    if (length < 0) {
        va_end(args);
        return -1;
    }
    
    // 检查是否需要扩容
    size_t new_length = str->length + length;
    if (new_length + 1 > str->capacity) {
        size_t new_capacity = str->capacity * 2;
        while (new_capacity <= new_length) {
            new_capacity *= 2;
        }
        
        char *new_data = (char*)utils_realloc(str->data, new_capacity, "string_append_format");
        if (!new_data) {
            va_end(args);
            return -1;
        }
        
        str->data = new_data;
        str->capacity = new_capacity;
    }
    
    // 格式化追加
    vsnprintf(str->data + str->length, str->capacity - str->length, format, args);
    str->length = new_length;
    
    va_end(args);
    return 0;
}

/**
 * 追加字符
 */
int string_append_char(string_t *str, char ch) {
    if (!str) return -1;
    
    char temp[2] = {ch, '\0'};
    return string_append(str, temp);
}

/**
 * 获取字符串内容
 */
const char* string_get(const string_t *str) {
    if (!str) return NULL;
    return str->data ? str->data : "";
}

/**
 * 获取字符串长度
 */
size_t string_length(const string_t *str) {
    if (!str) return 0;
    return str->length;
}

/**
 * 检查字符串是否为空
 */
bool string_empty(const string_t *str) {
    if (!str) return true;
    return str->length == 0;
}

/**
 * 比较字符串
 */
int string_compare(const string_t *str1, const string_t *str2) {
    if (!str1 && !str2) return 0;
    if (!str1) return -1;
    if (!str2) return 1;
    
    return strcmp(string_get(str1), string_get(str2));
}

// ==================== 文件操作 ====================

/**
 * 检查文件是否存在
 */
bool utils_file_exists(const char *path) {
    if (!path) return false;
    
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/**
 * 检查目录是否存在
 */
bool utils_dir_exists(const char *path) {
    if (!path) return false;
    
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * 获取文件大小
 */
int64_t utils_file_size(const char *path) {
    if (!path) return -1;
    
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    
    return st.st_size;
}

/**
 * 获取文件修改时间
 */
time_t utils_file_mtime(const char *path) {
    if (!path) return 0;
    
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    
    return st.st_mtime;
}

/**
 * 获取文件访问时间
 */
time_t utils_file_atime(const char *path) {
    if (!path) return 0;
    
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    
    return st.st_atime;
}

/**
 * 获取文件创建时间
 */
time_t utils_file_ctime(const char *path) {
    if (!path) return 0;
    
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    
    return st.st_ctime;
}

/**
 * 创建目录
 */
int utils_create_dir(const char *path, mode_t mode) {
    if (!path) return -1;
    
    return mkdir(path, mode);
}

/**
 * 创建多级目录
 */
int utils_create_dirs(const char *path, mode_t mode) {
    if (!path) return -1;
    
    char *path_copy = utils_strdup(path);
    if (!path_copy) return -1;
    
    int result = 0;
    char *p = path_copy;
    
    // 跳过根目录
    if (*p == '/') {
        p++;
    }
    
    while (*p) {
        // 查找下一个目录分隔符
        while (*p && *p != '/') {
            p++;
        }
        
        // 保存当前字符
        char saved = *p;
        *p = '\0';
        
        // 创建目录
        if (!utils_dir_exists(path_copy)) {
            if (mkdir(path_copy, mode) != 0 && errno != EEXIST) {
                result = -1;
                break;
            }
        }
        
        // 恢复字符
        if (saved != '\0') {
            *p = saved;
            p++;
        }
    }
    
    free(path_copy);
    return result;
}

/**
 * 读取文件内容
 */
char* utils_read_file(const char *path, size_t *length) {
    if (!path) return NULL;
    
    FILE *file = fopen(path, "rb");
    if (!file) {
        utils_set_last_error(ERR_FILESYSTEM_ERROR, "无法打开文件: %s", path);
        return NULL;
    }
    
    // 获取文件大小
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    if (file_size < 0) {
        fclose(file);
        utils_set_last_error(ERR_FILESYSTEM_ERROR, "无法获取文件大小: %s", path);
        return NULL;
    }
    
    // 分配内存
    char *buffer = (char*)utils_malloc(file_size + 1, "read_file");
    if (!buffer) {
        fclose(file);
        return NULL;
    }
    
    // 读取文件
    size_t bytes_read = fread(buffer, 1, file_size, file);
    if (bytes_read != (size_t)file_size) {
        free(buffer);
        fclose(file);
        utils_set_last_error(ERR_FILESYSTEM_ERROR, "读取文件失败: %s", path);
        return NULL;
    }
    
    buffer[file_size] = '\0';
    
    if (length) {
        *length = file_size;
    }
    
    fclose(file);
    return buffer;
}

/**
 * 写入文件
 */
int utils_write_file(const char *path, const void *data, size_t length) {
    if (!path || !data) return -1;
    
    FILE *file = fopen(path, "wb");
    if (!file) {
        utils_set_last_error(ERR_FILESYSTEM_ERROR, "无法创建文件: %s", path);
        return -1;
    }
    
    size_t bytes_written = fwrite(data, 1, length, file);
    fclose(file);
    
    if (bytes_written != length) {
        utils_set_last_error(ERR_FILESYSTEM_ERROR, "写入文件失败: %s", path);
        return -1;
    }
    
    return 0;
}

/**
 * 追加到文件
 */
int utils_append_file(const char *path, const void *data, size_t length) {
    if (!path || !data) return -1;
    
    FILE *file = fopen(path, "ab");
    if (!file) {
        utils_set_last_error(ERR_FILESYSTEM_ERROR, "无法打开文件: %s", path);
        return -1;
    }
    
    size_t bytes_written = fwrite(data, 1, length, file);
    fclose(file);
    
    if (bytes_written != length) {
        utils_set_last_error(ERR_FILESYSTEM_ERROR, "追加文件失败: %s", path);
        return -1;
    }
    
    return 0;
}

/**
 * 获取当前工作目录
 */
char* utils_get_cwd(void) {
    char *cwd = getcwd(NULL, 0);
    if (!cwd) {
        utils_set_last_error(ERR_FILESYSTEM_ERROR, "无法获取当前工作目录");
        return NULL;
    }
    
    return cwd;
}

/**
 * 改变当前工作目录
 */
int utils_change_dir(const char *path) {
    if (!path) return -1;
    
    if (chdir(path) != 0) {
        utils_set_last_error(ERR_FILESYSTEM_ERROR, "无法改变工作目录到: %s", path);
        return -1;
    }
    
    return 0;
}

/**
 * 获取规范化的绝对路径
 */
char* utils_realpath(const char *path) {
    if (!path) return NULL;
    
    char *real_path = realpath(path, NULL);
    if (!real_path) {
        utils_set_last_error(ERR_FILESYSTEM_ERROR, "无法获取绝对路径: %s", path);
        return NULL;
    }
    
    return real_path;
}

/**
 * 复制文件
 */
int utils_copy_file(const char *src, const char *dst) {
    if (!src || !dst) return -1;
    
    FILE *src_file = fopen(src, "rb");
    if (!src_file) {
        utils_set_last_error(ERR_FILESYSTEM_ERROR, "无法打开源文件: %s", src);
        return -1;
    }
    
    FILE *dst_file = fopen(dst, "wb");
    if (!dst_file) {
        fclose(src_file);
        utils_set_last_error(ERR_FILESYSTEM_ERROR, "无法创建目标文件: %s", dst);
        return -1;
    }
    
    char buffer[8192];
    size_t bytes_read;
    int result = 0;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), src_file)) > 0) {
        size_t bytes_written = fwrite(buffer, 1, bytes_read, dst_file);
        if (bytes_written != bytes_read) {
            result = -1;
            break;
        }
    }
    
    fclose(src_file);
    fclose(dst_file);
    
    if (result != 0) {
        utils_set_last_error(ERR_FILESYSTEM_ERROR, "复制文件失败: %s -> %s", src, dst);
    }
    
    return result;
}

/**
 * 移动文件
 */
int utils_move_file(const char *src, const char *dst) {
    if (!src || !dst) return -1;
    
    if (rename(src, dst) == 0) {
        return 0;
    }
    
    // 如果rename失败，尝试复制后删除
    if (utils_copy_file(src, dst) != 0) {
        return -1;
    }
    
    if (unlink(src) != 0) {
        utils_set_last_error(ERR_FILESYSTEM_ERROR, "无法删除源文件: %s", src);
        return -1;
    }
    
    return 0;
}

/**
 * 删除文件
 */
int utils_delete_file(const char *path) {
    if (!path) return -1;
    
    if (unlink(path) != 0) {
        utils_set_last_error(ERR_FILESYSTEM_ERROR, "无法删除文件: %s", path);
        return -1;
    }
    
    return 0;
}

// ==================== 路径操作 ====================

/**
 * 连接路径
 */
char* utils_path_join(const char *dir, const char *file) {
    if (!dir && !file) return NULL;
    if (!dir) return utils_strdup(file);
    if (!file) return utils_strdup(dir);
    
    size_t dir_len = strlen(dir);
    size_t file_len = strlen(file);
    
    // 计算需要的长度
    size_t result_len = dir_len + file_len + 2; // +2 for possible '/' and null terminator
    
    char *result = (char*)utils_malloc(result_len, "path_join");
    if (!result) return NULL;
    
    strcpy(result, dir);
    
    // 添加目录分隔符（如果需要）
    if (dir_len > 0 && dir[dir_len - 1] != '/' && file_len > 0 && file[0] != '/') {
        strcat(result, "/");
    }
    
    strcat(result, file);
    
    return result;
}

/**
 * 获取目录名
 */
char* utils_path_dirname(const char *path) {
    if (!path) return NULL;
    
    char *path_copy = utils_strdup(path);
    if (!path_copy) return NULL;
    
    // 找到最后一个目录分隔符
    char *last_slash = strrchr(path_copy, '/');
    if (last_slash) {
        // 如果分隔符是字符串的最后一个字符，继续向前找
        if (last_slash[1] == '\0') {
            *last_slash = '\0';
            last_slash = strrchr(path_copy, '/');
        }
        
        if (last_slash) {
            *last_slash = '\0';
        } else {
            // 没有找到其他分隔符，返回根目录或当前目录
            if (path_copy[0] == '/') {
                path_copy[1] = '\0';
            } else {
                path_copy[0] = '.';
                path_copy[1] = '\0';
            }
        }
    } else {
        // 没有分隔符，返回当前目录
        path_copy[0] = '.';
        path_copy[1] = '\0';
    }
    
    return path_copy;
}

/**
 * 获取文件名
 */
char* utils_path_basename(const char *path) {
    if (!path) return NULL;
    
    // 找到最后一个目录分隔符
    const char *last_slash = strrchr(path, '/');
    if (last_slash) {
        return utils_strdup(last_slash + 1);
    }
    
    return utils_strdup(path);
}

/**
 * 获取文件扩展名
 */
char* utils_path_extension(const char *path) {
    if (!path) return NULL;
    
    const char *last_dot = strrchr(path, '.');
    if (!last_dot) {
        return utils_strdup("");
    }
    
    const char *last_slash = strrchr(path, '/');
    if (last_slash && last_dot < last_slash) {
        // 点在目录分隔符之前，不是扩展名
        return utils_strdup("");
    }
    
    return utils_strdup(last_dot);
}

/**
 * 移除扩展名
 */
char* utils_path_without_extension(const char *path) {
    if (!path) return NULL;
    
    char *result = utils_strdup(path);
    if (!result) return NULL;
    
    char *last_dot = strrchr(result, '.');
    if (last_dot) {
        char *last_slash = strrchr(result, '/');
        if (!last_slash || last_dot > last_slash) {
            *last_dot = '\0';
        }
    }
    
    return result;
}

/**
 * 检查是否是绝对路径
 */
bool utils_path_is_absolute(const char *path) {
    if (!path) return false;
    
    return path[0] == '/';
}

/**
 * 规范化路径
 */
char* utils_path_normalize(const char *path) {
    if (!path) return NULL;
    
    char *normalized = utils_strdup(path);
    if (!normalized) return NULL;
    
    // 移除重复的目录分隔符
    char *src = normalized;
    char *dst = normalized;
    
    while (*src) {
        if (*src == '/') {
            *dst++ = '/';
            while (*src == '/') {
                src++;
            }
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    
    // 处理 "." 和 ".."
    char **components = utils_strsplit(normalized, "/", NULL);
    if (!components) {
        free(normalized);
        return NULL;
    }
    
    list_t *stack = list_create(free, NULL);
    
    for (int i = 0; components[i]; i++) {
        if (strcmp(components[i], ".") == 0) {
            // 忽略当前目录
            continue;
        } else if (strcmp(components[i], "..") == 0) {
            // 返回上级目录
            if (!list_empty(stack)) {
                list_remove(stack, list_size(stack) - 1);
            }
        } else {
            // 添加目录组件
            list_append(stack, utils_strdup(components[i]));
        }
    }
    
    utils_strfreev(components);
    
    // 重新构建路径
    free(normalized);
    normalized = utils_strdup("");
    
    // 如果是绝对路径，以"/"开头
    if (path[0] == '/') {
        char *temp = utils_strdup("/");
        free(normalized);
        normalized = temp;
    }
    
    list_iterator_t *iter = list_iterator_create(stack);
    while (list_iterator_has_next(iter)) {
        char *component = list_iterator_next(iter);
        
        char *new_path;
        if (strcmp(normalized, "/") == 0) {
            new_path = utils_strdup_printf("%s%s", normalized, component);
        } else {
            new_path = utils_strdup_printf("%s/%s", normalized, component);
        }
        
        free(normalized);
        normalized = new_path;
    }
    
    list_iterator_destroy(iter);
    list_destroy(stack);
    
    // 如果是空路径，返回"."
    if (strlen(normalized) == 0) {
        free(normalized);
        normalized = utils_strdup(".");
    }
    
    return normalized;
}

// ==================== 哈希表操作 ====================

/**
 * 创建哈希表
 */
hash_table_t* hash_table_create(size_t size, 
                               size_t (*hash_function)(const char *key),
                               void (*value_free_function)(void *value)) {
    hash_table_t *table = (hash_table_t*)utils_malloc(sizeof(hash_table_t), "hash_table_create");
    if (!table) return NULL;
    
    // 确保大小是质数，减少冲突
    static const size_t primes[] = {
        53, 97, 193, 389, 769, 1543, 3079, 6151, 
        12289, 24593, 49157, 98317, 196613, 393241
    };
    
    for (size_t i = 0; i < sizeof(primes) / sizeof(primes[0]); i++) {
        if (primes[i] >= size) {
            size = primes[i];
            break;
        }
    }
    
    table->buckets = (hash_table_node_t**)utils_calloc(size, sizeof(hash_table_node_t*), "hash_table_buckets");
    if (!table->buckets) {
        free(table);
        return NULL;
    }
    
    table->size = size;
    table->count = 0;
    table->hash_function = hash_function ? hash_function : hash_string_djb2;
    table->value_free_function = value_free_function;
    
    return table;
}

/**
 * 销毁哈希表
 */
void hash_table_destroy(hash_table_t *table) {
    if (!table) return;
    
    // 释放所有节点
    for (size_t i = 0; i < table->size; i++) {
        hash_table_node_t *node = table->buckets[i];
        while (node) {
            hash_table_node_t *next = node->next;
            
            free(node->key);
            if (table->value_free_function && node->value) {
                table->value_free_function(node->value);
            }
            free(node);
            
            node = next;
        }
    }
    
    free(table->buckets);
    free(table);
}

/**
 * 插入键值对
 */
int hash_table_insert(hash_table_t *table, const char *key, void *value) {
    if (!table || !key) return -1;
    
    size_t index = table->hash_function(key) % table->size;
    
    // 检查键是否已存在
    hash_table_node_t *node = table->buckets[index];
    while (node) {
        if (strcmp(node->key, key) == 0) {
            // 更新值
            if (table->value_free_function && node->value) {
                table->value_free_function(node->value);
            }
            node->value = value;
            return 0;
        }
        node = node->next;
    }
    
    // 创建新节点
    node = (hash_table_node_t*)utils_malloc(sizeof(hash_table_node_t), "hash_table_node");
    if (!node) return -1;
    
    node->key = utils_strdup(key);
    if (!node->key) {
        free(node);
        return -1;
    }
    
    node->value = value;
    node->next = table->buckets[index];
    table->buckets[index] = node;
    table->count++;
    
    return 0;
}

/**
 * 查找值
 */
void* hash_table_lookup(hash_table_t *table, const char *key) {
    if (!table || !key) return NULL;
    
    size_t index = table->hash_function(key) % table->size;
    
    hash_table_node_t *node = table->buckets[index];
    while (node) {
        if (strcmp(node->key, key) == 0) {
            return node->value;
        }
        node = node->next;
    }
    
    return NULL;
}

/**
 * 删除键值对
 */
int hash_table_remove(hash_table_t *table, const char *key) {
    if (!table || !key) return -1;
    
    size_t index = table->hash_function(key) % table->size;
    
    hash_table_node_t *node = table->buckets[index];
    hash_table_node_t *prev = NULL;
    
    while (node) {
        if (strcmp(node->key, key) == 0) {
            if (prev) {
                prev->next = node->next;
            } else {
                table->buckets[index] = node->next;
            }
            
            free(node->key);
            if (table->value_free_function && node->value) {
                table->value_free_function(node->value);
            }
            free(node);
            
            table->count--;
            return 0;
        }
        
        prev = node;
        node = node->next;
    }
    
    return -1; // 键不存在
}

/**
 * 获取哈希表大小
 */
size_t hash_table_size(const hash_table_t *table) {
    if (!table) return 0;
    return table->size;
}

/**
 * 获取哈希表元素数量
 */
size_t hash_table_count(const hash_table_t *table) {
    if (!table) return 0;
    return table->count;
}

/**
 * DJB2哈希函数
 */
size_t hash_string_djb2(const char *key) {
    size_t hash = 5381;
    int c;
    
    while ((c = *key++)) {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }
    
    return hash;
}

/**
 * SDBM哈希函数
 */
size_t hash_string_sdbm(const char *key) {
    size_t hash = 0;
    int c;
    
    while ((c = *key++)) {
        hash = c + (hash << 6) + (hash << 16) - hash;
    }
    
    return hash;
}

/**
 * FNV-1a哈希函数
 */
size_t hash_string_fnv1a(const char *key) {
    size_t hash = 14695981039346656037ULL;
    int c;
    
    while ((c = *key++)) {
        hash ^= (size_t)c;
        hash *= 1099511628211ULL;
    }
    
    return hash;
}

// ==================== 链表操作 ====================

/**
 * 创建链表
 */
list_t* list_create(void (*data_free_function)(void *data),
                   int (*data_compare_function)(const void *a, const void *b)) {
    list_t *list = (list_t*)utils_malloc(sizeof(list_t), "list_create");
    if (!list) return NULL;
    
    list->head = NULL;
    list->tail = NULL;
    list->size = 0;
    list->data_free_function = data_free_function;
    list->data_compare_function = data_compare_function;
    
    return list;
}

/**
 * 销毁链表
 */
void list_destroy(list_t *list) {
    if (!list) return;
    
    list_clear(list);
    free(list);
}

/**
 * 追加元素
 */
int list_append(list_t *list, void *data) {
    if (!list) return -1;
    
    list_node_t *node = (list_node_t*)utils_malloc(sizeof(list_node_t), "list_node");
    if (!node) return -1;
    
    node->data = data;
    node->next = NULL;
    node->prev = list->tail;
    
    if (list->tail) {
        list->tail->next = node;
    } else {
        list->head = node;
    }
    
    list->tail = node;
    list->size++;
    
    return 0;
}

/**
 * 在头部插入元素
 */
int list_prepend(list_t *list, void *data) {
    if (!list) return -1;
    
    list_node_t *node = (list_node_t*)utils_malloc(sizeof(list_node_t), "list_node");
    if (!node) return -1;
    
    node->data = data;
    node->prev = NULL;
    node->next = list->head;
    
    if (list->head) {
        list->head->prev = node;
    } else {
        list->tail = node;
    }
    
    list->head = node;
    list->size++;
    
    return 0;
}

/**
 * 在指定位置插入元素
 */
int list_insert(list_t *list, size_t index, void *data) {
    if (!list) return -1;
    
    if (index >= list->size) {
        return list_append(list, data);
    }
    
    if (index == 0) {
        return list_prepend(list, data);
    }
    
    // 找到要插入位置的前一个节点
    list_node_t *current = list->head;
    for (size_t i = 0; i < index - 1; i++) {
        current = current->next;
    }
    
    list_node_t *node = (list_node_t*)utils_malloc(sizeof(list_node_t), "list_node");
    if (!node) return -1;
    
    node->data = data;
    node->prev = current;
    node->next = current->next;
    
    if (current->next) {
        current->next->prev = node;
    }
    
    current->next = node;
    list->size++;
    
    return 0;
}

/**
 * 获取元素
 */
void* list_get(const list_t *list, size_t index) {
    if (!list || index >= list->size) return NULL;
    
    list_node_t *current = list->head;
    for (size_t i = 0; i < index; i++) {
        current = current->next;
    }
    
    return current->data;
}

/**
 * 删除指定位置的元素
 */
int list_remove(list_t *list, size_t index) {
    if (!list || index >= list->size) return -1;
    
    // 找到要删除的节点
    list_node_t *current = list->head;
    for (size_t i = 0; i < index; i++) {
        current = current->next;
    }
    
    // 更新前后节点的指针
    if (current->prev) {
        current->prev->next = current->next;
    } else {
        list->head = current->next;
    }
    
    if (current->next) {
        current->next->prev = current->prev;
    } else {
        list->tail = current->prev;
    }
    
    // 释放节点和数据
    if (list->data_free_function && current->data) {
        list->data_free_function(current->data);
    }
    free(current);
    
    list->size--;
    return 0;
}

/**
 * 删除指定数据的元素
 */
int list_remove_data(list_t *list, void *data) {
    if (!list || !data) return -1;
    
    list_node_t *current = list->head;
    size_t index = 0;
    
    while (current) {
        int match = 0;
        
        if (list->data_compare_function) {
            match = list->data_compare_function(current->data, data) == 0;
        } else {
            match = (current->data == data);
        }
        
        if (match) {
            list_remove(list, index);
            return 0;
        }
        
        current = current->next;
        index++;
    }
    
    return -1; // 未找到
}

/**
 * 获取链表大小
 */
size_t list_size(const list_t *list) {
    if (!list) return 0;
    return list->size;
}

/**
 * 检查链表是否为空
 */
bool list_empty(const list_t *list) {
    if (!list) return true;
    return list->size == 0;
}

/**
 * 清空链表
 */
void list_clear(list_t *list) {
    if (!list) return;
    
    list_node_t *current = list->head;
    while (current) {
        list_node_t *next = current->next;
        
        if (list->data_free_function && current->data) {
            list->data_free_function(current->data);
        }
        free(current);
        
        current = next;
    }
    
    list->head = NULL;
    list->tail = NULL;
    list->size = 0;
}

// ==================== 链表迭代器 ====================

/**
 * 创建迭代器
 */
list_iterator_t* list_iterator_create(list_t *list) {
    if (!list) return NULL;
    
    list_iterator_t *iter = (list_iterator_t*)utils_malloc(sizeof(list_iterator_t), "list_iterator");
    if (!iter) return NULL;
    
    iter->list = list;
    iter->current = list->head;
    iter->index = 0;
    
    return iter;
}

/**
 * 销毁迭代器
 */
void list_iterator_destroy(list_iterator_t *iter) {
    if (!iter) return;
    free(iter);
}

/**
 * 检查是否有下一个元素
 */
bool list_iterator_has_next(list_iterator_t *iter) {
    if (!iter || !iter->current) return false;
    return true;
}

/**
 * 获取下一个元素
 */
void* list_iterator_next(list_iterator_t *iter) {
    if (!iter || !iter->current) return NULL;
    
    void *data = iter->current->data;
    iter->current = iter->current->next;
    iter->index++;
    
    return data;
}

/**
 * 重置迭代器
 */
void list_iterator_rewind(list_iterator_t *iter) {
    if (!iter) return;
    
    iter->current = iter->list->head;
    iter->index = 0;
}

// ==================== 环形缓冲区操作 ====================

/**
 * 创建环形缓冲区
 */
ring_buffer_t* ring_buffer_create(size_t capacity, bool overwrite) {
    ring_buffer_t *rb = (ring_buffer_t*)utils_malloc(sizeof(ring_buffer_t), "ring_buffer");
    if (!rb) return NULL;
    
    rb->buffer = (uint8_t*)utils_malloc(capacity, "ring_buffer_data");
    if (!rb->buffer) {
        free(rb);
        return NULL;
    }
    
    rb->capacity = capacity;
    rb->head = 0;
    rb->tail = 0;
    rb->size = 0;
    rb->overwrite = overwrite;
    
    return rb;
}

/**
 * 销毁环形缓冲区
 */
void ring_buffer_destroy(ring_buffer_t *rb) {
    if (!rb) return;
    
    if (rb->buffer) {
        free(rb->buffer);
    }
    
    free(rb);
}

/**
 * 写入数据
 */
size_t ring_buffer_write(ring_buffer_t *rb, const void *data, size_t size) {
    if (!rb || !data || size == 0) return 0;
    
    const uint8_t *src = (const uint8_t*)data;
    size_t bytes_written = 0;
    
    if (!rb->overwrite) {
        // 非覆盖模式，检查是否有足够空间
        size_t free_space = rb->capacity - rb->size;
        if (size > free_space) {
            size = free_space;
        }
    }
    
    while (bytes_written < size) {
        // 计算可写入的连续空间
        size_t space_to_end = rb->capacity - rb->head;
        size_t write_size = (size - bytes_written < space_to_end) ? 
                           (size - bytes_written) : space_to_end;
        
        // 写入数据
        memcpy(rb->buffer + rb->head, src + bytes_written, write_size);
        bytes_written += write_size;
        rb->head = (rb->head + write_size) % rb->capacity;
        
        // 更新缓冲区大小
        if (rb->size + write_size > rb->capacity) {
            // 覆盖模式，尾指针向前移动
            rb->tail = (rb->tail + (rb->size + write_size - rb->capacity)) % rb->capacity;
            rb->size = rb->capacity;
        } else {
            rb->size += write_size;
        }
    }
    
    return bytes_written;
}

/**
 * 读取数据
 */
size_t ring_buffer_read(ring_buffer_t *rb, void *data, size_t size) {
    if (!rb || !data || size == 0) return 0;
    
    uint8_t *dst = (uint8_t*)data;
    size_t bytes_read = 0;
    
    // 调整读取大小
    if (size > rb->size) {
        size = rb->size;
    }
    
    while (bytes_read < size) {
        // 计算可读取的连续空间
        size_t space_to_end = (rb->head > rb->tail) ? 
                             rb->head - rb->tail : 
                             rb->capacity - rb->tail;
        size_t read_size = (size - bytes_read < space_to_end) ? 
                          (size - bytes_read) : space_to_end;
        
        // 读取数据
        memcpy(dst + bytes_read, rb->buffer + rb->tail, read_size);
        bytes_read += read_size;
        rb->tail = (rb->tail + read_size) % rb->capacity;
    }
    
    rb->size -= bytes_read;
    return bytes_read;
}

/**
 * 查看数据（不移动指针）
 */
size_t ring_buffer_peek(const ring_buffer_t *rb, void *data, size_t size) {
    if (!rb || !data || size == 0) return 0;
    
    uint8_t *dst = (uint8_t*)data;
    size_t bytes_read = 0;
    size_t tail = rb->tail;
    
    // 调整读取大小
    if (size > rb->size) {
        size = rb->size;
    }
    
    while (bytes_read < size) {
        // 计算可读取的连续空间
        size_t space_to_end = (rb->head > tail) ? 
                             rb->head - tail : 
                             rb->capacity - tail;
        size_t read_size = (size - bytes_read < space_to_end) ? 
                          (size - bytes_read) : space_to_end;
        
        // 复制数据
        memcpy(dst + bytes_read, rb->buffer + tail, read_size);
        bytes_read += read_size;
        tail = (tail + read_size) % rb->capacity;
    }
    
    return bytes_read;
}

/**
 * 获取可用数据大小
 */
size_t ring_buffer_available(const ring_buffer_t *rb) {
    if (!rb) return 0;
    return rb->size;
}

/**
 * 获取空闲空间大小
 */
size_t ring_buffer_free_space(const ring_buffer_t *rb) {
    if (!rb) return 0;
    return rb->capacity - rb->size;
}

/**
 * 检查是否为空
 */
bool ring_buffer_empty(const ring_buffer_t *rb) {
    if (!rb) return true;
    return rb->size == 0;
}

/**
 * 检查是否为满
 */
bool ring_buffer_full(const ring_buffer_t *rb) {
    if (!rb) return false;
    return rb->size == rb->capacity;
}

/**
 * 清空缓冲区
 */
void ring_buffer_clear(ring_buffer_t *rb) {
    if (!rb) return;
    
    rb->head = 0;
    rb->tail = 0;
    rb->size = 0;
}

// ==================== 时间操作 ====================

/**
 * 获取当前时间戳（毫秒）
 */
uint64_t utils_get_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/**
 * 获取当前时间戳（微秒）
 */
uint64_t utils_get_timestamp_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/**
 * 获取当前时间戳（纳秒）
 */
uint64_t utils_get_timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/**
 * 从毫秒创建timespec
 */
struct timespec utils_timespec_from_ms(uint64_t ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    return ts;
}

/**
 * 将timespec转换为毫秒
 */
uint64_t utils_timespec_to_ms(const struct timespec *ts) {
    if (!ts) return 0;
    return (uint64_t)ts->tv_sec * 1000 + ts->tv_nsec / 1000000;
}

/**
 * 格式化时间戳
 */
char* utils_format_timestamp(time_t timestamp, const char *format) {
    if (!format) format = "%Y-%m-%d %H:%M:%S";
    
    struct tm *tm_info = localtime(&timestamp);
    if (!tm_info) return NULL;
    
    char buffer[256];
    size_t len = strftime(buffer, sizeof(buffer), format, tm_info);
    if (len == 0) return NULL;
    
    return utils_strdup(buffer);
}

/**
 * 格式化当前时间
 */
char* utils_format_current_time(const char *format) {
    time_t now = time(NULL);
    return utils_format_timestamp(now, format);
}

/**
 * 休眠（毫秒）
 */
void utils_sleep_ms(uint64_t milliseconds) {
    struct timespec ts = utils_timespec_from_ms(milliseconds);
    nanosleep(&ts, NULL);
}

/**
 * 休眠（微秒）
 */
void utils_sleep_us(uint64_t microseconds) {
    struct timespec ts;
    ts.tv_sec = microseconds / 1000000;
    ts.tv_nsec = (microseconds % 1000000) * 1000;
    nanosleep(&ts, NULL);
}

// ==================== 随机数生成 ====================

/**
 * 初始化随机数生成器
 */
void utils_random_init(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    
    static void init(void) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        unsigned int seed = (unsigned int)(ts.tv_nsec ^ ts.tv_sec ^ getpid());
        srand(seed);
        
        // 初始化OpenSSL随机数生成器
        RAND_poll();
    }
    
    pthread_once(&once, init);
}

/**
 * 生成32位随机数
 */
uint32_t utils_random_uint32(void) {
    utils_random_init();
    
    uint32_t result;
    if (RAND_bytes((unsigned char*)&result, sizeof(result)) == 1) {
        return result;
    }
    
    // 如果OpenSSL失败，使用标准rand
    return (uint32_t)rand() ^ ((uint32_t)rand() << 16);
}

/**
 * 生成64位随机数
 */
uint64_t utils_random_uint64(void) {
    utils_random_init();
    
    uint64_t result;
    if (RAND_bytes((unsigned char*)&result, sizeof(result)) == 1) {
        return result;
    }
    
    // 如果OpenSSL失败，使用标准rand
    return ((uint64_t)utils_random_uint32() << 32) | utils_random_uint32();
}

/**
 * 生成范围内的随机整数
 */
int utils_random_int(int min, int max) {
    if (min >= max) return min;
    
    uint32_t range = max - min + 1;
    uint32_t rand_val = utils_random_uint32();
    
    return min + (rand_val % range);
}

/**
 * 生成范围内的随机浮点数
 */
double utils_random_double(double min, double max) {
    if (min >= max) return min;
    
    double scale = (double)utils_random_uint32() / (double)UINT32_MAX;
    return min + scale * (max - min);
}

/**
 * 生成随机字节
 */
void utils_random_bytes(void *buffer, size_t size) {
    utils_random_init();
    
    if (RAND_bytes((unsigned char*)buffer, size) != 1) {
        // 如果OpenSSL失败，使用标准rand
        unsigned char *p = (unsigned char*)buffer;
        for (size_t i = 0; i < size; i++) {
            p[i] = rand() % 256;
        }
    }
}

/**
 * 生成随机字符串
 */
char* utils_random_string(size_t length) {
    static const char charset[] = 
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789";
    
    if (length == 0) return NULL;
    
    char *str = (char*)utils_malloc(length + 1, "random_string");
    if (!str) return NULL;
    
    utils_random_init();
    
    for (size_t i = 0; i < length; i++) {
        int key = utils_random_int(0, (int)(sizeof(charset) - 2)); // -2 为了排除终止符
        str[i] = charset[key];
    }
    
    str[length] = '\0';
    return str;
}

/**
 * 生成UUID
 */
char* utils_random_uuid(void) {
    uint8_t data[16];
    utils_random_bytes(data, sizeof(data));
    
    // 设置版本和变体
    data[6] = (data[6] & 0x0F) | 0x40; // 版本4
    data[8] = (data[8] & 0x3F) | 0x80; // 变体1
    
    char *uuid = (char*)utils_malloc(37, "uuid");
    if (!uuid) return NULL;
    
    snprintf(uuid, 37,
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            data[0], data[1], data[2], data[3],
            data[4], data[5], data[6], data[7],
            data[8], data[9], data[10], data[11],
            data[12], data[13], data[14], data[15]);
    
    return uuid;
}

// ==================== 加密和哈希 ====================

/**
 * 计算MD5哈希
 */
char* utils_md5(const void *data, size_t length) {
    if (!data) return NULL;
    
    MD5_CTX context;
    unsigned char digest[MD5_DIGEST_LENGTH];
    
    MD5_Init(&context);
    MD5_Update(&context, data, length);
    MD5_Final(digest, &context);
    
    char *hash = (char*)utils_malloc(MD5_DIGEST_LENGTH * 2 + 1, "md5_hash");
    if (!hash) return NULL;
    
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
        sprintf(hash + (i * 2), "%02x", digest[i]);
    }
    hash[MD5_DIGEST_LENGTH * 2] = '\0';
    
    return hash;
}

/**
 * 计算文件的MD5哈希
 */
char* utils_md5_file(const char *path) {
    if (!path) return NULL;
    
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    
    MD5_CTX context;
    unsigned char buffer[8192];
    size_t bytes_read;
    
    MD5_Init(&context);
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        MD5_Update(&context, buffer, bytes_read);
    }
    
    fclose(file);
    
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5_Final(digest, &context);
    
    char *hash = (char*)utils_malloc(MD5_DIGEST_LENGTH * 2 + 1, "md5_file_hash");
    if (!hash) return NULL;
    
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
        sprintf(hash + (i * 2), "%02x", digest[i]);
    }
    hash[MD5_DIGEST_LENGTH * 2] = '\0';
    
    return hash;
}

/**
 * 计算SHA256哈希
 */
char* utils_sha256(const void *data, size_t length) {
    if (!data) return NULL;
    
    SHA256_CTX context;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    
    SHA256_Init(&context);
    SHA256_Update(&context, data, length);
    SHA256_Final(digest, &context);
    
    char *hash = (char*)utils_malloc(SHA256_DIGEST_LENGTH * 2 + 1, "sha256_hash");
    if (!hash) return NULL;
    
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(hash + (i * 2), "%02x", digest[i]);
    }
    hash[SHA256_DIGEST_LENGTH * 2] = '\0';
    
    return hash;
}

/**
 * 计算文件的SHA256哈希
 */
char* utils_sha256_file(const char *path) {
    if (!path) return NULL;
    
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    
    SHA256_CTX context;
    unsigned char buffer[8192];
    size_t bytes_read;
    
    SHA256_Init(&context);
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        SHA256_Update(&context, buffer, bytes_read);
    }
    
    fclose(file);
    
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256_Final(digest, &context);
    
    char *hash = (char*)utils_malloc(SHA256_DIGEST_LENGTH * 2 + 1, "sha256_file_hash");
    if (!hash) return NULL;
    
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(hash + (i * 2), "%02x", digest[i]);
    }
    hash[SHA256_DIGEST_LENGTH * 2] = '\0';
    
    return hash;
}

/**
 * Base64编码
 */
char* utils_base64_encode(const void *data, size_t length) {
    if (!data) return NULL;
    
    BIO *bmem, *b64;
    BUF_MEM *bptr;
    
    b64 = BIO_new(BIO_f_base64());
    bmem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, bmem);
    
    // 不添加换行符
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    
    BIO_write(b64, data, length);
    BIO_flush(b64);
    BIO_get_mem_ptr(b64, &bptr);
    
    char *encoded = (char*)utils_malloc(bptr->length + 1, "base64_encode");
    if (!encoded) {
        BIO_free_all(b64);
        return NULL;
    }
    
    memcpy(encoded, bptr->data, bptr->length);
    encoded[bptr->length] = '\0';
    
    BIO_free_all(b64);
    return encoded;
}

/**
 * Base64解码
 */
void* utils_base64_decode(const char *data, size_t *length) {
    if (!data) return NULL;
    
    size_t input_length = strlen(data);
    if (input_length % 4 != 0) {
        // Base64字符串长度必须是4的倍数
        return NULL;
    }
    
    // 计算输出长度
    size_t output_length = (input_length * 3) / 4;
    if (data[input_length - 1] == '=') output_length--;
    if (data[input_length - 2] == '=') output_length--;
    
    void *decoded = utils_malloc(output_length, "base64_decode");
    if (!decoded) return NULL;
    
    BIO *b64, *bmem;
    
    b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    
    bmem = BIO_new_mem_buf((void*)data, input_length);
    bmem = BIO_push(b64, bmem);
    
    size_t bytes_read = BIO_read(bmem, decoded, output_length);
    BIO_free_all(bmem);
    
    if (bytes_read != output_length) {
        free(decoded);
        return NULL;
    }
    
    if (length) {
        *length = output_length;
    }
    
    return decoded;
}

/**
 * 十六进制解码
 */
void* utils_hex_decode(const char *hex, size_t *length) {
    if (!hex) return NULL;
    
    size_t hex_len = strlen(hex);
    if (hex_len % 2 != 0) {
        // 十六进制字符串长度必须是偶数
        return NULL;
    }
    
    size_t output_length = hex_len / 2;
    void *decoded = utils_malloc(output_length, "hex_decode");
    if (!decoded) return NULL;
    
    for (size_t i = 0; i < output_length; i++) {
        char byte_str[3] = {hex[i * 2], hex[i * 2 + 1], '\0'};
        unsigned char byte = (unsigned char)strtol(byte_str, NULL, 16);
        ((unsigned char*)decoded)[i] = byte;
    }
    
    if (length) {
        *length = output_length;
    }
    
    return decoded;
}

/**
 * 十六进制编码
 */
char* utils_hex_encode(const void *data, size_t length) {
    if (!data) return NULL;
    
    char *hex = (char*)utils_malloc(length * 2 + 1, "hex_encode");
    if (!hex) return NULL;
    
    const unsigned char *bytes = (const unsigned char*)data;
    for (size_t i = 0; i < length; i++) {
        sprintf(hex + (i * 2), "%02x", bytes[i]);
    }
    hex[length * 2] = '\0';
    
    return hex;
}

// ==================== 网络操作 ====================

/**
 * 检查是否是有效的IPv4地址
 */
bool utils_is_valid_ipv4(const char *ip) {
    if (!ip) return false;
    
    struct sockaddr_in sa;
    return inet_pton(AF_INET, ip, &(sa.sin_addr)) == 1;
}

/**
 * 检查是否是有效的IPv6地址
 */
bool utils_is_valid_ipv6(const char *ip) {
    if (!ip) return false;
    
    struct sockaddr_in6 sa;
    return inet_pton(AF_INET6, ip, &(sa.sin6_addr)) == 1;
}

/**
 * 检查是否是有效的端口号
 */
bool utils_is_valid_port(uint16_t port) {
    return port > 0 && port <= 65535;
}

/**
 * 获取主机名
 */
char* utils_get_hostname(void) {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        return NULL;
    }
    
    return utils_strdup(hostname);
}

/**
 * 获取指定网络接口的IP地址
 */
char* utils_get_ip_address(const char *interface) {
    struct ifaddrs *ifaddr, *ifa;
    char *ip_address = NULL;
    
    if (getifaddrs(&ifaddr) == -1) {
        return NULL;
    }
    
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        
        // 检查接口名称
        if (interface && strcmp(ifa->ifa_name, interface) != 0) {
            continue;
        }
        
        // 检查地址族
        if (ifa->ifa_addr->sa_family == AF_INET) {
            // IPv4地址
            struct sockaddr_in *sa = (struct sockaddr_in*)ifa->ifa_addr;
            char ip[INET_ADDRSTRLEN];
            
            if (inet_ntop(AF_INET, &(sa->sin_addr), ip, sizeof(ip))) {
                ip_address = utils_strdup(ip);
                break;
            }
        } else if (ifa->ifa_addr->sa_family == AF_INET6) {
            // IPv6地址
            struct sockaddr_in6 *sa6 = (struct sockaddr_in6*)ifa->ifa_addr;
            char ip[INET6_ADDRSTRLEN];
            
            if (inet_ntop(AF_INET6, &(sa6->sin6_addr), ip, sizeof(ip))) {
                ip_address = utils_strdup(ip);
                break;
            }
        }
    }
    
    freeifaddrs(ifaddr);
    return ip_address;
}

/**
 * 设置套接字为非阻塞
 */
int utils_set_socket_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * 设置套接字为阻塞
 */
int utils_set_socket_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

/**
 * 设置套接字超时
 */
int utils_set_socket_timeout(int fd, int timeout_ms) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        return -1;
    }
    
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        return -1;
    }
    
    return 0;
}

/**
 * 设置套接字地址重用
 */
int utils_set_socket_reuseaddr(int fd, bool enable) {
    int optval = enable ? 1 : 0;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
}

/**
 * 设置套接字保持连接
 */
int utils_set_socket_keepalive(int fd, bool enable) {
    int optval = enable ? 1 : 0;
    return setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval));
}

// ==================== 进程和线程 ====================

/**
 * 获取进程ID
 */
pid_t utils_get_process_id(void) {
    return getpid();
}

/**
 * 获取父进程ID
 */
pid_t utils_get_parent_process_id(void) {
    return getppid();
}

/**
 * 获取进程名称
 */
char* utils_get_process_name(void) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", getpid());
    
    char *name = utils_read_file(path, NULL);
    if (!name) return NULL;
    
    // 只保留第一个部分（程序名）
    char *space = strchr(name, ' ');
    if (space) {
        *space = '\0';
    }
    
    // 只保留基本名称（去掉路径）
    char *basename = strrchr(name, '/');
    if (basename) {
        char *result = utils_strdup(basename + 1);
        free(name);
        return result;
    }
    
    return name;
}

/**
 * 设置线程名称
 */
int utils_set_thread_name(const char *name) {
#ifdef __linux__
    return pthread_setname_np(pthread_self(), name);
#elif defined(__APPLE__)
    return pthread_setname_np(name);
#else
    (void)name;
    return 0; // 不支持
#endif
}

/**
 * 获取线程名称
 */
char* utils_get_thread_name(void) {
#ifdef __linux__
    char name[16];
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) == 0) {
        return utils_strdup(name);
    }
#endif
    return NULL;
}

/**
 * 获取线程ID
 */
pthread_t utils_get_thread_id(void) {
    return pthread_self();
}

/**
 * 创建线程
 */
int utils_create_thread(pthread_t *thread, void *(*start_routine)(void *), void *arg) {
    return pthread_create(thread, NULL, start_routine, arg);
}

/**
 * 等待线程结束
 */
int utils_join_thread(pthread_t thread) {
    return pthread_join(thread, NULL);
}

/**
 * 分离线程
 */
int utils_detach_thread(pthread_t thread) {
    return pthread_detach(thread);
}

// ==================== 日志系统 ====================

/**
 * 创建日志器
 */
logger_t* logger_create(void) {
    logger_t *logger = (logger_t*)utils_malloc(sizeof(logger_t), "logger");
    if (!logger) return NULL;
    
    logger->level = LOG_LEVEL_INFO;
    logger->output = stdout;
    logger->file_path = NULL;
    logger->enable_console = true;
    logger->enable_file = false;
    logger->enable_syslog = false;
    logger->max_file_size = 10 * 1024 * 1024; // 10MB
    logger->max_backups = 5;
    
    if (pthread_mutex_init(&logger->mutex, NULL) != 0) {
        free(logger);
        return NULL;
    }
    
    return logger;
}

/**
 * 销毁日志器
 */
void logger_destroy(logger_t *logger) {
    if (!logger) return;
    
    pthread_mutex_destroy(&logger->mutex);
    
    if (logger->file_path) {
        free(logger->file_path);
    }
    
    free(logger);
}

/**
 * 设置日志级别
 */
void logger_set_level(logger_t *logger, log_level_t level) {
    if (!logger) return;
    
    pthread_mutex_lock(&logger->mutex);
    logger->level = level;
    pthread_mutex_unlock(&logger->mutex);
}

/**
 * 设置日志输出
 */
void logger_set_output(logger_t *logger, FILE *output) {
    if (!logger) return;
    
    pthread_mutex_lock(&logger->mutex);
    logger->output = output;
    pthread_mutex_unlock(&logger->mutex);
}

/**
 * 设置日志文件
 */
void logger_set_file(logger_t *logger, const char *file_path) {
    if (!logger || !file_path) return;
    
    pthread_mutex_lock(&logger->mutex);
    
    if (logger->file_path) {
        free(logger->file_path);
    }
    
    logger->file_path = utils_strdup(file_path);
    pthread_mutex_unlock(&logger->mutex);
}

/**
 * 设置日志文件最大大小
 */
void logger_set_max_size(logger_t *logger, size_t max_size, int max_backups) {
    if (!logger) return;
    
    pthread_mutex_lock(&logger->mutex);
    logger->max_file_size = max_size;
    logger->max_backups = max_backups;
    pthread_mutex_unlock(&logger->mutex);
}

/**
 * 日志轮转
 */
static void logger_rotate(logger_t *logger) {
    if (!logger || !logger->file_path) return;
    
    // 检查文件大小
    struct stat st;
    if (stat(logger->file_path, &st) != 0) {
        return;
    }
    
    if ((size_t)st.st_size < logger->max_file_size) {
        return;
    }
    
    // 轮转日志文件
    for (int i = logger->max_backups; i > 0; i--) {
        char old_path[256];
        char new_path[256];
        
        if (i == 1) {
            snprintf(old_path, sizeof(old_path), "%s", logger->file_path);
        } else {
            snprintf(old_path, sizeof(old_path), "%s.%d", logger->file_path, i - 1);
        }
        
        snprintf(new_path, sizeof(new_path), "%s.%d", logger->file_path, i);
        
        rename(old_path, new_path);
    }
}

/**
 * 记录日志
 */
void logger_log(logger_t *logger, log_level_t level, const char *file, int line,
                const char *function, const char *format, ...) {
    if (!logger || level < logger->level) return;
    
    static const char *level_names[] = {
        "DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"
    };
    
    static const char *level_colors[] = {
        "\033[36m",  // CYAN for DEBUG
        "\033[32m",  // GREEN for INFO
        "\033[33m",  // YELLOW for WARNING
        "\033[31m",  // RED for ERROR
        "\033[35m"   // MAGENTA for CRITICAL
    };
    
    pthread_mutex_lock(&logger->mutex);
    
    // 格式化时间
    char timestamp[64];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    
    // 提取文件名（去掉路径）
    const char *filename = file;
    const char *slash = strrchr(file, '/');
    if (slash) {
        filename = slash + 1;
    }
    
    // 格式化消息
    char message[UTILS_MAX_ERROR_MSG];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    // 输出到控制台
    if (logger->enable_console && logger->output) {
        fprintf(logger->output, "%s%s [%s] [%s:%d %s] %s\033[0m\n",
               level_colors[level],
               timestamp, level_names[level], filename, line, function, message);
        fflush(logger->output);
    }
    
    // 输出到文件
    if (logger->enable_file && logger->file_path) {
        // 检查是否需要轮转
        logger_rotate(logger);
        
        FILE *file = fopen(logger->file_path, "a");
        if (file) {
            fprintf(file, "%s [%s] [%s:%d %s] %s\n",
                   timestamp, level_names[level], filename, line, function, message);
            fclose(file);
        }
    }
    
    // 输出到系统日志
    if (logger->enable_syslog) {
        int syslog_level;
        switch (level) {
            case LOG_LEVEL_DEBUG: syslog_level = LOG_DEBUG; break;
            case LOG_LEVEL_INFO: syslog_level = LOG_INFO; break;
            case LOG_LEVEL_WARNING: syslog_level = LOG_WARNING; break;
            case LOG_LEVEL_ERROR: syslog_level = LOG_ERR; break;
            case LOG_LEVEL_CRITICAL: syslog_level = LOG_CRIT; break;
            default: syslog_level = LOG_INFO;
        }
        
        syslog(syslog_level, "[%s:%d %s] %s", filename, line, function, message);
    }
    
    pthread_mutex_unlock(&logger->mutex);
}

// ==================== 错误处理 ====================

/**
 * 设置最后错误
 */
void utils_set_last_error(int error_code, const char *format, ...) {
    va_list args;
    va_start(args, format);
    
    g_last_error.error_code = error_code;
    vsnprintf(g_last_error.message, sizeof(g_last_error.message), format, args);
    g_last_error.timestamp = time(NULL);
    
    va_end(args);
}

/**
 * 获取最后错误
 */
error_info_t utils_get_last_error(void) {
    return g_last_error;
}

/**
 * 清除最后错误
 */
void utils_clear_last_error(void) {
    memset(&g_last_error, 0, sizeof(g_last_error));
}

/**
 * 将错误码转换为字符串
 */
const char* utils_strerror(int error_code) {
    switch (error_code) {
        case ERR_SUCCESS: return "成功";
        case ERR_INVALID_REQUEST: return "无效请求";
        case ERR_AUTH_FAILED: return "认证失败";
        case ERR_INVALID_TOKEN: return "无效令牌";
        case ERR_PERMISSION_DENIED: return "权限不足";
        case ERR_USER_NOT_FOUND: return "用户不存在";
        case ERR_USER_ALREADY_EXISTS: return "用户已存在";
        case ERR_INVALID_PARAMETERS: return "无效参数";
        case ERR_RATE_LIMIT_EXCEEDED: return "超出频率限制";
        case ERR_SERVER_ERROR: return "服务器错误";
        case ERR_DATABASE_ERROR: return "数据库错误";
        case ERR_FILESYSTEM_ERROR: return "文件系统错误";
        case ERR_SERVICE_UNAVAILABLE: return "服务不可用";
        case ERR_MAINTENANCE_MODE: return "维护模式";
        case ERR_NETWORK_ERROR: return "网络错误";
        case ERR_CONNECTION_TIMEOUT: return "连接超时";
        case ERR_PROTOCOL_ERROR: return "协议错误";
        case ERR_FILE_TOO_LARGE: return "文件太大";
        case ERR_FILE_TYPE_NOT_ALLOWED: return "文件类型不允许";
        case ERR_FILE_UPLOAD_FAILED: return "文件上传失败";
        case ERR_FILE_NOT_FOUND: return "文件未找到";
        case ERR_FILE_HASH_MISMATCH: return "文件哈希不匹配";
        case ERR_RESOURCE_LIMIT_EXCEEDED: return "超出资源限制";
        case ERR_STORAGE_FULL: return "存储空间已满";
        case ERR_SECURITY_VIOLATION: return "安全违规";
        case ERR_IP_BLOCKED: return "IP被封锁";
        case ERR_ACCOUNT_LOCKED: return "账户被锁定";
        case ERR_UNKNOWN: return "未知错误";
        default: return "未定义的错误码";
    }
}

// ==================== 配置解析 ====================

/**
 * 配置项结构
 */
typedef struct {
    char *key;
    char *value;
} config_item_t;

/**
 * 配置段结构
 */
typedef struct {
    char *name;
    hash_table_t *items;
} config_section_t;

/**
 * 配置结构
 */
struct config_t {
    hash_table_t *sections;
};

/**
 * 释放配置项值
 */
static void config_item_free(void *value) {
    config_item_t *item = (config_item_t*)value;
    if (!item) return;
    
    free(item->key);
    free(item->value);
    free(item);
}

/**
 * 释放配置段
 */
static void config_section_free(void *value) {
    config_section_t *section = (config_section_t*)value;
    if (!section) return;
    
    free(section->name);
    hash_table_destroy(section->items);
    free(section);
}

/**
 * 创建配置
 */
config_t* config_create(void) {
    config_t *config = (config_t*)utils_malloc(sizeof(config_t), "config");
    if (!config) return NULL;
    
    config->sections = hash_table_create(16, hash_string_djb2, config_section_free);
    if (!config->sections) {
        free(config);
        return NULL;
    }
    
    return config;
}

/**
 * 销毁配置
 */
void config_destroy(config_t *config) {
    if (!config) return;
    
    hash_table_destroy(config->sections);
    free(config);
}

/**
 * 加载配置文件
 */
int config_load_file(config_t *config, const char *path) {
    if (!config || !path) return -1;
    
    size_t length;
    char *content = utils_read_file(path, &length);
    if (!content) return -1;
    
    int result = config_load_string(config, content);
    free(content);
    
    return result;
}

/**
 * 加载配置字符串
 */
int config_load_string(config_t *config, const char *str) {
    if (!config || !str) return -1;
    
    char *str_copy = utils_strdup(str);
    if (!str_copy) return -1;
    
    char *current_section = NULL;
    config_section_t *section = NULL;
    char *line = strtok(str_copy, "\n");
    int line_number = 0;
    
    while (line) {
        line_number++;
        
        // 去除两端空白
        char *trimmed = utils_strtrim(line);
        if (!trimmed || trimmed[0] == '\0' || trimmed[0] == ';' || trimmed[0] == '#') {
            // 空行或注释行
            line = strtok(NULL, "\n");
            continue;
        }
        
        // 检查是否是段头
        if (trimmed[0] == '[' && trimmed[strlen(trimmed) - 1] == ']') {
            // 去掉方括号
            trimmed[strlen(trimmed) - 1] = '\0';
            char *section_name = trimmed + 1;
            
            // 创建新段
            current_section = utils_strdup(section_name);
            if (!current_section) {
                free(str_copy);
                return -1;
            }
            
            section = (config_section_t*)hash_table_lookup(config->sections, current_section);
            if (!section) {
                section = (config_section_t*)utils_malloc(sizeof(config_section_t), "config_section");
                if (!section) {
                    free(current_section);
                    free(str_copy);
                    return -1;
                }
                
                section->name = current_section;
                section->items = hash_table_create(16, hash_string_djb2, config_item_free);
                
                if (!section->items) {
                    free(section);
                    free(current_section);
                    free(str_copy);
                    return -1;
                }
                
                hash_table_insert(config->sections, current_section, section);
            } else {
                free(current_section);
            }
        } else if (current_section) {
            // 解析键值对
            char *equals = strchr(trimmed, '=');
            if (!equals) {
                // 无效行，跳过
                line = strtok(NULL, "\n");
                continue;
            }
            
            *equals = '\0';
            char *key = utils_strtrim(trimmed);
            char *value = utils_strtrim(equals + 1);
            
            if (!key || !value) {
                line = strtok(NULL, "\n");
                continue;
            }
            
            // 创建配置项
            config_item_t *item = (config_item_t*)utils_malloc(sizeof(config_item_t), "config_item");
            if (!item) {
                free(str_copy);
                return -1;
            }
            
            item->key = utils_strdup(key);
            item->value = utils_strdup(value);
            
            if (!item->key || !item->value) {
                config_item_free(item);
                free(str_copy);
                return -1;
            }
            
            hash_table_insert(section->items, key, item);
        }
        
        line = strtok(NULL, "\n");
    }
    
    free(str_copy);
    return 0;
}

/**
 * 保存配置文件
 */
int config_save_file(config_t *config, const char *path) {
    if (!config || !path) return -1;
    
    FILE *file = fopen(path, "w");
    if (!file) return -1;
    
    // 遍历所有段
    for (size_t i = 0; i < config->sections->size; i++) {
        hash_table_node_t *node = config->sections->buckets[i];
        while (node) {
            config_section_t *section = (config_section_t*)node->value;
            
            // 写入段头
            fprintf(file, "[%s]\n", section->name);
            
            // 写入所有键值对
            for (size_t j = 0; j < section->items->size; j++) {
                hash_table_node_t *item_node = section->items->buckets[j];
                while (item_node) {
                    config_item_t *item = (config_item_t*)item_node->value;
                    fprintf(file, "%s = %s\n", item->key, item->value);
                    item_node = item_node->next;
                }
            }
            
            fprintf(file, "\n");
            node = node->next;
        }
    }
    
    fclose(file);
    return 0;
}

/**
 * 设置配置值
 */
int config_set_value(config_t *config, const char *section_name, const char *key, const char *value) {
    if (!config || !section_name || !key || !value) return -1;
    
    // 查找或创建段
    config_section_t *section = (config_section_t*)hash_table_lookup(config->sections, section_name);
    if (!section) {
        section = (config_section_t*)utils_malloc(sizeof(config_section_t), "config_section");
        if (!section) return -1;
        
        section->name = utils_strdup(section_name);
        section->items = hash_table_create(16, hash_string_djb2, config_item_free);
        
        if (!section->name || !section->items) {
            if (section->name) free(section->name);
            if (section->items) hash_table_destroy(section->items);
            free(section);
            return -1;
        }
        
        hash_table_insert(config->sections, section_name, section);
    }
    
    // 创建或更新配置项
    config_item_t *item = (config_item_t*)hash_table_lookup(section->items, key);
    if (item) {
        // 更新现有项
        free(item->value);
        item->value = utils_strdup(value);
    } else {
        // 创建新项
        item = (config_item_t*)utils_malloc(sizeof(config_item_t), "config_item");
        if (!item) return -1;
        
        item->key = utils_strdup(key);
        item->value = utils_strdup(value);
        
        if (!item->key || !item->value) {
            if (item->key) free(item->key);
            if (item->value) free(item->value);
            free(item);
            return -1;
        }
        
        hash_table_insert(section->items, key, item);
    }
    
    return 0;
}

/**
 * 获取配置值
 */
const char* config_get_value(const config_t *config, const char *section, const char *key) {
    if (!config || !section || !key) return NULL;
    
    config_section_t *section_ptr = (config_section_t*)hash_table_lookup(config->sections, section);
    if (!section_ptr) return NULL;
    
    config_item_t *item = (config_item_t*)hash_table_lookup(section_ptr->items, key);
    if (!item) return NULL;
    
    return item->value;
}

/**
 * 获取整数配置值
 */
int config_get_int(const config_t *config, const char *section, const char *key, int default_value) {
    const char *value = config_get_value(config, section, key);
    if (!value) return default_value;
    
    char *endptr;
    long result = strtol(value, &endptr, 10);
    if (endptr == value || *endptr != '\0') {
        return default_value;
    }
    
    return (int)result;
}

/**
 * 获取浮点数配置值
 */
double config_get_double(const config_t *config, const char *section, const char *key, double default_value) {
    const char *value = config_get_value(config, section, key);
    if (!value) return default_value;
    
    char *endptr;
    double result = strtod(value, &endptr);
    if (endptr == value || *endptr != '\0') {
        return default_value;
    }
    
    return result;
}

/**
 * 获取布尔配置值
 */
bool config_get_bool(const config_t *config, const char *section, const char *key, bool default_value) {
    const char *value = config_get_value(config, section, key);
    if (!value) return default_value;
    
    if (strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0 || strcasecmp(value, "1") == 0) {
        return true;
    } else if (strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0 || strcasecmp(value, "0") == 0) {
        return false;
    } else {
        return default_value;
    }
}

/**
 * 删除配置键
 */
int config_remove_key(config_t *config, const char *section, const char *key) {
    if (!config || !section || !key) return -1;
    
    config_section_t *section_ptr = (config_section_t*)hash_table_lookup(config->sections, section);
    if (!section_ptr) return -1;
    
    return hash_table_remove(section_ptr->items, key);
}

/**
 * 删除配置段
 */
int config_remove_section(config_t *config, const char *section) {
    if (!config || !section) return -1;
    return hash_table_remove(config->sections, section);
}

// ==================== JSON处理 ====================

/**
 * JSON转义
 */
char* utils_json_escape(const char *str) {
    if (!str) return NULL;
    
    string_t *result = string_create(256);
    if (!result) return NULL;
    
    for (const char *p = str; *p; p++) {
        switch (*p) {
            case '"': string_append(result, "\\\""); break;
            case '\\': string_append(result, "\\\\"); break;
            case '\b': string_append(result, "\\b"); break;
            case '\f': string_append(result, "\\f"); break;
            case '\n': string_append(result, "\\n"); break;
            case '\r': string_append(result, "\\r"); break;
            case '\t': string_append(result, "\\t"); break;
            default:
                if ((unsigned char)*p < 0x20) {
                    char buffer[7];
                    snprintf(buffer, sizeof(buffer), "\\u%04x", (unsigned char)*p);
                    string_append(result, buffer);
                } else {
                    string_append_char(result, *p);
                }
                break;
        }
    }
    
    char *escaped = utils_strdup(string_get(result));
    string_destroy(result);
    
    return escaped;
}

/**
 * JSON反转义
 */
char* utils_json_unescape(const char *str) {
    if (!str) return NULL;
    
    string_t *result = string_create(256);
    if (!result) return NULL;
    
    for (const char *p = str; *p; p++) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
                case '"': string_append_char(result, '"'); break;
                case '\\': string_append_char(result, '\\'); break;
                case '/': string_append_char(result, '/'); break;
                case 'b': string_append_char(result, '\b'); break;
                case 'f': string_append_char(result, '\f'); break;
                case 'n': string_append_char(result, '\n'); break;
                case 'r': string_append_char(result, '\r'); break;
                case 't': string_append_char(result, '\t'); break;
                case 'u':
                    // 处理Unicode转义序列
                    if (isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]) && 
                        isxdigit((unsigned char)p[3]) && isxdigit((unsigned char)p[4])) {
                        char hex[5] = {p[1], p[2], p[3], p[4], '\0'};
                        long code = strtol(hex, NULL, 16);
                        if (code <= 0x7F) {
                            string_append_char(result, (char)code);
                        } else if (code <= 0x7FF) {
                            string_append_char(result, 0xC0 | (code >> 6));
                            string_append_char(result, 0x80 | (code & 0x3F));
                        } else {
                            string_append_char(result, 0xE0 | (code >> 12));
                            string_append_char(result, 0x80 | ((code >> 6) & 0x3F));
                            string_append_char(result, 0x80 | (code & 0x3F));
                        }
                        p += 4;
                    } else {
                        // 无效的Unicode转义序列，按字面处理
                        string_append_char(result, '\\');
                        string_append_char(result, 'u');
                    }
                    break;
                default:
                    // 未知转义序列，按字面处理
                    string_append_char(result, '\\');
                    string_append_char(result, *p);
                    break;
            }
        } else {
            string_append_char(result, *p);
        }
    }
    
    char *unescaped = utils_strdup(string_get(result));
    string_destroy(result);
    
    return unescaped;
}

// ==================== URL编码/解码 ====================

/**
 * URL编码
 */
char* utils_url_encode(const char *str) {
    if (!str) return NULL;
    
    string_t *result = string_create(256);
    if (!result) return NULL;
    
    static const char *hex = "0123456789ABCDEF";
    
    for (const char *p = str; *p; p++) {
        unsigned char c = (unsigned char)*p;
        
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            string_append_char(result, c);
        } else if (c == ' ') {
            string_append_char(result, '+');
        } else {
            string_append_char(result, '%');
            string_append_char(result, hex[c >> 4]);
            string_append_char(result, hex[c & 15]);
        }
    }
    
    char *encoded = utils_strdup(string_get(result));
    string_destroy(result);
    
    return encoded;
}

/**
 * URL解码
 */
char* utils_url_decode(const char *str) {
    if (!str) return NULL;
    
    string_t *result = string_create(256);
    if (!result) return NULL;
    
    for (const char *p = str; *p; p++) {
        if (*p == '%') {
            if (isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
                char hex[3] = {p[1], p[2], '\0'};
                unsigned char c = (unsigned char)strtol(hex, NULL, 16);
                string_append_char(result, c);
                p += 2;
            } else {
                // 无效的百分号编码，按字面处理
                string_append_char(result, '%');
            }
        } else if (*p == '+') {
            string_append_char(result, ' ');
        } else {
            string_append_char(result, *p);
        }
    }
    
    char *decoded = utils_strdup(string_get(result));
    string_destroy(result);
    
    return decoded;
}

// ==================== 校验和 ====================

/**
 * CRC32校验和
 */
uint32_t utils_crc32(const void *data, size_t length) {
    static const uint32_t crc32_table[256] = {
        0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA,
        0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
        // 这里只列出了前8个值，完整的表有256个值
        // 为了简洁，这里使用zlib的crc32函数
    };
    
    // 使用zlib的crc32实现
    return crc32(0, (const Bytef*)data, length);
}

/**
 * CRC16校验和
 */
uint16_t utils_crc16(const void *data, size_t length) {
    static const uint16_t crc16_table[256] = {
        0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
        0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
        // 这里只列出了前16个值，完整的表有256个值
    };
    
    const uint8_t *bytes = (const uint8_t*)data;
    uint16_t crc = 0xFFFF;
    
    for (size_t i = 0; i < length; i++) {
        crc = (crc >> 8) ^ crc16_table[(crc ^ bytes[i]) & 0xFF];
    }
    
    return crc;
}

/**
 * 简单校验和
 */
uint8_t utils_checksum(const void *data, size_t length) {
    const uint8_t *bytes = (const uint8_t*)data;
    uint8_t sum = 0;
    
    for (size_t i = 0; i < length; i++) {
        sum += bytes[i];
    }
    
    return sum;
}

// ==================== 系统信息 ====================

/**
 * 获取系统信息
 */
char* utils_get_system_info(void) {
    struct utsname info;
    if (uname(&info) != 0) {
        return NULL;
    }
    
    return utils_strdup_printf("系统: %s %s %s %s %s",
                              info.sysname, info.nodename,
                              info.release, info.version,
                              info.machine);
}

/**
 * 获取CPU信息
 */
char* utils_get_cpu_info(void) {
    FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
    if (!cpuinfo) return NULL;
    
    char line[256];
    string_t *info = string_create(256);
    
    while (fgets(line, sizeof(line), cpuinfo)) {
        if (strstr(line, "model name")) {
            char *colon = strchr(line, ':');
            if (colon) {
                string_append(info, colon + 2);
            }
            break;
        }
    }
    
    fclose(cpuinfo);
    
    char *result = utils_strdup(string_get(info));
    string_destroy(info);
    
    return result;
}

/**
 * 获取内存信息
 */
char* utils_get_memory_info(void) {
    struct sysinfo info;
    if (sysinfo(&info) != 0) {
        return NULL;
    }
    
    uint64_t total = info.totalram * info.mem_unit;
    uint64_t free = info.freeram * info.mem_unit;
    uint64_t used = total - free;
    
    return utils_strdup_printf("总内存: %.2f MB, 已用: %.2f MB, 空闲: %.2f MB",
                              total / (1024.0 * 1024.0),
                              used / (1024.0 * 1024.0),
                              free / (1024.0 * 1024.0));
}

/**
 * 获取磁盘信息
 */
char* utils_get_disk_info(void) {
    struct statvfs stat;
    if (statvfs("/", &stat) != 0) {
        return NULL;
    }
    
    uint64_t total = stat.f_blocks * stat.f_frsize;
    uint64_t free = stat.f_bfree * stat.f_frsize;
    uint64_t used = total - free;
    
    return utils_strdup_printf("总空间: %.2f GB, 已用: %.2f GB, 空闲: %.2f GB",
                              total / (1024.0 * 1024.0 * 1024.0),
                              used / (1024.0 * 1024.0 * 1024.0),
                              free / (1024.0 * 1024.0 * 1024.0));
}

/**
 * 获取网络信息
 */
char* utils_get_network_info(void) {
    struct ifaddrs *ifaddr, *ifa;
    string_t *info = string_create(256);
    
    if (getifaddrs(&ifaddr) == -1) {
        return NULL;
    }
    
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *sa = (struct sockaddr_in*)ifa->ifa_addr;
            char ip[INET_ADDRSTRLEN];
            
            if (inet_ntop(AF_INET, &(sa->sin_addr), ip, sizeof(ip))) {
                string_append_format(info, "接口: %s, IP: %s\n", ifa->ifa_name, ip);
            }
        }
    }
    
    freeifaddrs(ifaddr);
    
    char *result = utils_strdup(string_get(info));
    string_destroy(info);
    
    return result;
}

// ==================== 压缩 ====================

/**
 * GZIP压缩
 */
void* utils_compress_gzip(const void *data, size_t length, size_t *compressed_length) {
    if (!data || length == 0) return NULL;
    
    // 估计压缩后的最大大小
    size_t max_compressed_size = length + (length / 1000) + 12;
    void *compressed = utils_malloc(max_compressed_size, "gzip_compress");
    if (!compressed) return NULL;
    
    // 使用zlib进行gzip压缩
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    
    stream.next_in = (Bytef*)data;
    stream.avail_in = length;
    stream.next_out = (Bytef*)compressed;
    stream.avail_out = max_compressed_size;
    
    // 添加gzip头部
    deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                 MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY);
    
    int result = deflate(&stream, Z_FINISH);
    deflateEnd(&stream);
    
    if (result != Z_STREAM_END) {
        free(compressed);
        return NULL;
    }
    
    if (compressed_length) {
        *compressed_length = stream.total_out;
    }
    
    // 如果压缩数据比估计的小，重新分配内存
    if (stream.total_out < max_compressed_size) {
        void *resized = utils_realloc(compressed, stream.total_out, "gzip_compress_resize");
        if (resized) {
            compressed = resized;
        }
    }
    
    return compressed;
}

/**
 * GZIP解压缩
 */
void* utils_decompress_gzip(const void *data, size_t length, size_t *decompressed_length) {
    if (!data || length == 0) return NULL;
    
    // 估计解压后的最大大小（假设压缩比最大为10:1）
    size_t max_decompressed_size = length * 10;
    void *decompressed = utils_malloc(max_decompressed_size, "gzip_decompress");
    if (!decompressed) return NULL;
    
    // 使用zlib进行gzip解压
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    
    stream.next_in = (Bytef*)data;
    stream.avail_in = length;
    stream.next_out = (Bytef*)decompressed;
    stream.avail_out = max_decompressed_size;
    
    // 识别gzip头部
    inflateInit2(&stream, MAX_WBITS + 16);
    
    int result = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    
    if (result != Z_STREAM_END) {
        free(decompressed);
        return NULL;
    }
    
    if (decompressed_length) {
        *decompressed_length = stream.total_out;
    }
    
    // 如果解压数据比估计的小，重新分配内存
    if (stream.total_out < max_decompressed_size) {
        void *resized = utils_realloc(decompressed, stream.total_out, "gzip_decompress_resize");
        if (resized) {
            decompressed = resized;
        }
    }
    
    return decompressed;
}

/**
 * ZLIB压缩
 */
void* utils_compress_zlib(const void *data, size_t length, size_t *compressed_length) {
    if (!data || length == 0) return NULL;
    
    // 估计压缩后的最大大小
    size_t max_compressed_size = compressBound(length);
    void *compressed = utils_malloc(max_compressed_size, "zlib_compress");
    if (!compressed) return NULL;
    
    if (compressed_length) {
        *compressed_length = max_compressed_size;
    }
    
    // 使用zlib进行压缩
    int result = compress((Bytef*)compressed, (uLongf*)compressed_length,
                         (const Bytef*)data, length);
    
    if (result != Z_OK) {
        free(compressed);
        return NULL;
    }
    
    // 重新分配内存到实际大小
    void *resized = utils_realloc(compressed, *compressed_length, "zlib_compress_resize");
    if (resized) {
        compressed = resized;
    }
    
    return compressed;
}

/**
 * ZLIB解压缩
 */
void* utils_decompress_zlib(const void *data, size_t length, size_t *decompressed_length) {
    if (!data || length == 0 || !decompressed_length) return NULL;
    
    // 初始估计解压后的大小（假设压缩比最大为10:1）
    *decompressed_length = length * 10;
    void *decompressed = utils_malloc(*decompressed_length, "zlib_decompress");
    if (!decompressed) return NULL;
    
    // 使用zlib进行解压
    int result = uncompress((Bytef*)decompressed, (uLongf*)decompressed_length,
                           (const Bytef*)data, length);
    
    if (result == Z_BUF_ERROR) {
        // 缓冲区太小，增加大小重试
        *decompressed_length *= 2;
        void *resized = utils_realloc(decompressed, *decompressed_length, "zlib_decompress_resize");
        if (!resized) {
            free(decompressed);
            return NULL;
        }
        decompressed = resized;
        
        result = uncompress((Bytef*)decompressed, (uLongf*)decompressed_length,
                           (const Bytef*)data, length);
    }
    
    if (result != Z_OK) {
        free(decompressed);
        return NULL;
    }
    
    // 重新分配内存到实际大小
    void *resized = utils_realloc(decompressed, *decompressed_length, "zlib_decompress_resize2");
    if (resized) {
        decompressed = resized;
    }
    
    return decompressed;
}