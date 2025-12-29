#include "auth.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

void test_auth_module(void) {
    printf("=== Testing Authentication Module ===\n");
    
    // 初始化认证模块
    auth_error_code_t result = auth_init("your-secret-key-here-with-at-least-32-chars", NULL);
    assert(result == AUTH_SUCCESS);
    printf("✓ auth_init successful\n");
    
    // 测试用户注册
    auth_token_pair_t tokens;
    result = auth_register_user("testuser", "TestPassword123!", 
                               "test@example.com", "Test User",
                               "192.168.1.100", &tokens);
    assert(result == AUTH_SUCCESS);
    printf("✓ User registration successful\n");
    
    // 测试用户登录
    auth_token_pair_t login_tokens;
    result = auth_login_user("testuser", "TestPassword123!",
                            "192.168.1.100", "Test Client",
                            &login_tokens);
    assert(result == AUTH_SUCCESS);
    printf("✓ User login successful\n");
    
    // 测试令牌验证
    uint64_t user_id;
    result = auth_validate_token(login_tokens.access_token, &user_id);
    assert(result == AUTH_SUCCESS);
    printf("✓ Token validation successful, user_id: %llu\n", 
           (unsigned long long)user_id);
    
    // 测试密码强度检查
    result = auth_register_user("weakuser", "123", 
                               "weak@example.com", "Weak User",
                               "192.168.1.101", NULL);
    assert(result == AUTH_PASSWORD_TOO_WEAK);
    printf("✓ Password strength check working\n");
    
    // 测试重复用户名
    result = auth_register_user("testuser", "AnotherPassword123!", 
                               "another@example.com", "Another User",
                               "192.168.1.102", NULL);
    assert(result == AUTH_INVALID_CREDENTIALS);
    printf("✓ Duplicate username check working\n");
    
    // 测试错误密码
    auth_token_pair_t wrong_pass_tokens;
    result = auth_login_user("testuser", "WrongPassword123!",
                            "192.168.1.100", "Test Client",
                            &wrong_pass_tokens);
    assert(result == AUTH_INVALID_CREDENTIALS);
    printf("✓ Wrong password detection working\n");
    
    // 测试用户登出
    result = auth_logout_user(user_id, login_tokens.access_token);
    assert(result == AUTH_SUCCESS);
    printf("✓ User logout successful\n");
    
    // 测试令牌过期
    result = auth_validate_token(login_tokens.access_token, &user_id);
    assert(result == AUTH_TOKEN_EXPIRED);
    printf("✓ Token invalidation after logout working\n");
    
    // 清理
    auth_cleanup();
    printf("✓ auth_cleanup successful\n");
    
    printf("=== All tests passed ===\n");
}

int main() {
    test_auth_module();
    return 0;
}