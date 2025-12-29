#ifndef SECURE_CHAT_WEBSOCKET_SERVER_H
#define SECURE_CHAT_WEBSOCKET_SERVER_H

#include "websocket_protocol.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ==================== WebSocket服务器API ====================

/**
 * 创建WebSocket服务器
 * @param config 服务器配置，如果为NULL则使用默认配置
 * @return 服务器实例指针，失败返回NULL
 */
websocket_server_t* websocket_server_create(const websocket_config_t *config);

/**
 * 启动WebSocket服务器
 * @param server 服务器实例
 * @return 成功返回0，失败返回-1
 */
int websocket_server_start(websocket_server_t *server);

/**
 * 停止WebSocket服务器
 * @param server 服务器实例
 * @return 成功返回0，失败返回-1
 */
int websocket_server_stop(websocket_server_t *server);

/**
 * 销毁WebSocket服务器
 * @param server 服务器实例
 */
void websocket_server_destroy(websocket_server_t *server);

/**
 * 获取默认配置
 * @return 默认配置
 */
websocket_config_t websocket_get_default_config(void);

/**
 * 设置连接回调函数
 * @param server 服务器实例
 * @param callback 回调函数
 */
void websocket_set_connect_callback(websocket_server_t *server, websocket_connect_cb callback);

/**
 * 设置消息回调函数
 * @param server 服务器实例
 * @param callback 回调函数
 */
void websocket_set_message_callback(websocket_server_t *server, websocket_message_cb callback);

/**
 * 设置关闭回调函数
 * @param server 服务器实例
 * @param callback 回调函数
 */
void websocket_set_close_callback(websocket_server_t *server, websocket_close_cb callback);

/**
 * 设置错误回调函数
 * @param server 服务器实例
 * @param callback 回调函数
 */
void websocket_set_error_callback(websocket_server_t *server, websocket_error_cb callback);

/**
 * 设置二进制消息回调函数
 * @param server 服务器实例
 * @param callback 回调函数
 */
void websocket_set_binary_callback(websocket_server_t *server, websocket_binary_cb callback);

/**
 * 发送文本消息到指定客户端
 * @param server 服务器实例
 * @param client_id 客户端ID
 * @param text 文本消息
 * @return 成功返回0，失败返回-1
 */
int websocket_send_text(websocket_server_t *server, uint64_t client_id, const char *text);

/**
 * 发送二进制消息到指定客户端
 * @param server 服务器实例
 * @param client_id 客户端ID
 * @param data 二进制数据
 * @param length 数据长度
 * @return 成功返回0，失败返回-1
 */
int websocket_send_binary(websocket_server_t *server, uint64_t client_id, const uint8_t *data, size_t length);

/**
 * 发送JSON消息到指定客户端
 * @param server 服务器实例
 * @param client_id 客户端ID
 * @param json JSON字符串
 * @return 成功返回0，失败返回-1
 */
int websocket_send_json(websocket_server_t *server, uint64_t client_id, const char *json);

/**
 * 广播文本消息到所有客户端
 * @param server 服务器实例
 * @param text 文本消息
 * @return 成功发送的客户端数量
 */
int websocket_broadcast_text(websocket_server_t *server, const char *text);

/**
 * 广播二进制消息到所有客户端
 * @param server 服务器实例
 * @param data 二进制数据
 * @param length 数据长度
 * @return 成功发送的客户端数量
 */
int websocket_broadcast_binary(websocket_server_t *server, const uint8_t *data, size_t length);

/**
 * 关闭指定客户端连接
 * @param server 服务器实例
 * @param client_id 客户端ID
 * @param status_code 关闭状态码
 * @param reason 关闭原因
 * @return 成功返回0，失败返回-1
 */
int websocket_close_client(websocket_server_t *server, uint64_t client_id, 
                          uint16_t status_code, const char *reason);

/**
 * 获取指定客户端
 * @param server 服务器实例
 * @param client_id 客户端ID
 * @return 客户端指针，未找到返回NULL
 */
websocket_client_t* websocket_get_client(websocket_server_t *server, uint64_t client_id);

/**
 * 获取当前连接客户端数量
 * @param server 服务器实例
 * @return 客户端数量
 */
int websocket_get_client_count(websocket_server_t *server);

/**
 * 获取服务器统计信息
 * @param server 服务器实例
 * @param stats 统计信息存储位置
 * @return 成功返回0，失败返回-1
 */
int websocket_get_stats(websocket_server_t *server, websocket_stats_t *stats);

/**
 * 重置服务器统计信息
 * @param server 服务器实例
 */
void websocket_reset_stats(websocket_server_t *server);

/**
 * 解析WebSocket帧
 * @param data 原始数据
 * @param length 数据长度
 * @param opcode 返回操作码
 * @param payload 返回负载数据指针
 * @param payload_len 返回负载长度
 * @param is_final 返回是否为最终帧
 * @return 成功返回0，需要更多数据返回-2，失败返回-1
 */
int websocket_parse_frame(const uint8_t *data, size_t length, 
                         websocket_opcode_t *opcode, uint8_t **payload, 
                         size_t *payload_len, bool *is_final);

/**
 * 创建WebSocket帧
 * @param opcode 操作码
 * @param payload 负载数据
 * @param payload_len 负载长度
 * @param frame 返回的帧数据指针（需要调用者释放）
 * @param frame_len 返回的帧长度
 * @param mask 是否使用掩码
 * @param masking_key 掩码密钥
 * @return 成功返回0，失败返回-1
 */
int websocket_create_frame(websocket_opcode_t opcode, const uint8_t *payload, 
                          size_t payload_len, uint8_t **frame, size_t *frame_len,
                          bool mask, uint32_t masking_key);

/**
 * 生成WebSocket接受密钥
 * @param client_key 客户端密钥
 * @param accept_key 存储接受密钥的缓冲区
 * @param accept_key_len 缓冲区长度
 * @return 成功返回0，失败返回-1
 */
int websocket_generate_accept_key(const char *client_key, char *accept_key, size_t accept_key_len);

/**
 * 将状态码转换为字符串
 * @param status_code 状态码
 * @return 状态码字符串
 */
const char* websocket_status_code_to_string(uint16_t status_code);

/**
 * 将操作码转换为字符串
 * @param opcode 操作码
 * @return 操作码字符串
 */
const char* websocket_opcode_to_string(websocket_opcode_t opcode);

#ifdef __cplusplus
}
#endif

#endif // SECURE_CHAT_WEBSOCKET_SERVER_H