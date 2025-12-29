#include "client.h"
#include "../common/protocol.h"
#include "../common/security.h"
#include "../common/logger.h"
#include "../common/buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <ctype.h>
#include <termios.h>
#include <sys/select.h>

// 全局客户端实例
static secure_chat_client_t *global_client = NULL;

// 信号处理器
static void signal_handler(int signum) {
    LOG_INFO("收到信号 %d，正在关闭客户端...", signum);
    
    if (global_client) {
        client_disconnect(global_client, true);
    }
}

// 显示使用帮助
static void print_usage(const char *program_name) {
    printf("SecureChat 客户端 v%d.%d.%d\n\n", 
           PROTOCOL_VERSION_MAJOR, PROTOCOL_VERSION_MINOR, PROTOCOL_VERSION_PATCH);
    
    printf("用法: %s [选项]\n\n", program_name);
    printf("选项:\n");
    printf("  -h, --help              显示此帮助信息\n");
    printf("  -s, --server HOST       服务器地址 (默认: localhost)\n");
    printf("  -p, --port PORT         服务器端口 (默认: 8888)\n");
    printf("  -u, --user USERNAME     用户名\n");
    printf("  -w, --password PASSWORD 密码\n");
    printf("  -c, --config FILE       配置文件路径\n");
    printf("  -l, --log FILE          日志文件路径\n");
    printf("  -v, --verbose           详细输出\n");
    printf("  -t, --test              测试模式\n");
    printf("  -e, --encrypt           启用加密\n");
    printf("  --ssl                   使用SSL/TLS连接\n");
    printf("\n命令:\n");
    printf("  connect                 连接到服务器\n");
    printf("  disconnect              断开连接\n");
    printf("  login                   登录\n");
    printf("  register                注册新用户\n");
    printf("  send <用户ID> <消息>    发送消息\n");
    printf("  broadcast <消息>        广播消息\n");
    printf("  users                   查看在线用户\n");
    printf("  status <状态>           更新状态 (online/away/busy/offline)\n");
    printf("  file <用户ID> <文件>    发送文件\n");
    printf("  transfers               查看文件传输\n");
    printf("  cancel <传输ID>         取消文件传输\n");
    printf("  quit, exit              退出程序\n");
    printf("  help                    显示此帮助\n");
    printf("\n示例:\n");
    printf("  %s -s chat.example.com -p 8888\n", program_name);
    printf("  %s -u alice -w password123\n", program_name);
}

// 获取密码（不显示在终端）
static int get_password(char *password, size_t max_len, const char *prompt) {
    struct termios old_term, new_term;
    int i = 0;
    int ch;
    
    printf("%s", prompt);
    fflush(stdout);
    
    // 保存终端设置
    tcgetattr(STDIN_FILENO, &old_term);
    new_term = old_term;
    
    // 禁用回显
    new_term.c_lflag &= ~(ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_term);
    
    // 读取密码
    while ((ch = getchar()) != '\n' && ch != EOF && i < max_len - 1) {
        password[i++] = ch;
    }
    password[i] = '\0';
    
    // 恢复终端设置
    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    printf("\n");
    
    return i;
}

// 解析命令行参数
static int parse_arguments(int argc, char *argv[], client_config_t *config) {
    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"server", required_argument, 0, 's'},
        {"port", required_argument, 0, 'p'},
        {"user", required_argument, 0, 'u'},
        {"password", required_argument, 0, 'w'},
        {"config", required_argument, 0, 'c'},
        {"log", required_argument, 0, 'l'},
        {"verbose", no_argument, 0, 'v'},
        {"test", no_argument, 0, 't'},
        {"encrypt", no_argument, 0, 'e'},
        {"ssl", no_argument, 0, 1000},
        {0, 0, 0, 0}
    };
    
    const char *config_file = NULL;
    
    // 初始化默认配置
    client_config_init(config);
    
    int opt;
    while ((opt = getopt_long(argc, argv, "hs:p:u:w:c:l:vte", long_options, NULL)) != -1) {
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                exit(0);
                
            case 's':
                strncpy(config->server_host, optarg, sizeof(config->server_host) - 1);
                config->server_host[sizeof(config->server_host) - 1] = '\0';
                break;
                
            case 'p':
                config->server_port = atoi(optarg);
                break;
                
            case 'u':
                strncpy(config->username, optarg, sizeof(config->username) - 1);
                config->username[sizeof(config->username) - 1] = '\0';
                break;
                
            case 'w':
                strncpy(config->password, optarg, sizeof(config->password) - 1);
                config->password[sizeof(config->password) - 1] = '\0';
                break;
                
            case 'c':
                config_file = optarg;
                break;
                
            case 'l':
                strncpy(config->log_file, optarg, sizeof(config->log_file) - 1);
                config->log_file[sizeof(config->log_file) - 1] = '\0';
                break;
                
            case 'v':
                config->log_level = LOG_LEVEL_DEBUG;
                break;
                
            case 't':
                config->reconnect_attempts = 0; // 测试模式不重连
                break;
                
            case 'e':
                config->enable_encryption = true;
                break;
                
            case 1000: // --ssl
                config->use_ssl = true;
                break;
                
            default:
                fprintf(stderr, "未知选项\n");
                return -1;
        }
    }
    
    // 加载配置文件（如果提供）
    if (config_file) {
        if (client_config_load_from_file(config, config_file) != 0) {
            fprintf(stderr, "警告: 无法加载配置文件 %s\n", config_file);
        }
    }
    
    return 0;
}

// 消息处理器：文本消息
static void handle_text_message(uint32_t msg_type, const void *data, size_t len, void *user_data) {
    secure_chat_client_t *client = (secure_chat_client_t*)user_data;
    
    if (msg_type == MSG_CHAT_TEXT_RECEIVE) {
        const message_content_t *msg = (const message_content_t*)data;
        
        // 查找发送者信息
        const char *sender_name = "未知用户";
        for (size_t i = 0; i < client->chat_session_count; i++) {
            if (client->chat_sessions[i].user_id == msg->sender_id) {
                sender_name = client->chat_sessions[i].nickname;
                break;
            }
        }
        
        // 显示消息
        time_t timestamp = msg->timestamp / 1000;
        struct tm *timeinfo = localtime(&timestamp);
        
        printf("\n[%02d:%02d] %s: %s\n", 
               timeinfo->tm_hour, timeinfo->tm_min,
               sender_name, msg->content);
        
        printf("> ");
        fflush(stdout);
    }
}

// 消息处理器：用户列表
static void handle_user_list(uint32_t msg_type, const void *data, size_t len, void *user_data) {
    secure_chat_client_t *client = (secure_chat_client_t*)user_data;
    
    if (msg_type == MSG_USER_LIST_GET_RESPONSE) {
        const user_info_t *users = (const user_info_t*)data;
        uint32_t count = len / sizeof(user_info_t);
        
        printf("\n=== 在线用户 (%u) ===\n", count);
        
        for (uint32_t i = 0; i < count; i++) {
            const user_info_t *user = &users[i];
            time_t last_seen = user->last_seen / 1000;
            struct tm *timeinfo = localtime(&last_seen);
            
            printf("%llu. %s [%s] %s (最后在线: %02d:%02d)\n",
                   (unsigned long long)user->user_id,
                   user->nickname,
                   user->username,
                   user_status_to_string(user->status),
                   timeinfo->tm_hour, timeinfo->tm_min);
        }
        
        printf("> ");
        fflush(stdout);
    }
}

// 消息处理器：文件传输进度
static void handle_file_progress(uint32_t msg_type, const void *data, size_t len, void *user_data) {
    secure_chat_client_t *client = (secure_chat_client_t*)user_data;
    
    if (msg_type == MSG_FILE_PROGRESS_UPDATE) {
        const file_transfer_status_t *progress = (const file_transfer_status_t*)data;
        
        // 更新传输状态
        for (size_t i = 0; i < client->file_transfer_count; i++) {
            if (client->file_transfers[i].transfer_id == progress->transfer_id) {
                client->file_transfers[i] = *progress;
                
                printf("\r文件传输: %s - %u%%", 
                       progress->filename, progress->progress);
                fflush(stdout);
                
                if (progress->status == 2) { // 完成
                    printf("\n文件传输完成: %s\n", progress->filename);
                    printf("> ");
                    fflush(stdout);
                } else if (progress->status == 3 || progress->status == 4) { // 失败或取消
                    printf("\n文件传输失败/取消: %s\n", progress->filename);
                    printf("> ");
                    fflush(stdout);
                }
                break;
            }
        }
    }
}

// 错误回调
static void error_callback(int error_code, const char *error_msg, void *user_data) {
    secure_chat_client_t *client = (secure_chat_client_t*)user_data;
    
    fprintf(stderr, "\n错误 [%d]: %s\n", error_code, error_msg);
    if (client_get_state(client) != CLIENT_STATE_DISCONNECTED) {
        printf("> ");
        fflush(stdout);
    }
}

// 状态变更回调
static void state_change_callback(client_state_t old_state, client_state_t new_state, void *user_data) {
    secure_chat_client_t *client = (secure_chat_client_t*)user_data;
    
    const char *state_names[] = {
        "已断开",
        "连接中",
        "已连接",
        "认证中",
        "已认证",
        "重连中",
        "断开中",
        "错误"
    };
    
    printf("\n状态变更: %s -> %s\n", 
           state_names[old_state], state_names[new_state]);
    
    if (new_state == CLIENT_STATE_AUTHENTICATED) {
        printf("欢迎回来，%s！\n", client->current_nickname);
    }
    
    if (new_state != CLIENT_STATE_DISCONNECTED && new_state != CLIENT_STATE_ERROR) {
        printf("> ");
        fflush(stdout);
    }
}

// 交互式命令行
static void interactive_shell(secure_chat_client_t *client) {
    char input[1024];
    char command[256];
    char arg1[256];
    char arg2[256];
    char message[512];
    
    printf("SecureChat 客户端已启动。输入 'help' 查看命令。\n");
    
    while (client_get_state(client) != CLIENT_STATE_DISCONNECTED) {
        printf("> ");
        fflush(stdout);
        
        if (!fgets(input, sizeof(input), stdin)) {
            break;
        }
        
        // 移除换行符
        input[strcspn(input, "\n")] = 0;
        
        if (strlen(input) == 0) {
            continue;
        }
        
        // 解析命令
        int args = sscanf(input, "%255s %255s %511[^\n]", command, arg1, arg2);
        
        for (char *p = command; *p; p++) {
            *p = tolower(*p);
        }
        
        if (strcmp(command, "help") == 0 || strcmp(command, "?") == 0) {
            printf("\n可用命令:\n");
            printf("  connect                连接到服务器\n");
            printf("  disconnect             断开连接\n");
            printf("  login                  登录\n");
            printf("  register               注册新用户\n");
            printf("  send <ID> <消息>       发送私聊消息\n");
            printf("  broadcast <消息>       发送广播消息\n");
            printf("  users                  查看在线用户\n");
            printf("  status <状态>          更新状态\n");
            printf("  file <ID> <文件>       发送文件\n");
            printf("  transfers              查看文件传输\n");
            printf("  cancel <传输ID>        取消文件传输\n");
            printf("  stats                  查看统计信息\n");
            printf("  quit, exit             退出程序\n");
            printf("  help                   显示此帮助\n");
        }
        else if (strcmp(command, "connect") == 0) {
            int result = client_connect(client);
            if (result == 0) {
                printf("连接请求已发送\n");
            } else {
                printf("连接失败: %d\n", result);
            }
        }
        else if (strcmp(command, "disconnect") == 0) {
            client_disconnect(client, true);
        }
        else if (strcmp(command, "login") == 0) {
            char username[64] = "";
            char password[128] = "";
            
            if (strlen(client->config.username) == 0) {
                printf("用户名: ");
                fflush(stdout);
                if (!fgets(username, sizeof(username), stdin)) continue;
                username[strcspn(username, "\n")] = 0;
            } else {
                strcpy(username, client->config.username);
            }
            
            if (strlen(client->config.password) == 0) {
                get_password(password, sizeof(password), "密码: ");
            } else {
                strcpy(password, client->config.password);
            }
            
            int result = client_login(client, username, password);
            if (result == 0) {
                printf("登录请求已发送\n");
            } else {
                printf("登录失败: %d\n", result);
            }
        }
        else if (strcmp(command, "register") == 0) {
            printf("注册功能暂未实现\n");
        }
        else if (strcmp(command, "send") == 0) {
            if (args >= 3) {
                uint64_t receiver_id = strtoull(arg1, NULL, 10);
                uint64_t message_id;
                
                int result = client_send_text_message(client, receiver_id, arg2, strlen(arg2), &message_id);
                if (result == 0) {
                    printf("消息已发送 (ID: %llu)\n", (unsigned long long)message_id);
                } else {
                    printf("发送失败: %d\n", result);
                }
            } else {
                printf("用法: send <用户ID> <消息>\n");
            }
        }
        else if (strcmp(command, "broadcast") == 0) {
            if (args >= 2) {
                snprintf(message, sizeof(message), "%s %s", arg1, arg2);
                uint64_t message_id;
                
                int result = client_send_text_message(client, 0, message, strlen(message), &message_id);
                if (result == 0) {
                    printf("广播消息已发送 (ID: %llu)\n", (unsigned long long)message_id);
                } else {
                    printf("发送失败: %d\n", result);
                }
            } else {
                printf("用法: broadcast <消息>\n");
            }
        }
        else if (strcmp(command, "users") == 0) {
            int result = client_request_user_list(client, true);
            if (result != 0) {
                printf("请求失败: %d\n", result);
            }
        }
        else if (strcmp(command, "status") == 0) {
            if (args >= 2) {
                uint8_t status = USER_STATUS_ONLINE;
                
                if (strcasecmp(arg1, "online") == 0) status = USER_STATUS_ONLINE;
                else if (strcasecmp(arg1, "away") == 0) status = USER_STATUS_AWAY;
                else if (strcasecmp(arg1, "busy") == 0) status = USER_STATUS_BUSY;
                else if (strcasecmp(arg1, "offline") == 0) status = USER_STATUS_OFFLINE;
                else {
                    printf("无效状态: %s (可用: online, away, busy, offline)\n", arg1);
                    continue;
                }
                
                const char *status_msg = (args >= 3) ? arg2 : "";
                int result = client_update_status(client, status, status_msg);
                if (result == 0) {
                    printf("状态已更新\n");
                } else {
                    printf("更新失败: %d\n", result);
                }
            } else {
                printf("用法: status <状态> [状态消息]\n");
            }
        }
        else if (strcmp(command, "file") == 0) {
            if (args >= 3) {
                uint64_t receiver_id = strtoull(arg1, NULL, 10);
                uint64_t transfer_id;
                
                int result = client_send_file(client, receiver_id, arg2, &transfer_id);
                if (result == 0) {
                    printf("文件传输已启动 (传输ID: %llu)\n", (unsigned long long)transfer_id);
                } else {
                    printf("发送失败: %d\n", result);
                }
            } else {
                printf("用法: file <用户ID> <文件路径>\n");
            }
        }
        else if (strcmp(command, "transfers") == 0) {
            file_transfer_status_t *transfers;
            size_t count;
            
            int result = client_get_file_transfers(client, &transfers, &count);
            if (result == 0 && count > 0) {
                printf("\n=== 文件传输 (%zu) ===\n", count);
                
                for (size_t i = 0; i < count; i++) {
                    const file_transfer_status_t *t = &transfers[i];
                    const char *status_str[] = {"等待", "传输中", "完成", "失败", "取消"};
                    
                    printf("传输ID: %llu\n", (unsigned long long)t->transfer_id);
                    printf("  文件: %s\n", t->filename);
                    printf("  大小: %llu 字节\n", (unsigned long long)t->file_size);
                    printf("  进度: %u%%\n", t->progress);
                    printf("  状态: %s\n", status_str[t->status]);
                    
                    if (i < count - 1) {
                        printf("  ---\n");
                    }
                }
                
                client_free_file_transfers(transfers, count);
            } else {
                printf("没有进行中的文件传输\n");
            }
        }
        else if (strcmp(command, "cancel") == 0) {
            if (args >= 2) {
                uint64_t transfer_id = strtoull(arg1, NULL, 10);
                int result = client_cancel_file_transfer(client, transfer_id);
                if (result == 0) {
                    printf("文件传输已取消\n");
                } else {
                    printf("取消失败: %d\n", result);
                }
            } else {
                printf("用法: cancel <传输ID>\n");
            }
        }
        else if (strcmp(command, "stats") == 0) {
            uint64_t sent, received, bytes_sent, bytes_received, connection_time;
            
            client_get_statistics(client, &sent, &received, &bytes_sent, &bytes_received, &connection_time);
            
            printf("\n=== 统计信息 ===\n");
            printf("发送消息: %llu\n", (unsigned long long)sent);
            printf("接收消息: %llu\n", (unsigned long long)received);
            printf("发送字节: %llu\n", (unsigned long long)bytes_sent);
            printf("接收字节: %llu\n", (unsigned long long)bytes_received);
            printf("连接时间: %llu 秒\n", (unsigned long long)connection_time / 1000);
        }
        else if (strcmp(command, "quit") == 0 || strcmp(command, "exit") == 0) {
            printf("正在退出...\n");
            client_disconnect(client, true);
            break;
        }
        else {
            printf("未知命令: %s (输入 'help' 查看可用命令)\n", command);
        }
    }
}

int main(int argc, char *argv[]) {
    // 初始化日志系统
    if (!logger_init(LOG_LEVEL_INFO, NULL)) {
        fprintf(stderr, "日志系统初始化失败\n");
        return 1;
    }
    
    // 初始化安全模块
    if (!security_init()) {
        LOG_ERROR("安全模块初始化失败");
        return 1;
    }
    
    // 解析命令行参数
    client_config_t config;
    if (parse_arguments(argc, argv, &config) != 0) {
        return 1;
    }
    
    // 验证配置
    char error_buffer[256];
    if (!client_config_validate(&config, error_buffer, sizeof(error_buffer))) {
        fprintf(stderr, "配置无效: %s\n", error_buffer);
        return 1;
    }
    
    // 创建客户端实例
    global_client = client_create(&config);
    if (!global_client) {
        fprintf(stderr, "客户端创建失败\n");
        return 1;
    }
    
    // 设置信号处理器
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 设置回调函数
    client_set_error_callback(global_client, error_callback, global_client);
    client_set_state_change_callback(global_client, state_change_callback, global_client);
    
    // 注册消息处理器
    CLIENT_REGISTER_HANDLER(global_client, MSG_CHAT_TEXT_RECEIVE, handle_text_message, global_client);
    CLIENT_REGISTER_HANDLER(global_client, MSG_USER_LIST_GET_RESPONSE, handle_user_list, global_client);
    CLIENT_REGISTER_HANDLER(global_client, MSG_FILE_PROGRESS_UPDATE, handle_file_progress, global_client);
    
    // 运行交互式命令行
    interactive_shell(global_client);
    
    // 清理资源
    client_destroy(global_client);
    security_cleanup();
    logger_cleanup();
    
    printf("客户端已关闭\n");
    return 0;
}