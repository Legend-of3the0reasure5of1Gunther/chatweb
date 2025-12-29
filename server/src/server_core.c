#include "server.h"
#include "../common/protocol.h"
#include "../common/security.h"
#include "../common/logger.h"
#include "../common/buffer.h"
#include "../common/utils.h"
#include "../common/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pwd.h>
#include <grp.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>

// ==================== 内部函数声明 ====================

static int server_init_network(server_instance_t *server);
static int server_init_ssl(server_instance_t *server);
static int server_init_database(server_instance_t *server);
static int server_init_thread_pool(server_instance_t *server);
static int server_init_websocket(server_instance_t *server);
static int server_init_monitoring(server_instance_t *server);
static int server_init_components(server_instance_t *server);
static void server_cleanup_components(server_instance_t *server);

static void* accept_thread_func(void *arg);
static void* cleanup_thread_func(void *arg);
static void* maintenance_thread_func(void *arg);

static void handle_signal(int sig);
static bool set_nonblocking(int fd);
static bool set_socket_options(int fd);
static bool drop_privileges(server_instance_t *server);
static bool daemonize_server(server_instance_t *server);
static bool write_pid_file(server_instance_t *server);
static void remove_pid_file(server_instance_t *server);

static void update_server_state(server_instance_t *server, server_state_t new_state);
static void update_statistics(server_instance_t *server, int stat_type, uint64_t value);
static void log_audit_event(server_instance_t *server, const char *event, 
                           uint64_t user_id, uint64_t conn_id, const char *details);

static int handle_tcp_connection(server_instance_t *server, int fd, struct sockaddr_in *addr);
static int handle_websocket_connection(server_instance_t *server, int fd, struct sockaddr_in *addr);
static void handle_client_event(server_instance_t *server, int fd, uint32_t events);

static bool check_system_limits(server_instance_t *server);
static bool allocate_resources(server_instance_t *server);
static void release_resources(server_instance_t *server);

// ==================== 服务器实例创建和销毁 ====================

server_instance_t* server_create(const server_config_t *config) {
    server_instance_t *server = (server_instance_t*)calloc(1, sizeof(server_instance_t));
    if (!server) {
        LOG_ERROR("服务器内存分配失败");
        return NULL;
    }
    
    // 复制配置
    if (config) {
        memcpy(&server->config, config, sizeof(server_config_t));
    } else {
        server_config_init(&server->config);
    }
    
    // 初始化状态
    server->state = SERVER_STATE_INITIALIZING;
    server->running = false;
    server->shutdown_requested = false;
    
    // 初始化互斥锁和条件变量
    if (pthread_mutex_init(&server->state_mutex, NULL) != 0 ||
        pthread_mutex_init(&server->stats_mutex, NULL) != 0 ||
        pthread_mutex_init(&server->audit_mutex, NULL) != 0) {
        LOG_ERROR("互斥锁初始化失败");
        free(server);
        return NULL;
    }
    
    if (pthread_cond_init(&server->state_cond, NULL) != 0) {
        LOG_ERROR("条件变量初始化失败");
        pthread_mutex_destroy(&server->state_mutex);
        pthread_mutex_destroy(&server->stats_mutex);
        pthread_mutex_destroy(&server->audit_mutex);
        free(server);
        return NULL;
    }
    
    // 初始化统计信息
    memset(&server->stats, 0, sizeof(server_statistics_t));
    server->stats.startup_time = get_current_timestamp_ms();
    
    // 设置信号处理器
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    sigaction(SIGPIPE, SIG_IGN, NULL);
    
    // 忽略SIGPIPE，避免程序因客户端断开而崩溃
    signal(SIGPIPE, SIG_IGN);
    
    LOG_INFO("服务器实例创建成功");
    return server;
}

void server_destroy(server_instance_t *server) {
    if (!server) {
        return;
    }
    
    LOG_INFO("正在销毁服务器实例...");
    
    // 停止服务器
    if (server->running) {
        server_stop(server, true);
    }
    
    // 等待状态变为SHUTDOWN
    if (server->state != SERVER_STATE_SHUTDOWN) {
        int timeout = 10000; // 10秒
        while (timeout > 0 && server->state != SERVER_STATE_SHUTDOWN) {
            usleep(100000); // 100ms
            timeout -= 100;
        }
    }
    
    // 清理组件
    server_cleanup_components(server);
    
    // 清理审计日志
    if (server->audit_log) {
        pthread_mutex_lock(&server->audit_mutex);
        fclose(server->audit_log);
        server->audit_log = NULL;
        pthread_mutex_unlock(&server->audit_mutex);
    }
    
    // 删除PID文件
    remove_pid_file(server);
    
    // 销毁同步原语
    pthread_cond_destroy(&server->state_cond);
    pthread_mutex_destroy(&server->state_mutex);
    pthread_mutex_destroy(&server->stats_mutex);
    pthread_mutex_destroy(&server->audit_mutex);
    
    // 释放服务器结构
    free(server);
    
    LOG_INFO("服务器实例已销毁");
}

// ==================== 服务器初始化 ====================

int server_init(server_instance_t *server) {
    if (!server) {
        return -1;
    }
    
    if (server->state != SERVER_STATE_INITIALIZING) {
        LOG_ERROR("服务器状态无效: %d", server->state);
        return -2;
    }
    
    LOG_INFO("开始初始化服务器...");
    
    // 检查系统限制
    if (!check_system_limits(server)) {
        LOG_ERROR("系统限制检查失败");
        return -3;
    }
    
    // 分配资源
    if (!allocate_resources(server)) {
        LOG_ERROR("资源分配失败");
        return -4;
    }
    
    // 降级权限（如果需要）
    if (server->config.run_as_user && !drop_privileges(server)) {
        LOG_ERROR("权限降级失败");
        return -5;
    }
    
    // 守护进程化（如果需要）
    if (server->config.run_as_daemon && !daemonize_server(server)) {
        LOG_ERROR("守护进程化失败");
        return -6;
    }
    
    // 写入PID文件
    if (server->config.pid_file && !write_pid_file(server)) {
        LOG_ERROR("写入PID文件失败");
        return -7;
    }
    
    // 初始化SSL
    if (server->config.enable_ssl && !server_init_ssl(server)) {
        LOG_ERROR("SSL初始化失败");
        return -8;
    }
    
    // 初始化网络
    if (!server_init_network(server)) {
        LOG_ERROR("网络初始化失败");
        return -9;
    }
    
    // 初始化数据库
    if (!server_init_database(server)) {
        LOG_ERROR("数据库初始化失败");
        return -10;
    }
    
    // 初始化线程池
    if (!server_init_thread_pool(server)) {
        LOG_ERROR("线程池初始化失败");
        return -11;
    }
    
    // 初始化WebSocket
    if (!server_init_websocket(server)) {
        LOG_ERROR("WebSocket初始化失败");
        return -12;
    }
    
    // 初始化监控
    if (server->config.enable_monitoring && !server_init_monitoring(server)) {
        LOG_ERROR("监控初始化失败");
        // 不是致命错误，继续
    }
    
    // 初始化其他组件
    if (!server_init_components(server)) {
        LOG_ERROR("组件初始化失败");
        return -13;
    }
    
    // 更新状态
    update_server_state(server, SERVER_STATE_RUNNING);
    
    // 触发启动事件
    SERVER_TRIGGER_EVENT(server, SERVER_EVENT_STARTUP, 0, 0, 
                        "服务器启动完成", NULL, 0);
    
    LOG_INFO("服务器初始化完成");
    return 0;
}

static int server_init_network(server_instance_t *server) {
    // 创建TCP socket
    server->tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->tcp_fd < 0) {
        LOG_ERROR("TCP socket创建失败: %s", strerror(errno));
        return false;
    }
    
    // 设置socket选项
    if (!set_socket_options(server->tcp_fd)) {
        LOG_ERROR("TCP socket选项设置失败");
        close(server->tcp_fd);
        return false;
    }
    
    // 绑定地址
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(server->config.tcp_port);
    
    if (bind(server->tcp_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("TCP socket绑定失败: %s", strerror(errno));
        close(server->tcp_fd);
        return false;
    }
    
    // 监听
    if (listen(server->tcp_fd, server->config.backlog_size) < 0) {
        LOG_ERROR("TCP socket监听失败: %s", strerror(errno));
        close(server->tcp_fd);
        return false;
    }
    
    // 创建epoll实例
    server->epoll_fd = epoll_create1(0);
    if (server->epoll_fd < 0) {
        LOG_ERROR("epoll创建失败: %s", strerror(errno));
        close(server->tcp_fd);
        return false;
    }
    
    // 添加TCP socket到epoll
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = server->tcp_fd;
    
    if (epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, server->tcp_fd, &ev) < 0) {
        LOG_ERROR("epoll_ctl TCP添加失败: %s", strerror(errno));
        close(server->epoll_fd);
        close(server->tcp_fd);
        return false;
    }
    
    // 设置非阻塞模式
    if (!set_nonblocking(server->tcp_fd)) {
        LOG_ERROR("设置非阻塞模式失败");
        close(server->epoll_fd);
        close(server->tcp_fd);
        return false;
    }
    
    LOG_INFO("TCP服务器初始化完成，端口: %d", server->config.tcp_port);
    return true;
}

static int server_init_ssl(server_instance_t *server) {
    // 初始化OpenSSL
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    
    // 创建SSL上下文
    server->ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (!server->ssl_ctx) {
        LOG_ERROR("SSL上下文创建失败");
        return false;
    }
    
    // 设置选项
    SSL_CTX_set_options(server->ssl_ctx, 
                       SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_mode(server->ssl_ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);
    SSL_CTX_set_mode(server->ssl_ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    
    // 加载证书和私钥
    if (SSL_CTX_use_certificate_file(server->ssl_ctx, 
                                     server->config.ssl_cert_path, 
                                     SSL_FILETYPE_PEM) <= 0) {
        LOG_ERROR("SSL证书加载失败: %s", server->config.ssl_cert_path);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(server->ssl_ctx);
        server->ssl_ctx = NULL;
        return false;
    }
    
    if (SSL_CTX_use_PrivateKey_file(server->ssl_ctx, 
                                    server->config.ssl_key_path, 
                                    SSL_FILETYPE_PEM) <= 0) {
        LOG_ERROR("SSL私钥加载失败: %s", server->config.ssl_key_path);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(server->ssl_ctx);
        server->ssl_ctx = NULL;
        return false;
    }
    
    // 验证私钥匹配
    if (!SSL_CTX_check_private_key(server->ssl_ctx)) {
        LOG_ERROR("SSL证书和私钥不匹配");
        SSL_CTX_free(server->ssl_ctx);
        server->ssl_ctx = NULL;
        return false;
    }
    
    // 加载CA证书（如果需要客户端验证）
    if (server->config.ssl_ca_path) {
        if (SSL_CTX_load_verify_locations(server->ssl_ctx, 
                                          server->config.ssl_ca_path, 
                                          NULL) <= 0) {
            LOG_ERROR("CA证书加载失败: %s", server->config.ssl_ca_path);
            ERR_print_errors_fp(stderr);
        }
    }
    
    LOG_INFO("SSL初始化完成，证书: %s, 私钥: %s", 
             server->config.ssl_cert_path, server->config.ssl_key_path);
    return true;
}

static int server_init_database(server_instance_t *server) {
    int rc = sqlite3_open(server->config.database_path, &server->db_conn);
    if (rc != SQLITE_OK) {
        LOG_ERROR("无法打开数据库: %s", sqlite3_errmsg(server->db_conn));
        return false;
    }
    
    // 设置繁忙超时
    sqlite3_busy_timeout(server->db_conn, 5000);
    
    // 启用外键约束
    sqlite3_exec(server->db_conn, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
    
    // 启用WAL模式（提高并发性能）
    sqlite3_exec(server->db_conn, "PRAGMA journal_mode = WAL;", NULL, NULL, NULL);
    
    // 创建表（如果不存在）
    const char *create_tables_sql = 
        "CREATE TABLE IF NOT EXISTS users ("
        "    user_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    username TEXT UNIQUE NOT NULL,"
        "    password_hash TEXT NOT NULL,"
        "    salt TEXT NOT NULL,"
        "    nickname TEXT,"
        "    email TEXT,"
        "    avatar TEXT,"
        "    status INTEGER DEFAULT 0,"
        "    status_message TEXT,"
        "    last_seen INTEGER,"
        "    created_at INTEGER,"
        "    updated_at INTEGER,"
        "    is_active INTEGER DEFAULT 1,"
        "    login_attempts INTEGER DEFAULT 0,"
        "    lockout_until INTEGER DEFAULT 0"
        ");"
        
        "CREATE TABLE IF NOT EXISTS sessions ("
        "    session_id TEXT PRIMARY KEY,"
        "    user_id INTEGER NOT NULL,"
        "    token TEXT NOT NULL,"
        "    created_at INTEGER,"
        "    expires_at INTEGER,"
        "    ip_address TEXT,"
        "    user_agent TEXT,"
        "    FOREIGN KEY(user_id) REFERENCES users(user_id) ON DELETE CASCADE"
        ");"
        
        "CREATE TABLE IF NOT EXISTS messages ("
        "    message_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    sender_id INTEGER NOT NULL,"
        "    receiver_id INTEGER,"
        "    group_id INTEGER,"
        "    message_type INTEGER NOT NULL,"
        "    content BLOB,"
        "    content_hash TEXT,"
        "    timestamp INTEGER,"
        "    status INTEGER DEFAULT 0,"
        "    delivered_at INTEGER,"
        "    read_at INTEGER,"
        "    FOREIGN KEY(sender_id) REFERENCES users(user_id) ON DELETE CASCADE,"
        "    FOREIGN KEY(receiver_id) REFERENCES users(user_id) ON DELETE CASCADE"
        ");"
        
        "CREATE TABLE IF NOT EXISTS file_transfers ("
        "    transfer_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    sender_id INTEGER NOT NULL,"
        "    receiver_id INTEGER,"
        "    filename TEXT NOT NULL,"
        "    file_path TEXT,"
        "    file_size INTEGER,"
        "    file_hash TEXT,"
        "    status INTEGER DEFAULT 0,"
        "    progress INTEGER DEFAULT 0,"
        "    start_time INTEGER,"
        "    end_time INTEGER,"
        "    error_message TEXT,"
        "    FOREIGN KEY(sender_id) REFERENCES users(user_id) ON DELETE CASCADE,"
        "    FOREIGN KEY(receiver_id) REFERENCES users(user_id) ON DELETE CASCADE"
        ");"
        
        "CREATE TABLE IF NOT EXISTS audit_log ("
        "    audit_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    event_type INTEGER NOT NULL,"
        "    user_id INTEGER,"
        "    connection_id INTEGER,"
        "    ip_address TEXT,"
        "    description TEXT,"
        "    details TEXT,"
        "    timestamp INTEGER"
        ");"
        
        "CREATE INDEX IF NOT EXISTS idx_messages_sender ON messages(sender_id);"
        "CREATE INDEX IF NOT EXISTS idx_messages_receiver ON messages(receiver_id);"
        "CREATE INDEX IF NOT EXISTS idx_messages_timestamp ON messages(timestamp);"
        "CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id);"
        "CREATE INDEX IF NOT EXISTS idx_sessions_expires ON sessions(expires_at);"
        "CREATE INDEX IF NOT EXISTS idx_audit_timestamp ON audit_log(timestamp);";
    
    char *errmsg = NULL;
    rc = sqlite3_exec(server->db_conn, create_tables_sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        LOG_ERROR("创建数据库表失败: %s", errmsg);
        sqlite3_free(errmsg);
        sqlite3_close(server->db_conn);
        server->db_conn = NULL;
        return false;
    }
    
    LOG_INFO("数据库初始化完成: %s", server->config.database_path);
    return true;
}

static int server_init_thread_pool(server_instance_t *server) {
    server->thread_pool = thread_pool_create(server->config.thread_pool_size);
    if (!server->thread_pool) {
        LOG_ERROR("线程池创建失败");
        return false;
    }
    
    LOG_INFO("线程池初始化完成，线程数: %d", server->config.thread_pool_size);
    return true;
}

static int server_init_websocket(server_instance_t *server) {
    server->ws_server = websocket_server_create(server->config.websocket_port);
    if (!server->ws_server) {
        LOG_ERROR("WebSocket服务器创建失败");
        return false;
    }
    
    // 启动WebSocket服务器
    if (!websocket_server_start(server->ws_server)) {
        LOG_ERROR("WebSocket服务器启动失败");
        websocket_server_destroy(server->ws_server);
        server->ws_server = NULL;
        return false;
    }
    
    // 获取WebSocket socket并添加到epoll
    int ws_fd = websocket_server_get_fd(server->ws_server);
    if (ws_fd > 0) {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = ws_fd;
        
        if (epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, ws_fd, &ev) < 0) {
            LOG_ERROR("epoll_ctl WebSocket添加失败: %s", strerror(errno));
        } else {
            server->ws_fd = ws_fd;
        }
    }
    
    LOG_INFO("WebSocket服务器初始化完成，端口: %d", server->config.websocket_port);
    return true;
}

static int server_init_monitoring(server_instance_t *server) {
    server->monitoring = monitoring_server_create(server->config.monitoring_host,
                                                 server->config.monitoring_port);
    if (!server->monitoring) {
        LOG_ERROR("监控服务器创建失败");
        return false;
    }
    
    // 启动监控服务器
    if (!monitoring_server_start(server->monitoring)) {
        LOG_ERROR("监控服务器启动失败");
        monitoring_server_destroy(server->monitoring);
        server->monitoring = NULL;
        return false;
    }
    
    LOG_INFO("监控服务器初始化完成，地址: %s:%d", 
             server->config.monitoring_host, server->config.monitoring_port);
    return true;
}

static int server_init_components(server_instance_t *server) {
    // 初始化连接池
    server->connection_pool = connection_pool_create(server->config.max_clients);
    if (!server->connection_pool) {
        LOG_ERROR("连接池创建失败");
        return false;
    }
    
    // 初始化用户管理器
    server->user_manager = user_manager_create(server->db_conn);
    if (!server->user_manager) {
        LOG_ERROR("用户管理器创建失败");
        return false;
    }
    
    // 初始化消息队列
    server->message_queue = message_queue_create();
    if (!server->message_queue) {
        LOG_ERROR("消息队列创建失败");
        return false;
    }
    
    // 初始化文件传输管理器
    server->file_transfer_mgr = file_transfer_manager_create(server->config.upload_dir);
    if (!server->file_transfer_mgr) {
        LOG_ERROR("文件传输管理器创建失败");
        return false;
    }
    
    // 初始化审计日志
    if (server->config.enable_audit) {
        char audit_log_path[512];
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        snprintf(audit_log_path, sizeof(audit_log_path), 
                "audit-%04d%02d%02d.log", 
                tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
        
        server->audit_log = fopen(audit_log_path, "a");
        if (!server->audit_log) {
            LOG_ERROR("审计日志文件打开失败: %s", audit_log_path);
        } else {
            setbuf(server->audit_log, NULL); // 无缓冲
        }
    }
    
    LOG_INFO("服务器组件初始化完成");
    return true;
}

static void server_cleanup_components(server_instance_t *server) {
    // 销毁文件传输管理器
    if (server->file_transfer_mgr) {
        file_transfer_manager_destroy(server->file_transfer_mgr);
        server->file_transfer_mgr = NULL;
    }
    
    // 销毁消息队列
    if (server->message_queue) {
        message_queue_destroy(server->message_queue);
        server->message_queue = NULL;
    }
    
    // 销毁用户管理器
    if (server->user_manager) {
        user_manager_destroy(server->user_manager);
        server->user_manager = NULL;
    }
    
    // 销毁连接池
    if (server->connection_pool) {
        connection_pool_destroy(server->connection_pool);
        server->connection_pool = NULL;
    }
    
    // 销毁监控服务器
    if (server->monitoring) {
        monitoring_server_destroy(server->monitoring);
        server->monitoring = NULL;
    }
    
    // 销毁WebSocket服务器
    if (server->ws_server) {
        websocket_server_destroy(server->ws_server);
        server->ws_server = NULL;
    }
    
    // 销毁线程池
    if (server->thread_pool) {
        thread_pool_destroy(server->thread_pool);
        server->thread_pool = NULL;
    }
    
    // 关闭数据库
    if (server->db_conn) {
        sqlite3_close(server->db_conn);
        server->db_conn = NULL;
    }
    
    // 销毁SSL上下文
    if (server->ssl_ctx) {
        SSL_CTX_free(server->ssl_ctx);
        server->ssl_ctx = NULL;
    }
    
    // 关闭文件描述符
    if (server->epoll_fd >= 0) {
        close(server->epoll_fd);
        server->epoll_fd = -1;
    }
    
    if (server->tcp_fd >= 0) {
        close(server->tcp_fd);
        server->tcp_fd = -1;
    }
    
    // 释放事件数组
    if (server->events) {
        free(server->events);
        server->events = NULL;
    }
}

// ==================== 服务器启动和停止 ====================

int server_start(server_instance_t *server) {
    if (!server) {
        return -1;
    }
    
    if (server->running) {
        LOG_WARNING("服务器已经在运行");
        return 0;
    }
    
    LOG_INFO("启动服务器...");
    
    // 初始化服务器
    int init_result = server_init(server);
    if (init_result != 0) {
        LOG_ERROR("服务器初始化失败: %d", init_result);
        return init_result;
    }
    
    // 启动接受线程
    if (pthread_create(&server->accept_thread, NULL, accept_thread_func, server) != 0) {
        LOG_ERROR("接受线程创建失败");
        return -2;
    }
    
    // 启动清理线程
    if (pthread_create(&server->cleanup_thread, NULL, cleanup_thread_func, server) != 0) {
        LOG_ERROR("清理线程创建失败");
        server->running = false;
        pthread_join(server->accept_thread, NULL);
        return -3;
    }
    
    // 启动维护线程
    if (pthread_create(&server->maintenance_thread, NULL, maintenance_thread_func, server) != 0) {
        LOG_ERROR("维护线程创建失败");
        server->running = false;
        pthread_cancel(server->cleanup_thread);
        pthread_join(server->accept_thread, NULL);
        pthread_join(server->cleanup_thread, NULL);
        return -4;
    }
    
    server->running = true;
    
    // 更新统计信息
    server->stats.startup_time = get_current_timestamp_ms();
    
    LOG_INFO("服务器启动成功");
    SERVER_TRIGGER_EVENT(server, SERVER_EVENT_STARTUP, 0, 0, 
                        "服务器启动成功", NULL, 0);
    
    return 0;
}

int server_stop(server_instance_t *server, bool graceful) {
    if (!server) {
        return -1;
    }
    
    if (!server->running) {
        LOG_WARNING("服务器没有在运行");
        return 0;
    }
    
    LOG_INFO("正在停止服务器%s...", graceful ? "（优雅关闭）" : "");
    
    // 设置关闭请求标志
    server->shutdown_requested = true;
    server->running = false;
    
    // 更新状态
    update_server_state(server, SERVER_STATE_SHUTTING_DOWN);
    
    if (graceful) {
        // 停止接受新连接
        if (server->tcp_fd >= 0) {
            shutdown(server->tcp_fd, SHUT_RDWR);
        }
        
        // 通知所有客户端服务器正在关闭
        const char *shutdown_msg = "服务器正在关闭，请保存您的工作";
        server_broadcast_message(server, MSG_SYSTEM_NOTIFICATION, 
                                shutdown_msg, strlen(shutdown_msg), 0);
        
        // 等待所有连接关闭（最多30秒）
        int timeout = 30000; // 30秒
        while (timeout > 0 && server->connection_pool &&
               connection_pool_get_active_count(server->connection_pool) > 0) {
            usleep(100000); // 100ms
            timeout -= 100;
        }
        
        if (timeout <= 0) {
            LOG_WARNING("等待连接关闭超时，强制关闭");
        }
    }
    
    // 停止线程
    pthread_cancel(server->maintenance_thread);
    pthread_cancel(server->cleanup_thread);
    pthread_cancel(server->accept_thread);
    
    // 等待线程结束
    pthread_join(server->accept_thread, NULL);
    pthread_join(server->cleanup_thread, NULL);
    pthread_join(server->maintenance_thread, NULL);
    
    // 清理资源
    server_cleanup_components(server);
    
    // 更新状态
    update_server_state(server, SERVER_STATE_SHUTDOWN);
    
    LOG_INFO("服务器已停止");
    SERVER_TRIGGER_EVENT(server, SERVER_EVENT_SHUTDOWN, 0, 0, 
                        "服务器已停止", NULL, 0);
    
    return 0;
}

// ==================== 线程函数 ====================

static void* accept_thread_func(void *arg) {
    server_instance_t *server = (server_instance_t*)arg;
    
    LOG_DEBUG("接受线程已启动");
    
    // 分配事件数组
    int max_events = 64;
    server->events = (struct epoll_event*)malloc(max_events * sizeof(struct epoll_event));
    if (!server->events) {
        LOG_ERROR("事件数组分配失败");
        return NULL;
    }
    
    while (server->running) {
        // 等待事件
        int nfds = epoll_wait(server->epoll_fd, server->events, max_events, 100);
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_ERROR("epoll_wait失败: %s", strerror(errno));
            break;
        }
        
        // 处理事件
        for (int i = 0; i < nfds; i++) {
            int fd = server->events[i].data.fd;
            uint32_t events = server->events[i].events;
            
            if (fd == server->tcp_fd) {
                // 接受新的TCP连接
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                
                int client_fd = accept(server->tcp_fd, (struct sockaddr*)&client_addr, &addr_len);
                if (client_fd < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        LOG_ERROR("接受连接失败: %s", strerror(errno));
                    }
                    continue;
                }
                
                // 处理新连接
                handle_tcp_connection(server, client_fd, &client_addr);
            }
            else if (fd == server->ws_fd) {
                // 处理WebSocket连接
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                
                int client_fd = accept(fd, (struct sockaddr*)&client_addr, &addr_len);
                if (client_fd < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        LOG_ERROR("接受WebSocket连接失败: %s", strerror(errno));
                    }
                    continue;
                }
                
                // 处理WebSocket连接
                handle_websocket_connection(server, client_fd, &client_addr);
            }
            else {
                // 处理客户端事件
                handle_client_event(server, fd, events);
            }
        }
        
        // 检查是否需要扩展事件数组
        if (nfds == max_events) {
            int new_max = max_events * 2;
            struct epoll_event *new_events = realloc(server->events, 
                                                     new_max * sizeof(struct epoll_event));
            if (new_events) {
                server->events = new_events;
                max_events = new_max;
            }
        }
    }
    
    free(server->events);
    server->events = NULL;
    
    LOG_DEBUG("接受线程已退出");
    return NULL;
}

static void* cleanup_thread_func(void *arg) {
    server_instance_t *server = (server_instance_t*)arg;
    
    LOG_DEBUG("清理线程已启动");
    
    while (server->running) {
        // 每隔10秒清理一次
        sleep(10);
        
        if (!server->running) {
            break;
        }
        
        // 清理过期会话
        if (server->connection_pool) {
            size_t cleaned = connection_pool_cleanup_expired(server->connection_pool,
                                                           server->config.session_timeout);
            if (cleaned > 0) {
                LOG_DEBUG("清理了 %zu 个过期连接", cleaned);
            }
        }
        
        // 清理临时文件
        if (server->file_transfer_mgr) {
            size_t files_cleaned = file_transfer_manager_cleanup_temp_files(server->file_transfer_mgr);
            if (files_cleaned > 0) {
                LOG_DEBUG("清理了 %zu 个临时文件", files_cleaned);
            }
        }
    }
    
    LOG_DEBUG("清理线程已退出");
    return NULL;
}

static void* maintenance_thread_func(void *arg) {
    server_instance_t *server = (server_instance_t*)arg;
    
    LOG_DEBUG("维护线程已启动");
    
    // 维护间隔：1小时
    const int maintenance_interval = 3600;
    
    while (server->running) {
        sleep(maintenance_interval);
        
        if (!server->running) {
            break;
        }
        
        LOG_INFO("执行定期维护任务...");
        
        // 执行维护任务
        server_perform_maintenance(server);
        
        // 更新统计信息
        server->stats.uptime = get_current_timestamp_ms() - server->stats.startup_time;
        
        // 触发维护事件
        SERVER_TRIGGER_EVENT(server, SERVER_EVENT_MAINTENANCE, 0, 0, 
                            "定期维护任务完成", NULL, 0);
    }
    
    LOG_DEBUG("维护线程已退出");
    return NULL;
}

// ==================== 连接处理 ====================

static int handle_tcp_connection(server_instance_t *server, int fd, struct sockaddr_in *addr) {
    // 检查最大连接数
    if (server->connection_pool && 
        connection_pool_get_active_count(server->connection_pool) >= server->config.max_clients) {
        LOG_WARNING("达到最大连接数限制，拒绝新连接");
        close(fd);
        
        update_statistics(server, STAT_TYPE_CONNECTIONS_REJECTED, 1);
        return -1;
    }
    
    // 设置非阻塞模式
    if (!set_nonblocking(fd)) {
        LOG_ERROR("设置非阻塞模式失败");
        close(fd);
        return -2;
    }
    
    // 设置socket选项
    if (!set_socket_options(fd)) {
        LOG_ERROR("设置socket选项失败");
        close(fd);
        return -3;
    }
    
    // 创建连接
    client_connection_t *conn = connection_create(server->connection_pool, fd, addr);
    if (!conn) {
        LOG_ERROR("创建连接失败");
        close(fd);
        return -4;
    }
    
    // 设置SSL（如果启用）
    if (server->config.enable_ssl && server->ssl_ctx) {
        conn->ssl = SSL_new(server->ssl_ctx);
        if (conn->ssl) {
            SSL_set_fd(conn->ssl, fd);
            conn->encryption_enabled = true;
        }
    }
    
    // 添加到epoll
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    ev.data.fd = fd;
    
    if (epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        LOG_ERROR("epoll_ctl添加客户端失败: %s", strerror(errno));
        connection_close(server->connection_pool, conn);
        return -5;
    }
    
    // 更新统计信息
    update_statistics(server, STAT_TYPE_CONNECTIONS_TOTAL, 1);
    update_statistics(server, STAT_TYPE_CONNECTIONS_ACTIVE, 1);
    
    uint64_t active_count = connection_pool_get_active_count(server->connection_pool);
    if (active_count > server->stats.max_concurrent_connections) {
        server->stats.max_concurrent_connections = active_count;
    }
    
    // 记录审计日志
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, client_ip, sizeof(client_ip));
    
    log_audit_event(server, "CLIENT_CONNECT", 0, conn->id, 
                   client_ip);
    
    // 触发事件
    SERVER_TRIGGER_EVENT(server, SERVER_EVENT_CLIENT_CONNECT, 0, conn->id,
                        "客户端连接", client_ip, strlen(client_ip));
    
    LOG_INFO("新TCP连接: %s:%d (连接ID: %lu)", 
             client_ip, ntohs(addr->sin_port), conn->id);
    
    return 0;
}

static int handle_websocket_connection(server_instance_t *server, int fd, struct sockaddr_in *addr) {
    // WebSocket连接通过WebSocket服务器处理
    // 这里只记录日志
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, client_ip, sizeof(client_ip));
    
    LOG_INFO("新WebSocket连接: %s:%d", client_ip, ntohs(addr->sin_port));
    
    return 0;
}

static void handle_client_event(server_instance_t *server, int fd, uint32_t events) {
    // 获取连接
    client_connection_t *conn = connection_pool_get_by_fd(server->connection_pool, fd);
    if (!conn) {
        LOG_WARNING("找不到连接对应的fd: %d", fd);
        return;
    }
    
    // 检查连接关闭
    if (events & EPOLLRDHUP || events & EPOLLHUP) {
        LOG_DEBUG("连接 %lu 关闭", conn->id);
        connection_close(server->connection_pool, conn);
        return;
    }
    
    // 处理错误
    if (events & EPOLLERR) {
        LOG_WARNING("连接 %lu 发生错误", conn->id);
        connection_close(server->connection_pool, conn);
        return;
    }
    
    // 处理可读事件
    if (events & EPOLLIN) {
        // 读取数据
        uint8_t buffer[8192];
        ssize_t n = connection_receive(conn, buffer, sizeof(buffer));
        
        if (n > 0) {
            // 处理接收到的数据
            connection_process_data(conn, buffer, n);
            
            // 更新统计信息
            update_statistics(server, STAT_TYPE_NETWORK_IN, n);
            update_statistics(server, STAT_TYPE_MESSAGES_RECEIVED, 1);
        }
        else if (n == 0) {
            // 连接关闭
            LOG_DEBUG("连接 %lu 正常关闭", conn->id);
            connection_close(server->connection_pool, conn);
        }
        else {
            // 读取错误
            LOG_WARNING("连接 %lu 读取错误", conn->id);
            connection_close(server->connection_pool, conn);
        }
    }
    
    // 处理可写事件（如果需要）
    if (events & EPOLLOUT) {
        // 处理待发送数据
        connection_process_send_queue(conn);
    }
}

// ==================== 辅助函数 ====================

static void handle_signal(int sig) {
    LOG_INFO("收到信号: %d", sig);
    
    // 这里可以通过全局变量通知服务器
    // 实际处理在服务器主循环中
}

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return false;
    }
    
    return true;
}

static bool set_socket_options(int fd) {
    int opt = 1;
    
    // 设置SO_REUSEADDR
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        return false;
    }
    
    // 设置TCP_NODELAY（禁用Nagle算法）
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        // 不是致命错误
    }
    
    // 设置SO_KEEPALIVE
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) < 0) {
        // 不是致命错误
    }
    
    // 设置接收和发送缓冲区大小
    int buf_size = 65536;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    
    return true;
}

static bool drop_privileges(server_instance_t *server) {
    if (!server->config.run_as_user) {
        return true;
    }
    
    struct passwd *pw = getpwnam(server->config.run_as_user);
    if (!pw) {
        LOG_ERROR("用户 %s 不存在", server->config.run_as_user);
        return false;
    }
    
    // 设置组ID
    if (setgid(pw->pw_gid) != 0) {
        LOG_ERROR("设置组ID失败: %s", strerror(errno));
        return false;
    }
    
    // 设置用户ID
    if (setuid(pw->pw_uid) != 0) {
        LOG_ERROR("设置用户ID失败: %s", strerror(errno));
        return false;
    }
    
    // 设置umask
    umask(0077);
    
    LOG_INFO("权限已降级为用户: %s (uid=%d, gid=%d)", 
             server->config.run_as_user, pw->pw_uid, pw->pw_gid);
    return true;
}

static bool daemonize_server(server_instance_t *server) {
    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("fork失败: %s", strerror(errno));
        return false;
    }
    
    if (pid > 0) {
        // 父进程退出
        exit(0);
    }
    
    // 子进程成为会话领导
    if (setsid() < 0) {
        LOG_ERROR("setsid失败: %s", strerror(errno));
        return false;
    }
    
    // 再次fork确保不是会话领导
    pid = fork();
    if (pid < 0) {
        LOG_ERROR("第二次fork失败: %s", strerror(errno));
        return false;
    }
    
    if (pid > 0) {
        exit(0);
    }
    
    // 关闭标准文件描述符
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    // 重定向到/dev/null
    int fd = open("/dev/null", O_RDWR);
    if (fd != STDIN_FILENO) {
        LOG_ERROR("无法重定向标准输入");
        return false;
    }
    
    if (dup2(STDIN_FILENO, STDOUT_FILENO) != STDOUT_FILENO) {
        LOG_ERROR("无法重定向标准输出");
        return false;
    }
    
    if (dup2(STDIN_FILENO, STDERR_FILENO) != STDERR_FILENO) {
        LOG_ERROR("无法重定向标准错误");
        return false;
    }
    
    // 改变工作目录
    if (chdir("/") < 0) {
        LOG_ERROR("chdir失败: %s", strerror(errno));
        return false;
    }
    
    LOG_INFO("服务器已守护进程化");
    return true;
}

static bool write_pid_file(server_instance_t *server) {
    if (!server->config.pid_file) {
        return true;
    }
    
    FILE *pidfile = fopen(server->config.pid_file, "w");
    if (!pidfile) {
        LOG_ERROR("无法打开PID文件: %s", server->config.pid_file);
        return false;
    }
    
    fprintf(pidfile, "%d\n", getpid());
    fclose(pidfile);
    
    LOG_INFO("PID写入文件: %s", server->config.pid_file);
    return true;
}

static void remove_pid_file(server_instance_t *server) {
    if (server->config.pid_file && access(server->config.pid_file, F_OK) == 0) {
        if (unlink(server->config.pid_file) == 0) {
            LOG_INFO("PID文件已删除: %s", server->config.pid_file);
        } else {
            LOG_WARNING("无法删除PID文件: %s", server->config.pid_file);
        }
    }
}

static void update_server_state(server_instance_t *server, server_state_t new_state) {
    pthread_mutex_lock(&server->state_mutex);
    
    server_state_t old_state = server->state;
    server->state = new_state;
    
    pthread_cond_broadcast(&server->state_cond);
    pthread_mutex_unlock(&server->state_mutex);
    
    LOG_DEBUG("服务器状态变更: %d -> %d", old_state, new_state);
}

static void update_statistics(server_instance_t *server, int stat_type, uint64_t value) {
    pthread_mutex_lock(&server->stats_mutex);
    
    switch (stat_type) {
        case STAT_TYPE_CONNECTIONS_TOTAL:
            server->stats.total_connections += value;
            break;
        case STAT_TYPE_CONNECTIONS_ACTIVE:
            server->stats.active_connections += value;
            break;
        case STAT_TYPE_CONNECTIONS_REJECTED:
            server->stats.rejected_connections += value;
            break;
        case STAT_TYPE_MESSAGES_RECEIVED:
            server->stats.messages_received += value;
            break;
        case STAT_TYPE_MESSAGES_SENT:
            server->stats.messages_sent += value;
            break;
        case STAT_TYPE_NETWORK_IN:
            server->stats.network_in_bytes += value;
            break;
        case STAT_TYPE_NETWORK_OUT:
            server->stats.network_out_bytes += value;
            break;
        case STAT_TYPE_FILE_TRANSFERS:
            server->stats.file_transfers += value;
            break;
        case STAT_TYPE_FILE_TRANSFER_BYTES:
            server->stats.file_transfer_bytes += value;
            break;
    }
    
    pthread_mutex_unlock(&server->stats_mutex);
}

static void log_audit_event(server_instance_t *server, const char *event, 
                           uint64_t user_id, uint64_t conn_id, const char *details) {
    if (!server->audit_log) {
        return;
    }
    
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm);
    
    pthread_mutex_lock(&server->audit_mutex);
    fprintf(server->audit_log, "[%s] EVENT=%s USER=%llu CONN=%llu DETAILS=%s\n",
            timestamp, event, (unsigned long long)user_id, 
            (unsigned long long)conn_id, details ? details : "");
    pthread_mutex_unlock(&server->audit_mutex);
}

static bool check_system_limits(server_instance_t *server) {
    struct rlimit rlim;
    
    // 检查文件描述符限制
    if (getrlimit(RLIMIT_NOFILE, &rlim) == 0) {
        uint64_t required_fds = server->config.max_clients + 100; // 额外100个用于系统
        if (rlim.rlim_cur < required_fds) {
            LOG_WARNING("文件描述符限制可能不足: 当前=%lu, 需要=%lu", 
                       (unsigned long)rlim.rlim_cur, (unsigned long)required_fds);
            
            // 尝试提高限制
            rlim.rlim_cur = required_fds * 2;
            rlim.rlim_max = required_fds * 2;
            
            if (setrlimit(RLIMIT_NOFILE, &rlim) != 0) {
                LOG_ERROR("无法提高文件描述符限制");
                return false;
            }
        }
    }
    
    // 检查进程限制
    if (getrlimit(RLIMIT_NPROC, &rlim) == 0) {
        if (rlim.rlim_cur < 1000) {
            LOG_WARNING("进程限制较低: %lu", (unsigned long)rlim.rlim_cur);
        }
    }
    
    return true;
}

static bool allocate_resources(server_instance_t *server) {
    // 这里可以分配其他资源
    return true;
}

static void release_resources(server_instance_t *server) {
    // 这里可以释放其他资源
}

// ==================== 公共API实现 ====================

int server_reload_config(server_instance_t *server, const server_config_t *new_config) {
    if (!server || !new_config) {
        return -1;
    }
    
    if (!server->running) {
        LOG_ERROR("服务器没有运行，无法重载配置");
        return -2;
    }
    
    LOG_INFO("重新加载服务器配置...");
    
    // 验证新配置
    char error_buffer[256];
    if (!server_config_validate(new_config, error_buffer, sizeof(error_buffer))) {
        LOG_ERROR("配置验证失败: %s", error_buffer);
        return -3;
    }
    
    // 复制配置
    memcpy(&server->config, new_config, sizeof(server_config_t));
    
    // 应用配置更改（这里需要根据实际配置项处理）
    // 例如：更新日志级别、重新打开日志文件等
    
    // 触发配置重载事件
    SERVER_TRIGGER_EVENT(server, SERVER_EVENT_CONFIG_RELOAD, 0, 0, 
                        "配置重载完成", NULL, 0);
    
    LOG_INFO("配置重载完成");
    return 0;
}

server_state_t server_get_state(const server_instance_t *server) {
    if (!server) {
        return SERVER_STATE_ERROR;
    }
    
    pthread_mutex_lock((pthread_mutex_t*)&server->state_mutex);
    server_state_t state = server->state;
    pthread_mutex_unlock((pthread_mutex_t*)&server->state_mutex);
    
    return state;
}

int server_wait_for_state(server_instance_t *server, server_state_t target_state, int timeout_ms) {
    if (!server) {
        return -1;
    }
    
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    ts.tv_sec += timeout_ms / 1000 + ts.tv_nsec / 1000000000;
    ts.tv_nsec %= 1000000000;
    
    pthread_mutex_lock(&server->state_mutex);
    
    while (server->state != target_state) {
        if (timeout_ms == 0) {
            // 无限等待
            pthread_cond_wait(&server->state_cond, &server->state_mutex);
        } else {
            int result = pthread_cond_timedwait(&server->state_cond, 
                                               &server->state_mutex, &ts);
            if (result == ETIMEDOUT) {
                pthread_mutex_unlock(&server->state_mutex);
                return -2;
            }
        }
    }
    
    pthread_mutex_unlock(&server->state_mutex);
    return 0;
}

int server_get_statistics(server_instance_t *server, server_statistics_t *stats) {
    if (!server) {
        return -1;
    }
    
    pthread_mutex_lock(&server->stats_mutex);
    
    if (stats) {
        memcpy(stats, &server->stats, sizeof(server_statistics_t));
        
        // 更新实时数据
        stats->uptime = get_current_timestamp_ms() - stats->startup_time;
        stats->active_connections = server->connection_pool ? 
            connection_pool_get_active_count(server->connection_pool) : 0;
    }
    
    pthread_mutex_unlock(&server->stats_mutex);
    return 0;
}

int server_reset_statistics(server_instance_t *server) {
    if (!server) {
        return -1;
    }
    
    pthread_mutex_lock(&server->stats_mutex);
    
    // 重置统计信息
    memset(&server->stats, 0, sizeof(server_statistics_t));
    server->stats.startup_time = get_current_timestamp_ms();
    
    pthread_mutex_unlock(&server->stats_mutex);
    
    LOG_INFO("统计信息已重置");
    return 0;
}

size_t server_broadcast_message(server_instance_t *server, uint32_t message_type, 
                               const void *data, size_t len, uint64_t exclude_user_id) {
    if (!server || !server->connection_pool) {
        return 0;
    }
    
    size_t sent_count = 0;
    
    // 获取所有活跃连接
    connection_info_t *connections = NULL;
    size_t count = 0;
    
    if (server_get_active_connections(server, &connections, &count) == 0 && count > 0) {
        for (size_t i = 0; i < count; i++) {
            // 排除指定用户
            if (exclude_user_id > 0 && connections[i].user_id == exclude_user_id) {
                continue;
            }
            
            // 发送消息
            if (server_send_to_connection(server, connections[i].connection_id, 
                                         message_type, data, len) == 0) {
                sent_count++;
            }
        }
        
        server_free_connection_list(connections, count);
    }
    
    return sent_count;
}

int server_send_to_user(server_instance_t *server, uint64_t user_id, 
                       uint32_t message_type, const void *data, size_t len) {
    if (!server || !server->connection_pool) {
        return -1;
    }
    
    // 查找用户的连接
    connection_info_t *connections = NULL;
    size_t count = 0;
    
    if (server_get_active_connections(server, &connections, &count) == 0 && count > 0) {
        int sent = 0;
        
        for (size_t i = 0; i < count; i++) {
            if (connections[i].user_id == user_id) {
                if (server_send_to_connection(server, connections[i].connection_id, 
                                             message_type, data, len) == 0) {
                    sent++;
                }
            }
        }
        
        server_free_connection_list(connections, count);
        return sent > 0 ? 0 : -2;
    }
    
    return -3;
}

int server_send_to_connection(server_instance_t *server, uint64_t conn_id,
                             uint32_t message_type, const void *data, size_t len) {
    if (!server || !server->connection_pool) {
        return -1;
    }
    
    // 获取连接
    client_connection_t *conn = connection_pool_get_by_id(server->connection_pool, conn_id);
    if (!conn) {
        return -2;
    }
    
    // 发送消息
    int result = connection_send_message(conn, message_type, data, len);
    
    if (result == 0) {
        update_statistics(server, STAT_TYPE_MESSAGES_SENT, 1);
        update_statistics(server, STAT_TYPE_NETWORK_OUT, len);
    }
    
    return result;
}

int server_kick_user(server_instance_t *server, uint64_t user_id, const char *reason) {
    if (!server || !server->connection_pool) {
        return -1;
    }
    
    // 查找用户的连接
    connection_info_t *connections = NULL;
    size_t count = 0;
    
    if (server_get_active_connections(server, &connections, &count) == 0 && count > 0) {
        int kicked = 0;
        
        for (size_t i = 0; i < count; i++) {
            if (connections[i].user_id == user_id) {
                server_kick_connection(server, connections[i].connection_id, reason);
                kicked++;
            }
        }
        
        server_free_connection_list(connections, count);
        
        if (kicked > 0) {
            // 记录审计日志
            log_audit_event(server, "USER_KICKED", user_id, 0, reason);
            
            // 触发事件
            SERVER_TRIGGER_EVENT(server, SERVER_EVENT_CLIENT_DISCONNECT, 
                               user_id, 0, "用户被踢出", reason, strlen(reason));
            
            return 0;
        }
    }
    
    return -2;
}

int server_kick_connection(server_instance_t *server, uint64_t conn_id, const char *reason) {
    if (!server || !server->connection_pool) {
        return -1;
    }
    
    // 获取连接
    client_connection_t *conn = connection_pool_get_by_id(server->connection_pool, conn_id);
    if (!conn) {
        return -2;
    }
    
    // 发送踢出通知（如果可能）
    if (reason && strlen(reason) > 0) {
        connection_send_message(conn, MSG_SYSTEM_NOTIFICATION, reason, strlen(reason));
    }
    
    // 关闭连接
    connection_close(server->connection_pool, conn);
    
    // 记录审计日志
    log_audit_event(server, "CONNECTION_KICKED", conn->user_id, conn_id, reason);
    
    // 触发事件
    SERVER_TRIGGER_EVENT(server, SERVER_EVENT_CLIENT_DISCONNECT, 
                       conn->user_id, conn_id, "连接被踢出", reason, strlen(reason));
    
    return 0;
}

int server_get_online_users(server_instance_t *server, user_info_t **users, size_t *count) {
    if (!server || !server->user_manager || !users || !count) {
        return -1;
    }
    
    return user_manager_get_online_users(server->user_manager, users, count);
}

void server_free_user_list(user_info_t *users, size_t count) {
    if (!users) {
        return;
    }
    
    for (size_t i = 0; i < count; i++) {
        // 释放动态分配的内存（如果有）
        // 这里假设user_info_t没有动态分配的字段
    }
    
    free(users);
}

int server_get_active_connections(server_instance_t *server, 
                                 connection_info_t **connections, size_t *count) {
    if (!server || !server->connection_pool || !connections || !count) {
        return -1;
    }
    
    return connection_pool_get_active_connections(server->connection_pool, 
                                                 connections, count);
}

void server_free_connection_list(connection_info_t *connections, size_t count) {
    if (!connections) {
        return;
    }
    
    free(connections);
}

void server_set_event_callback(server_instance_t *server, 
                              server_event_callback_t callback, void *user_data) {
    if (server) {
        server->event_callback = callback;
        server->event_callback_data = user_data;
    }
}

void server_set_user_data(server_instance_t *server, void *user_data) {
    if (server) {
        server->user_data = user_data;
    }
}

void* server_get_user_data(const server_instance_t *server) {
    return server ? server->user_data : NULL;
}

int server_backup_data(server_instance_t *server, const char *backup_path) {
    if (!server || !backup_path) {
        return -1;
    }
    
    LOG_INFO("开始备份服务器数据到: %s", backup_path);
    
    // 备份数据库
    if (server->db_conn) {
        sqlite3 *backup_db;
        int rc = sqlite3_open(backup_path, &backup_db);
        if (rc != SQLITE_OK) {
            LOG_ERROR("无法创建备份数据库: %s", sqlite3_errmsg(backup_db));
            return -2;
        }
        
        sqlite3_backup *backup = sqlite3_backup_init(backup_db, "main", 
                                                     server->db_conn, "main");
        if (!backup) {
            LOG_ERROR("无法初始化备份: %s", sqlite3_errmsg(backup_db));
            sqlite3_close(backup_db);
            return -3;
        }
        
        rc = sqlite3_backup_step(backup, -1);
        sqlite3_backup_finish(backup);
        
        if (rc != SQLITE_DONE) {
            LOG_ERROR("备份失败: %s", sqlite3_errmsg(backup_db));
            sqlite3_close(backup_db);
            return -4;
        }
        
        sqlite3_close(backup_db);
    }
    
    // 备份文件
    if (server->config.upload_dir) {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "cp -r %s %s.uploads", 
                 server->config.upload_dir, backup_path);
        
        if (system(cmd) != 0) {
            LOG_WARNING("文件备份可能失败");
        }
    }
    
    server->stats.last_backup_time = get_current_timestamp_ms();
    
    LOG_INFO("备份完成: %s", backup_path);
    
    // 触发备份事件
    SERVER_TRIGGER_EVENT(server, SERVER_EVENT_BACKUP, 0, 0, 
                        "数据备份完成", backup_path, strlen(backup_path));
    
    return 0;
}

int server_restore_data(server_instance_t *server, const char *backup_path) {
    if (!server || !backup_path) {
        return -1;
    }
    
    LOG_INFO("开始从备份恢复数据: %s", backup_path);
    
    // 检查备份文件是否存在
    if (access(backup_path, F_OK) != 0) {
        LOG_ERROR("备份文件不存在: %s", backup_path);
        return -2;
    }
    
    // 停止服务器（如果正在运行）
    bool was_running = server->running;
    if (was_running) {
        server_stop(server, true);
    }
    
    // 恢复数据库
    if (server->db_conn) {
        sqlite3_close(server->db_conn);
        server->db_conn = NULL;
        
        // 复制备份文件
        char db_path[512];
        snprintf(db_path, sizeof(db_path), "%s", server->config.database_path);
        
        if (rename(backup_path, db_path) != 0) {
            LOG_ERROR("恢复数据库失败: %s", strerror(errno));
            return -3;
        }
        
        // 重新打开数据库
        int rc = sqlite3_open(db_path, &server->db_conn);
        if (rc != SQLITE_OK) {
            LOG_ERROR("无法打开恢复的数据库: %s", sqlite3_errmsg(server->db_conn));
            return -4;
        }
    }
    
    // 恢复文件（如果需要）
    
    // 重新启动服务器（如果之前正在运行）
    if (was_running) {
        server_start(server);
    }
    
    LOG_INFO("数据恢复完成: %s", backup_path);
    
    // 触发恢复事件
    SERVER_TRIGGER_EVENT(server, SERVER_EVENT_RESTORE, 0, 0, 
                        "数据恢复完成", backup_path, strlen(backup_path));
    
    return 0;
}

// ==================== 配置管理函数 ====================

void server_config_init(server_config_t *config) {
    if (!config) {
        return;
    }
    
    memset(config, 0, sizeof(server_config_t));
    
    // 网络配置
    config->tcp_port = DEFAULT_PORT;
    config->websocket_port = WEBSOCKET_PORT;
    config->max_clients = MAX_CLIENTS;
    config->backlog_size = BACKLOG_SIZE;
    
    // SSL配置
    config->enable_ssl = false;
    config->ssl_cert_path = NULL;
    config->ssl_key_path = NULL;
    config->ssl_ca_path = NULL;
    
    // 连接管理
    config->thread_pool_size = 16;
    config->heartbeat_interval = HEARTBEAT_INTERVAL;
    config->heartbeat_timeout = HEARTBEAT_TIMEOUT;
    config->session_timeout = SESSION_TIMEOUT;
    config->rate_limit_per_minute = RATE_LIMIT_PER_MINUTE;
    config->max_message_size = MAX_MESSAGE_SIZE;
    config->max_file_size = MAX_FILE_SIZE;
    
    // 数据库配置
    config->database_path = "securechat.db";
    config->database_encryption = false;
    config->database_key = NULL;
    
    // 存储配置
    config->upload_dir = "./uploads";
    config->temp_dir = "/tmp/securechat";
    config->max_upload_size = 100 * 1024 * 1024; // 100MB
    config->enable_file_scan = false;
    
    // 日志配置
    config->log_level = LOG_LEVEL_INFO;
    config->log_file = NULL;
    config->log_max_size = 10 * 1024 * 1024; // 10MB
    config->log_max_files = 10;
    
    // 安全配置
    config->enable_audit = true;
    config->enable_encryption = true;
    config->require_client_auth = false;
    config->max_login_attempts = 5;
    config->login_lockout_time = 300000; // 5分钟
    
    // 监控配置
    config->enable_monitoring = true;
    config->monitoring_port = 9090;
    config->monitoring_host = "localhost";
    
    // 运行配置
    config->run_as_daemon = false;
    config->run_as_user = NULL;
    config->pid_file = NULL;
    config->test_mode = false;
}

bool server_config_load_from_file(server_config_t *config, const char *filename) {
    // 使用JSON库解析配置文件
    // 这里省略具体实现，假设使用jansson库
    LOG_INFO("从文件加载配置: %s", filename);
    return true;
}

bool server_config_save_to_file(const server_config_t *config, const char *filename) {
    // 使用JSON库保存配置文件
    LOG_INFO("保存配置到文件: %s", filename);
    return true;
}

bool server_config_validate(const server_config_t *config, 
                           char *error_buffer, size_t error_buffer_size) {
    if (!config) {
        if (error_buffer && error_buffer_size > 0) {
            strncpy(error_buffer, "配置为空", error_buffer_size - 1);
            error_buffer[error_buffer_size - 1] = '\0';
        }
        return false;
    }
    
    // 检查端口
    if (config->tcp_port <= 0 || config->tcp_port > 65535) {
        if (error_buffer && error_buffer_size > 0) {
            snprintf(error_buffer, error_buffer_size, "无效的TCP端口: %d", config->tcp_port);
        }
        return false;
    }
    
    if (config->websocket_port <= 0 || config->websocket_port > 65535) {
        if (error_buffer && error_buffer_size > 0) {
            snprintf(error_buffer, error_buffer_size, "无效的WebSocket端口: %d", config->websocket_port);
        }
        return false;
    }
    
    // 检查端口冲突
    if (config->tcp_port == config->websocket_port) {
        if (error_buffer && error_buffer_size > 0) {
            snprintf(error_buffer, error_buffer_size, "TCP端口和WebSocket端口冲突: %d", config->tcp_port);
        }
        return false;
    }
    
    // 检查最大客户端数
    if (config->max_clients <= 0 || config->max_clients > 100000) {
        if (error_buffer && error_buffer_size > 0) {
            snprintf(error_buffer, error_buffer_size, "无效的最大客户端数: %d", config->max_clients);
        }
        return false;
    }
    
    // 检查SSL配置
    if (config->enable_ssl) {
        if (!config->ssl_cert_path || strlen(config->ssl_cert_path) == 0) {
            if (error_buffer && error_buffer_size > 0) {
                strncpy(error_buffer, "SSL启用但未指定证书路径", error_buffer_size - 1);
            }
            return false;
        }
        
        if (!config->ssl_key_path || strlen(config->ssl_key_path) == 0) {
            if (error_buffer && error_buffer_size > 0) {
                strncpy(error_buffer, "SSL启用但未指定私钥路径", error_buffer_size - 1);
            }
            return false;
        }
        
        // 检查文件是否存在
        if (access(config->ssl_cert_path, R_OK) != 0) {
            if (error_buffer && error_buffer_size > 0) {
                snprintf(error_buffer, error_buffer_size, "SSL证书文件不可读: %s", config->ssl_cert_path);
            }
            return false;
        }
        
        if (access(config->ssl_key_path, R_OK) != 0) {
            if (error_buffer && error_buffer_size > 0) {
                snprintf(error_buffer, error_buffer_size, "SSL私钥文件不可读: %s", config->ssl_key_path);
            }
            return false;
        }
    }
    
    // 检查数据库路径
    if (!config->database_path || strlen(config->database_path) == 0) {
        if (error_buffer && error_buffer_size > 0) {
            strncpy(error_buffer, "数据库路径不能为空", error_buffer_size - 1);
        }
        return false;
    }
    
    return true;
}

// ==================== 工具函数 ====================

int server_generate_status_report(server_instance_t *server, 
                                 char *report_buffer, size_t buffer_size) {
    if (!server || !report_buffer || buffer_size == 0) {
        return -1;
    }
    
    server_statistics_t stats;
    server_get_statistics(server, &stats);
    
    int written = snprintf(report_buffer, buffer_size,
                          "SecureChat 服务器状态报告\n"
                          "=======================\n"
                          "状态: %s\n"
                          "运行时间: %llu 秒\n"
                          "总连接数: %llu\n"
                          "活跃连接: %llu\n"
                          "最大并发连接: %llu\n"
                          "拒绝连接: %llu\n"
                          "接收消息: %llu\n"
                          "发送消息: %llu\n"
                          "文件传输: %llu\n"
                          "文件传输字节: %llu\n"
                          "网络接收: %llu 字节\n"
                          "网络发送: %llu 字节\n"
                          "在线用户: %llu\n"
                          "认证失败: %llu\n",
                          server_state_to_string(server->state),
                          (unsigned long long)stats.uptime / 1000,
                          (unsigned long long)stats.total_connections,
                          (unsigned long long)stats.active_connections,
                          (unsigned long long)stats.max_concurrent_connections,
                          (unsigned long long)stats.rejected_connections,
                          (unsigned long long)stats.messages_received,
                          (unsigned long long)stats.messages_sent,
                          (unsigned long long)stats.file_transfers,
                          (unsigned long long)stats.file_transfer_bytes,
                          (unsigned long long)stats.network_in_bytes,
                          (unsigned long long)stats.network_out_bytes,
                          (unsigned long long)stats.online_users,
                          (unsigned long long)stats.auth_failures);
    
    return written;
}

int server_check_health(server_instance_t *server) {
    if (!server) {
        return -1;
    }
    
    int health_status = 0;
    
    // 检查数据库连接
    if (server->db_conn) {
        char *errmsg = NULL;
        if (sqlite3_exec(server->db_conn, "SELECT 1;", NULL, NULL, &errmsg) != SQLITE_OK) {
            LOG_ERROR("数据库健康检查失败: %s", errmsg);
            sqlite3_free(errmsg);
            health_status |= 0x01;
        }
    }
    
    // 检查文件系统
    if (server->config.upload_dir) {
        if (access(server->config.upload_dir, W_OK) != 0) {
            LOG_ERROR("上传目录不可写: %s", server->config.upload_dir);
            health_status |= 0x02;
        }
    }
    
    // 检查内存使用
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        if (usage.ru_maxrss > 500 * 1024) { // 超过500MB
            LOG_WARNING("内存使用较高: %ld KB", usage.ru_maxrss);
            health_status |= 0x04;
        }
    }
    
    // 检查连接数
    if (server->connection_pool) {
        size_t active_count = connection_pool_get_active_count(server->connection_pool);
        if (active_count > server->config.max_clients * 0.9) {
            LOG_WARNING("连接数接近上限: %zu/%d", active_count, server->config.max_clients);
            health_status |= 0x08;
        }
    }
    
    return health_status;
}

int server_perform_maintenance(server_instance_t *server) {
    if (!server) {
        return -1;
    }
    
    LOG_INFO("执行服务器维护...");
    
    // 清理过期数据
    size_t cleaned = server_cleanup_expired_data(server);
    if (cleaned > 0) {
        LOG_INFO("清理了 %zu 条过期数据", cleaned);
    }
    
    // 数据库优化
    if (server->db_conn) {
        char *errmsg = NULL;
        sqlite3_exec(server->db_conn, "VACUUM;", NULL, NULL, &errmsg);
        if (errmsg) {
            LOG_WARNING("数据库VACUUM失败: %s", errmsg);
            sqlite3_free(errmsg);
        } else {
            LOG_DEBUG("数据库优化完成");
        }
    }
    
    // 更新统计信息
    server->stats.uptime = get_current_timestamp_ms() - server->stats.startup_time;
    
    LOG_INFO("服务器维护完成");
    return 0;
}

size_t server_cleanup_expired_data(server_instance_t *server) {
    if (!server || !server->db_conn) {
        return 0;
    }
    
    size_t total_cleaned = 0;
    uint64_t now = get_current_timestamp_ms();
    uint64_t expired_time = now - server->config.session_timeout;
    
    // 清理过期会话
    sqlite3_stmt *stmt;
    const char *sql = "DELETE FROM sessions WHERE expires_at < ?;";
    
    if (sqlite3_prepare_v2(server->db_conn, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, expired_time);
        
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            int changes = sqlite3_changes(server->db_conn);
            total_cleaned += changes;
            
            if (changes > 0) {
                LOG_DEBUG("清理了 %d 个过期会话", changes);
            }
        }
        
        sqlite3_finalize(stmt);
    }
    
    // 清理旧消息（保留30天）
    uint64_t message_expiry = now - (30 * 24 * 3600 * 1000);
    sql = "DELETE FROM messages WHERE timestamp < ?;";
    
    if (sqlite3_prepare_v2(server->db_conn, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, message_expiry);
        
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            int changes = sqlite3_changes(server->db_conn);
            total_cleaned += changes;
            
            if (changes > 0) {
                LOG_DEBUG("清理了 %d 条旧消息", changes);
            }
        }
        
        sqlite3_finalize(stmt);
    }
    
    // 清理旧审计日志（保留7天）
    uint64_t audit_expiry = now - (7 * 24 * 3600 * 1000);
    sql = "DELETE FROM audit_log WHERE timestamp < ?;";
    
    if (sqlite3_prepare_v2(server->db_conn, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, audit_expiry);
        
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            int changes = sqlite3_changes(server->db_conn);
            total_cleaned += changes;
            
            if (changes > 0) {
                LOG_DEBUG("清理了 %d 条旧审计日志", changes);
            }
        }
        
        sqlite3_finalize(stmt);
    }
    
    return total_cleaned;
}

// ==================== 状态字符串转换 ====================

static const char* server_state_to_string(server_state_t state) {
    static const char* state_names[] = {
        "初始化中",
        "运行中",
        "关闭中",
        "已关闭",
        "错误"
    };
    
    if (state >= sizeof(state_names) / sizeof(state_names[0])) {
        return "未知";
    }
    
    return state_names[state];
}