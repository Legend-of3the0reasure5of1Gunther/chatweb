#ifndef SECURITY_H
#define SECURITY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 加密配置 ====================
#define SECURITY_KEY_LENGTH 32
#define SECURITY_IV_LENGTH 16
#define SECURITY_HMAC_LENGTH 32
#define SECURITY_NONCE_LENGTH 12

// ==================== 函数声明 ====================

// 加密/解密
int security_encrypt_aes_gcm(const uint8_t* plaintext, size_t plaintext_len,
                            const uint8_t* key, const uint8_t* iv,
                            uint8_t* ciphertext, uint8_t* tag);
int security_decrypt_aes_gcm(const uint8_t* ciphertext, size_t ciphertext_len,
                            const uint8_t* tag, const uint8_t* key,
                            const uint8_t* iv, uint8_t* plaintext);

// 哈希函数
void security_hash_sha256(const uint8_t* data, size_t data_len, uint8_t* hash);
void security_hash_sha512(const uint8_t* data, size_t data_len, uint8_t* hash);
bool security_verify_hash(const uint8_t* data, size_t data_len,
                         const uint8_t* expected_hash);

// HMAC
void security_hmac_sha256(const uint8_t* key, size_t key_len,
                         const uint8_t* data, size_t data_len,
                         uint8_t* hmac);
bool security_verify_hmac(const uint8_t* key, size_t key_len,
                         const uint8_t* data, size_t data_len,
                         const uint8_t* expected_hmac);

// 密钥派生
int security_derive_key_pbkdf2(const char* password, const uint8_t* salt,
                              size_t salt_len, int iterations,
                              uint8_t* key, size_t key_len);

// 随机数生成
void security_generate_random_bytes(uint8_t* buffer, size_t length);
void security_generate_iv(uint8_t* iv);
void security_generate_nonce(uint8_t* nonce);

// 安全比较
bool security_memcmp_constant_time(const void* a, const void* b, size_t len);

// 输入清理
void security_cleanse_memory(void* ptr, size_t len);
char* security_sanitize_string(const char* input, size_t max_len);

// 证书验证
bool security_verify_certificate(const char* cert_path, const char* ca_path);

#ifdef __cplusplus
}
#endif

#endif // SECURITY_H