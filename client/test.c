#include "client.h"
#include <assert.h>
#include <string.h>

// 测试回调函数
static void test_error_callback(int error_code, const char *error_msg, void *user_data) {
    printf("错误回调: [%d] %s\n", error_code, error_msg);
}

static void test_state_callback(client_state_t old_state, client_state_t new_state, void *user_data) {
    const char *state_names[] = {
        "已断开", "连接中", "已连接", "认证中", "已认证", "重连中", "断开中", "错误"
    };
    printf("状态变更: %s -> %s\n", state_names[old_state], state_names[new_state]);
}

// 测试客户端创建和销毁
static void test_client_create_destroy() {
    printf("测试客户端创建和销毁...\n");
    
    client_config_t config;
    client_config_init(&config);
    strcpy(config.server_host, "localhost");
    config.server_port = 8888;
    
    secure_chat_client_t *client = client_create(&config);
    assert(client != NULL);
    assert(client_get_state(client) == CLIENT_STATE_DISCONNECTED);
    
    client_destroy(client);
    printf("✓ 客户端创建和销毁测试通过\n");
}

// 测试配置验证
static void test_config_validation() {
    printf("测试配置验证...\n");
    
    client_config_t config;
    client_config_init(&config);
    
    char error_buffer[256];
    
    // 有效配置
    strcpy(config.server_host, "example.com");
    config.server_port = 8888;
    assert(client_config_validate(&config, error_buffer, sizeof(error_buffer)) == true);
    
    // 无效主机名
    strcpy(config.server_host, "");
    assert(client_config_validate(&config, error_buffer, sizeof(error_buffer)) == false);
    
    // 无效端口
    strcpy(config.server_host, "example.com");
    config.server_port = 0;
    assert(client_config_validate(&config, error_buffer, sizeof(error_buffer)) == false);
    
    config.server_port = 70000;
    assert(client_config_validate(&config, error_buffer, sizeof(error_buffer)) == false);
    
    printf("✓ 配置验证测试通过\n");
}

// 测试消息处理器
static void test_message_handler_callback(uint32_t msg_type, const void *data, size_t len, void *user_data) {
    int *callback_count = (int*)user_data;
    (*callback_count)++;
}

static void test_message_handlers() {
    printf("测试消息处理器...\n");
    
    client_config_t config;
    client_config_init(&config);
    
    secure_chat_client_t *client = client_create(&config);
    assert(client != NULL);
    
    int callback_count = 0;
    
    // 注册消息处理器
    assert(client_register_message_handler(client, 0x12345678, 
                                          test_message_handler_callback, 
                                          &callback_count) == 0);
    
    // 再次注册相同的处理器
    assert(client_register_message_handler(client, 0x12345678,
                                          test_message_handler_callback,
                                          &callback_count) == 0);
    
    // 模拟消息处理（这里不实际发送消息，只测试注册功能）
    client_destroy(client);
    
    printf("✓ 消息处理器测试通过\n");
}

// 测试统计信息
static void test_statistics() {
    printf("测试统计信息...\n");
    
    client_config_t config;
    client_config_init(&config);
    
    secure_chat_client_t *client = client_create(&config);
    assert(client != NULL);
    
    uint64_t sent, received, bytes_sent, bytes_received, connection_time;
    
    // 初始统计信息应为0
    client_get_statistics(client, &sent, &received, &bytes_sent, &bytes_received, &connection_time);
    assert(sent == 0);
    assert(received == 0);
    assert(bytes_sent == 0);
    assert(bytes_received == 0);
    assert(connection_time == 0);
    
    client_destroy(client);
    
    printf("✓ 统计信息测试通过\n");
}

// 主测试函数
int main(int argc, char *argv[]) {
    printf("开始客户端测试...\n\n");
    
    // 初始化日志系统
    logger_init(LOG_LEVEL_INFO, NULL);
    
    // 运行测试
    test_client_create_destroy();
    test_config_validation();
    test_message_handlers();
    test_statistics();
    
    printf("\n所有测试通过！\n");
    
    return 0;
}