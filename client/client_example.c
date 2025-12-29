// client_example.c
#include "client.h"
#include <stdio.h>

int main(int argc, char *argv[]) {
    client_config_t config;
    
    // 方法1：使用默认配置
    client_config_init(&config);
    strcpy(config.server_host, "chat.example.com");
    config.server_port = 8888;
    
    // 方法2：从文件加载
    if (client_config_load_from_file(&config, "~/.securechat/config.json") != 0) {
        printf("使用默认配置\n");
        client_config_init(&config);
        strcpy(config.server_host, "localhost");
        config.server_port = 8888;
    }
    
    // 方法3：验证配置
    char error_buffer[256];
    if (!client_config_validate(&config, error_buffer, sizeof(error_buffer))) {
        printf("配置错误: %s\n", error_buffer);
        return 1;
    }
    
    // 创建客户端
    secure_chat_client_t *client = client_create(&config);
    if (!client) {
        printf("客户端创建失败\n");
        return 1;
    }
    
    // 设置回调
    client_set_error_callback(client, error_callback, client);
    client_set_state_change_callback(client, state_change_callback, client);
    
    // 连接到服务器
    if (client_connect(client) != 0) {
        printf("连接失败\n");
        client_destroy(client);
        return 1;
    }
    
    // 登录
    if (strlen(config.username) > 0 && strlen(config.password) > 0) {
        if (client_login(client, config.username, config.password) != 0) {
            printf("登录失败\n");
        }
    }
    
    // 等待用户操作...
    
    // 断开连接
    client_disconnect(client, true);
    client_destroy(client);
    
    return 0;
}