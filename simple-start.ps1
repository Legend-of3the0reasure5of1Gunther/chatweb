# 最简单的启动脚本 - 避免编码和语法问题
Write-Host "Starting SecureChat..." -ForegroundColor Green

# 1. 检查基本工具
try {
    gcc --version 2>&1 | Out-Null
    Write-Host "gcc: OK" -ForegroundColor Green
} catch {
    Write-Host "gcc: ERROR - Not found" -ForegroundColor Red
    exit 1
}

try {
    make --version 2>&1 | Out-Null
    Write-Host "make: OK" -ForegroundColor Green
} catch {
    Write-Host "make: ERROR - Not found" -ForegroundColor Red
    exit 1
}

try {
    docker --version 2>&1 | Out-Null
    Write-Host "docker: OK" -ForegroundColor Green
} catch {
    Write-Host "docker: ERROR - Not found" -ForegroundColor Red
    exit 1
}

# 2. 启动Docker
Write-Host "`nStarting Docker services..." -ForegroundColor Yellow
docker-compose up -d
Start-Sleep -Seconds 5

# 3. 编译项目
Write-Host "`nBuilding project..." -ForegroundColor Yellow
if (Test-Path "Makefile") {
    make clean 2>&1 | Out-Null
    make all 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "Build: SUCCESS" -ForegroundColor Green
    } else {
        Write-Host "Build: FAILED" -ForegroundColor Red
    }
}

# 4. 运行
Write-Host "`nChecking for executable..." -ForegroundColor Yellow
if (Test-Path "securechat.exe") {
    Write-Host "Found: securechat.exe" -ForegroundColor Green
    Write-Host "`nTo run the server, execute:" -ForegroundColor Cyan
    Write-Host "  .\securechat.exe" -ForegroundColor White
} elseif (Test-Path "bin\securechat.exe") {
    Write-Host "Found: bin\securechat.exe" -ForegroundColor Green
    Write-Host "`nTo run the server, execute:" -ForegroundColor Cyan
    Write-Host "  .\bin\securechat.exe" -ForegroundColor White
} else {
    Write-Host "No executable found" -ForegroundColor Red
}

Write-Host "`nServices running on:" -ForegroundColor Cyan
Write-Host "  PostgreSQL: localhost:5432" -ForegroundColor White
Write-Host "  Redis: localhost:6379" -ForegroundColor White

Write-Host "`nDone!" -ForegroundColor Green
