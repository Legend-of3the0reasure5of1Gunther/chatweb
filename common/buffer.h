#ifndef SECURE_CHAT_BUFFER_H
#define SECURE_CHAT_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// 安全缓冲区结构
typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t length;
    size_t position;
    bool is_owned;
    const char *tag;  // 调试标签
} secure_buffer_t;

// 缓冲区创建和销毁
secure_buffer_t* buffer_create(size_t initial_capacity, const char *tag);
secure_buffer_t* buffer_create_from_data(const uint8_t *data, size_t length, const char *tag);
void buffer_destroy(secure_buffer_t *buffer);

// 数据操作
bool buffer_append(secure_buffer_t *buffer, const void *data, size_t length);
bool buffer_append_string(secure_buffer_t *buffer, const char *str);
bool buffer_read(secure_buffer_t *buffer, void *output, size_t length);
bool buffer_skip(secure_buffer_t *buffer, size_t length);
bool buffer_clear(secure_buffer_t *buffer);

// 大小和位置管理
bool buffer_resize(secure_buffer_t *buffer, size_t new_capacity);
bool buffer_ensure_capacity(secure_buffer_t *buffer, size_t required);
void buffer_rewind(secure_buffer_t *buffer);
bool buffer_seek(secure_buffer_t *buffer, size_t position);

// 安全检查
bool buffer_is_valid(const secure_buffer_t *buffer);
size_t buffer_remaining(const secure_buffer_t *buffer);
bool buffer_can_read(const secure_buffer_t *buffer, size_t length);

// 内存保护
void buffer_lock(secure_buffer_t *buffer);   // 锁定内存防止交换
void buffer_unlock(secure_buffer_t *buffer); // 解锁内存
void buffer_wipe(secure_buffer_t *buffer);   // 安全擦除内容

// 工具函数
secure_buffer_t* buffer_clone(const secure_buffer_t *src);
bool buffer_equals(const secure_buffer_t *a, const secure_buffer_t *b);
uint8_t* buffer_detach(secure_buffer_t *buffer);  // 转移所有权

#endif // SECURE_CHAT_BUFFER_H