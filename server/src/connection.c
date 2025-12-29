#include "connection.h"
#include "../common/protocol.h"
#include "../common/security.h"
#include "../common/logger.h"
#include "../common/buffer.h"
#include "../common/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

// ==================== 内部函数声明 ====================

static client_connection_t* allocate_connection(connection_pool_t *pool);
static void free_connection(connection_pool_t *pool, client_connection_t *conn);
static bool resize_connection_pool(connection_pool_t *pool, size_t new_capacity);

static bool init_connection_buffers(client_connection_t *conn);
static void free_connection_buffers(client_connection_t *conn);
static bool resize_recv_buffer(client_connection_t *conn, size_t new_size);
static bool resize_send_buffer(client_connection_t *conn, size_t new_size);

static ssize_t ssl_send(client_connection_t *conn, const void *data, size_t len);
static ssize_t ssl_receive(client_connection_t *conn, void *buffer, size_t buffer_len);
static ssize_t plain_send(client_connection_t *conn, const void *data, size_t len);
static ssize_t plain_receive(client_connection_t *conn, void *buffer, size_t buffer_len);

static bool parse_message_header(client_connection_t *conn, message_header_t *header);
static bool process_complete_message(client_connection_t *conn, message_header_t *header);
static void handle_heartbeat(client_connection_t *conn, uint64_t timestamp);

static void update_rate_limiter(client_connection_t *conn, size_t message_size);
static bool check_rate_limit_reset(client_connection_t *conn);
static void reset_rate_limiter(client_connection_t *conn);

// ==================== 连接池实现 ====================

connection_pool_t* connection_pool_create(size_t initial_capacity) {
    connection_pool_t *pool = (connection_pool_t*)calloc(1, sizeof(connection_pool_t));
    if (!pool) {
        LOG_ERROR("连接池内存分配失败");
        return NULL;
    }
    
    // 分配连接数组
    pool->connections = (client_connection_t*)calloc(initial_capacity, 
                                                     sizeof(client_connection_t));
    if (!pool->connections) {
        LOG_ERROR("连接数组分配失败");
        free(pool);
        return NULL;
    }
    
    pool->capacity = initial_capacity;
    pool->count = 0;
    pool->next_connection_id = 1;
    
    // 初始化同步原语
    if (pthread_mutex_init(&pool->lock, NULL) != 0) {
        LOG_ERROR("互斥锁初始化失败");
        free(pool->connections);
        free(pool);
        return NULL;
    }
    
    if (pthread_rwlock_init(&pool->rwlock, NULL) != 0) {
        LOG_ERROR("读写锁初始化失败");
        pthread_mutex_destroy(&pool->lock);
        free(pool->connections);
        free(pool);
        return NULL;
    }
    
    // 初始化所有连接槽位
    for (size_t i = 0; i < pool->capacity; i++) {
        pool->connections[i].fd = -1; // 标记为空闲
    }
    
    LOG_DEBUG("连接池创建成功，容量: %zu", initial_capacity);
    return pool;
}

void connection_pool_destroy(connection_pool_t *pool) {
    if (!pool) {
        return;
    }
    
    LOG_DEBUG("销毁连接池...");
    
    // 销毁所有活跃连接
    pthread_mutex_lock(&pool->lock);
    
    for (size_t i = 0; i < pool->capacity; i++) {
        client_connection_t *conn = &pool->connections[i];
        if (conn->fd != -1) {
            connection_close(conn, true);
            connection_destroy(conn);
        }
    }
    
    pthread_mutex_unlock(&pool->lock);
    
    // 销毁同步原语
    pthread_rwlock_destroy(&pool->rwlock);
    pthread_mutex_destroy(&pool->lock);
    
    // 释放内存
    free(pool->connections);
    free(pool);
    
    LOG_DEBUG("连接池已销毁");
}

static client_connection_t* allocate_connection(connection_pool_t *pool) {
    // 查找空闲槽位
    for (size_t i = 0; i < pool->capacity; i++) {
        if (pool->connections[i].fd == -1) {
            return &pool->connections[i];
        }
    }
    
    // 没有空闲槽位，扩展容量
    size_t new_capacity = pool->capacity * 2;
    if (!resize_connection_pool(pool, new_capacity)) {
        return NULL;
    }
    
    return &pool->connections[pool->capacity / 2]; // 返回新分配的区域
}

static bool resize_connection_pool(connection_pool_t *pool, size_t new_capacity) {
    pthread_mutex_lock(&pool->lock);
    
    client_connection_t *new_connections = (client_connection_t*)realloc(
        pool->connections, new_capacity * sizeof(client_connection_t));
    
    if (!new_connections) {
        LOG_ERROR("连接池扩展失败");
        pthread_mutex_unlock(&pool->lock);
        return false;
    }
    
    // 初始化新分配的区域
    for (size_t i = pool->capacity; i < new_capacity; i++) {
        memset(&new_connections[i], 0, sizeof(client_connection_t));
        new_connections[i].fd = -1;
    }
    
    pool->connections = new_connections;
    pool->capacity = new_capacity;
    
    pthread_mutex_unlock(&pool->lock);
    
    LOG_DEBUG("连接池已扩展，新容量: %zu", new_capacity);
    return true;
}

static void free_connection(connection_pool_t *pool, client_connection_t *conn) {
    if (!conn) {
        return;
    }
    
    // 标记为空闲
    conn->fd = -1;
    
    // 更新计数
    pool->count--;
}

client_connection_t* connection_pool_add(connection_pool_t *pool, int fd, struct sockaddr_in *addr) {
    if (!pool || fd < 0 || !addr) {
        return NULL;
    }
    
    pthread_mutex_lock(&pool->lock);
    
    // 检查是否达到最大连接数
    if (pool->count >= pool->capacity && !resize_connection_pool(pool, pool->capacity * 2)) {
        LOG_ERROR("连接池已满且扩展失败");
        pthread_mutex_unlock(&pool->lock);
        return NULL;
    }
    
    // 分配连接
    client_connection_t *conn = allocate_connection(pool);
    if (!conn) {
        pthread_mutex_unlock(&pool->lock);
        return NULL;
    }
    
    // 初始化连接
    memset(conn, 0, sizeof(client_connection_t));
    
    conn->id = pool->next_connection_id++;
    conn->fd = fd;
    conn->client_addr = *addr;
    conn->state = CONNECTION_STATE_NEW;
    conn->connected_at = get_current_timestamp_ms();
    conn->last_activity = conn->connected_at;
    conn->last_heartbeat = conn->connected_at;
    
    // 初始化缓冲区
    if (!init_connection_buffers(conn)) {
        LOG_ERROR("连接缓冲区初始化失败");
        memset(conn, 0, sizeof(client_connection_t));
        conn->fd = -1;
        pthread_mutex_unlock(&pool->lock);
        return NULL;
    }
    
    // 初始化互斥锁和条件变量
    if (pthread_mutex_init(&conn->mutex, NULL) != 0) {
        LOG_ERROR("连接互斥锁初始化失败");
        free_connection_buffers(conn);
        memset(conn, 0, sizeof(client_connection_t));
        conn->fd = -1;
        pthread_mutex_unlock(&pool->lock);
        return NULL;
    }
    
    if (pthread_cond_init(&conn->data_ready, NULL) != 0) {
        LOG_ERROR("连接条件变量初始化失败");
        pthread_mutex_destroy(&conn->mutex);
        free_connection_buffers(conn);
        memset(conn, 0, sizeof(client_connection_t));
        conn->fd = -1;
        pthread_mutex_unlock(&pool->lock);
        return NULL;
    }
    
    // 初始化速率限制器
    connection_init_rate_limiter(conn, DEFAULT_MESSAGES_PER_MINUTE, DEFAULT_BYTES_PER_MINUTE);
    
    pool->count++;
    pool->stats.total_connections++;
    pool->stats.active_connections++;
    
    if (pool->stats.active_connections > pool->stats.max_concurrent_connections) {
        pool->stats.max_concurrent_connections = pool->stats.active_connections;
    }
    
    pthread_mutex_unlock(&pool->lock);
    
    // 记录日志
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str));
    
    LOG_INFO("新连接: ID=%llu, IP=%s:%d", 
             (unsigned long long)conn->id, ip_str, ntohs(addr->sin_port));
    
    return conn;
}

client_connection_t* connection_pool_get_by_id(connection_pool_t *pool, uint64_t connection_id) {
    if (!pool) {
        return NULL;
    }
    
    pthread_rwlock_rdlock(&pool->rwlock);
    
    for (size_t i = 0; i < pool->capacity; i++) {
        client_connection_t *conn = &pool->connections[i];
        if (conn->fd != -1 && conn->id == connection_id) {
            pthread_rwlock_unlock(&pool->rwlock);
            return conn;
        }
    }
    
    pthread_rwlock_unlock(&pool->rwlock);
    return NULL;
}

client_connection_t* connection_pool_get_by_fd(connection_pool_t *pool, int fd) {
    if (!pool) {
        return NULL;
    }
    
    pthread_rwlock_rdlock(&pool->rwlock);
    
    for (size_t i = 0; i < pool->capacity; i++) {
        client_connection_t *conn = &pool->connections[i];
        if (conn->fd != -1 && conn->fd == fd) {
            pthread_rwlock_unlock(&pool->rwlock);
            return conn;
        }
    }
    
    pthread_rwlock_unlock(&pool->rwlock);
    return NULL;
}

int connection_pool_get_by_user(connection_pool_t *pool, uint64_t user_id, 
                               client_connection_t ***connections, size_t *count) {
    if (!pool || !connections || !count) {
        return -1;
    }
    
    pthread_rwlock_rdlock(&pool->rwlock);
    
    // 第一次遍历计数
    size_t match_count = 0;
    for (size_t i = 0; i < pool->capacity; i++) {
        client_connection_t *conn = &pool->connections[i];
        if (conn->fd != -1 && conn->user_id == user_id && 
            conn->state == CONNECTION_STATE_AUTHENTICATED) {
            match_count++;
        }
    }
    
    if (match_count == 0) {
        *connections = NULL;
        *count = 0;
        pthread_rwlock_unlock(&pool->rwlock);
        return 0;
    }
    
    // 分配数组
    client_connection_t **result = (client_connection_t**)calloc(match_count, 
                                                                 sizeof(client_connection_t*));
    if (!result) {
        pthread_rwlock_unlock(&pool->rwlock);
        return -2;
    }
    
    // 第二次遍历填充数组
    size_t index = 0;
    for (size_t i = 0; i < pool->capacity; i++) {
        client_connection_t *conn = &pool->connections[i];
        if (conn->fd != -1 && conn->user_id == user_id && 
            conn->state == CONNECTION_STATE_AUTHENTICATED) {
            result[index++] = conn;
        }
    }
    
    *connections = result;
    *count = match_count;
    
    pthread_rwlock_unlock(&pool->rwlock);
    return 0;
}

int connection_pool_remove(connection_pool_t *pool, uint64_t connection_id) {
    if (!pool) {
        return -1;
    }
    
    pthread_mutex_lock(&pool->lock);
    
    for (size_t i = 0; i < pool->capacity; i++) {
        client_connection_t *conn = &pool->connections[i];
        if (conn->fd != -1 && conn->id == connection_id) {
            // 关闭连接
            connection_close(conn, true);
            
            // 销毁连接资源
            connection_destroy(conn);
            
            // 标记为空闲
            free_connection(pool, conn);
            
            pool->stats.active_connections--;
            
            pthread_mutex_unlock(&pool->lock);
            
            LOG_INFO("连接移除: ID=%llu", (unsigned long long)connection_id);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&pool->lock);
    return -2;
}

size_t connection_pool_get_active_count(connection_pool_t *pool) {
    if (!pool) {
        return 0;
    }
    
    pthread_rwlock_rdlock(&pool->rwlock);
    size_t count = pool->stats.active_connections;
    pthread_rwlock_unlock(&pool->rwlock);
    
    return count;
}

size_t connection_pool_cleanup_expired(connection_pool_t *pool, uint64_t timeout_ms) {
    if (!pool) {
        return 0;
    }
    
    size_t cleaned_count = 0;
    uint64_t now = get_current_timestamp_ms();
    
    pthread_mutex_lock(&pool->lock);
    
    for (size_t i = 0; i < pool->capacity; i++) {
        client_connection_t *conn = &pool->connections[i];
        
        if (conn->fd != -1) {
            // 检查连接是否超时
            if (now - conn->last_activity > timeout_ms) {
                LOG_INFO("清理过期连接: ID=%llu, 超时时间=%llums", 
                         (unsigned long long)conn->id, 
                         (unsigned long long)(now - conn->last_activity));
                
                connection_close(conn, false);
                connection_destroy(conn);
                free_connection(pool, conn);
                
                pool->stats.active_connections--;
                cleaned_count++;
            }
        }
    }
    
    pthread_mutex_unlock(&pool->lock);
    
    if (cleaned_count > 0) {
        LOG_DEBUG("清理了 %zu 个过期连接", cleaned_count);
    }
    
    return cleaned_count;
}

int connection_pool_get_statistics(connection_pool_t *pool, connection_pool_stats_t *stats) {
    if (!pool || !stats) {
        return -1;
    }
    
    pthread_rwlock_rdlock(&pool->rwlock);
    memcpy(stats, &pool->stats, sizeof(connection_pool_stats_t));
    pthread_rwlock_unlock(&pool->rwlock);
    
    return 0;
}

int connection_pool_get_active_connections(connection_pool_t *pool, 
                                         connection_info_t **connections, size_t *count) {
    if (!pool || !connections || !count) {
        return -1;
    }
    
    pthread_rwlock_rdlock(&pool->rwlock);
    
    // 第一次遍历计数
    size_t active_count = 0;
    for (size_t i = 0; i < pool->capacity; i++) {
        if (pool->connections[i].fd != -1) {
            active_count++;
        }
    }
    
    if (active_count == 0) {
        *connections = NULL;
        *count = 0;
        pthread_rwlock_unlock(&pool->rwlock);
        return 0;
    }
    
    // 分配数组
    connection_info_t *result = (connection_info_t*)calloc(active_count, 
                                                          sizeof(connection_info_t));
    if (!result) {
        pthread_rwlock_unlock(&pool->rwlock);
        return -2;
    }
    
    // 第二次遍历填充信息
    size_t index = 0;
    for (size_t i = 0; i < pool->capacity; i++) {
        client_connection_t *conn = &pool->connections[i];
        if (conn->fd != -1) {
            connection_info_t *info = &result[index++];
            
            info->connection_id = conn->id;
            info->user_id = conn->user_id;
            info->connected_at = conn->connected_at;
            info->last_activity = conn->last_activity;
            info->state = conn->state;
            info->bytes_sent = conn->bytes_sent;
            info->bytes_received = conn->bytes_received;
            
            strncpy(info->username, conn->username, sizeof(info->username) - 1);
            strncpy(info->nickname, conn->nickname, sizeof(info->nickname) - 1);
            
            inet_ntop(AF_INET, &conn->client_addr.sin_addr, info->ip_address, 
                     sizeof(info->ip_address));
            info->port = ntohs(conn->client_addr.sin_port);
        }
    }
    
    *connections = result;
    *count = active_count;
    
    pthread_rwlock_unlock(&pool->rwlock);
    return 0;
}

void connection_pool_free_connection_list(connection_info_t *connections, size_t count) {
    if (!connections) {
        return;
    }
    
    free(connections);
}

// ==================== 连接管理实现 ====================

client_connection_t* connection_create(int fd, struct sockaddr_in *addr, SSL_CTX *ssl_ctx) {
    if (fd < 0 || !addr) {
        return NULL;
    }
    
    client_connection_t *conn = (client_connection_t*)calloc(1, sizeof(client_connection_t));
    if (!conn) {
        LOG_ERROR("连接内存分配失败");
        return NULL;
    }
    
    // 初始化基本信息
    conn->id = 0; // 将由连接池分配
    conn->fd = fd;
    conn->client_addr = *addr;
    conn->state = CONNECTION_STATE_NEW;
    conn->connected_at = get_current_timestamp_ms();
    conn->last_activity = conn->connected_at;
    conn->last_heartbeat = conn->connected_at;
    
    // 初始化缓冲区
    if (!init_connection_buffers(conn)) {
        LOG_ERROR("连接缓冲区初始化失败");
        free(conn);
        return NULL;
    }
    
    // 初始化互斥锁和条件变量
    if (pthread_mutex_init(&conn->mutex, NULL) != 0) {
        LOG_ERROR("连接互斥锁初始化失败");
        free_connection_buffers(conn);
        free(conn);
        return NULL;
    }
    
    if (pthread_cond_init(&conn->data_ready, NULL) != 0) {
        LOG_ERROR("连接条件变量初始化失败");
        pthread_mutex_destroy(&conn->mutex);
        free_connection_buffers(conn);
        free(conn);
        return NULL;
    }
    
    // 初始化SSL（如果启用）
    if (ssl_ctx) {
        if (connection_enable_ssl(conn, ssl_ctx) != 0) {
            LOG_ERROR("SSL启用失败");
            pthread_cond_destroy(&conn->data_ready);
            pthread_mutex_destroy(&conn->mutex);
            free_connection_buffers(conn);
            free(conn);
            return NULL;
        }
    }
    
    // 初始化速率限制器
    connection_init_rate_limiter(conn, DEFAULT_MESSAGES_PER_MINUTE, DEFAULT_BYTES_PER_MINUTE);
    
    LOG_DEBUG("连接创建成功，FD=%d", fd);
    return conn;
}

void connection_destroy(client_connection_t *conn) {
    if (!conn) {
        return;
    }
    
    LOG_DEBUG("销毁连接: ID=%llu", (unsigned long long)conn->id);
    
    // 关闭SSL
    if (conn->ssl) {
        SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
        conn->ssl = NULL;
    }
    
    // 销毁同步原语
    pthread_cond_destroy(&conn->data_ready);
    pthread_mutex_destroy(&conn->mutex);
    
    // 释放缓冲区
    free_connection_buffers(conn);
    
    // 释放连接结构
    free(conn);
}

int connection_close(client_connection_t *conn, bool graceful) {
    if (!conn) {
        return -1;
    }
    
    if (conn->state == CONNECTION_STATE_CLOSING || 
        conn->state == CONNECTION_STATE_CLOSED) {
        return 0;
    }
    
    LOG_DEBUG("%s关闭连接: ID=%llu", 
              graceful ? "优雅" : "强制", 
              (unsigned long long)conn->id);
    
    // 更新状态
    conn->state = graceful ? CONNECTION_STATE_CLOSING : CONNECTION_STATE_CLOSED;
    
    // 发送关闭通知（如果优雅关闭）
    if (graceful && conn->state == CONNECTION_STATE_AUTHENTICATED) {
        // 这里可以发送断开连接通知
    }
    
    // 关闭SSL
    if (conn->ssl) {
        SSL_shutdown(conn->ssl);
    }
    
    // 关闭套接字
    if (conn->fd != -1) {
        if (graceful) {
            shutdown(conn->fd, SHUT_RDWR);
        }
        close(conn->fd);
        conn->fd = -1;
    }
    
    // 最终状态
    conn->state = CONNECTION_STATE_CLOSED;
    
    return 0;
}

static bool init_connection_buffers(client_connection_t *conn) {
    // 分配接收缓冲区
    conn->recv_buffer_size = CONNECTION_RECV_BUFFER_SIZE;
    conn->recv_buffer = (uint8_t*)malloc(conn->recv_buffer_size);
    if (!conn->recv_buffer) {
        return false;
    }
    conn->recv_buffer_used = 0;
    
    // 分配发送缓冲区
    conn->send_buffer_size = CONNECTION_SEND_BUFFER_SIZE;
    conn->send_buffer = (uint8_t*)malloc(conn->send_buffer_size);
    if (!conn->send_buffer) {
        free(conn->recv_buffer);
        return false;
    }
    conn->send_buffer_used = 0;
    
    return true;
}

static void free_connection_buffers(client_connection_t *conn) {
    if (conn->recv_buffer) {
        free(conn->recv_buffer);
        conn->recv_buffer = NULL;
        conn->recv_buffer_size = 0;
        conn->recv_buffer_used = 0;
    }
    
    if (conn->send_buffer) {
        free(conn->send_buffer);
        conn->send_buffer = NULL;
        conn->send_buffer_size = 0;
        conn->send_buffer_used = 0;
    }
}

static bool resize_recv_buffer(client_connection_t *conn, size_t new_size) {
    if (new_size <= conn->recv_buffer_size) {
        return true;
    }
    
    uint8_t *new_buffer = (uint8_t*)realloc(conn->recv_buffer, new_size);
    if (!new_buffer) {
        return false;
    }
    
    conn->recv_buffer = new_buffer;
    conn->recv_buffer_size = new_size;
    
    return true;
}

static bool resize_send_buffer(client_connection_t *conn, size_t new_size) {
    if (new_size <= conn->send_buffer_size) {
        return true;
    }
    
    uint8_t *new_buffer = (uint8_t*)realloc(conn->send_buffer, new_size);
    if (!new_buffer) {
        return false;
    }
    
    conn->send_buffer = new_buffer;
    conn->send_buffer_size = new_size;
    
    return true;
}

ssize_t connection_send(client_connection_t *conn, const void *data, size_t len) {
    if (!conn || conn->fd == -1 || !data || len == 0) {
        return -1;
    }
    
    // 检查速率限制
    if (!connection_check_rate_limit(conn, len)) {
        LOG_WARNING("连接 %llu 超过速率限制", (unsigned long long)conn->id);
        return -2;
    }
    
    ssize_t sent = 0;
    
    if (conn->ssl) {
        sent = ssl_send(conn, data, len);
    } else {
        sent = plain_send(conn, data, len);
    }
    
    if (sent > 0) {
        // 更新统计信息
        pthread_mutex_lock(&conn->mutex);
        conn->bytes_sent += sent;
        conn->last_activity = get_current_timestamp_ms();
        pthread_mutex_unlock(&conn->mutex);
        
        // 更新速率限制器
        update_rate_limiter(conn, sent);
    }
    
    return sent;
}

ssize_t connection_receive(client_connection_t *conn, void *buffer, size_t buffer_len) {
    if (!conn || conn->fd == -1 || !buffer || buffer_len == 0) {
        return -1;
    }
    
    ssize_t received = 0;
    
    if (conn->ssl) {
        received = ssl_receive(conn, buffer, buffer_len);
    } else {
        received = plain_receive(conn, buffer, buffer_len);
    }
    
    if (received > 0) {
        // 更新统计信息
        pthread_mutex_lock(&conn->mutex);
        conn->bytes_received += received;
        conn->last_activity = get_current_timestamp_ms();
        pthread_mutex_unlock(&conn->mutex);
    }
    
    return received;
}

static ssize_t ssl_send(client_connection_t *conn, const void *data, size_t len) {
    if (!conn->ssl) {
        return -1;
    }
    
    ssize_t total_sent = 0;
    
    while (total_sent < (ssize_t)len) {
        ssize_t sent = SSL_write(conn->ssl, (const uint8_t*)data + total_sent, 
                                len - total_sent);
        
        if (sent <= 0) {
            int err = SSL_get_error(conn->ssl, sent);
            
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                // 需要重试
                continue;
            } else if (err == SSL_ERROR_ZERO_RETURN) {
                // 连接关闭
                break;
            } else {
                LOG_ERROR("SSL写入错误: %d", err);
                return -1;
            }
        }
        
        total_sent += sent;
    }
    
    return total_sent;
}

static ssize_t ssl_receive(client_connection_t *conn, void *buffer, size_t buffer_len) {
    if (!conn->ssl) {
        return -1;
    }
    
    ssize_t received = SSL_read(conn->ssl, buffer, buffer_len);
    
    if (received <= 0) {
        int err = SSL_get_error(conn->ssl, received);
        
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            // 没有数据可读，但不是错误
            return 0;
        } else if (err == SSL_ERROR_ZERO_RETURN) {
            // 连接关闭
            return 0;
        } else {
            LOG_ERROR("SSL读取错误: %d", err);
            return -1;
        }
    }
    
    return received;
}

static ssize_t plain_send(client_connection_t *conn, const void *data, size_t len) {
    ssize_t total_sent = 0;
    
    while (total_sent < (ssize_t)len) {
        ssize_t sent = send(conn->fd, (const uint8_t*)data + total_sent, 
                           len - total_sent, MSG_NOSIGNAL);
        
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 需要重试
                continue;
            } else if (errno == EPIPE || errno == ECONNRESET) {
                // 连接关闭
                break;
            } else {
                LOG_ERROR("套接字写入错误: %s", strerror(errno));
                return -1;
            }
        }
        
        total_sent += sent;
    }
    
    return total_sent;
}

static ssize_t plain_receive(client_connection_t *conn, void *buffer, size_t buffer_len) {
    ssize_t received = recv(conn->fd, buffer, buffer_len, 0);
    
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 没有数据可读，但不是错误
            return 0;
        } else if (errno == ECONNRESET) {
            // 连接重置
            return 0;
        } else {
            LOG_ERROR("套接字读取错误: %s", strerror(errno));
            return -1;
        }
    } else if (received == 0) {
        // 连接关闭
        return 0;
    }
    
    return received;
}

int connection_send_message(client_connection_t *conn, uint32_t message_type, 
                           const void *data, size_t len) {
    if (!conn) {
        return -1;
    }
    
    // 检查消息大小限制
    if (len > MAX_MESSAGE_SIZE) {
        LOG_ERROR("消息太大: %zu > %u", len, MAX_MESSAGE_SIZE);
        return -2;
    }
    
    // 创建消息头
    message_header_t header;
    memset(&header, 0, sizeof(header));
    
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = message_type;
    header.timestamp = get_current_timestamp_ms();
    header.message_id = ++conn->messages_sent;
    header.body_length = len;
    
    // 计算校验和
    if (len > 0) {
        header.checksum = calculate_message_checksum(data, len);
    }
    
    // 序列化消息头
    uint8_t header_buffer[sizeof(message_header_t)];
    serialize_message_header(&header, header_buffer);
    
    // 发送消息头
    ssize_t sent = connection_send(conn, header_buffer, sizeof(header_buffer));
    if (sent != sizeof(header_buffer)) {
        LOG_ERROR("消息头发送失败: %zd/%zu", sent, sizeof(header_buffer));
        return -3;
    }
    
    // 发送消息体（如果有）
    if (len > 0) {
        sent = connection_send(conn, data, len);
        if (sent != (ssize_t)len) {
            LOG_ERROR("消息体发送失败: %zd/%zu", sent, len);
            return -4;
        }
    }
    
    LOG_DEBUG("消息发送成功: 类型=0x%08x, 长度=%zu", message_type, len);
    return 0;
}

int connection_receive_message(client_connection_t *conn, uint32_t *message_type, 
                              void **data, size_t *len) {
    if (!conn || !message_type || !data || !len) {
        return -1;
    }
    
    // 检查是否有完整的消息头
    if (conn->recv_buffer_used < sizeof(message_header_t)) {
        return 0; // 需要更多数据
    }
    
    // 解析消息头
    message_header_t header;
    if (!parse_message_header(conn, &header)) {
        LOG_ERROR("消息头解析失败");
        return -2;
    }
    
    // 检查魔数和版本
    if (header.magic != PROTOCOL_MAGIC) {
        LOG_ERROR("无效的魔数: 0x%08x", header.magic);
        return -3;
    }
    
    if (header.version != PROTOCOL_VERSION) {
        LOG_WARNING("协议版本不匹配: 服务器=%d, 客户端=%d", 
                   header.version, PROTOCOL_VERSION);
        // 继续处理，但记录警告
    }
    
    // 检查是否有完整的消息体
    if (conn->recv_buffer_used < sizeof(message_header_t) + header.body_length) {
        return 0; // 需要更多数据
    }
    
    // 分配内存给消息体
    void *message_body = malloc(header.body_length);
    if (!message_body) {
        LOG_ERROR("消息体内存分配失败");
        return -4;
    }
    
    // 复制消息体
    memcpy(message_body, 
           conn->recv_buffer + sizeof(message_header_t), 
           header.body_length);
    
    // 验证校验和
    if (header.body_length > 0) {
        uint32_t calculated_checksum = calculate_message_checksum(message_body, header.body_length);
        if (calculated_checksum != header.checksum) {
            LOG_ERROR("消息校验和错误: 期望=0x%08x, 实际=0x%08x", 
                     header.checksum, calculated_checksum);
            free(message_body);
            return -5;
        }
    }
    
    // 更新缓冲区
    size_t total_len = sizeof(message_header_t) + header.body_length;
    memmove(conn->recv_buffer, 
            conn->recv_buffer + total_len, 
            conn->recv_buffer_used - total_len);
    conn->recv_buffer_used -= total_len;
    
    // 更新统计信息
    pthread_mutex_lock(&conn->mutex);
    conn->messages_received++;
    pthread_mutex_unlock(&conn->mutex);
    
    // 设置输出参数
    *message_type = header.type;
    *data = message_body;
    *len = header.body_length;
    
    LOG_DEBUG("消息接收成功: 类型=0x%08x, 长度=%zu", header.type, header.body_length);
    return 1;
}

static bool parse_message_header(client_connection_t *conn, message_header_t *header) {
    if (!conn || !header) {
        return false;
    }
    
    return deserialize_message_header(conn->recv_buffer, header);
}

int connection_process_data(client_connection_t *conn, const void *data, size_t len) {
    if (!conn || !data || len == 0) {
        return -1;
    }
    
    // 确保接收缓冲区有足够空间
    if (conn->recv_buffer_used + len > conn->recv_buffer_size) {
        if (!resize_recv_buffer(conn, (conn->recv_buffer_used + len) * 2)) {
            LOG_ERROR("接收缓冲区扩展失败");
            return -2;
        }
    }
    
    // 复制数据到接收缓冲区
    memcpy(conn->recv_buffer + conn->recv_buffer_used, data, len);
    conn->recv_buffer_used += len;
    
    // 处理消息
    int processed = 0;
    
    while (conn->recv_buffer_used >= sizeof(message_header_t)) {
        message_header_t header;
        
        if (!parse_message_header(conn, &header)) {
            LOG_ERROR("消息头解析失败");
            break;
        }
        
        // 检查是否有完整的消息
        if (conn->recv_buffer_used < sizeof(message_header_t) + header.body_length) {
            break; // 需要更多数据
        }
        
        // 处理完整消息
        if (!process_complete_message(conn, &header)) {
            LOG_ERROR("消息处理失败");
            break;
        }
        
        // 从缓冲区移除已处理的消息
        size_t total_len = sizeof(message_header_t) + header.body_length;
        memmove(conn->recv_buffer, 
                conn->recv_buffer + total_len, 
                conn->recv_buffer_used - total_len);
        conn->recv_buffer_used -= total_len;
        
        processed++;
    }
    
    return processed;
}

static bool process_complete_message(client_connection_t *conn, message_header_t *header) {
    if (!conn || !header) {
        return false;
    }
    
    // 处理系统消息
    switch (header->type) {
        case MSG_SYSTEM_HEARTBEAT:
            if (header->body_length == sizeof(uint64_t)) {
                uint64_t timestamp;
                memcpy(&timestamp, 
                       conn->recv_buffer + sizeof(message_header_t), 
                       sizeof(uint64_t));
                handle_heartbeat(conn, timestamp);
            }
            return true;
            
        case MSG_SYSTEM_ERROR:
            // 处理系统错误
            LOG_ERROR("系统错误消息");
            return true;
            
        default:
            // 其他消息由上层处理
            return true;
    }
}

static void handle_heartbeat(client_connection_t *conn, uint64_t timestamp) {
    if (!conn) {
        return;
    }
    
    // 更新最后心跳时间
    conn->last_heartbeat = get_current_timestamp_ms();
    
    // 发送心跳响应
    connection_send_message(conn, MSG_SYSTEM_HEARTBEAT, &timestamp, sizeof(timestamp));
    
    LOG_DEBUG("心跳处理完成: 连接ID=%llu", (unsigned long long)conn->id);
}

int connection_process_send_queue(client_connection_t *conn) {
    if (!conn) {
        return -1;
    }
    
    // 这里实现发送队列的处理
    // 由于实现较复杂，这里只提供框架
    
    return 0;
}

void connection_update_activity(client_connection_t *conn) {
    if (!conn) {
        return;
    }
    
    pthread_mutex_lock(&conn->mutex);
    conn->last_activity = get_current_timestamp_ms();
    pthread_mutex_unlock(&conn->mutex);
}

int connection_set_user_info(client_connection_t *conn, uint64_t user_id, 
                            const char *username, const char *nickname) {
    if (!conn) {
        return -1;
    }
    
    pthread_mutex_lock(&conn->mutex);
    
    conn->user_id = user_id;
    
    if (username) {
        strncpy(conn->username, username, sizeof(conn->username) - 1);
        conn->username[sizeof(conn->username) - 1] = '\0';
    }
    
    if (nickname) {
        strncpy(conn->nickname, nickname, sizeof(conn->nickname) - 1);
        conn->nickname[sizeof(conn->nickname) - 1] = '\0';
    }
    
    conn->state = CONNECTION_STATE_AUTHENTICATED;
    
    pthread_mutex_unlock(&conn->mutex);
    
    LOG_INFO("连接用户信息设置: ID=%llu, 用户=%s", 
             (unsigned long long)conn->id, username);
    
    return 0;
}

void connection_clear_user_info(client_connection_t *conn) {
    if (!conn) {
        return;
    }
    
    pthread_mutex_lock(&conn->mutex);
    
    conn->user_id = 0;
    memset(conn->username, 0, sizeof(conn->username));
    memset(conn->nickname, 0, sizeof(conn->nickname));
    conn->state = CONNECTION_STATE_NEW;
    
    pthread_mutex_unlock(&conn->mutex);
    
    LOG_DEBUG("连接用户信息清除: ID=%llu", (unsigned long long)conn->id);
}

int connection_get_info(client_connection_t *conn, connection_info_t *info) {
    if (!conn || !info) {
        return -1;
    }
    
    pthread_mutex_lock(&conn->mutex);
    
    info->connection_id = conn->id;
    info->user_id = conn->user_id;
    info->connected_at = conn->connected_at;
    info->last_activity = conn->last_activity;
    info->state = conn->state;
    info->bytes_sent = conn->bytes_sent;
    info->bytes_received = conn->bytes_received;
    
    strncpy(info->username, conn->username, sizeof(info->username) - 1);
    info->username[sizeof(info->username) - 1] = '\0';
    
    inet_ntop(AF_INET, &conn->client_addr.sin_addr, info->ip_address, 
             sizeof(info->ip_address));
    info->port = ntohs(conn->client_addr.sin_port);
    
    pthread_mutex_unlock(&conn->mutex);
    
    return 0;
}

bool connection_is_alive(client_connection_t *conn, uint64_t timeout_ms) {
    if (!conn) {
        return false;
    }
    
    pthread_mutex_lock(&conn->mutex);
    
    bool alive = (conn->fd != -1 && 
                  conn->state != CONNECTION_STATE_CLOSING &&
                  conn->state != CONNECTION_STATE_CLOSED &&
                  (get_current_timestamp_ms() - conn->last_activity) <= timeout_ms);
    
    pthread_mutex_unlock(&conn->mutex);
    
    return alive;
}

void connection_set_user_data(client_connection_t *conn, void *user_data) {
    if (conn) {
        conn->user_data = user_data;
    }
}

void* connection_get_user_data(client_connection_t *conn) {
    return conn ? conn->user_data : NULL;
}

int connection_enable_ssl(client_connection_t *conn, SSL_CTX *ssl_ctx) {
    if (!conn || !ssl_ctx) {
        return -1;
    }
    
    if (conn->ssl) {
        LOG_WARNING("SSL已经启用");
        return 0;
    }
    
    conn->ssl = SSL_new(ssl_ctx);
    if (!conn->ssl) {
        LOG_ERROR("SSL创建失败");
        return -2;
    }
    
    SSL_set_fd(conn->ssl, conn->fd);
    
    // 执行SSL握手
    int ret = SSL_accept(conn->ssl);
    if (ret <= 0) {
        int err = SSL_get_error(conn->ssl, ret);
        LOG_ERROR("SSL握手失败: %d", err);
        SSL_free(conn->ssl);
        conn->ssl = NULL;
        return -3;
    }
    
    conn->encryption_enabled = true;
    
    LOG_INFO("SSL已启用: 连接ID=%llu, 协议=%s, 加密=%s",
             (unsigned long long)conn->id,
             SSL_get_version(conn->ssl),
             SSL_get_cipher(conn->ssl));
    
    return 0;
}

void connection_disable_ssl(client_connection_t *conn) {
    if (!conn || !conn->ssl) {
        return;
    }
    
    SSL_shutdown(conn->ssl);
    SSL_free(conn->ssl);
    conn->ssl = NULL;
    conn->encryption_enabled = false;
    
    LOG_DEBUG("SSL已禁用: 连接ID=%llu", (unsigned long long)conn->id);
}

// ==================== 消息队列实现 ====================

message_queue_t* message_queue_create(size_t capacity) {
    message_queue_t *queue = (message_queue_t*)calloc(1, sizeof(message_queue_t));
    if (!queue) {
        LOG_ERROR("消息队列内存分配失败");
        return NULL;
    }
    
    queue->messages = (void**)calloc(capacity, sizeof(void*));
    if (!queue->messages) {
        LOG_ERROR("消息数组分配失败");
        free(queue);
        return NULL;
    }
    
    queue->capacity = capacity;
    queue->count = 0;
    queue->head = 0;
    queue->tail = 0;
    
    if (pthread_mutex_init(&queue->lock, NULL) != 0) {
        LOG_ERROR("消息队列互斥锁初始化失败");
        free(queue->messages);
        free(queue);
        return NULL;
    }
    
    if (pthread_cond_init(&queue->not_empty, NULL) != 0) {
        LOG_ERROR("消息队列条件变量初始化失败");
        pthread_mutex_destroy(&queue->lock);
        free(queue->messages);
        free(queue);
        return NULL;
    }
    
    if (pthread_cond_init(&queue->not_full, NULL) != 0) {
        LOG_ERROR("消息队列条件变量初始化失败");
        pthread_cond_destroy(&queue->not_empty);
        pthread_mutex_destroy(&queue->lock);
        free(queue->messages);
        free(queue);
        return NULL;
    }
    
    LOG_DEBUG("消息队列创建成功，容量: %zu", capacity);
    return queue;
}

void message_queue_destroy(message_queue_t *queue) {
    if (!queue) {
        return;
    }
    
    LOG_DEBUG("销毁消息队列...");
    
    // 清空队列
    message_queue_clear(queue);
    
    // 销毁同步原语
    pthread_cond_destroy(&queue->not_full);
    pthread_cond_destroy(&queue->not_empty);
    pthread_mutex_destroy(&queue->lock);
    
    // 释放内存
    free(queue->messages);
    free(queue);
    
    LOG_DEBUG("消息队列已销毁");
}

int message_queue_push(message_queue_t *queue, void *message, int timeout_ms) {
    if (!queue || !message) {
        return -1;
    }
    
    pthread_mutex_lock(&queue->lock);
    
    // 等待队列不满
    struct timespec ts;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (timeout_ms % 1000) * 1000000;
        ts.tv_sec += timeout_ms / 1000 + ts.tv_nsec / 1000000000;
        ts.tv_nsec %= 1000000000;
        
        while (queue->count >= queue->capacity) {
            if (pthread_cond_timedwait(&queue->not_full, &queue->lock, &ts) != 0) {
                pthread_mutex_unlock(&queue->lock);
                return -2; // 超时
            }
        }
    } else {
        while (queue->count >= queue->capacity) {
            pthread_cond_wait(&queue->not_full, &queue->lock);
        }
    }
    
    // 添加消息
    queue->messages[queue->tail] = message;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count++;
    
    // 通知等待的消费者
    pthread_cond_signal(&queue->not_empty);
    
    pthread_mutex_unlock(&queue->lock);
    
    return 0;
}

void* message_queue_pop(message_queue_t *queue, int timeout_ms) {
    if (!queue) {
        return NULL;
    }
    
    pthread_mutex_lock(&queue->lock);
    
    // 等待队列不空
    struct timespec ts;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (timeout_ms % 1000) * 1000000;
        ts.tv_sec += timeout_ms / 1000 + ts.tv_nsec / 1000000000;
        ts.tv_nsec %= 1000000000;
        
        while (queue->count == 0) {
            if (pthread_cond_timedwait(&queue->not_empty, &queue->lock, &ts) != 0) {
                pthread_mutex_unlock(&queue->lock);
                return NULL; // 超时
            }
        }
    } else {
        while (queue->count == 0) {
            pthread_cond_wait(&queue->not_empty, &queue->lock);
        }
    }
    
    // 取出消息
    void *message = queue->messages[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    
    // 通知等待的生产者
    pthread_cond_signal(&queue->not_full);
    
    pthread_mutex_unlock(&queue->lock);
    
    return message;
}

size_t message_queue_size(message_queue_t *queue) {
    if (!queue) {
        return 0;
    }
    
    pthread_mutex_lock(&queue->lock);
    size_t size = queue->count;
    pthread_mutex_unlock(&queue->lock);
    
    return size;
}

void message_queue_clear(message_queue_t *queue) {
    if (!queue) {
        return;
    }
    
    pthread_mutex_lock(&queue->lock);
    
    // 释放所有消息（假设消息需要释放）
    for (size_t i = 0; i < queue->count; i++) {
        size_t index = (queue->head + i) % queue->capacity;
        if (queue->messages[index]) {
            free(queue->messages[index]);
            queue->messages[index] = NULL;
        }
    }
    
    queue->count = 0;
    queue->head = 0;
    queue->tail = 0;
    
    pthread_mutex_unlock(&queue->lock);
    
    LOG_DEBUG("消息队列已清空");
}

// ==================== 工具函数实现 ====================

const char* connection_state_to_string(connection_state_t state) {
    static const char* state_names[] = {
        "新连接",
        "已认证",
        "关闭中",
        "已关闭",
        "错误"
    };
    
    if (state >= sizeof(state_names) / sizeof(state_names[0])) {
        return "未知";
    }
    
    return state_names[state];
}

bool connection_validate_address(struct sockaddr_in *addr) {
    if (!addr) {
        return false;
    }
    
    // 检查IP地址是否有效
    uint32_t ip = ntohl(addr->sin_addr.s_addr);
    
    // 排除私有地址（如果需要）
    // 0.0.0.0/8 - 当前网络
    // 10.0.0.0/8 - 私有网络
    // 127.0.0.0/8 - 环回地址
    // 169.254.0.0/16 - 链路本地
    // 172.16.0.0/12 - 私有网络
    // 192.168.0.0/16 - 私有网络
    
    if ((ip >> 24) == 0 ||           // 0.0.0.0/8
        (ip >> 24) == 10 ||          // 10.0.0.0/8
        (ip >> 24) == 127 ||         // 127.0.0.0/8
        (ip >> 16) == 0xA9FE ||      // 169.254.0.0/16
        (ip >> 20) == 0xAC1 ||       // 172.16.0.0/12
        (ip >> 16) == 0xC0A8) {      // 192.168.0.0/16
        // 可以记录日志但不一定拒绝
        LOG_DEBUG("内部网络地址: %u.%u.%u.%u", 
                 (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                 (ip >> 8) & 0xFF, ip & 0xFF);
    }
    
    // 检查端口是否有效
    if (addr->sin_port == 0) {
        return false;
    }
    
    return true;
}

void connection_init_rate_limiter(client_connection_t *conn, 
                                 uint32_t messages_per_minute, 
                                 uint32_t bytes_per_minute) {
    if (!conn) {
        return;
    }
    
    conn->rate_limiter.messages_per_minute = messages_per_minute;
    conn->rate_limiter.bytes_per_minute = bytes_per_minute;
    conn->rate_limiter.last_reset_time = get_current_timestamp_ms();
    conn->rate_limiter.message_count = 0;
    conn->rate_limiter.byte_count = 0;
    
    LOG_DEBUG("速率限制器初始化: 消息=%u/分钟, 字节=%u/分钟", 
             messages_per_minute, bytes_per_minute);
}

static bool check_rate_limit_reset(client_connection_t *conn) {
    uint64_t now = get_current_timestamp_ms();
    uint64_t elapsed = now - conn->rate_limiter.last_reset_time;
    
    // 每分钟重置一次
    if (elapsed >= 60000) {
        reset_rate_limiter(conn);
        return true;
    }
    
    return false;
}

static void reset_rate_limiter(client_connection_t *conn) {
    conn->rate_limiter.last_reset_time = get_current_timestamp_ms();
    conn->rate_limiter.message_count = 0;
    conn->rate_limiter.byte_count = 0;
}

static void update_rate_limiter(client_connection_t *conn, size_t message_size) {
    check_rate_limit_reset(conn);
    
    conn->rate_limiter.message_count++;
    conn->rate_limiter.byte_count += message_size;
}

bool connection_check_rate_limit(client_connection_t *conn, size_t message_size) {
    if (!conn) {
        return false;
    }
    
    check_rate_limit_reset(conn);
    
    // 检查消息数量限制
    if (conn->rate_limiter.messages_per_minute > 0 &&
        conn->rate_limiter.message_count >= conn->rate_limiter.messages_per_minute) {
        LOG_WARNING("消息数量超过限制: %u/%u", 
                   conn->rate_limiter.message_count, 
                   conn->rate_limiter.messages_per_minute);
        return false;
    }
    
    // 检查字节数限制
    if (conn->rate_limiter.bytes_per_minute > 0 &&
        conn->rate_limiter.byte_count + message_size > conn->rate_limiter.bytes_per_minute) {
        LOG_WARNING("字节数超过限制: %u/%u", 
                   conn->rate_limiter.byte_count + message_size,
                   conn->rate_limiter.bytes_per_minute);
        return false;
    }
    
    return true;
}