/**
 * @file main.c
 * @brief Secure Chat System - 主服务器程序
 * @version 3.0.0
 * 
 * 这是一个健壮、优雅且安全的安全聊天系统服务器主程序。
 * 设计原则：安全第一、性能第二、可维护性第三。
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <getopt.h>
#include <time.h>

// 项目头文件
#include "config.h"
#include "server.h"
#include "database.h"
#include "utils.h"
#include "monitoring_protocol.h"
#include "auth.h"
#include "file_transfer.h"

// ==================== 全局常量 ====================
#define PROGRAM_NAME        "secure-chat-server"
#define PROGRAM_VERSION     "3.0.0"
#define PROGRAM_AUTHOR      "Secure Chat Team"
#define PROGRAM_LICENSE     "Apache-2.0"
#define DEFAULT_CONFIG_PATH "/etc/secure-chat/server.conf"
#define DEFAULT_PID_FILE    "/var/run/secure-chat-server.pid"
#define DEFAULT_LOG_FILE    "/var/log/secure-chat-server.log"
#define DEFAULT_WORK_DIR    "/var/lib/secure-chat"
#define LOCK_FILE_MODE      0644

// ==================== 全局变量 ====================
typedef struct {
    bool daemon_mode;           // 守护进程模式
    bool foreground;           // 前台运行
    bool test_mode;           // 测试模式
    bool reload_config;        // 重载配置
    bool show_version;         // 显示版本
    bool show_help;           // 显示帮助
    bool check_config;        // 检查配置
    bool verbose;             // 详细输出
    
    char *config_path;        // 配置文件路径
    char *pid_file_path;      // PID文件路径
    char *log_file_path;      // 日志文件路径
    char *working_dir;        // 工作目录
    
    int log_level;            // 日志级别
    int exit_code;            // 退出代码
    
    server_config_t server_config;  // 服务器配置
    config_manager_t *config_manager; // 配置管理器
    server_instance_t *server_instance; // 服务器实例
    
    // 信号处理
    volatile sig_atomic_t sig_terminate;
    volatile sig_atomic_t sig_reload;
    volatile sig_atomic_t sig_usr1;
    volatile sig_atomic_t sig_usr2;
} program_context_t;

static program_context_t g_ctx = {0};
static logger_t *g_logger = NULL;
static pthread_mutex_t g_ctx_mutex = PTHREAD_MUTEX_INITIALIZER;

// ==================== 函数声明 ====================
static void signal_handler(int signum);
static void register_signal_handlers(void);
static void print_banner(void);
static void print_version(void);
static void print_usage(const char *progname);
static void parse_arguments(int argc, char *argv[]);
static int check_privileges(void);
static int create_pid_file(void);
static int remove_pid_file(void);
static int daemonize(void);
static int load_configuration(void);
static int validate_environment(void);
static int initialize_modules(void);
static int cleanup_modules(void);
static void graceful_shutdown(void);
static void handle_signal_events(void);
static void *monitoring_thread(void *arg);
static void server_event_callback(int event_type, void *event_data, void *user_data);

// ==================== 信号处理 ====================
static void signal_handler(int signum) {
    switch (signum) {
        case SIGINT:
        case SIGTERM:
            fprintf(stderr, "\nReceived signal %d, shutting down gracefully...\n", signum);
            g_ctx.sig_terminate = 1;
            break;
            
        case SIGHUP:
            fprintf(stderr, "\nReceived SIGHUP, reloading configuration...\n");
            g_ctx.sig_reload = 1;
            break;
            
        case SIGUSR1:
            fprintf(stderr, "\nReceived SIGUSR1, dumping status...\n");
            g_ctx.sig_usr1 = 1;
            break;
            
        case SIGUSR2:
            fprintf(stderr, "\nReceived SIGUSR2, rotating logs...\n");
            g_ctx.sig_usr2 = 1;
            break;
            
        case SIGPIPE:
            // 忽略SIGPIPE，让write返回EPIPE
            fprintf(stderr, "Caught SIGPIPE, ignoring...\n");
            break;
            
        default:
            fprintf(stderr, "Received unknown signal %d\n", signum);
            break;
    }
}

static void register_signal_handlers(void) {
    struct sigaction sa;
    
    // 设置终止信号处理
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    
    // 忽略SIGPIPE
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}

// ==================== 程序信息 ====================
static void print_banner(void) {
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║                  SECURE CHAT SYSTEM v%s                ║\n", PROGRAM_VERSION);
    printf("║             End-to-End Encrypted Communication            ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n");
    printf("\n");
}

static void print_version(void) {
    printf("%s version %s\n", PROGRAM_NAME, PROGRAM_VERSION);
    printf("Copyright (c) %s. Licensed under %s.\n", 
           PROGRAM_AUTHOR, PROGRAM_LICENSE);
    printf("\n");
    printf("Build features:\n");
    printf("  - End-to-End Encryption (AES-256-GCM)\n");
    printf("  - Hardware Key Support (WebAuthn/FIDO2)\n");
    printf("  - Two-Factor Authentication\n");
    printf("  - File Transfer with Resume Support\n");
    printf("  - Group Chat with Admin Controls\n");
    printf("  - Real-time Monitoring & Metrics\n");
    printf("  - SQLite Database with Encryption\n");
    printf("  - Thread Pool & Connection Pool\n");
    printf("\n");
}

static void print_usage(const char *progname) {
    printf("Usage: %s [OPTIONS]\n", progname);
    printf("\n");
    printf("Options:\n");
    printf("  -c, --config FILE      Configuration file path\n");
    printf("  -d, --daemon           Run as daemon (background)\n");
    printf("  -f, --foreground       Run in foreground (not daemon)\n");
    printf("  -p, --pid FILE         PID file path\n");
    printf("  -l, --log FILE         Log file path\n");
    printf("  -w, --workdir DIR      Working directory\n");
    printf("  -v, --verbose          Verbose output\n");
    printf("  -t, --test             Test mode (no daemon)\n");
    printf("  -C, --check-config     Validate configuration and exit\n");
    printf("  -V, --version          Show version information\n");
    printf("  -h, --help             Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s -c /etc/secure-chat/server.conf -d\n", progname);
    printf("  %s --foreground --verbose\n", progname);
    printf("  %s --check-config --config server.conf\n", progname);
    printf("\n");
}

// ==================== 参数解析 ====================
static void parse_arguments(int argc, char *argv[]) {
    static struct option long_options[] = {
        {"config",      required_argument, 0, 'c'},
        {"daemon",      no_argument,       0, 'd'},
        {"foreground",  no_argument,       0, 'f'},
        {"pid",         required_argument, 0, 'p'},
        {"log",         required_argument, 0, 'l'},
        {"workdir",     required_argument, 0, 'w'},
        {"verbose",     no_argument,       0, 'v'},
        {"test",        no_argument,       0, 't'},
        {"check-config", no_argument,      0, 'C'},
        {"version",     no_argument,       0, 'V'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    int option_index = 0;
    
    // 设置默认值
    g_ctx.daemon_mode = false;
    g_ctx.foreground = false;
    g_ctx.test_mode = false;
    g_ctx.reload_config = false;
    g_ctx.show_version = false;
    g_ctx.show_help = false;
    g_ctx.check_config = false;
    g_ctx.verbose = false;
    
    g_ctx.config_path = strdup(DEFAULT_CONFIG_PATH);
    g_ctx.pid_file_path = strdup(DEFAULT_PID_FILE);
    g_ctx.log_file_path = strdup(DEFAULT_LOG_FILE);
    g_ctx.working_dir = strdup(DEFAULT_WORK_DIR);
    g_ctx.log_level = LOG_LEVEL_INFO;
    g_ctx.exit_code = 0;
    
    while ((opt = getopt_long(argc, argv, "c:dfp:l:w:vtCVh", 
                              long_options, &option_index)) != -1) {
        switch (opt) {
            case 'c':
                free(g_ctx.config_path);
                g_ctx.config_path = strdup(optarg);
                break;
                
            case 'd':
                g_ctx.daemon_mode = true;
                break;
                
            case 'f':
                g_ctx.foreground = true;
                break;
                
            case 'p':
                free(g_ctx.pid_file_path);
                g_ctx.pid_file_path = strdup(optarg);
                break;
                
            case 'l':
                free(g_ctx.log_file_path);
                g_ctx.log_file_path = strdup(optarg);
                break;
                
            case 'w':
                free(g_ctx.working_dir);
                g_ctx.working_dir = strdup(optarg);
                break;
                
            case 'v':
                g_ctx.verbose = true;
                g_ctx.log_level = LOG_LEVEL_DEBUG;
                break;
                
            case 't':
                g_ctx.test_mode = true;
                g_ctx.foreground = true;
                break;
                
            case 'C':
                g_ctx.check_config = true;
                break;
                
            case 'V':
                g_ctx.show_version = true;
                break;
                
            case 'h':
                g_ctx.show_help = true;
                break;
                
            default:
                fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }
    
    // 参数冲突检查
    if (g_ctx.daemon_mode && g_ctx.foreground) {
        fprintf(stderr, "Error: Cannot specify both --daemon and --foreground\n");
        exit(EXIT_FAILURE);
    }
    
    if (g_ctx.show_help) {
        print_usage(argv[0]);
        exit(EXIT_SUCCESS);
    }
    
    if (g_ctx.show_version) {
        print_version();
        exit(EXIT_SUCCESS);
    }
}

// ==================== 权限检查 ====================
static int check_privileges(void) {
    uid_t uid = getuid();
    
    if (uid == 0) {
        // root用户运行，检查是否可以降权
        if (g_ctx.server_config.run_as_user != NULL && 
            strlen(g_ctx.server_config.run_as_user) > 0) {
            LOG_INFO(g_logger, "Running as root, will drop privileges to user: %s",
                     g_ctx.server_config.run_as_user);
        }
        return 0;
    } else if (uid < 1000) {
        // 系统用户
        LOG_WARNING(g_logger, "Running as system user (uid=%d)", uid);
        return 0;
    } else {
        // 普通用户
        LOG_WARNING(g_logger, "Running as non-root user (uid=%d)", uid);
        
        // 检查必要的权限
        if (access(g_ctx.config_path, R_OK) != 0) {
            LOG_ERROR(g_logger, "Cannot read config file: %s", g_ctx.config_path);
            return -1;
        }
        
        // 检查工作目录权限
        if (access(g_ctx.working_dir, W_OK) != 0) {
            LOG_ERROR(g_logger, "No write permission to working directory: %s",
                     g_ctx.working_dir);
            return -1;
        }
        
        return 0;
    }
}

// ==================== PID文件管理 ====================
static int create_pid_file(void) {
    char pid_str[32];
    int fd, len;
    
    fd = open(g_ctx.pid_file_path, O_WRONLY | O_CREAT | O_EXCL, LOCK_FILE_MODE);
    if (fd < 0) {
        if (errno == EEXIST) {
            LOG_ERROR(g_logger, "PID file already exists: %s", g_ctx.pid_file_path);
            return -1;
        }
        LOG_ERROR(g_logger, "Cannot create PID file: %s", strerror(errno));
        return -1;
    }
    
    len = snprintf(pid_str, sizeof(pid_str), "%d\n", getpid());
    if (write(fd, pid_str, len) != len) {
        LOG_ERROR(g_logger, "Cannot write to PID file: %s", strerror(errno));
        close(fd);
        return -1;
    }
    
    close(fd);
    LOG_DEBUG(g_logger, "PID file created: %s (pid=%d)", g_ctx.pid_file_path, getpid());
    return 0;
}

static int remove_pid_file(void) {
    if (unlink(g_ctx.pid_file_path) == 0) {
        LOG_DEBUG(g_logger, "PID file removed: %s", g_ctx.pid_file_path);
        return 0;
    }
    
    if (errno != ENOENT) {
        LOG_WARNING(g_logger, "Cannot remove PID file: %s", strerror(errno));
        return -1;
    }
    
    return 0;
}

// ==================== 守护进程化 ====================
static int daemonize(void) {
    pid_t pid, sid;
    
    // 第一次fork
    pid = fork();
    if (pid < 0) {
        LOG_ERROR(g_logger, "First fork failed: %s", strerror(errno));
        return -1;
    }
    
    if (pid > 0) {
        // 父进程退出
        exit(EXIT_SUCCESS);
    }
    
    // 创建新会话
    sid = setsid();
    if (sid < 0) {
        LOG_ERROR(g_logger, "Cannot create new session: %s", strerror(errno));
        return -1;
    }
    
    // 第二次fork
    pid = fork();
    if (pid < 0) {
        LOG_ERROR(g_logger, "Second fork failed: %s", strerror(errno));
        return -1;
    }
    
    if (pid > 0) {
        // 父进程退出
        exit(EXIT_SUCCESS);
    }
    
    // 改变工作目录
    if (chdir("/") < 0) {
        LOG_ERROR(g_logger, "Cannot change directory to /: %s", strerror(errno));
        return -1;
    }
    
    // 关闭标准文件描述符
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    // 重定向到/dev/null
    open("/dev/null", O_RDONLY);
    open("/dev/null", O_WRONLY);
    open("/dev/null", O_WRONLY);
    
    LOG_INFO(g_logger, "Daemon started successfully (pid=%d)", getpid());
    return 0;
}

// ==================== 配置管理 ====================
static int load_configuration(void) {
    char error_buffer[1024];
    
    LOG_INFO(g_logger, "Loading configuration from: %s", g_ctx.config_path);
    
    // 创建配置管理器
    g_ctx.config_manager = config_manager_create(g_ctx.config_path);
    if (!g_ctx.config_manager) {
        LOG_ERROR(g_logger, "Failed to create config manager");
        return -1;
    }
    
    // 加载配置
    if (config_load(g_ctx.config_manager) != 0) {
        LOG_ERROR(g_logger, "Failed to load configuration file");
        config_manager_destroy(g_ctx.config_manager);
        g_ctx.config_manager = NULL;
        return -1;
    }
    
    // 验证配置
    if (!config_validate(g_ctx.config_manager, error_buffer, sizeof(error_buffer))) {
        LOG_ERROR(g_logger, "Configuration validation failed: %s", error_buffer);
        config_manager_destroy(g_ctx.config_manager);
        g_ctx.config_manager = NULL;
        return -1;
    }
    
    LOG_INFO(g_logger, "Configuration loaded successfully");
    return 0;
}

static int reload_configuration(void) {
    LOG_INFO(g_logger, "Reloading configuration...");
    
    if (config_reload(g_ctx.config_manager) != 0) {
        LOG_ERROR(g_logger, "Failed to reload configuration");
        return -1;
    }
    
    // 更新服务器配置
    server_config_t new_config = g_ctx.server_config;
    
    // 从配置管理器读取新配置
    new_config.tcp_port = config_get_int(g_ctx.config_manager, "server", "tcp_port", 8888);
    new_config.websocket_port = config_get_int(g_ctx.config_manager, "server", "websocket_port", 8889);
    new_config.max_clients = config_get_int(g_ctx.config_manager, "server", "max_clients", 1000);
    
    // 应用新配置到服务器
    if (server_reload_config(g_ctx.server_instance, &new_config) != 0) {
        LOG_ERROR(g_logger, "Failed to apply new configuration");
        return -1;
    }
    
    LOG_INFO(g_logger, "Configuration reloaded successfully");
    return 0;
}

// ==================== 环境验证 ====================
static int validate_environment(void) {
    struct stat st;
    
    // 检查配置文件
    if (stat(g_ctx.config_path, &st) != 0) {
        LOG_ERROR(g_logger, "Configuration file not found: %s", g_ctx.config_path);
        return -1;
    }
    
    // 检查工作目录
    if (stat(g_ctx.working_dir, &st) != 0) {
        LOG_INFO(g_logger, "Creating working directory: %s", g_ctx.working_dir);
        if (mkdir(g_ctx.working_dir, 0755) != 0) {
            LOG_ERROR(g_logger, "Cannot create working directory: %s", strerror(errno));
            return -1;
        }
    }
    
    // 检查必要的子目录
    const char *subdirs[] = {"uploads", "temp", "logs", "backups", NULL};
    for (int i = 0; subdirs[i]; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", g_ctx.working_dir, subdirs[i]);
        
        if (stat(path, &st) != 0) {
            LOG_DEBUG(g_logger, "Creating directory: %s", path);
            if (mkdir(path, 0755) != 0) {
                LOG_ERROR(g_logger, "Cannot create directory %s: %s", path, strerror(errno));
                return -1;
            }
        }
    }
    
    // 检查SSL证书（如果启用SSL）
    if (g_ctx.server_config.enable_ssl) {
        if (access(g_ctx.server_config.ssl_cert_path, R_OK) != 0) {
            LOG_ERROR(g_logger, "SSL certificate not readable: %s", 
                     g_ctx.server_config.ssl_cert_path);
            return -1;
        }
        
        if (access(g_ctx.server_config.ssl_key_path, R_OK) != 0) {
            LOG_ERROR(g_logger, "SSL key not readable: %s", 
                     g_ctx.server_config.ssl_key_path);
            return -1;
        }
    }
    
    return 0;
}

// ==================== 模块初始化 ====================
static int initialize_modules(void) {
    int ret = 0;
    
    LOG_INFO(g_logger, "Initializing modules...");
    
    // 1. 初始化内存管理
    LOG_DEBUG(g_logger, "Initializing memory management...");
    
    // 2. 初始化数据库
    LOG_DEBUG(g_logger, "Initializing database...");
    db_config_t db_config = {
        .db_path = {0},
        .max_connections = 10,
        .query_timeout_ms = 5000,
        .wal_mode = true
    };
    
    snprintf(db_config.db_path, sizeof(db_config.db_path),
             "%s/database/secure_chat.db", g_ctx.working_dir);
    
    if (db_init(&db_config) != DB_SUCCESS) {
        LOG_ERROR(g_logger, "Failed to initialize database");
        ret = -1;
        goto cleanup;
    }
    
    // 3. 初始化认证模块
    LOG_DEBUG(g_logger, "Initializing authentication module...");
    if (auth_init("your-secret-key-here", db_config.db_path) != AUTH_SUCCESS) {
        LOG_ERROR(g_logger, "Failed to initialize authentication module");
        ret = -1;
        goto cleanup;
    }
    
    // 4. 初始化文件传输模块
    LOG_DEBUG(g_logger, "Initializing file transfer module...");
    ft_config_t ft_config = {
        .chunk_size = 65536,
        .max_retries = 3,
        .timeout_ms = 30000,
        .max_concurrent_transfers = 100,
        .enable_encryption = true,
        .enable_compression = true,
        .enable_resume = true,
        .verify_checksum = true,
        .max_file_size = 100 * 1024 * 1024  // 100MB
    };
    
    snprintf(ft_config.storage_path, sizeof(ft_config.storage_path),
             "%s/uploads", g_ctx.working_dir);
    snprintf(ft_config.temp_path, sizeof(ft_config.temp_path),
             "%s/temp", g_ctx.working_dir);
    
    if (ft_init(&ft_config) != FT_SUCCESS) {
        LOG_ERROR(g_logger, "Failed to initialize file transfer module");
        ret = -1;
        goto cleanup;
    }
    
    // 5. 初始化服务器实例
    LOG_DEBUG(g_logger, "Initializing server instance...");
    
    // 配置服务器
    server_config_init(&g_ctx.server_config);
    g_ctx.server_config.tcp_port = 8888;
    g_ctx.server_config.websocket_port = 8889;
    g_ctx.server_config.max_clients = 1000;
    g_ctx.server_config.enable_ssl = false;
    g_ctx.server_config.enable_encryption = true;
    g_ctx.server_config.enable_monitoring = true;
    g_ctx.server_config.monitoring_port = 8890;
    
    snprintf(g_ctx.server_config.database_path, sizeof(g_ctx.server_config.database_path),
             "%s/database/secure_chat.db", g_ctx.working_dir);
    snprintf(g_ctx.server_config.upload_dir, sizeof(g_ctx.server_config.upload_dir),
             "%s/uploads", g_ctx.working_dir);
    snprintf(g_ctx.server_config.temp_dir, sizeof(g_ctx.server_config.temp_dir),
             "%s/temp", g_ctx.working_dir);
    snprintf(g_ctx.server_config.log_file, sizeof(g_ctx.server_config.log_file),
             "%s/server.log", g_ctx.working_dir);
    
    g_ctx.server_instance = server_create(&g_ctx.server_config);
    if (!g_ctx.server_instance) {
        LOG_ERROR(g_logger, "Failed to create server instance");
        ret = -1;
        goto cleanup;
    }
    
    // 设置服务器事件回调
    server_set_event_callback(g_ctx.server_instance, server_event_callback, NULL);
    
    // 6. 初始化服务器
    LOG_DEBUG(g_logger, "Initializing server...");
    if (server_init(g_ctx.server_instance) != 0) {
        LOG_ERROR(g_logger, "Failed to initialize server");
        ret = -1;
        goto cleanup;
    }
    
    LOG_INFO(g_logger, "All modules initialized successfully");
    return 0;
    
cleanup:
    LOG_ERROR(g_logger, "Module initialization failed, cleaning up...");
    cleanup_modules();
    return ret;
}

static int cleanup_modules(void) {
    LOG_INFO(g_logger, "Cleaning up modules...");
    
    // 1. 停止并销毁服务器
    if (g_ctx.server_instance) {
        LOG_DEBUG(g_logger, "Stopping server...");
        server_stop(g_ctx.server_instance, true);
        
        LOG_DEBUG(g_logger, "Destroying server...");
        server_destroy(g_ctx.server_instance);
        g_ctx.server_instance = NULL;
    }
    
    // 2. 清理文件传输模块
    LOG_DEBUG(g_logger, "Cleaning up file transfer module...");
    ft_cleanup();
    
    // 3. 清理认证模块
    LOG_DEBUG(g_logger, "Cleaning up authentication module...");
    auth_cleanup();
    
    // 4. 清理数据库
    LOG_DEBUG(g_logger, "Cleaning up database...");
    db_cleanup();
    
    // 5. 清理配置管理器
    if (g_ctx.config_manager) {
        LOG_DEBUG(g_logger, "Destroying config manager...");
        config_manager_destroy(g_ctx.config_manager);
        g_ctx.config_manager = NULL;
    }
    
    // 6. 清理日志系统
    if (g_logger) {
        LOG_DEBUG(g_logger, "Destroying logger...");
        logger_destroy(g_logger);
        g_logger = NULL;
    }
    
    // 7. 释放内存
    if (g_ctx.config_path) {
        free(g_ctx.config_path);
        g_ctx.config_path = NULL;
    }
    
    if (g_ctx.pid_file_path) {
        free(g_ctx.pid_file_path);
        g_ctx.pid_file_path = NULL;
    }
    
    if (g_ctx.log_file_path) {
        free(g_ctx.log_file_path);
        g_ctx.log_file_path = NULL;
    }
    
    if (g_ctx.working_dir) {
        free(g_ctx.working_dir);
        g_ctx.working_dir = NULL;
    }
    
    LOG_INFO(g_logger, "Cleanup completed");
    return 0;
}

// ==================== 优雅关闭 ====================
static void graceful_shutdown(void) {
    LOG_INFO(g_logger, "Initiating graceful shutdown...");
    
    // 1. 停止接受新连接
    if (g_ctx.server_instance) {
        LOG_DEBUG(g_logger, "Stopping server...");
        server_state_t state = server_get_state(g_ctx.server_instance);
        if (state == SERVER_STATE_RUNNING) {
            server_stop(g_ctx.server_instance, true);
        }
    }
    
    // 2. 等待活动连接关闭
    LOG_DEBUG(g_logger, "Waiting for active connections to close...");
    int timeout = 30; // 30秒超时
    while (timeout-- > 0) {
        if (g_ctx.server_instance) {
            server_statistics_t stats;
            if (server_get_statistics(g_ctx.server_instance, &stats) == 0) {
                if (stats.active_connections == 0) {
                    LOG_DEBUG(g_logger, "All connections closed");
                    break;
                }
                LOG_DEBUG(g_logger, "Waiting for %lu connections...", 
                         stats.active_connections);
            }
        }
        sleep(1);
    }
    
    // 3. 清理模块
    cleanup_modules();
    
    // 4. 删除PID文件
    remove_pid_file();
    
    LOG_INFO(g_logger, "Graceful shutdown completed");
}

// ==================== 信号事件处理 ====================
static void handle_signal_events(void) {
    pthread_mutex_lock(&g_ctx_mutex);
    
    if (g_ctx.sig_terminate) {
        g_ctx.sig_terminate = 0;
        graceful_shutdown();
        exit(EXIT_SUCCESS);
    }
    
    if (g_ctx.sig_reload) {
        g_ctx.sig_reload = 0;
        if (reload_configuration() != 0) {
            LOG_ERROR(g_logger, "Configuration reload failed");
        }
    }
    
    if (g_ctx.sig_usr1) {
        g_ctx.sig_usr1 = 0;
        
        // 生成状态报告
        char report[4096];
        if (g_ctx.server_instance) {
            int len = server_generate_status_report(g_ctx.server_instance, 
                                                   report, sizeof(report));
            if (len > 0) {
                LOG_INFO(g_logger, "Status report:\n%s", report);
            }
        }
    }
    
    if (g_ctx.sig_usr2) {
        g_ctx.sig_usr2 = 0;
        
        // 日志轮转
        LOG_INFO(g_logger, "Rotating logs...");
        // TODO: 实现日志轮转逻辑
    }
    
    pthread_mutex_unlock(&g_ctx_mutex);
}

// ==================== 监控线程 ====================
static void *monitoring_thread(void *arg) {
    (void)arg;
    
    LOG_INFO(g_logger, "Monitoring thread started");
    
    while (!g_ctx.sig_terminate) {
        // 每5秒检查一次健康状态
        if (g_ctx.server_instance) {
            int health = server_check_health(g_ctx.server_instance);
            if (health != 0) {
                LOG_WARNING(g_logger, "Server health check failed: %d", health);
            }
        }
        
        // 每60秒执行维护任务
        static time_t last_maintenance = 0;
        time_t now = time(NULL);
        
        if (now - last_maintenance >= 60) {
            if (g_ctx.server_instance) {
                server_perform_maintenance(g_ctx.server_instance);
                last_maintenance = now;
            }
        }
        
        // 检查信号事件
        handle_signal_events();
        
        // 休眠5秒
        sleep(5);
    }
    
    LOG_INFO(g_logger, "Monitoring thread stopped");
    return NULL;
}

// ==================== 服务器事件回调 ====================
static void server_event_callback(int event_type, void *event_data, void *user_data) {
    (void)user_data;
    
    server_event_t *event = (server_event_t *)event_data;
    
    switch (event_type) {
        case SERVER_EVENT_CLIENT_CONNECT:
            LOG_INFO(g_logger, "Client connected: id=%lu, ip=%s", 
                    event->connection_id, "unknown");
            break;
            
        case SERVER_EVENT_CLIENT_DISCONNECT:
            LOG_INFO(g_logger, "Client disconnected: id=%lu, user=%lu", 
                    event->connection_id, event->user_id);
            break;
            
        case SERVER_EVENT_USER_LOGIN:
            LOG_INFO(g_logger, "User logged in: id=%lu", event->user_id);
            break;
            
        case SERVER_EVENT_USER_LOGOUT:
            LOG_INFO(g_logger, "User logged out: id=%lu", event->user_id);
            break;
            
        case SERVER_EVENT_ERROR:
            LOG_ERROR(g_logger, "Server error: %s", event->description);
            break;
            
        case SERVER_EVENT_WARNING:
            LOG_WARNING(g_logger, "Server warning: %s", event->description);
            break;
            
        default:
            LOG_DEBUG(g_logger, "Server event: type=%d", event_type);
            break;
    }
}

// ==================== 主函数 ====================
int main(int argc, char *argv[]) {
    int ret = EXIT_SUCCESS;
    pthread_t monitor_thread;
    
    // 1. 参数解析
    parse_arguments(argc, argv);
    
    // 2. 显示横幅
    if (!g_ctx.daemon_mode || g_ctx.verbose) {
        print_banner();
    }
    
    // 3. 检查配置模式
    if (g_ctx.check_config) {
        printf("Checking configuration file: %s\n", g_ctx.config_path);
        
        if (load_configuration() == 0) {
            printf("✓ Configuration is valid\n");
            ret = EXIT_SUCCESS;
        } else {
            printf("✗ Configuration is invalid\n");
            ret = EXIT_FAILURE;
        }
        
        cleanup_modules();
        return ret;
    }
    
    // 4. 初始化日志系统
    g_logger = logger_create();
    if (!g_logger) {
        fprintf(stderr, "Failed to create logger\n");
        return EXIT_FAILURE;
    }
    
    logger_set_level(g_logger, g_ctx.log_level);
    logger_set_file(g_logger, g_ctx.log_file_path);
    
    // 5. 加载配置
    if (load_configuration() != 0) {
        LOG_ERROR(g_logger, "Failed to load configuration");
        ret = EXIT_FAILURE;
        goto cleanup;
    }
    
    // 6. 权限检查
    if (check_privileges() != 0) {
        LOG_ERROR(g_logger, "Privilege check failed");
        ret = EXIT_FAILURE;
        goto cleanup;
    }
    
    // 7. 环境验证
    if (validate_environment() != 0) {
        LOG_ERROR(g_logger, "Environment validation failed");
        ret = EXIT_FAILURE;
        goto cleanup;
    }
    
    // 8. 注册信号处理器
    register_signal_handlers();
    
    // 9. 创建PID文件
    if (create_pid_file() != 0) {
        LOG_ERROR(g_logger, "Failed to create PID file");
        ret = EXIT_FAILURE;
        goto cleanup;
    }
    
    // 10. 守护进程化
    if (g_ctx.daemon_mode && !g_ctx.foreground) {
        if (daemonize() != 0) {
            LOG_ERROR(g_logger, "Failed to daemonize");
            ret = EXIT_FAILURE;
            goto cleanup;
        }
    }
    
    // 11. 初始化模块
    if (initialize_modules() != 0) {
        LOG_ERROR(g_logger, "Failed to initialize modules");
        ret = EXIT_FAILURE;
        goto cleanup;
    }
    
    // 12. 启动监控线程
    if (pthread_create(&monitor_thread, NULL, monitoring_thread, NULL) != 0) {
        LOG_ERROR(g_logger, "Failed to create monitoring thread");
        ret = EXIT_FAILURE;
        goto cleanup;
    }
    
    // 13. 启动服务器
    LOG_INFO(g_logger, "Starting server...");
    if (server_start(g_ctx.server_instance) != 0) {
        LOG_ERROR(g_logger, "Failed to start server");
        ret = EXIT_FAILURE;
        goto cleanup;
    }
    
    // 14. 主循环
    LOG_INFO(g_logger, "Server is running (pid=%d)", getpid());
    printf("Secure Chat Server v%s is running (pid=%d)\n", 
           PROGRAM_VERSION, getpid());
    printf("Press Ctrl+C to stop\n\n");
    
    while (!g_ctx.sig_terminate) {
        // 服务器主循环
        server_state_t state = server_get_state(g_ctx.server_instance);
        if (state == SERVER_STATE_ERROR) {
            LOG_ERROR(g_logger, "Server entered error state");
            break;
        }
        
        // 处理信号事件
        handle_signal_events();
        
        // 休眠100ms
        usleep(100000);
    }
    
    // 15. 等待监控线程结束
    pthread_join(monitor_thread, NULL);
    
cleanup:
    // 16. 优雅关闭
    graceful_shutdown();
    
    // 17. 清理资源
    cleanup_modules();
    
    LOG_INFO(g_logger, "Server stopped");
    return ret;
}