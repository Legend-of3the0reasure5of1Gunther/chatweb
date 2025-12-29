#!/bin/bash
# scripts/security_check.sh

set -e

echo "=== SecureChat 安全扫描开始 ==="

# 1. 检查依赖漏洞
echo "检查依赖漏洞..."
npm audit --production
pip-audit
cargo audit

# 2. 检查已知安全漏洞
echo "检查已知安全漏洞..."
gitleaks detect --source . -v
trufflehog filesystem .

# 3. 代码安全检查
echo "代码安全检查..."
# 使用cppcheck检查C代码
cppcheck --enable=all --suppress=missingIncludeSystem server/ common/

# 使用semgrep检查安全问题
semgrep --config auto .

# 4. 网络安全检查
echo "网络端口检查..."
nmap -sV -p 1-65535 localhost

# 5. SSL/TLS检查
echo "SSL/TLS配置检查..."
testssl.sh localhost:8889

# 6. 文件权限检查
echo "文件权限检查..."
find . -type f \( -name "*.sh" -o -name "*.py" \) -exec ls -la {} \;
find . -type f -perm /4000 -exec ls -la {} \;

# 7. 密码强度检查
echo "密码策略检查..."
echo "检查密码哈希算法..."
grep -r "password" . --include="*.c" --include="*.cpp" --include="*.js" | grep -v "//"

# 8. SQL注入检查
echo "SQL注入漏洞检查..."
sqlmap -u "http://localhost:8888" --batch

# 9. XSS检查
echo "XSS漏洞检查..."
# 使用ZAP进行安全扫描
# zap-cli quick-scan --self-contained --start-options '-config api.disablekey=true' http://localhost:8000

# 10. 输出报告
echo "生成安全报告..."
echo "=== 安全扫描完成 ==="
echo ""
echo "建议的安全改进："
echo "1. 启用所有连接的SSL/TLS加密"
echo "2. 实现Web应用防火墙(WAF)"
echo "3. 定期更新依赖包"
echo "4. 实现完整的审计日志"
echo "5. 启用双因素认证"
echo "6. 实施严格的输入验证"
echo "7. 使用安全头（CSP, HSTS等）"
echo "8. 定期进行渗透测试"