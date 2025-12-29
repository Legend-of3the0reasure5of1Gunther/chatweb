#include "websocket_server.h"
#include "threadpool.h"
#include "protocol.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/time.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <pthread.h>
#include <ctype.h>
#include <time.h>

// ==================== 内部宏定义 ====================
#define WS_MIN(a, b) ((a) < (b) ? (a) : (b))
#define WS_MAX(a, b) ((a) > (b) ? (a) : (b))

#define LOCK(mutex) pthread_mutex_lock(&(mutex))
#define UNLOCK(mutex) pthread_mutex_unlock(&(mutex))

#define WS_CHECK_NULL(ptr) if ((ptr) == NULL) return -1
#define WS_CHECK_SERVER(server) if ((server) == NULL || !(server)->running) return -1

#define WS_LOG_ERROR(fmt, ...) \
    fprintf(stderr, "[WebSocket Error] %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#define WS_LOG_INFO(fmt, ...) \
    fprintf(stdout, "[WebSocket Info] " fmt "\n", ##__VA_ARGS__)

// ==================== 内部结构 ====================
typedef struct {
    websocket_server_t *server;
    int client_fd;
    struct sockaddr_in client_addr;
} client_connection_t;

// ==================== 静态函数声明 ====================
static void* accept_thread_function(void *arg);
static void* worker_thread_function(void *arg);
static void* cleanup_thread_function(void *arg);
static int handle_client_connection(websocket_server_t *server, int client_fd, 
                                   struct sockaddr_in *client_addr);
static int process_websocket_frame(websocket_server_t *server, 
                                  websocket_client_t *client, 
                                  const uint8_t *data, size_t length);
static int send_websocket_frame(websocket_client_t *client, 
                               websocket_opcode_t opcode, 
                               const uint8_t *payload, size_t payload_len);
static int handle_handshake(websocket_client_t *client, const char *request);
static int create_websocket_response(const char *key, char *response, size_t response_len);
static void remove_client(websocket_server_t *server, uint64_t client_id, 
                         uint16_t status_code, const char *reason);
static void update_client_activity(websocket_client_t *client);
static void check_client_timeout(websocket_server_t *server);
static websocket_client_t* create_client(int fd, struct sockaddr_in *addr);
static void destroy_client(websocket_client_t *client);

// ==================== 工具函数 ====================

/**
 * 设置套接字为非阻塞
 */
static int set_socket_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * 计算SHA1哈希
 */
static void compute_sha1(const char *input, char *output) {
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1((unsigned char*)input, strlen(input), hash);
    
    // 转换为十六进制字符串
    for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
        sprintf(output + (i * 2), "%02x", hash[i]);
    }
    output[SHA_DIGEST_LENGTH * 2] = '\0';
}

/**
 * 获取当前时间（毫秒）
 */
static uint64_t get_current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ==================== 主要函数实现 ====================

/**
 * 获取默认配置
 */
websocket_config_t websocket_get_default_config(void) {
    websocket_config_t config = {
        .port = WEBSOCKET_DEFAULT_PORT,
        .max_clients = 1000,
        .max_message_size = WEBSOCKET_MAX_MESSAGE_SIZE,
        .ping_interval = WEBSOCKET_PING_INTERVAL,
        .timeout = WEBSOCKET_TIMEOUT,
        .buffer_size = 8192,
        .enable_ssl = false,
        .ssl_cert_path = NULL,
        .ssl_key_path = NULL,
        .bind_address = "0.0.0.0",
        .reuse_addr = true,
        .backlog = 128
    };
    return config;
}

/**
 * 创建WebSocket服务器
 */
websocket_server_t* websocket_server_create(const websocket_config_t *config) {
    websocket_server_t *server = (websocket_server_t*)calloc(1, sizeof(websocket_server_t));
    if (!server) {
        WS_LOG_ERROR("Failed to allocate memory for server");
        return NULL;
    }
    
    // 设置配置
    if (config) {
        server->config = *config;
    } else {
        server->config = websocket_get_default_config();
    }
    
    // 初始化客户端数组
    server->max_clients = server->config.max_clients;
    server->clients = (websocket_client_t**)calloc(server->max_clients, sizeof(websocket_client_t*));
    if (!server->clients) {
        WS_LOG_ERROR("Failed to allocate memory for clients array");
        free(server);
        return NULL;
    }
    
    // 初始化互斥锁
    if (pthread_mutex_init(&server->clients_mutex, NULL) != 0 ||
        pthread_mutex_init(&server->stats_mutex, NULL) != 0 ||
        pthread_cond_init(&server->activity_cond, NULL) != 0) {
        WS_LOG_ERROR("Failed to initialize mutexes");
        free(server->clients);
        free(server);
        return NULL;
    }
    
    // 初始化统计
    server->stats.start_time = get_current_time_ms();
    server->next_client_id = 1;
    server->running = false;
    server->shutdown_requested = false;
    
    // 初始化SSL
    if (server->config.enable_ssl) {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        
        server->ssl_ctx = SSL_CTX_new(TLS_server_method());
        if (!server->ssl_ctx) {
            WS_LOG_ERROR("Failed to create SSL context");
            websocket_server_destroy(server);
            return NULL;
        }
        
        // 加载证书和密钥
        if (SSL_CTX_use_certificate_file(server->ssl_ctx, 
                                         server->config.ssl_cert_path, 
                                         SSL_FILETYPE_PEM) <= 0) {
            WS_LOG_ERROR("Failed to load SSL certificate");
            websocket_server_destroy(server);
            return NULL;
        }
        
        if (SSL_CTX_use_PrivateKey_file(server->ssl_ctx, 
                                        server->config.ssl_key_path, 
                                        SSL_FILETYPE_PEM) <= 0) {
            WS_LOG_ERROR("Failed to load SSL private key");
            websocket_server_destroy(server);
            return NULL;
        }
    }
    
    WS_LOG_INFO("WebSocket server created with configuration:");
    WS_LOG_INFO("  Port: %d", server->config.port);
    WS_LOG_INFO("  Max clients: %d", server->config.max_clients);
    WS_LOG_INFO("  Max message size: %u", server->config.max_message_size);
    WS_LOG_INFO("  SSL enabled: %s", server->config.enable_ssl ? "yes" : "no");
    
    return server;
}

/**
 * 启动WebSocket服务器
 */
int websocket_server_start(websocket_server_t *server) {
    WS_CHECK_NULL(server);
    
    if (server->running) {
        WS_LOG_ERROR("Server is already running");
        return -1;
    }
    
    // 创建套接字
    server->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->server_fd < 0) {
        WS_LOG_ERROR("Failed to create socket: %s", strerror(errno));
        return -1;
    }
    
    // 设置套接字选项
    int opt = 1;
    if (server->config.reuse_addr) {
        if (setsockopt(server->server_fd, SOL_SOCKET, SO_REUSEADDR, 
                      &opt, sizeof(opt)) < 0) {
            WS_LOG_ERROR("Failed to set SO_REUSEADDR: %s", strerror(errno));
            close(server->server_fd);
            return -1;
        }
    }
    
    // 绑定地址
    memset(&server->server_addr, 0, sizeof(server->server_addr));
    server->server_addr.sin_family = AF_INET;
    server->server_addr.sin_port = htons(server->config.port);
    
    if (strcmp(server->config.bind_address, "0.0.0.0") == 0) {
        server->server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        server->server_addr.sin_addr.s_addr = inet_addr(server->config.bind_address);
    }
    
    if (bind(server->server_fd, (struct sockaddr*)&server->server_addr, 
             sizeof(server->server_addr)) < 0) {
        WS_LOG_ERROR("Failed to bind socket: %s", strerror(errno));
        close(server->server_fd);
        return -1;
    }
    
    // 开始监听
    if (listen(server->server_fd, server->config.backlog) < 0) {
        WS_LOG_ERROR("Failed to listen on socket: %s", strerror(errno));
        close(server->server_fd);
        return -1;
    }
    
    // 设置套接字为非阻塞
    if (set_socket_nonblocking(server->server_fd) < 0) {
        WS_LOG_ERROR("Failed to set socket non-blocking: %s", strerror(errno));
        close(server->server_fd);
        return -1;
    }
    
    server->running = true;
    
    // 创建线程
    if (pthread_create(&server->accept_thread, NULL, accept_thread_function, server) != 0) {
        WS_LOG_ERROR("Failed to create accept thread");
        server->running = false;
        close(server->server_fd);
        return -1;
    }
    
    if (pthread_create(&server->worker_thread, NULL, worker_thread_function, server) != 0) {
        WS_LOG_ERROR("Failed to create worker thread");
        server->running = false;
        pthread_cancel(server->accept_thread);
        close(server->server_fd);
        return -1;
    }
    
    if (pthread_create(&server->cleanup_thread, NULL, cleanup_thread_function, server) != 0) {
        WS_LOG_ERROR("Failed to create cleanup thread");
        server->running = false;
        pthread_cancel(server->accept_thread);
        pthread_cancel(server->worker_thread);
        close(server->server_fd);
        return -1;
    }
    
    WS_LOG_INFO("WebSocket server started on port %d", server->config.port);
    return 0;
}

/**
 * 停止WebSocket服务器
 */
int websocket_server_stop(websocket_server_t *server) {
    WS_CHECK_NULL(server);
    
    if (!server->running) {
        WS_LOG_ERROR("Server is not running");
        return -1;
    }
    
    server->shutdown_requested = true;
    
    // 等待线程结束
    pthread_join(server->accept_thread, NULL);
    pthread_join(server->worker_thread, NULL);
    pthread_join(server->cleanup_thread, NULL);
    
    // 关闭所有客户端连接
    LOCK(server->clients_mutex);
    for (uint32_t i = 0; i < server->max_clients; i++) {
        if (server->clients[i]) {
            destroy_client(server->clients[i]);
            server->clients[i] = NULL;
        }
    }
    UNLOCK(server->clients_mutex);
    
    // 关闭服务器套接字
    close(server->server_fd);
    server->running = false;
    
    WS_LOG_INFO("WebSocket server stopped");
    return 0;
}

/**
 * 销毁WebSocket服务器
 */
void websocket_server_destroy(websocket_server_t *server) {
    if (!server) return;
    
    if (server->running) {
        websocket_server_stop(server);
    }
    
    // 清理SSL
    if (server->config.enable_ssl && server->ssl_ctx) {
        SSL_CTX_free((SSL_CTX*)server->ssl_ctx);
    }
    
    // 销毁互斥锁和条件变量
    pthread_mutex_destroy(&server->clients_mutex);
    pthread_mutex_destroy(&server->stats_mutex);
    pthread_cond_destroy(&server->activity_cond);
    
    // 释放客户端数组
    free(server->clients);
    
    // 释放服务器
    free(server);
}

/**
 * 接受连接线程函数
 */
static void* accept_thread_function(void *arg) {
    websocket_server_t *server = (websocket_server_t*)arg;
    
    while (server->running && !server->shutdown_requested) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        // 接受新连接
        int client_fd = accept(server->server_fd, 
                              (struct sockaddr*)&client_addr, 
                              &addr_len);
        
        if (client_fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                WS_LOG_ERROR("Accept failed: %s", strerror(errno));
            }
            usleep(1000); // 1ms延迟
            continue;
        }
        
        // 检查是否达到最大客户端数
        LOCK(server->clients_mutex);
        if (server->client_count >= server->max_clients) {
            UNLOCK(server->clients_mutex);
            WS_LOG_INFO("Rejecting connection: maximum clients reached");
            close(client_fd);
            continue;
        }
        UNLOCK(server->clients_mutex);
        
        // 设置客户端套接字为非阻塞
        if (set_socket_nonblocking(client_fd) < 0) {
            WS_LOG_ERROR("Failed to set client socket non-blocking");
            close(client_fd);
            continue;
        }
        
        // 处理客户端连接
        if (handle_client_connection(server, client_fd, &client_addr) != 0) {
            close(client_fd);
        }
    }
    
    return NULL;
}

/**
 * 工作线程函数
 */
static void* worker_thread_function(void *arg) {
    websocket_server_t *server = (websocket_server_t*)arg;
    
    while (server->running && !server->shutdown_requested) {
        // 使用select处理多个客户端
        fd_set read_fds;
        fd_set write_fds;
        int max_fd = 0;
        
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        
        LOCK(server->clients_mutex);
        
        // 设置文件描述符集合
        for (uint32_t i = 0; i < server->max_clients; i++) {
            websocket_client_t *client = server->clients[i];
            if (!client) continue;
            
            if (client->state == WS_CONNECTION_OPEN) {
                FD_SET(client->fd, &read_fds);
                if (max_fd < client->fd) {
                    max_fd = client->fd;
                }
            }
        }
        
        UNLOCK(server->clients_mutex);
        
        // 设置超时
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        // 等待活动
        int activity = select(max_fd + 1, &read_fds, &write_fds, NULL, &timeout);
        
        if (activity < 0 && errno != EINTR) {
            WS_LOG_ERROR("Select error: %s", strerror(errno));
            continue;
        }
        
        if (activity == 0) {
            // 超时，检查客户端超时
            check_client_timeout(server);
            continue;
        }
        
        // 处理活动的客户端
        LOCK(server->clients_mutex);
        
        for (uint32_t i = 0; i < server->max_clients; i++) {
            websocket_client_t *client = server->clients[i];
            if (!client) continue;
            
            if (FD_ISSET(client->fd, &read_fds)) {
                // 读取数据
                uint8_t buffer[server->config.buffer_size];
                ssize_t bytes_read;
                
                // 处理SSL
                if (server->config.enable_ssl && client->user_data) {
                    SSL *ssl = (SSL*)client->user_data;
                    bytes_read = SSL_read(ssl, buffer, sizeof(buffer));
                } else {
                    bytes_read = recv(client->fd, buffer, sizeof(buffer), 0);
                }
                
                if (bytes_read > 0) {
                    // 更新活动时间
                    update_client_activity(client);
                    
                    // 处理数据
                    if (client->state == WS_CONNECTION_CONNECTING) {
                        // 握手阶段
                        buffer[bytes_read] = '\0';
                        handle_handshake(client, (char*)buffer);
                    } else if (client->state == WS_CONNECTION_OPEN) {
                        // WebSocket通信阶段
                        process_websocket_frame(server, client, buffer, bytes_read);
                    }
                    
                    // 更新统计
                    LOCK(server->stats_mutex);
                    server->stats.bytes_received += bytes_read;
                    UNLOCK(server->stats_mutex);
                    
                    client->bytes_received += bytes_read;
                } else if (bytes_read == 0) {
                    // 连接关闭
                    remove_client(server, client->client_id, 
                                 WS_STATUS_NORMAL_CLOSURE, "Connection closed");
                } else {
                    // 读取错误
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        remove_client(server, client->client_id, 
                                     WS_STATUS_ABNORMAL_CLOSURE, "Read error");
                    }
                }
            }
        }
        
        UNLOCK(server->clients_mutex);
    }
    
    return NULL;
}

/**
 * 清理线程函数
 */
static void* cleanup_thread_function(void *arg) {
    websocket_server_t *server = (websocket_server_t*)arg;
    
    while (server->running && !server->shutdown_requested) {
        // 定期发送ping消息
        LOCK(server->clients_mutex);
        
        uint64_t current_time = get_current_time_ms();
        
        for (uint32_t i = 0; i < server->max_clients; i++) {
            websocket_client_t *client = server->clients[i];
            if (!client || client->state != WS_CONNECTION_OPEN) continue;
            
            // 检查是否需要发送ping
            if (current_time - client->last_ping_sent > server->config.ping_interval * 1000) {
                uint8_t ping_frame[2] = {0x89, 0x00}; // FIN=1, opcode=9, mask=0, payload_len=0
                
                if (send(client->fd, ping_frame, 2, 0) > 0) {
                    client->last_ping_sent = current_time;
                    
                    LOCK(server->stats_mutex);
                    server->stats.ping_count++;
                    UNLOCK(server->stats_mutex);
                }
            }
        }
        
        UNLOCK(server->clients_mutex);
        
        // 每秒清理一次
        sleep(1);
    }
    
    return NULL;
}

/**
 * 处理客户端连接
 */
static int handle_client_connection(websocket_server_t *server, int client_fd, 
                                   struct sockaddr_in *client_addr) {
    // 创建客户端
    websocket_client_t *client = create_client(client_fd, client_addr);
    if (!client) {
        return -1;
    }
    
    // 分配客户端ID
    client->client_id = server->next_client_id++;
    
    // 添加到客户端数组
    LOCK(server->clients_mutex);
    
    for (uint32_t i = 0; i < server->max_clients; i++) {
        if (server->clients[i] == NULL) {
            server->clients[i] = client;
            server->client_count++;
            break;
        }
    }
    
    UNLOCK(server->clients_mutex);
    
    // 更新统计
    LOCK(server->stats_mutex);
    server->stats.connections_total++;
    server->stats.connections_active++;
    UNLOCK(server->stats_mutex);
    
    WS_LOG_INFO("New client connected: %s:%d (ID: %lu)", 
                client->client_ip, client->client_port, client->client_id);
    
    return 0;
}

/**
 * 创建客户端
 */
static websocket_client_t* create_client(int fd, struct sockaddr_in *addr) {
    websocket_client_t *client = (websocket_client_t*)calloc(1, sizeof(websocket_client_t));
    if (!client) return NULL;
    
    client->fd = fd;
    client->state = WS_CONNECTION_CONNECTING;
    client->connected_at = get_current_time_ms();
    client->last_activity = client->connected_at;
    
    // 获取客户端IP和端口
    inet_ntop(AF_INET, &addr->sin_addr, client->client_ip, sizeof(client->client_ip));
    client->client_port = ntohs(addr->sin_port);
    
    // 初始化消息缓冲区
    client->message_capacity = 4096;
    client->message_buffer = (uint8_t*)malloc(client->message_capacity);
    if (!client->message_buffer) {
        free(client);
        return NULL;
    }
    
    return client;
}

/**
 * 销毁客户端
 */
static void destroy_client(websocket_client_t *client) {
    if (!client) return;
    
    // 调用用户数据释放函数
    if (client->user_data_free && client->user_data) {
        client->user_data_free(client->user_data);
    }
    
    // 关闭套接字
    if (client->fd > 0) {
        close(client->fd);
    }
    
    // 释放消息缓冲区
    if (client->message_buffer) {
        free(client->message_buffer);
    }
    
    free(client);
}

/**
 * 处理WebSocket握手
 */
static int handle_handshake(websocket_client_t *client, const char *request) {
    char sec_websocket_key[256] = {0};
    char *key_start = strstr(request, "Sec-WebSocket-Key:");
    
    if (!key_start) {
        return -1;
    }
    
    // 提取Sec-WebSocket-Key
    key_start += strlen("Sec-WebSocket-Key:");
    while (*key_start == ' ') key_start++;
    
    char *key_end = strstr(key_start, "\r\n");
    if (!key_end) {
        return -1;
    }
    
    size_t key_len = key_end - key_start;
    if (key_len >= sizeof(sec_websocket_key)) {
        return -1;
    }
    
    strncpy(sec_websocket_key, key_start, key_len);
    sec_websocket_key[key_len] = '\0';
    
    // 保存密钥
    strncpy(client->sec_websocket_key, sec_websocket_key, 
            sizeof(client->sec_websocket_key) - 1);
    
    // 生成握手响应
    char accept_key[29] = {0}; // Base64编码后长度为24，加上null终止符
    char response[1024];
    
    if (websocket_generate_accept_key(sec_websocket_key, accept_key, sizeof(accept_key)) != 0) {
        return -1;
    }
    
    // 创建握手响应
    snprintf(response, sizeof(response),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n"
             "Sec-WebSocket-Protocol: chat\r\n"
             "\r\n",
             accept_key);
    
    // 发送握手响应
    if (send(client->fd, response, strlen(response), 0) <= 0) {
        return -1;
    }
    
    // 更新客户端状态
    client->state = WS_CONNECTION_OPEN;
    update_client_activity(client);
    
    return 0;
}

/**
 * 处理WebSocket帧
 */
static int process_websocket_frame(websocket_server_t *server, 
                                  websocket_client_t *client, 
                                  const uint8_t *data, size_t length) {
    if (length < 2) return -1;
    
    websocket_frame_header_t *header = (websocket_frame_header_t*)data;
    size_t payload_offset = 2;
    uint64_t payload_len = header->payload_len;
    
    // 处理扩展长度
    if (payload_len == 126) {
        if (length < 4) return -1;
        websocket_extended_len_16_t *ext = (websocket_extended_len_16_t*)(data + 2);
        payload_len = ntohs(ext->extended_len);
        payload_offset = 4;
    } else if (payload_len == 127) {
        if (length < 10) return -1;
        websocket_extended_len_64_t *ext = (websocket_extended_len_64_t*)(data + 2);
        payload_len = be64toh(ext->extended_len);
        payload_offset = 10;
    }
    
    // 检查负载长度是否超过限制
    if (payload_len > server->config.max_message_size) {
        WS_LOG_ERROR("Message too large: %lu bytes", payload_len);
        remove_client(server, client->client_id, WS_STATUS_MESSAGE_TOO_BIG, "Message too large");
        return -1;
    }
    
    // 处理掩码
    uint32_t masking_key = 0;
    if (header->mask) {
        if (length < payload_offset + 4) return -1;
        masking_key = *(uint32_t*)(data + payload_offset);
        payload_offset += 4;
    }
    
    // 检查是否有完整的负载
    if (length < payload_offset + payload_len) {
        return -1; // 需要更多数据
    }
    
    // 提取负载数据
    uint8_t *payload = (uint8_t*)data + payload_offset;
    
    // 如果使用了掩码，解码数据
    if (header->mask) {
        for (size_t i = 0; i < payload_len; i++) {
            payload[i] ^= ((uint8_t*)(&masking_key))[i % 4];
        }
    }
    
    // 处理操作码
    switch (header->opcode) {
        case WS_OPCODE_TEXT:
        case WS_OPCODE_BINARY: {
            // 处理文本或二进制消息
            if (header->fin) {
                // 完整消息
                websocket_message_t message = {
                    .opcode = header->opcode,
                    .data = payload,
                    .length = payload_len,
                    .client_id = client->client_id,
                    .timestamp = get_current_time_ms(),
                    .is_final = true
                };
                
                // 调用回调函数
                if (server->on_message) {
                    server->on_message(client, &message);
                }
                
                if (header->opcode == WS_OPCODE_BINARY && server->on_binary) {
                    server->on_binary(client, payload, payload_len);
                }
                
                // 更新统计
                LOCK(server->stats_mutex);
                server->stats.messages_received++;
                UNLOCK(server->stats_mutex);
                
                client->messages_received++;
            } else {
                // 分片消息的开始
                client->opcode = header->opcode;
                client->message_complete = false;
                client->fragment_size = 0;
                
                // 重新分配缓冲区
                if (client->message_capacity < payload_len) {
                    uint8_t *new_buffer = (uint8_t*)realloc(client->message_buffer, payload_len);
                    if (!new_buffer) {
                        WS_LOG_ERROR("Failed to allocate message buffer");
                        return -1;
                    }
                    client->message_buffer = new_buffer;
                    client->message_capacity = payload_len;
                }
                
                // 复制数据
                memcpy(client->message_buffer, payload, payload_len);
                client->message_length = payload_len;
                client->fragment_size = payload_len;
            }
            break;
        }
        
        case WS_OPCODE_CONTINUATION: {
            // 继续帧
            if (client->message_length + payload_len > client->message_capacity) {
                size_t new_capacity = client->message_length + payload_len;
                uint8_t *new_buffer = (uint8_t*)realloc(client->message_buffer, new_capacity);
                if (!new_buffer) {
                    WS_LOG_ERROR("Failed to allocate continuation buffer");
                    return -1;
                }
                client->message_buffer = new_buffer;
                client->message_capacity = new_capacity;
            }
            
            memcpy(client->message_buffer + client->message_length, payload, payload_len);
            client->message_length += payload_len;
            client->fragment_size += payload_len;
            
            if (header->fin) {
                // 分片消息结束
                websocket_message_t message = {
                    .opcode = client->opcode,
                    .data = client->message_buffer,
                    .length = client->message_length,
                    .client_id = client->client_id,
                    .timestamp = get_current_time_ms(),
                    .is_final = true
                };
                
                // 调用回调函数
                if (server->on_message) {
                    server->on_message(client, &message);
                }
                
                if (client->opcode == WS_OPCODE_BINARY && server->on_binary) {
                    server->on_binary(client, client->message_buffer, client->message_length);
                }
                
                // 更新统计
                LOCK(server->stats_mutex);
                server->stats.messages_received++;
                UNLOCK(server->stats_mutex);
                
                client->messages_received++;
                
                // 重置消息状态
                client->message_length = 0;
                client->fragment_size = 0;
                client->message_complete = true;
            }
            break;
        }
        
        case WS_OPCODE_PING: {
            // 发送pong响应
            uint8_t pong_frame[2] = {0x8A, 0x00}; // FIN=1, opcode=10, mask=0
            send(client->fd, pong_frame, 2, 0);
            break;
        }
        
        case WS_OPCODE_PONG: {
            // 更新最后pong时间
            client->last_pong_received = get_current_time_ms();
            
            LOCK(server->stats_mutex);
            server->stats.pong_count++;
            UNLOCK(server->stats_mutex);
            break;
        }
        
        case WS_OPCODE_CLOSE: {
            // 处理关闭帧
            uint16_t status_code = WS_STATUS_NORMAL_CLOSURE;
            char reason[256] = {0};
            
            if (payload_len >= 2) {
                status_code = (payload[0] << 8) | payload[1];
                if (payload_len > 2) {
                    size_t reason_len = WS_MIN(payload_len - 2, sizeof(reason) - 1);
                    memcpy(reason, payload + 2, reason_len);
                    reason[reason_len] = '\0';
                }
            }
            
            remove_client(server, client->client_id, status_code, reason);
            break;
        }
        
        default:
            WS_LOG_ERROR("Unknown opcode: %d", header->opcode);
            break;
    }
    
    return 0;
}

/**
 * 发送WebSocket帧
 */
static int send_websocket_frame(websocket_client_t *client, 
                               websocket_opcode_t opcode, 
                               const uint8_t *payload, size_t payload_len) {
    uint8_t header[14]; // 最大头部大小
    size_t header_len = 0;
    
    // 设置第一个字节
    header[0] = 0x80 | (opcode & 0x0F); // FIN=1, opcode
    
    // 设置负载长度
    if (payload_len <= 125) {
        header[1] = payload_len & 0x7F;
        header_len = 2;
    } else if (payload_len <= 65535) {
        header[1] = 126;
        uint16_t len = htons(payload_len);
        memcpy(header + 2, &len, 2);
        header_len = 4;
    } else {
        header[1] = 127;
        uint64_t len = htobe64(payload_len);
        memcpy(header + 2, &len, 8);
        header_len = 10;
    }
    
    // 发送头部
    ssize_t sent = send(client->fd, header, header_len, 0);
    if (sent <= 0) return -1;
    
    // 发送负载
    if (payload_len > 0) {
        sent = send(client->fd, payload, payload_len, 0);
        if (sent <= 0) return -1;
    }
    
    // 更新统计
    client->bytes_sent += header_len + payload_len;
    client->messages_sent++;
    
    return 0;
}

/**
 * 移除客户端
 */
static void remove_client(websocket_server_t *server, uint64_t client_id, 
                         uint16_t status_code, const char *reason) {
    LOCK(server->clients_mutex);
    
    for (uint32_t i = 0; i < server->max_clients; i++) {
        websocket_client_t *client = server->clients[i];
        if (!client || client->client_id != client_id) continue;
        
        // 发送关闭帧（如果连接仍然打开）
        if (client->state == WS_CONNECTION_OPEN) {
            uint8_t close_frame[128];
            size_t close_frame_len = 2;
            
            close_frame[0] = 0x88; // FIN=1, opcode=8
            close_frame[1] = 0x02; // 负载长度2（状态码）
            
            // 设置状态码
            uint16_t net_status_code = htons(status_code);
            memcpy(close_frame + 2, &net_status_code, 2);
            
            // 添加原因（如果有）
            if (reason && *reason) {
                size_t reason_len = strlen(reason);
                if (reason_len > 125) reason_len = 125; // 限制长度
                
                close_frame[1] = 2 + reason_len;
                memcpy(close_frame + 4, reason, reason_len);
                close_frame_len = 4 + reason_len;
            }
            
            send(client->fd, close_frame, close_frame_len, 0);
        }
        
        // 调用关闭回调
        if (server->on_close) {
            server->on_close(client, status_code, reason);
        }
        
        // 记录日志
        WS_LOG_INFO("Client disconnected: %s:%d (ID: %lu, Code: %d, Reason: %s)",
                   client->client_ip, client->client_port, client->client_id, 
                   status_code, reason ? reason : "none");
        
        // 清理客户端
        destroy_client(client);
        server->clients[i] = NULL;
        server->client_count--;
        
        // 更新统计
        LOCK(server->stats_mutex);
        server->stats.connections_active--;
        server->stats.connections_closed++;
        UNLOCK(server->stats_mutex);
        
        break;
    }
    
    UNLOCK(server->clients_mutex);
}

/**
 * 更新客户端活动时间
 */
static void update_client_activity(websocket_client_t *client) {
    client->last_activity = get_current_time_ms();
}

/**
 * 检查客户端超时
 */
static void check_client_timeout(websocket_server_t *server) {
    uint64_t current_time = get_current_time_ms();
    uint64_t timeout_ms = server->config.timeout * 1000;
    
    LOCK(server->clients_mutex);
    
    for (uint32_t i = 0; i < server->max_clients; i++) {
        websocket_client_t *client = server->clients[i];
        if (!client || client->state != WS_CONNECTION_OPEN) continue;
        
        if (current_time - client->last_activity > timeout_ms) {
            WS_LOG_INFO("Client timeout: %s:%d (ID: %lu)", 
                       client->client_ip, client->client_port, client->client_id);
            remove_client(server, client->client_id, 
                         WS_STATUS_GOING_AWAY, "Connection timeout");
        }
    }
    
    UNLOCK(server->clients_mutex);
}

// ==================== 公共API实现 ====================

/**
 * 发送文本消息
 */
int websocket_send_text(websocket_server_t *server, uint64_t client_id, const char *text) {
    WS_CHECK_SERVER(server);
    if (!text) return -1;
    
    websocket_client_t *client = websocket_get_client(server, client_id);
    if (!client || client->state != WS_CONNECTION_OPEN) {
        return -1;
    }
    
    size_t text_len = strlen(text);
    if (text_len > server->config.max_message_size) {
        return -1;
    }
    
    int result = send_websocket_frame(client, WS_OPCODE_TEXT, (uint8_t*)text, text_len);
    
    // 更新服务器统计
    if (result == 0) {
        LOCK(server->stats_mutex);
        server->stats.messages_sent++;
        server->stats.bytes_sent += text_len;
        UNLOCK(server->stats_mutex);
    }
    
    return result;
}

/**
 * 发送二进制消息
 */
int websocket_send_binary(websocket_server_t *server, uint64_t client_id, 
                         const uint8_t *data, size_t length) {
    WS_CHECK_SERVER(server);
    if (!data || length == 0) return -1;
    
    websocket_client_t *client = websocket_get_client(server, client_id);
    if (!client || client->state != WS_CONNECTION_OPEN) {
        return -1;
    }
    
    if (length > server->config.max_message_size) {
        return -1;
    }
    
    int result = send_websocket_frame(client, WS_OPCODE_BINARY, data, length);
    
    // 更新服务器统计
    if (result == 0) {
        LOCK(server->stats_mutex);
        server->stats.messages_sent++;
        server->stats.bytes_sent += length;
        UNLOCK(server->stats_mutex);
    }
    
    return result;
}

/**
 * 广播文本消息
 */
int websocket_broadcast_text(websocket_server_t *server, const char *text) {
    WS_CHECK_SERVER(server);
    if (!text) return -1;
    
    size_t text_len = strlen(text);
    if (text_len > server->config.max_message_size) {
        return -1;
    }
    
    int success_count = 0;
    
    LOCK(server->clients_mutex);
    
    for (uint32_t i = 0; i < server->max_clients; i++) {
        websocket_client_t *client = server->clients[i];
        if (!client || client->state != WS_CONNECTION_OPEN) continue;
        
        if (send_websocket_frame(client, WS_OPCODE_TEXT, (uint8_t*)text, text_len) == 0) {
            success_count++;
        }
    }
    
    UNLOCK(server->clients_mutex);
    
    // 更新服务器统计
    if (success_count > 0) {
        LOCK(server->stats_mutex);
        server->stats.messages_sent += success_count;
        server->stats.bytes_sent += text_len * success_count;
        UNLOCK(server->stats_mutex);
    }
    
    return success_count;
}

/**
 * 获取客户端
 */
websocket_client_t* websocket_get_client(websocket_server_t *server, uint64_t client_id) {
    WS_CHECK_NULL(server);
    
    LOCK(server->clients_mutex);
    
    for (uint32_t i = 0; i < server->max_clients; i++) {
        websocket_client_t *client = server->clients[i];
        if (client && client->client_id == client_id) {
            UNLOCK(server->clients_mutex);
            return client;
        }
    }
    
    UNLOCK(server->clients_mutex);
    return NULL;
}

/**
 * 获取客户端数量
 */
int websocket_get_client_count(websocket_server_t *server) {
    WS_CHECK_NULL(server);
    
    LOCK(server->clients_mutex);
    int count = server->client_count;
    UNLOCK(server->clients_mutex);
    
    return count;
}

/**
 * 获取统计信息
 */
int websocket_get_stats(websocket_server_t *server, websocket_stats_t *stats) {
    WS_CHECK_NULL(server);
    WS_CHECK_NULL(stats);
    
    LOCK(server->stats_mutex);
    *stats = server->stats;
    stats->connections_active = server->client_count;
    UNLOCK(server->stats_mutex);
    
    return 0;
}

// ==================== 续 websocket_server.c ====================

/**
 * 生成WebSocket接受密钥
 */
int websocket_generate_accept_key(const char *client_key, char *accept_key, size_t accept_key_len) {
    if (!client_key || !accept_key || accept_key_len < 29) return -1;
    
    char combined[512];
    snprintf(combined, sizeof(combined), "%s%s", client_key, WEBSOCKET_MAGIC_GUID);
    
    // 计算SHA1哈希
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1((unsigned char*)combined, strlen(combined), hash);
    
    // Base64编码
    BIO *bmem, *b64;
    BUF_MEM *bptr;
    
    b64 = BIO_new(BIO_f_base64());
    bmem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, bmem);
    
    BIO_write(b64, hash, SHA_DIGEST_LENGTH);
    BIO_flush(b64);
    BIO_get_mem_ptr(b64, &bptr);
    
    // 复制结果
    size_t len = bptr->length < accept_key_len - 1 ? bptr->length : accept_key_len - 1;
    memcpy(accept_key, bptr->data, len);
    accept_key[len] = '\0';
    
    BIO_free_all(b64);
    
    return 0;
}

/**
 * 设置连接回调函数
 */
void websocket_set_connect_callback(websocket_server_t *server, websocket_connect_cb callback) {
    if (server) {
        server->on_connect = callback;
    }
}

/**
 * 设置消息回调函数
 */
void websocket_set_message_callback(websocket_server_t *server, websocket_message_cb callback) {
    if (server) {
        server->on_message = callback;
    }
}

/**
 * 设置关闭回调函数
 */
void websocket_set_close_callback(websocket_server_t *server, websocket_close_cb callback) {
    if (server) {
        server->on_close = callback;
    }
}

/**
 * 设置错误回调函数
 */
void websocket_set_error_callback(websocket_server_t *server, websocket_error_cb callback) {
    if (server) {
        server->on_error = callback;
    }
}

/**
 * 设置二进制消息回调函数
 */
void websocket_set_binary_callback(websocket_server_t *server, websocket_binary_cb callback) {
    if (server) {
        server->on_binary = callback;
    }
}

/**
 * 发送JSON消息
 */
int websocket_send_json(websocket_server_t *server, uint64_t client_id, const char *json) {
    return websocket_send_text(server, client_id, json);
}

/**
 * 广播二进制消息
 */
int websocket_broadcast_binary(websocket_server_t *server, const uint8_t *data, size_t length) {
    WS_CHECK_SERVER(server);
    if (!data || length == 0) return -1;
    
    if (length > server->config.max_message_size) {
        return -1;
    }
    
    int success_count = 0;
    
    LOCK(server->clients_mutex);
    
    for (uint32_t i = 0; i < server->max_clients; i++) {
        websocket_client_t *client = server->clients[i];
        if (!client || client->state != WS_CONNECTION_OPEN) continue;
        
        if (send_websocket_frame(client, WS_OPCODE_BINARY, data, length) == 0) {
            success_count++;
        }
    }
    
    UNLOCK(server->clients_mutex);
    
    // 更新服务器统计
    if (success_count > 0) {
        LOCK(server->stats_mutex);
        server->stats.messages_sent += success_count;
        server->stats.bytes_sent += length * success_count;
        UNLOCK(server->stats_mutex);
    }
    
    return success_count;
}

/**
 * 关闭客户端连接
 */
int websocket_close_client(websocket_server_t *server, uint64_t client_id, 
                          uint16_t status_code, const char *reason) {
    WS_CHECK_SERVER(server);
    
    remove_client(server, client_id, status_code, reason);
    return 0;
}

/**
 * 重置统计信息
 */
void websocket_reset_stats(websocket_server_t *server) {
    if (!server) return;
    
    LOCK(server->stats_mutex);
    memset(&server->stats, 0, sizeof(websocket_stats_t));
    server->stats.start_time = get_current_time_ms();
    UNLOCK(server->stats_mutex);
}

/**
 * 解析WebSocket帧
 */
int websocket_parse_frame(const uint8_t *data, size_t length, 
                         websocket_opcode_t *opcode, uint8_t **payload, 
                         size_t *payload_len, bool *is_final) {
    if (length < 2) return -1;
    
    websocket_frame_header_t *header = (websocket_frame_header_t*)data;
    
    *opcode = header->opcode;
    *is_final = header->fin;
    
    size_t offset = 2;
    uint64_t len = header->payload_len;
    
    // 处理扩展长度
    if (len == 126) {
        if (length < 4) return -1;
        websocket_extended_len_16_t *ext = (websocket_extended_len_16_t*)(data + offset);
        len = ntohs(ext->extended_len);
        offset += 2;
    } else if (len == 127) {
        if (length < 10) return -1;
        websocket_extended_len_64_t *ext = (websocket_extended_len_64_t*)(data + offset);
        len = be64toh(ext->extended_len);
        offset += 8;
    }
    
    *payload_len = len;
    
    // 处理掩码
    if (header->mask) {
        if (length < offset + 4) return -1;
        offset += 4;
    }
    
    // 检查是否有足够的负载数据
    if (length < offset + len) {
        return -2; // 需要更多数据
    }
    
    *payload = (uint8_t*)(data + offset);
    
    // 如果使用了掩码，解码数据
    if (header->mask && *payload_len > 0) {
        uint32_t masking_key = *(uint32_t*)(data + offset - 4);
        for (size_t i = 0; i < *payload_len; i++) {
            (*payload)[i] ^= ((uint8_t*)(&masking_key))[i % 4];
        }
    }
    
    return 0;
}

/**
 * 创建WebSocket帧
 */
int websocket_create_frame(websocket_opcode_t opcode, const uint8_t *payload, 
                          size_t payload_len, uint8_t **frame, size_t *frame_len,
                          bool mask, uint32_t masking_key) {
    size_t header_size = 2;
    uint8_t header[14];
    
    // 设置第一个字节
    header[0] = 0x80 | (opcode & 0x0F); // FIN=1, opcode
    
    // 设置负载长度
    if (payload_len <= 125) {
        header[1] = payload_len & 0x7F;
    } else if (payload_len <= 65535) {
        header[1] = 126;
        uint16_t len = htons(payload_len);
        memcpy(header + 2, &len, 2);
        header_size = 4;
    } else {
        header[1] = 127;
        uint64_t len = htobe64(payload_len);
        memcpy(header + 2, &len, 8);
        header_size = 10;
    }
    
    // 设置掩码标志
    if (mask) {
        header[1] |= 0x80;
        
        // 添加掩码密钥
        memcpy(header + header_size, &masking_key, 4);
        header_size += 4;
    }
    
    // 分配帧缓冲区
    *frame_len = header_size + payload_len;
    *frame = (uint8_t*)malloc(*frame_len);
    if (!*frame) return -1;
    
    // 复制头部
    memcpy(*frame, header, header_size);
    
    // 复制负载（如果需要掩码则应用掩码）
    if (payload_len > 0) {
        if (mask) {
            for (size_t i = 0; i < payload_len; i++) {
                (*frame)[header_size + i] = payload[i] ^ ((uint8_t*)(&masking_key))[i % 4];
            }
        } else {
            memcpy(*frame + header_size, payload, payload_len);
        }
    }
    
    return 0;
}

/**
 * 将WebSocket状态码转换为字符串
 */
const char* websocket_status_code_to_string(uint16_t status_code) {
    switch (status_code) {
        case WS_STATUS_NORMAL_CLOSURE:      return "Normal closure";
        case WS_STATUS_GOING_AWAY:          return "Going away";
        case WS_STATUS_PROTOCOL_ERROR:      return "Protocol error";
        case WS_STATUS_UNSUPPORTED_DATA:    return "Unsupported data";
        case WS_STATUS_NO_STATUS_RECEIVED:  return "No status received";
        case WS_STATUS_ABNORMAL_CLOSURE:    return "Abnormal closure";
        case WS_STATUS_INVALID_PAYLOAD:     return "Invalid payload";
        case WS_STATUS_POLICY_VIOLATION:    return "Policy violation";
        case WS_STATUS_MESSAGE_TOO_BIG:     return "Message too big";
        case WS_STATUS_MISSING_EXTENSION:   return "Missing extension";
        case WS_STATUS_INTERNAL_ERROR:      return "Internal error";
        case WS_STATUS_SERVICE_RESTART:     return "Service restart";
        case WS_STATUS_TRY_AGAIN_LATER:     return "Try again later";
        case WS_STATUS_TLS_HANDSHAKE_FAIL:  return "TLS handshake fail";
        default:                            return "Unknown status code";
    }
}

/**
 * 将WebSocket操作码转换为字符串
 */
const char* websocket_opcode_to_string(websocket_opcode_t opcode) {
    switch (opcode) {
        case WS_OPCODE_CONTINUATION: return "Continuation";
        case WS_OPCODE_TEXT:         return "Text";
        case WS_OPCODE_BINARY:       return "Binary";
        case WS_OPCODE_CLOSE:        return "Close";
        case WS_OPCODE_PING:         return "Ping";
        case WS_OPCODE_PONG:         return "Pong";
        default:                     return "Unknown opcode";
    }
}

/**
 * 处理WebSocket握手
 */
int websocket_handshake(websocket_client_t *client, const char *request) {
    return handle_handshake(client, request);
}