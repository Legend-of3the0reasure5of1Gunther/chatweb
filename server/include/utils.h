#ifndef SECURE_CHAT_UTILS_H
#define SECURE_CHAT_UTILS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 错误处理 ====================
#define UTILS_MAX_ERROR_MSG 1024

typedef struct {
    int error_code;
    char message[UTILS_MAX_ERROR_MSG];
    char file[256];
    int line;
    time_t timestamp;
} error_info_t;

// ==================== 日志级别 ====================
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_WARNING = 2,
    LOG_LEVEL_ERROR = 3,
    LOG_LEVEL_CRITICAL = 4,
    LOG_LEVEL_NONE = 5
} log_level_t;

// ==================== 字符串处理 ====================
typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} string_t;

// ==================== 哈希表 ====================
typedef struct hash_table_node {
    char *key;
    void *value;
    struct hash_table_node *next;
} hash_table_node_t;

typedef struct {
    hash_table_node_t **buckets;
    size_t size;
    size_t count;
    size_t (*hash_function)(const char *key);
    void (*value_free_function)(void *value);
} hash_table_t;

// ==================== 链表 ====================
typedef struct list_node {
    void *data;
    struct list_node *next;
    struct list_node *prev;
} list_node_t;

typedef struct {
    list_node_t *head;
    list_node_t *tail;
    size_t size;
    void (*data_free_function)(void *data);
    int (*data_compare_function)(const void *a, const void *b);
} list_t;

// ==================== 环形缓冲区 ====================
typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t size;
    bool overwrite;
} ring_buffer_t;

// ==================== 连接池 ====================
typedef struct connection_pool connection_pool_t;

// ==================== 线程局部存储 ====================
typedef struct {
    pthread_key_t key;
    bool initialized;
} thread_local_t;

// ==================== 内存池 ====================
typedef struct memory_pool memory_pool_t;

// ==================== 函数声明 ====================

// ==================== 内存管理 ====================
void* utils_malloc(size_t size, const char *purpose);
void* utils_calloc(size_t count, size_t size, const char *purpose);
void* utils_realloc(void *ptr, size_t size, const char *purpose);
void utils_free(void **ptr);
void utils_memory_leak_check(void);

// ==================== 字符串操作 ====================
char* utils_strdup(const char *str);
char* utils_strndup(const char *str, size_t n);
char* utils_strdup_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
char* utils_strdup_vprintf(const char *format, va_list args);

char* utils_strtrim(char *str);
char* utils_strltrim(char *str);
char* utils_strrtrim(char *str);

char* utils_strlower(char *str);
char* utils_strupper(char *str);

bool utils_strstartswith(const char *str, const char *prefix);
bool utils_strendswith(const char *str, const char *suffix);

char* utils_strreplace(const char *str, const char *old, const char *new);

char** utils_strsplit(const char *str, const char *delim, int *count);
void utils_strfreev(char **str_array);

int utils_strcount(const char *str, char ch);
char* utils_strjoin(const char *separator, char **str_array);

// ==================== 动态字符串 ====================
string_t* string_create(size_t initial_capacity);
void string_destroy(string_t *str);
void string_clear(string_t *str);
int string_append(string_t *str, const char *text);
int string_append_format(string_t *str, const char *format, ...) __attribute__((format(printf, 2, 3)));
int string_append_char(string_t *str, char ch);
const char* string_get(const string_t *str);
size_t string_length(const string_t *str);
bool string_empty(const string_t *str);
int string_compare(const string_t *str1, const string_t *str2);

// ==================== 文件操作 ====================
bool utils_file_exists(const char *path);
bool utils_dir_exists(const char *path);
int64_t utils_file_size(const char *path);
time_t utils_file_mtime(const char *path);
time_t utils_file_atime(const char *path);
time_t utils_file_ctime(const char *path);

int utils_create_dir(const char *path, mode_t mode);
int utils_create_dirs(const char *path, mode_t mode);

char* utils_read_file(const char *path, size_t *length);
int utils_write_file(const char *path, const void *data, size_t length);
int utils_append_file(const char *path, const void *data, size_t length);

char* utils_get_cwd(void);
int utils_change_dir(const char *path);
char* utils_realpath(const char *path);

int utils_copy_file(const char *src, const char *dst);
int utils_move_file(const char *src, const char *dst);
int utils_delete_file(const char *path);

// ==================== 路径操作 ====================
char* utils_path_join(const char *dir, const char *file);
char* utils_path_dirname(const char *path);
char* utils_path_basename(const char *path);
char* utils_path_extension(const char *path);
char* utils_path_without_extension(const char *path);
bool utils_path_is_absolute(const char *path);
char* utils_path_normalize(const char *path);

// ==================== 哈希表操作 ====================
hash_table_t* hash_table_create(size_t size, 
                               size_t (*hash_function)(const char *key),
                               void (*value_free_function)(void *value));
void hash_table_destroy(hash_table_t *table);
int hash_table_insert(hash_table_t *table, const char *key, void *value);
void* hash_table_lookup(hash_table_t *table, const char *key);
int hash_table_remove(hash_table_t *table, const char *key);
size_t hash_table_size(const hash_table_t *table);
size_t hash_table_count(const hash_table_t *table);

// 内置哈希函数
size_t hash_string_djb2(const char *key);
size_t hash_string_sdbm(const char *key);
size_t hash_string_fnv1a(const char *key);

// ==================== 链表操作 ====================
list_t* list_create(void (*data_free_function)(void *data),
                   int (*data_compare_function)(const void *a, const void *b));
void list_destroy(list_t *list);
int list_append(list_t *list, void *data);
int list_prepend(list_t *list, void *data);
int list_insert(list_t *list, size_t index, void *data);
void* list_get(const list_t *list, size_t index);
int list_remove(list_t *list, size_t index);
int list_remove_data(list_t *list, void *data);
size_t list_size(const list_t *list);
bool list_empty(const list_t *list);
void list_clear(list_t *list);

// 迭代器
typedef struct {
    list_t *list;
    list_node_t *current;
    size_t index;
} list_iterator_t;

list_iterator_t* list_iterator_create(list_t *list);
void list_iterator_destroy(list_iterator_t *iter);
bool list_iterator_has_next(list_iterator_t *iter);
void* list_iterator_next(list_iterator_t *iter);
void list_iterator_rewind(list_iterator_t *iter);

// ==================== 环形缓冲区操作 ====================
ring_buffer_t* ring_buffer_create(size_t capacity, bool overwrite);
void ring_buffer_destroy(ring_buffer_t *rb);
size_t ring_buffer_write(ring_buffer_t *rb, const void *data, size_t size);
size_t ring_buffer_read(ring_buffer_t *rb, void *data, size_t size);
size_t ring_buffer_peek(const ring_buffer_t *rb, void *data, size_t size);
size_t ring_buffer_available(const ring_buffer_t *rb);
size_t ring_buffer_free_space(const ring_buffer_t *rb);
bool ring_buffer_empty(const ring_buffer_t *rb);
bool ring_buffer_full(const ring_buffer_t *rb);
void ring_buffer_clear(ring_buffer_t *rb);

// ==================== 时间操作 ====================
uint64_t utils_get_timestamp_ms(void);
uint64_t utils_get_timestamp_us(void);
uint64_t utils_get_timestamp_ns(void);

struct timespec utils_timespec_from_ms(uint64_t ms);
uint64_t utils_timespec_to_ms(const struct timespec *ts);

char* utils_format_timestamp(time_t timestamp, const char *format);
char* utils_format_current_time(const char *format);

void utils_sleep_ms(uint64_t milliseconds);
void utils_sleep_us(uint64_t microseconds);

// ==================== 随机数生成 ====================
void utils_random_init(void);
uint32_t utils_random_uint32(void);
uint64_t utils_random_uint64(void);
int utils_random_int(int min, int max);
double utils_random_double(double min, double max);
void utils_random_bytes(void *buffer, size_t size);
char* utils_random_string(size_t length);
char* utils_random_uuid(void);

// ==================== 加密和哈希 ====================
char* utils_md5(const void *data, size_t length);
char* utils_md5_file(const char *path);

char* utils_sha256(const void *data, size_t length);
char* utils_sha256_file(const char *path);

char* utils_base64_encode(const void *data, size_t length);
void* utils_base64_decode(const char *data, size_t *length);

void* utils_hex_decode(const char *hex, size_t *length);
char* utils_hex_encode(const void *data, size_t length);

// ==================== 网络操作 ====================
bool utils_is_valid_ipv4(const char *ip);
bool utils_is_valid_ipv6(const char *ip);
bool utils_is_valid_port(uint16_t port);

char* utils_get_hostname(void);
char* utils_get_ip_address(const char *interface);

int utils_set_socket_nonblocking(int fd);
int utils_set_socket_blocking(int fd);
int utils_set_socket_timeout(int fd, int timeout_ms);
int utils_set_socket_reuseaddr(int fd, bool enable);
int utils_set_socket_keepalive(int fd, bool enable);

// ==================== 进程和线程 ====================
pid_t utils_get_process_id(void);
pid_t utils_get_parent_process_id(void);
char* utils_get_process_name(void);

int utils_set_thread_name(const char *name);
char* utils_get_thread_name(void);
pthread_t utils_get_thread_id(void);

int utils_create_thread(pthread_t *thread, void *(*start_routine)(void *), void *arg);
int utils_join_thread(pthread_t thread);
int utils_detach_thread(pthread_t thread);

// ==================== 日志系统 ====================
typedef struct {
    log_level_t level;
    FILE *output;
    char *file_path;
    bool enable_console;
    bool enable_file;
    bool enable_syslog;
    size_t max_file_size;
    int max_backups;
    pthread_mutex_t mutex;
} logger_t;

logger_t* logger_create(void);
void logger_destroy(logger_t *logger);

void logger_set_level(logger_t *logger, log_level_t level);
void logger_set_output(logger_t *logger, FILE *output);
void logger_set_file(logger_t *logger, const char *file_path);
void logger_set_max_size(logger_t *logger, size_t max_size, int max_backups);

void logger_log(logger_t *logger, log_level_t level, const char *file, int line,
                const char *function, const char *format, ...) __attribute__((format(printf, 6, 7)));

// 日志宏
#define LOG_DEBUG(logger, ...) logger_log(logger, LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_INFO(logger, ...) logger_log(logger, LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_WARNING(logger, ...) logger_log(logger, LOG_LEVEL_WARNING, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_ERROR(logger, ...) logger_log(logger, LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_CRITICAL(logger, ...) logger_log(logger, LOG_LEVEL_CRITICAL, __FILE__, __LINE__, __func__, __VA_ARGS__)

// ==================== 错误处理 ====================
void utils_set_last_error(int error_code, const char *format, ...) __attribute__((format(printf, 2, 3)));
error_info_t utils_get_last_error(void);
void utils_clear_last_error(void);
const char* utils_strerror(int error_code);

// ==================== 配置解析 ====================
typedef struct {
    hash_table_t *sections;
} config_t;

config_t* config_create(void);
void config_destroy(config_t *config);

int config_load_file(config_t *config, const char *path);
int config_load_string(config_t *config, const char *str);
int config_save_file(config_t *config, const char *path);

int config_set_value(config_t *config, const char *section, const char *key, const char *value);
const char* config_get_value(const config_t *config, const char *section, const char *key);
int config_get_int(const config_t *config, const char *section, const char *key, int default_value);
double config_get_double(const config_t *config, const char *section, const char *key, double default_value);
bool config_get_bool(const config_t *config, const char *section, const char *key, bool default_value);

int config_remove_key(config_t *config, const char *section, const char *key);
int config_remove_section(config_t *config, const char *section);

// ==================== JSON处理 ====================
char* utils_json_escape(const char *str);
char* utils_json_unescape(const char *str);

// ==================== URL编码/解码 ====================
char* utils_url_encode(const char *str);
char* utils_url_decode(const char *str);

// ==================== 校验和 ====================
uint32_t utils_crc32(const void *data, size_t length);
uint16_t utils_crc16(const void *data, size_t length);
uint8_t utils_checksum(const void *data, size_t length);

// ==================== 系统信息 ====================
char* utils_get_system_info(void);
char* utils_get_cpu_info(void);
char* utils_get_memory_info(void);
char* utils_get_disk_info(void);
char* utils_get_network_info(void);

// ==================== 压缩 ====================
void* utils_compress_gzip(const void *data, size_t length, size_t *compressed_length);
void* utils_decompress_gzip(const void *data, size_t length, size_t *decompressed_length);
void* utils_compress_zlib(const void *data, size_t length, size_t *compressed_length);
void* utils_decompress_zlib(const void *data, size_t length, size_t *decompressed_length);

#ifdef __cplusplus
}
#endif

#endif // SECURE_CHAT_UTILS_H