#include "client.h"
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
#include <netdb.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

// ==================== 内部函数声明 ====================
static int client_connect_internal(secure_chat_client_t *client);
static void* receive_thread_func(void *arg);
static void* heartbeat_thread_func(void *arg);
static void* reconnect_thread_func(void *arg);
static int send_message_internal(secure_chat_client_t *client, uint32_t type, const void *data, size_t len);
static int receive_message_internal(secure_chat_client_t *client);
static int process_received_data(secure_chat_client_t *client);
static int handle_message(secure_chat_client_t *client, const message_header_t *header, const void *data);
static void update_client_state(secure_chat_client_t *client, client_state_t new_state);
static void cleanup_connection(secure_chat_client_t *client);

// ==================== 客户端创建和销毁 ====================

secure_chat_client_t* client_create(const client_config_t *config) {
    secure_chat_client_t *client = (secure_chat_client_t*)calloc(1, sizeof(secure_chat_client_t));
    if (!client) {
        LOG_ERROR("客户端内存分配失败");
        return NULL;
    }
    
    // 初始化配置
    if (config) {
        memcpy(&client->config, config, sizeof(client_config_t));
    } else {
        client_config_init(&client->config);
    }
    
    // 初始化状态
    client->state = CLIENT_STATE_DISCONNECTED;
    client->target_state = CLIENT_STATE_DISCONNECTED;
    client->connection_active = false;
    client->threads_running = false;
    
    // 初始化互斥锁和条件变量
    if (pthread_mutex_init(&client->state_mutex, NULL) != 0 ||
        pthread_mutex_init(&client->send_mutex, NULL) != 0 ||
        pthread_mutex_init(&client->recv_mutex, NULL) != 0) {
        LOG_ERROR("互斥锁初始化失败");
        free(client);
        return NULL;
    }
    
    if (pthread_cond_init(&client->data_ready, NULL) != 0) {
        LOG_ERROR("条件变量初始化失败");
        pthread_mutex_destroy(&client->state_mutex);
        pthread_mutex_destroy(&client->send_mutex);
        pthread_mutex_destroy(&client->recv_mutex);
        free(client);
        return NULL;
    }
    
    // 初始化接收缓冲区
    client->recv_buffer_size = BUFFER_SIZE;
    client->recv_buffer_used = 0;
    client->recv_buffer = (uint8_t*)malloc(client->recv_buffer_size);
    if (!client->recv_buffer) {
        LOG_ERROR("接收缓冲区分配失败");
        pthread_cond_destroy(&client->data_ready);
        pthread_mutex_destroy(&client->state_mutex);
        pthread_mutex_destroy(&client->send_mutex);
        pthread_mutex_destroy(&client->recv_mutex);
        free(client);
        return NULL;
    }
    
    // 初始化消息处理器
    client->message_handler_capacity = 16;
    client->message_handler_count = 0;
    client->message_handlers = (message_handler_t*)calloc(client->message_handler_capacity, 
                                                          sizeof(message_handler_t));
    if (!client->message_handlers) {
        LOG_ERROR("消息处理器数组分配失败");
        free(client->recv_buffer);
        pthread_cond_destroy(&client->data_ready);
        pthread_mutex_destroy(&client->state_mutex);
        pthread_mutex_destroy(&client->send_mutex);
        pthread_mutex_destroy(&client->recv_mutex);
        free(client);
        return NULL;
    }
    
    // 初始化聊天会话数组
    client->chat_session_capacity = 32;
    client->chat_session_count = 0;
    client->chat_sessions = (chat_session_t*)calloc(client->chat_session_capacity, 
                                                    sizeof(chat_session_t));
    if (!client->chat_sessions) {
        LOG_ERROR("聊天会话数组分配失败");
        free(client->message_handlers);
        free(client->recv_buffer);
        pthread_cond_destroy(&client->data_ready);
        pthread_mutex_destroy(&client->state_mutex);
        pthread_mutex_destroy(&client->send_mutex);
        pthread_mutex_destroy(&client->recv_mutex);
        free(client);
        return NULL;
    }
    
    // 初始化文件传输数组
    client->file_transfer_capacity = 16;
    client->file_transfer_count = 0;
    client->file_transfers = (file_transfer_status_t*)calloc(client->file_transfer_capacity,
                                                             sizeof(file_transfer_status_t));
    if (!client->file_transfers) {
        LOG_ERROR("文件传输数组分配失败");
        free(client->chat_sessions);
        free(client->message_handlers);
        free(client->recv_buffer);
        pthread_cond_destroy(&client->data_ready);
        pthread_mutex_destroy(&client->state_mutex);
        pthread_mutex_destroy(&client->send_mutex);
        pthread_mutex_destroy(&client->recv_mutex);
        free(client);
        return NULL;
    }
    
    // 初始化SSL上下文（如果启用）
    if (client->config.use_ssl) {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        
        client->ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (!client->ssl_ctx) {
            LOG_ERROR("SSL上下文创建失败");
            free(client->file_transfers);
            free(client->chat_sessions);
            free(client->message_handlers);
            free(client->recv_buffer);
            pthread_cond_destroy(&client->data_ready);
            pthread_mutex_destroy(&client->state_mutex);
            pthread_mutex_destroy(&client->send_mutex);
            pthread_mutex_destroy(&client->recv_mutex);
            free(client);
            return NULL;
        }
        
        SSL_CTX_set_verify(client->ssl_ctx, SSL_VERIFY_PEER, NULL);
        SSL_CTX_set_default_verify_paths(client->ssl_ctx);
    }
    
    // 初始化统计信息
    client->messages_sent = 0;
    client->messages_received = 0;
    client->bytes_sent = 0;
    client->bytes_received = 0;
    client->connection_start_time = 0;
    
    LOG_INFO("客户端实例创建成功");
    return client;
}

void client_destroy(secure_chat_client_t *client) {
    if (!client) {
        return;
    }
    
    // 断开连接
    if (client_get_state(client) != CLIENT_STATE_DISCONNECTED) {
        client_disconnect(client, true);
    }
    
    // 等待线程结束
    if (client->threads_running) {
        client->threads_running = false;
        pthread_cond_signal(&client->data_ready);
        
        // 给线程一点时间结束
        usleep(100000); // 100ms
    }
    
    // 清理SSL
    if (client->ssl) {
        SSL_free(client->ssl);
        client->ssl = NULL;
    }
    
    if (client->ssl_ctx) {
        SSL_CTX_free(client->ssl_ctx);
        client->ssl_ctx = NULL;
    }
    
    // 释放缓冲区
    if (client->recv_buffer) {
        free(client->recv_buffer);
        client->recv_buffer = NULL;
    }
    
    // 释放消息处理器数组
    if (client->message_handlers) {
        free(client->message_handlers);
        client->message_handlers = NULL;
    }
    
    // 释放聊天会话数组
    if (client->chat_sessions) {
        free(client->chat_sessions);
        client->chat_sessions = NULL;
    }
    
    // 释放文件传输数组
    if (client->file_transfers) {
        free(client->file_transfers);
        client->file_transfers = NULL;
    }
    
    // 销毁同步原语
    pthread_cond_destroy(&client->data_ready);
    pthread_mutex_destroy(&client->state_mutex);
    pthread_mutex_destroy(&client->send_mutex);
    pthread_mutex_destroy(&client->recv_mutex);
    
    // 释放客户端结构
    free(client);
    
    LOG_INFO("客户端实例已销毁");
}

// ==================== 连接管理 ====================

int client_connect(secure_chat_client_t *client) {
    if (!client) {
        return -1;
    }
    
    // 检查当前状态
    client_state_t current_state = client_get_state(client);
    if (current_state != CLIENT_STATE_DISCONNECTED && 
        current_state != CLIENT_STATE_ERROR) {
        LOG_WARNING("客户端已经在连接状态: %d", current_state);
        return -2;
    }
    
    // 更新状态为连接中
    update_client_state(client, CLIENT_STATE_CONNECTING);
    
    // 执行连接
    int result = client_connect_internal(client);
    if (result != 0) {
        update_client_state(client, CLIENT_STATE_ERROR);
        return result;
    }
    
    // 启动接收线程
    if (pthread_create(&client->receive_thread, NULL, receive_thread_func, client) != 0) {
        LOG_ERROR("接收线程创建失败");
        cleanup_connection(client);
        update_client_state(client, CLIENT_STATE_ERROR);
        return -3;
    }
    
    // 启动心跳线程
    if (pthread_create(&client->heartbeat_thread, NULL, heartbeat_thread_func, client) != 0) {
        LOG_ERROR("心跳线程创建失败");
        client->threads_running = false;
        pthread_join(client->receive_thread, NULL);
        cleanup_connection(client);
        update_client_state(client, CLIENT_STATE_ERROR);
        return -4;
    }
    
    client->threads_running = true;
    
    // 更新状态为已连接
    update_client_state(client, CLIENT_STATE_CONNECTED);
    client->connection_start_time = get_current_timestamp_ms();
    
    return 0;
}

static int client_connect_internal(secure_chat_client_t *client) {
    // 创建socket
    client->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (client->sockfd < 0) {
        LOG_ERROR("socket创建失败: %s", strerror(errno));
        return -1;
    }
    
    // 设置socket选项
    int opt = 1;
    if (setsockopt(client->sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_WARNING("setsockopt SO_REUSEADDR失败: %s", strerror(errno));
    }
    
    // 设置非阻塞模式（暂时）
    int flags = fcntl(client->sockfd, F_GETFL, 0);
    fcntl(client->sockfd, F_SETFL, flags | O_NONBLOCK);
    
    // 解析主机名
    struct hostent *host = gethostbyname(client->config.server_host);
    if (!host) {
        LOG_ERROR("无法解析主机名: %s", client->config.server_host);
        close(client->sockfd);
        client->sockfd = -1;
        return -2;
    }
    
    // 设置服务器地址
    memset(&client->server_addr, 0, sizeof(client->server_addr));
    client->server_addr.sin_family = AF_INET;
    client->server_addr.sin_port = htons(client->config.server_port);
    memcpy(&client->server_addr.sin_addr, host->h_addr, host->h_length);
    
    // 连接服务器
    int connect_result = connect(client->sockfd, (struct sockaddr*)&client->server_addr, 
                                 sizeof(client->server_addr));
    
    if (connect_result < 0 && errno != EINPROGRESS) {
        LOG_ERROR("连接失败: %s", strerror(errno));
        close(client->sockfd);
        client->sockfd = -1;
        return -3;
    }
    
    // 等待连接完成（使用select）
    fd_set writefds;
    struct timeval timeout;
    
    FD_ZERO(&writefds);
    FD_SET(client->sockfd, &writefds);
    
    timeout.tv_sec = client->config.connection_timeout / 1000;
    timeout.tv_usec = (client->config.connection_timeout % 1000) * 1000;
    
    int select_result = select(client->sockfd + 1, NULL, &writefds, NULL, &timeout);
    if (select_result <= 0) {
        LOG_ERROR("连接超时或失败");
        close(client->sockfd);
        client->sockfd = -1;
        return -4;
    }
    
    // 检查socket错误
    int socket_error = 0;
    socklen_t len = sizeof(socket_error);
    if (getsockopt(client->sockfd, SOL_SOCKET, SO_ERROR, &socket_error, &len) < 0) {
        LOG_ERROR("获取socket错误失败: %s", strerror(errno));
        close(client->sockfd);
        client->sockfd = -1;
        return -5;
    }
    
    if (socket_error != 0) {
        LOG_ERROR("连接错误: %s", strerror(socket_error));
        close(client->sockfd);
        client->sockfd = -1;
        return -6;
    }
    
    // 恢复阻塞模式
    fcntl(client->sockfd, F_SETFL, flags);
    
    // 建立SSL连接（如果启用）
    if (client->config.use_ssl && client->ssl_ctx) {
        client->ssl = SSL_new(client->ssl_ctx);
        if (!client->ssl) {
            LOG_ERROR("SSL创建失败");
            close(client->sockfd);
            client->sockfd = -1;
            return -7;
        }
        
        SSL_set_fd(client->ssl, client->sockfd);
        
        if (SSL_connect(client->ssl) <= 0) {
            LOG_ERROR("SSL连接失败");
            SSL_free(client->ssl);
            client->ssl = NULL;
            close(client->sockfd);
            client->sockfd = -1;
            return -8;
        }
        
        LOG_INFO("SSL连接已建立，协议: %s，加密: %s",
                 SSL_get_version(client->ssl),
                 SSL_get_cipher(client->ssl));
    }
    
    client->connection_active = true;
    LOG_INFO("已连接到服务器 %s:%d", 
             client->config.server_host, client->config.server_port);
    
    return 0;
}

int client_disconnect(secure_chat_client_t *client, bool graceful) {
    if (!client) {
        return -1;
    }
    
    client_state_t current_state = client_get_state(client);
    if (current_state == CLIENT_STATE_DISCONNECTED ||
        current_state == CLIENT_STATE_DISCONNECTING) {
        return 0;
    }
    
    // 更新状态为断开中
    update_client_state(client, CLIENT_STATE_DISCONNECTING);
    
    // 发送断开消息（如果优雅断开）
    if (graceful && current_state == CLIENT_STATE_AUTHENTICATED) {
        // 发送退出消息
        response_t logout_msg = {0};
        logout_msg.status_code = 0;
        send_message_internal(client, MSG_AUTH_LOGOUT, &logout_msg, sizeof(logout_msg));
        
        // 等待一小段时间让消息发送
        usleep(100000); // 100ms
    }
    
    // 停止线程
    client->threads_running = false;
    pthread_cond_signal(&client->data_ready);
    
    // 等待线程结束
    if (client->receive_thread) {
        pthread_join(client->receive_thread, NULL);
        client->receive_thread = 0;
    }
    
    if (client->heartbeat_thread) {
        pthread_join(client->heartbeat_thread, NULL);
        client->heartbeat_thread = 0;
    }
    
    if (client->reconnect_thread) {
        pthread_join(client->reconnect_thread, NULL);
        client->reconnect_thread = 0;
    }
    
    // 清理连接
    cleanup_connection(client);
    
    // 更新状态为已断开
    update_client_state(client, CLIENT_STATE_DISCONNECTED);
    
    LOG_INFO("已断开与服务器的连接");
    return 0;
}

static void cleanup_connection(secure_chat_client_t *client) {
    // 关闭SSL连接
    if (client->ssl) {
        SSL_shutdown(client->ssl);
        SSL_free(client->ssl);
        client->ssl = NULL;
    }
    
    // 关闭socket
    if (client->sockfd != -1) {
        close(client->sockfd);
        client->sockfd = -1;
    }
    
    // 重置连接状态
    client->connection_active = false;
    client->recv_buffer_used = 0;
    
    // 重置心跳状态
    client->last_heartbeat_sent = 0;
    client->last_heartbeat_received = 0;
    client->heartbeat_missed = 0;
    
    // 重置重连状态
    client->reconnect_count = 0;
    client->last_connection_attempt = 0;
}

// ==================== 线程函数 ====================

static void* receive_thread_func(void *arg) {
    secure_chat_client_t *client = (secure_chat_client_t*)arg;
    
    LOG_DEBUG("接收线程已启动");
    
    while (client->threads_running && client->connection_active) {
        // 接收数据
        int result = receive_message_internal(client);
        
        if (result < 0) {
            // 接收错误，可能连接已断开
            if (client->threads_running) {
                LOG_ERROR("接收数据失败，连接可能已断开");
                
                // 如果不是主动断开，尝试重连
                if (client->config.reconnect_attempts > 0) {
                    update_client_state(client, CLIENT_STATE_RECONNECTING);
                    
                    // 启动重连线程
                    if (!client->reconnect_thread) {
                        pthread_create(&client->reconnect_thread, NULL, reconnect_thread_func, client);
                    }
                } else {
                    update_client_state(client, CLIENT_STATE_ERROR);
                }
                
                client->connection_active = false;
            }
            break;
        }
    }
    
    LOG_DEBUG("接收线程已退出");
    return NULL;
}

static void* heartbeat_thread_func(void *arg) {
    secure_chat_client_t *client = (secure_chat_client_t*)arg;
    uint64_t heartbeat_interval = client->config.heartbeat_interval;
    
    LOG_DEBUG("心跳线程已启动，间隔: %llu ms", (unsigned long long)heartbeat_interval);
    
    while (client->threads_running && client->connection_active) {
        // 等待心跳间隔
        usleep(heartbeat_interval * 1000);
        
        if (!client->threads_running || !client->connection_active) {
            break;
        }
        
        // 检查是否需要发送心跳
        uint64_t now = get_current_timestamp_ms();
        uint64_t time_since_last = now - client->last_heartbeat_sent;
        
        if (time_since_last >= heartbeat_interval) {
            // 发送心跳
            uint64_t timestamp = now;
            send_message_internal(client, MSG_SYSTEM_HEARTBEAT, &timestamp, sizeof(timestamp));
            
            client->last_heartbeat_sent = now;
            client->heartbeat_missed++;
            
            LOG_DEBUG("心跳已发送，错过次数: %u", client->heartbeat_missed);
            
            // 检查是否错过太多心跳
            if (client->heartbeat_missed > 3) {
                LOG_WARNING("错过太多心跳，连接可能已断开");
                client->connection_active = false;
                break;
            }
        }
        
        // 检查是否收到心跳响应
        if (client->last_heartbeat_received > 0) {
            uint64_t time_since_response = now - client->last_heartbeat_received;
            if (time_since_response > heartbeat_interval * 3) {
                LOG_WARNING("长时间未收到心跳响应");
            }
        }
    }
    
    LOG_DEBUG("心跳线程已退出");
    return NULL;
}

static void* reconnect_thread_func(void *arg) {
    secure_chat_client_t *client = (secure_chat_client_t*)arg;
    
    LOG_INFO("重连线程已启动，最大尝试次数: %d", client->config.reconnect_attempts);
    
    while (client->reconnect_count < client->config.reconnect_attempts && 
           client->threads_running) {
        // 等待重连延迟
        usleep(client->config.reconnect_delay * 1000);
        
        if (!client->threads_running) {
            break;
        }
        
        LOG_INFO("尝试重新连接 (%d/%d)...", 
                 client->reconnect_count + 1, client->config.reconnect_attempts);
        
        // 尝试重连
        int result = client_connect_internal(client);
        if (result == 0) {
            LOG_INFO("重新连接成功");
            
            // 重新启动接收线程
            if (pthread_create(&client->receive_thread, NULL, receive_thread_func, client) == 0) {
                // 重新启动心跳线程
                pthread_create(&client->heartbeat_thread, NULL, heartbeat_thread_func, client);
                
                update_client_state(client, CLIENT_STATE_CONNECTED);
                break;
            }
        }
        
        client->reconnect_count++;
    }
    
    if (client->reconnect_count >= client->config.reconnect_attempts) {
        LOG_ERROR("达到最大重连次数，连接失败");
        update_client_state(client, CLIENT_STATE_ERROR);
    }
    
    client->reconnect_thread = 0;
    LOG_DEBUG("重连线程已退出");
    return NULL;
}

// ==================== 消息发送和接收 ====================

static int send_message_internal(secure_chat_client_t *client, uint32_t type, const void *data, size_t len) {
    if (!client || !client->connection_active || client->sockfd == -1) {
        return -1;
    }
    
    pthread_mutex_lock(&client->send_mutex);
    
    // 创建消息头
    message_header_t header;
    memset(&header, 0, sizeof(header));
    
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = type;
    header.timestamp = get_current_timestamp_ms();
    header.message_id = client->messages_sent + 1;
    header.body_length = len;
    
    // 计算校验和
    uint32_t checksum = calculate_message_checksum(data, len);
    header.checksum = checksum;
    
    // 序列化消息头
    uint8_t header_buffer[sizeof(header)];
    serialize_message_header(&header, header_buffer);
    
    // 发送消息头
    ssize_t sent = 0;
    
    if (client->ssl) {
        sent = SSL_write(client->ssl, header_buffer, sizeof(header_buffer));
    } else {
        sent = send(client->sockfd, header_buffer, sizeof(header_buffer), 0);
    }
    
    if (sent != sizeof(header_buffer)) {
        LOG_ERROR("消息头发送失败: %zd/%zu 字节", sent, sizeof(header_buffer));
        pthread_mutex_unlock(&client->send_mutex);
        return -2;
    }
    
    // 发送消息体
    if (len > 0) {
        if (client->ssl) {
            sent = SSL_write(client->ssl, data, len);
        } else {
            sent = send(client->sockfd, data, len, 0);
        }
        
        if (sent != (ssize_t)len) {
            LOG_ERROR("消息体发送失败: %zd/%zu 字节", sent, len);
            pthread_mutex_unlock(&client->send_mutex);
            return -3;
        }
    }
    
    // 更新统计信息
    client->messages_sent++;
    client->bytes_sent += sizeof(header_buffer) + len;
    
    pthread_mutex_unlock(&client->send_mutex);
    
    LOG_DEBUG("消息已发送: 类型=0x%08x, 长度=%zu", type, len);
    return 0;
}

static int receive_message_internal(secure_chat_client_t *client) {
    if (!client || !client->connection_active || client->sockfd == -1) {
        return -1;
    }
    
    // 接收数据到缓冲区
    ssize_t received = 0;
    
    pthread_mutex_lock(&client->recv_mutex);
    
    // 确保缓冲区有足够空间
    if (client->recv_buffer_used + 4096 > client->recv_buffer_size) {
        // 扩展缓冲区
        size_t new_size = client->recv_buffer_size * 2;
        uint8_t *new_buffer = (uint8_t*)realloc(client->recv_buffer, new_size);
        if (!new_buffer) {
            LOG_ERROR("接收缓冲区扩展失败");
            pthread_mutex_unlock(&client->recv_mutex);
            return -2;
        }
        
        client->recv_buffer = new_buffer;
        client->recv_buffer_size = new_size;
    }
    
    // 接收数据
    if (client->ssl) {
        received = SSL_read(client->ssl, client->recv_buffer + client->recv_buffer_used,
                           client->recv_buffer_size - client->recv_buffer_used);
    } else {
        received = recv(client->sockfd, client->recv_buffer + client->recv_buffer_used,
                       client->recv_buffer_size - client->recv_buffer_used, 0);
    }
    
    if (received <= 0) {
        if (received == 0) {
            LOG_INFO("连接已关闭");
        } else {
            if (client->ssl) {
                int ssl_err = SSL_get_error(client->ssl, received);
                if (ssl_err != SSL_ERROR_WANT_READ && ssl_err != SSL_ERROR_WANT_WRITE) {
                    LOG_ERROR("SSL读取错误: %d", ssl_err);
                }
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOG_ERROR("socket读取错误: %s", strerror(errno));
                }
            }
        }
        
        pthread_mutex_unlock(&client->recv_mutex);
        return -3;
    }
    
    client->recv_buffer_used += received;
    client->bytes_received += received;
    
    pthread_mutex_unlock(&client->recv_mutex);
    
    // 处理接收到的数据
    return process_received_data(client);
}

static int process_received_data(secure_chat_client_t *client) {
    int processed = 0;
    
    pthread_mutex_lock(&client->recv_mutex);
    
    while (client->recv_buffer_used >= sizeof(message_header_t)) {
        // 解析消息头
        message_header_t header;
        if (!deserialize_message_header(client->recv_buffer, &header)) {
            LOG_ERROR("消息头解析失败");
            break;
        }
        
        // 验证魔数和版本
        if (header.magic != PROTOCOL_MAGIC) {
            LOG_ERROR("无效的魔数: 0x%08x", header.magic);
            // 跳过无效数据
            memmove(client->recv_buffer, client->recv_buffer + 1, client->recv_buffer_used - 1);
            client->recv_buffer_used--;
            continue;
        }
        
        if (header.version != PROTOCOL_VERSION) {
            LOG_WARNING("协议版本不匹配: 服务器=%d, 客户端=%d", 
                       header.version, PROTOCOL_VERSION);
        }
        
        // 检查是否有完整的消息
        size_t total_len = sizeof(message_header_t) + header.body_length;
        if (client->recv_buffer_used < total_len) {
            // 数据不完整，等待更多数据
            break;
        }
        
        // 提取消息体
        void *message_body = client->recv_buffer + sizeof(message_header_t);
        
        // 验证校验和
        if (!verify_message_checksum(&header, message_body)) {
            LOG_ERROR("消息校验和错误");
            // 跳过这个无效消息
            memmove(client->recv_buffer, client->recv_buffer + total_len, 
                   client->recv_buffer_used - total_len);
            client->recv_buffer_used -= total_len;
            continue;
        }
        
        // 处理消息
        int result = handle_message(client, &header, message_body);
        if (result != 0) {
            LOG_WARNING("消息处理失败: %d", result);
        }
        
        // 从缓冲区移除已处理的消息
        memmove(client->recv_buffer, client->recv_buffer + total_len, 
               client->recv_buffer_used - total_len);
        client->recv_buffer_used -= total_len;
        
        processed++;
        client->messages_received++;
    }
    
    pthread_mutex_unlock(&client->recv_mutex);
    
    return processed;
}

static int handle_message(secure_chat_client_t *client, const message_header_t *header, const void *data) {
    uint32_t msg_type = header->type;
    
    LOG_DEBUG("收到消息: 类型=0x%08x, 长度=%u", msg_type, header->body_length);
    
    // 处理系统消息
    switch (msg_type) {
        case MSG_SYSTEM_HEARTBEAT:
            client->last_heartbeat_received = get_current_timestamp_ms();
            client->heartbeat_missed = 0;
            
            // 发送心跳响应
            send_message_internal(client, MSG_SYSTEM_HEARTBEAT, data, header->body_length);
            return 0;
            
        case MSG_SYSTEM_ERROR:
            if (client->error_callback) {
                const error_info_t *error = (const error_info_t*)data;
                client->error_callback(error->error_code, error->error_message, 
                                      client->callback_user_data);
            }
            return 0;
            
        case MSG_SYSTEM_NOTIFICATION:
            // 处理系统通知
            LOG_INFO("系统通知: %.*s", (int)header->body_length, (const char*)data);
            return 0;
    }
    
    // 调用注册的消息处理器
    for (size_t i = 0; i < client->message_handler_count; i++) {
        if (client->message_handlers[i].message_type == msg_type) {
            client->message_handlers[i].callback(msg_type, data, header->body_length,
                                                client->message_handlers[i].user_data);
        }
    }
    
    return 0;
}

// ==================== 公共API实现 ====================

int client_login(secure_chat_client_t *client, const char *username, const char *password) {
    if (!client || !username || !password) {
        return -1;
    }
    
    if (client_get_state(client) != CLIENT_STATE_CONNECTED) {
        return -2;
    }
    
    // 更新状态为认证中
    update_client_state(client, CLIENT_STATE_AUTHENTICATING);
    
    // 准备登录消息
    login_msg_t login_msg;
    memset(&login_msg, 0, sizeof(login_msg));
    
    strncpy(login_msg.username, username, MAX_USERNAME_LEN - 1);
    
    // 对密码进行哈希（在实际实现中应该使用安全哈希）
    // 这里使用简单的示例哈希
    password_hash_t hash;
    if (generate_password_hash(password, strlen(password), &hash) != SECURITY_SUCCESS) {
        LOG_ERROR("密码哈希失败");
        update_client_state(client, CLIENT_STATE_CONNECTED);
        return -3;
    }
    
    // 复制哈希后的密码（在实际实现中应该发送哈希值）
    memcpy(login_msg.password, &hash, sizeof(password_hash_t));
    
    login_msg.client_type = 0; // C语言客户端
    login_msg.remember_me = 1;
    
    // 发送登录消息
    int result = send_message_internal(client, MSG_AUTH_LOGIN, &login_msg, sizeof(login_msg));
    if (result != 0) {
        update_client_state(client, CLIENT_STATE_CONNECTED);
        return -4;
    }
    
    // 保存用户名
    strncpy(client->config.username, username, sizeof(client->config.username) - 1);
    
    return 0;
}

int client_send_text_message(secure_chat_client_t *client, uint64_t receiver_id, 
                            const char *message, size_t message_len, uint64_t *message_id) {
    if (!client || !message || message_len == 0) {
        return -1;
    }
    
    if (client_get_state(client) != CLIENT_STATE_AUTHENTICATED) {
        return -2;
    }
    
    // 限制消息长度
    if (message_len > MAX_MESSAGE_LEN) {
        message_len = MAX_MESSAGE_LEN;
    }
    
    // 准备消息内容
    message_content_t *msg = (message_content_t*)malloc(sizeof(message_content_t) + message_len + 1);
    if (!msg) {
        return -3;
    }
    
    memset(msg, 0, sizeof(message_content_t));
    
    msg->sender_id = client->current_user_id;
    msg->receiver_id = receiver_id;
    msg->timestamp = get_current_timestamp_ms();
    msg->message_type = 0; // 文本消息
    msg->content_length = message_len;
    memcpy(msg->content, message, message_len);
    msg->content[message_len] = '\0';
    
    // 计算内容哈希
    calculate_message_checksum(message, message_len);
    
    // 确定消息类型
    uint32_t msg_type = (receiver_id == 0) ? MSG_CHAT_TEXT_SEND : MSG_CHAT_TEXT_SEND;
    
    // 发送消息
    int result = send_message_internal(client, msg_type, msg, sizeof(message_content_t) + message_len);
    
    if (result == 0 && message_id) {
        *message_id = msg->message_id;
    }
    
    free(msg);
    return result;
}

int client_send_file(secure_chat_client_t *client, uint64_t receiver_id, 
                    const char *filepath, uint64_t *transfer_id) {
    if (!client || !filepath) {
        return -1;
    }
    
    if (client_get_state(client) != CLIENT_STATE_AUTHENTICATED) {
        return -2;
    }
    
    // 打开文件
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        LOG_ERROR("无法打开文件: %s", filepath);
        return -3;
    }
    
    // 获取文件大小
    fseek(file, 0, SEEK_END);
    uint64_t file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    if (file_size > MAX_FILE_SIZE) {
        LOG_ERROR("文件太大: %llu 字节 (最大: %u)", 
                 (unsigned long long)file_size, MAX_FILE_SIZE);
        fclose(file);
        return -4;
    }
    
    // 生成传输ID
    uint64_t new_transfer_id = get_current_timestamp_ms() + client->file_transfer_count;
    
    // 添加文件传输记录
    if (client->file_transfer_count >= client->file_transfer_capacity) {
        // 扩展数组
        size_t new_capacity = client->file_transfer_capacity * 2;
        file_transfer_status_t *new_array = (file_transfer_status_t*)realloc(
            client->file_transfers, new_capacity * sizeof(file_transfer_status_t));
        
        if (!new_array) {
            LOG_ERROR("文件传输数组扩展失败");
            fclose(file);
            return -5;
        }
        
        client->file_transfers = new_array;
        client->file_transfer_capacity = new_capacity;
    }
    
    file_transfer_status_t *transfer = &client->file_transfers[client->file_transfer_count++];
    memset(transfer, 0, sizeof(file_transfer_status_t));
    
    transfer->transfer_id = new_transfer_id;
    strncpy(transfer->filename, filepath, sizeof(transfer->filename) - 1);
    transfer->file_size = file_size;
    transfer->transferred = 0;
    transfer->progress = 0;
    transfer->status = 0; // 等待
    transfer->start_time = get_current_timestamp_ms();
    
    // 准备文件传输请求
    file_transfer_request_t request;
    memset(&request, 0, sizeof(request));
    
    request.transfer_id = new_transfer_id;
    request.sender_id = client->current_user_id;
    request.receiver_id = receiver_id;
    
    // 提取文件名
    const char *filename = strrchr(filepath, '/');
    if (filename) {
        filename++;
    } else {
        filename = filepath;
    }
    
    strncpy(request.metadata.filename, filename, MAX_FILENAME_LEN - 1);
    strncpy(request.metadata.original_name, filename, MAX_FILENAME_LEN - 1);
    request.metadata.file_size = file_size;
    request.metadata.uploaded_at = get_current_timestamp_ms();
    
    // 发送文件传输请求
    int result = send_message_internal(client, MSG_FILE_UPLOAD_REQUEST, &request, sizeof(request));
    
    if (result == 0 && transfer_id) {
        *transfer_id = new_transfer_id;
    }
    
    fclose(file);
    return result;
}

// 其他公共API实现（由于篇幅限制，这里只实现关键函数）
// 完整实现需要处理所有API函数...

// ==================== 状态管理 ====================

static void update_client_state(secure_chat_client_t *client, client_state_t new_state) {
    if (!client) {
        return;
    }
    
    pthread_mutex_lock(&client->state_mutex);
    
    client_state_t old_state = client->state;
    client->state = new_state;
    
    pthread_mutex_unlock(&client->state_mutex);
    
    // 调用状态变更回调
    if (client->state_change_callback && old_state != new_state) {
        client->state_change_callback(old_state, new_state, client->callback_user_data);
    }
}

client_state_t client_get_state(const secure_chat_client_t *client) {
    if (!client) {
        return CLIENT_STATE_ERROR;
    }
    
    pthread_mutex_lock((pthread_mutex_t*)&client->state_mutex);
    client_state_t state = client->state;
    pthread_mutex_unlock((pthread_mutex_t*)&client->state_mutex);
    
    return state;
}

// ==================== 消息处理器管理 ====================

int client_register_message_handler(secure_chat_client_t *client, uint32_t message_type,
                                   message_callback_t callback, void *user_data) {
    if (!client || !callback) {
        return -1;
    }
    
    // 检查是否已注册
    for (size_t i = 0; i < client->message_handler_count; i++) {
        if (client->message_handlers[i].message_type == message_type &&
            client->message_handlers[i].callback == callback) {
            // 更新用户数据
            client->message_handlers[i].user_data = user_data;
            return 0;
        }
    }
    
    // 扩展数组（如果需要）
    if (client->message_handler_count >= client->message_handler_capacity) {
        size_t new_capacity = client->message_handler_capacity * 2;
        message_handler_t *new_array = (message_handler_t*)realloc(
            client->message_handlers, new_capacity * sizeof(message_handler_t));
        
        if (!new_array) {
            return -2;
        }
        
        client->message_handlers = new_array;
        client->message_handler_capacity = new_capacity;
    }
    
    // 添加新的处理器
    message_handler_t *handler = &client->message_handlers[client->message_handler_count++];
    handler->message_type = message_type;
    handler->callback = callback;
    handler->user_data = user_data;
    
    return 0;
}

// ==================== 配置函数 ====================

void client_config_init(client_config_t *config) {
    if (!config) {
        return;
    }
    
    memset(config, 0, sizeof(client_config_t));
    
    strcpy(config->server_host, "localhost");
    config->server_port = DEFAULT_PORT;
    config->use_ssl = false;
    config->enable_encryption = false;
    config->connection_timeout = 10000; // 10秒
    config->heartbeat_interval = 30000; // 30秒
    config->reconnect_attempts = 5;
    config->reconnect_delay = 5000; // 5秒
    config->log_level = LOG_LEVEL_INFO;
}

bool client_config_validate(const client_config_t *config, char *error_buffer, size_t error_buffer_size) {
    if (!config) {
        if (error_buffer && error_buffer_size > 0) {
            strncpy(error_buffer, "配置为空", error_buffer_size - 1);
            error_buffer[error_buffer_size - 1] = '\0';
        }
        return false;
    }
    
    // 检查服务器地址
    if (strlen(config->server_host) == 0) {
        if (error_buffer && error_buffer_size > 0) {
            strncpy(error_buffer, "服务器地址不能为空", error_buffer_size - 1);
            error_buffer[error_buffer_size - 1] = '\0';
        }
        return false;
    }
    
    // 检查端口
    if (config->server_port <= 0 || config->server_port > 65535) {
        if (error_buffer && error_buffer_size > 0) {
            snprintf(error_buffer, error_buffer_size, "无效的端口号: %d", config->server_port);
        }
        return false;
    }
    
    // 检查连接超时
    if (config->connection_timeout < 1000) {
        if (error_buffer && error_buffer_size > 0) {
            snprintf(error_buffer, error_buffer_size, "连接超时太短: %dms", config->connection_timeout);
        }
        return false;
    }
    
    // 检查心跳间隔
    if (config->heartbeat_interval < 10000) {
        if (error_buffer && error_buffer_size > 0) {
            snprintf(error_buffer, error_buffer_size, "心跳间隔太短: %dms", config->heartbeat_interval);
        }
        return false;
    }
    
    return true;
}