#include "server.h"
#include "connection.h"
#include "auth.h"
#include "message_handler.h"
#include "file_transfer.h"
#include "database.h"
#include "thread_pool.h"
#include "websocket_server.h"
#include "monitoring.h"
#include "../common/security.h"
#include "../common/logger.h"
#include "../common/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>

// 全局服务器实例
static server_instance_t *server_instance = NULL;

// 信号处理
static void signal_handler(int signum) {
    LOG_INFO("收到信号 %d，正在优雅关闭...", signum);
    
    if (server_instance) {
        server_instance->running = false;
    }
}

// 设置进程用户
static bool drop_privileges(const char *username) {
    struct passwd *pw = getpwnam(username);
    if (!pw) {
        LOG_ERROR("用户 %s 不存在", username);
        return false;
    }
    
    // 设置组ID
    if (setgid(pw->pw_gid) != 0) {
        LOG_ERROR("设置组ID失败");
        return false;
    }
    
    // 设置用户ID
    if (setuid(pw->pw_uid) != 0) {
        LOG_ERROR("设置用户ID失败");
        return false;
    }
    
    LOG_INFO("权限已降级为用户: %s", username);
    return true;
}

// 守护进程化
static bool daemonize() {
    pid_t pid = fork();
    
    if (pid < 0) {
        LOG_ERROR("fork失败");
        return false;
    }
    
    if (pid > 0) {
        // 父进程退出
        exit(0);
    }
    
    // 子进程成为会话领导
    if (setsid() < 0) {
        LOG_ERROR("setsid失败");
        return false;
    }
    
    // 再次fork确保不是会话领导
    pid = fork();
    if (pid < 0) {
        LOG_ERROR("第二次fork失败");
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
    
    return true;
}

// 显示使用帮助
static void print_usage(const char *program_name) {
    printf("用法: %s [选项]\n\n", program_name);
    printf("选项:\n");
    printf("  -h, --help             显示此帮助信息\n");
    printf("  -c, --config FILE      配置文件路径\n");
    printf("  -p, --port PORT        监听端口 (默认: 8888)\n");
    printf("  -w, --ws-port PORT     WebSocket端口 (默认: 8889)\n");
    printf("  -d, --daemon           以守护进程运行\n");
    printf("  -u, --user USER        运行用户\n");
    printf("  -l, --log FILE         日志文件路径\n");
    printf("  -v, --verbose          详细日志输出\n");
    printf("  -t, --test             测试模式\n");
    printf("\n");
    printf("示例:\n");
    printf("  %s -c /etc/securechat/config.json\n", program_name);
    printf("  %s --port 9999 --daemon --user nobody\n", program_name);
}

// 服务器主循环
static void server_main_loop(server_instance_t *server) {
    struct timespec loop_interval = {0, 10000000}; // 10ms
    
    while (server->running) {
        // 处理事件
        connection_process_events(server);
        
        // 处理心跳
        connection_check_heartbeats(server);
        
        // 清理过期会话
        auth_cleanup_expired_sessions(server);
        
        // 更新监控指标
        monitoring_update_metrics(server);
        
        // 短暂休眠以避免CPU过度使用
        nanosleep(&loop_interval, NULL);
    }
}

int main(int argc, char *argv[]) {
    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"config", required_argument, 0, 'c'},
        {"port", required_argument, 0, 'p'},
        {"ws-port", required_argument, 0, 'w'},
        {"daemon", no_argument, 0, 'd'},
        {"user", required_argument, 0, 'u'},
        {"log", required_argument, 0, 'l'},
        {"verbose", no_argument, 0, 'v'},
        {"test", no_argument, 0, 't'},
        {0, 0, 0, 0}
    };
    
    // 默认配置
    server_config_t config = {
        .tcp_port = DEFAULT_PORT,
        .websocket_port = WEBSOCKET_PORT,
        .max_clients = MAX_CLIENTS,
        .thread_pool_size = 16,
        .heartbeat_interval = 30000,
        .heartbeat_timeout = 120000,
        .session_timeout = 3600000,
        .rate_limit_per_minute = 60,
        .enable_ssl = false,
        .ssl_cert_path = NULL,
        .ssl_key_path = NULL,
        .database_path = "securechat.db",
        .upload_dir = "./uploads",
        .log_level = LOG_LEVEL_INFO,
        .log_file = NULL,
        .run_as_daemon = false,
        .run_as_user = NULL,
        .enable_monitoring = true,
        .enable_audit = true,
        .test_mode = false
    };
    
    const char *config_file = NULL;
    bool verbose = false;
    
    // 解析命令行参数
    int opt;
    while ((opt = getopt_long(argc, argv, "hc:p:w:du:l:vt", long_options, NULL)) != -1) {
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                return 0;
                
            case 'c':
                config_file = optarg;
                break;
                
            case 'p':
                config.tcp_port = atoi(optarg);
                break;
                
            case 'w':
                config.websocket_port = atoi(optarg);
                break;
                
            case 'd':
                config.run_as_daemon = true;
                break;
                
            case 'u':
                config.run_as_user = optarg;
                break;
                
            case 'l':
                config.log_file = optarg;
                break;
                
            case 'v':
                verbose = true;
                config.log_level = LOG_LEVEL_DEBUG;
                break;
                
            case 't':
                config.test_mode = true;
                break;
                
            default:
                fprintf(stderr, "未知选项\n");
                print_usage(argv[0]);
                return 1;
        }
    }
    
    // 加载配置文件（如果提供）
    if (config_file) {
        if (!config_load_from_file(config_file, &config)) {
            fprintf(stderr, "无法加载配置文件: %s\n", config_file);
            return 1;
        }
    }
    
    // 初始化日志系统
    if (!logger_init(config.log_level, config.log_file)) {
        fprintf(stderr, "日志系统初始化失败\n");
        return 1;
    }
    
    LOG_INFO("启动SecureChat服务器 v%d.%d.%d", 
             PROTOCOL_VERSION_MAJOR,
             PROTOCOL_VERSION_MINOR,
             PROTOCOL_VERSION_PATCH);
    
    // 显示配置信息
    LOG_INFO("配置: TCP端口=%d, WebSocket端口=%d, 最大客户端=%d",
             config.tcp_port, config.websocket_port, config.max_clients);
    
    // 守护进程化
    if (config.run_as_daemon) {
        LOG_INFO("以守护进程模式运行");
        if (!daemonize()) {
            LOG_ERROR("守护进程化失败");
            return 1;
        }
    }
    
    // 降级权限
    if (config.run_as_user) {
        if (!drop_privileges(config.run_as_user)) {
            LOG_ERROR("权限降级失败");
            return 1;
        }
    }
    
    // 安装信号处理器
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    
    // 初始化安全模块
    if (!security_init()) {
        LOG_ERROR("安全模块初始化失败");
        return 1;
    }
    
    // 创建服务器实例
    server_instance = server_create(&config);
    if (!server_instance) {
        LOG_ERROR("服务器实例创建失败");
        return 1;
    }
    
    // 初始化数据库
    if (!database_init(server_instance->db_conn, config.database_path)) {
        LOG_ERROR("数据库初始化失败");
        server_destroy(server_instance);
        return 1;
    }
    
    // 初始化线程池
    server_instance->thread_pool = thread_pool_create(config.thread_pool_size);
    if (!server_instance->thread_pool) {
        LOG_ERROR("线程池创建失败");
        server_destroy(server_instance);
        return 1;
    }
    
    // 初始化TCP服务器
    if (!connection_init_tcp_server(server_instance)) {
        LOG_ERROR("TCP服务器初始化失败");
        server_destroy(server_instance);
        return 1;
    }
    
    // 初始化WebSocket服务器
    if (!websocket_server_init(server_instance)) {
        LOG_ERROR("WebSocket服务器初始化失败");
        server_destroy(server_instance);
        return 1;
    }
    
    // 初始化文件传输模块
    if (!file_transfer_init(server_instance)) {
        LOG_ERROR("文件传输模块初始化失败");
        server_destroy(server_instance);
        return 1;
    }
    
    // 初始化监控系统
    if (config.enable_monitoring) {
        if (!monitoring_init(server_instance)) {
            LOG_ERROR("监控系统初始化失败");
        } else {
            LOG_INFO("监控系统已启用");
        }
    }
    
    LOG_INFO("服务器初始化完成，开始处理请求...");
    
    // 运行服务器主循环
    server_instance->running = true;
    server_main_loop(server_instance);
    
    // 优雅关闭
    LOG_INFO("正在关闭服务器...");
    
    // 停止接受新连接
    server_instance->running = false;
    
    // 等待所有连接关闭
    connection_wait_for_clients(server_instance, 5000); // 等待5秒
    
    // 清理资源
    server_destroy(server_instance);
    security_cleanup();
    logger_cleanup();
    
    LOG_INFO("服务器已关闭");
    
    return 0;
}