@echo off
chcp 65001 >nul
echo ============================================
echo      SecureChat 启动脚本 (批处理版)
echo ============================================
echo.

echo [1/6] 检查环境配置...
where gcc >nul 2>nul
if %errorlevel% equ 0 (
    echo   [OK] gcc
) else (
    echo   [ERROR] gcc 未找到
    pause
    exit /b 1
)

where make >nul 2>nul
if %errorlevel% equ 0 (
    echo   [OK] make
) else (
    echo   [ERROR] make 未找到
    pause
    exit /b 1
)

where docker >nul 2>nul
if %errorlevel% equ 0 (
    echo   [OK] docker
) else (
    echo   [ERROR] docker 未找到
    pause
    exit /b 1
)

echo.
echo [2/6] 检查SSL证书...
if not exist "config\ssl\certificate.pem" (
    echo   [INFO] 生成SSL证书...
    if exist "Makefile" (
        make ssl-cert >nul 2>nul
    ) else (
        openssl req -x509 -nodes -days 365 -newkey rsa:2048 -keyout config\ssl\private.key -out config\ssl\certificate.pem -subj "/C=CN/ST=Beijing/L=Beijing/O=SecureChat/CN=localhost" >nul 2>nul
    )
    if %errorlevel% equ 0 (
        echo   [OK] SSL证书已生成
    ) else (
        echo   [ERROR] SSL证书生成失败
    )
) else (
    echo   [OK] SSL证书已存在
)

echo.
echo [3/6] 启动Docker服务...
docker-compose up -d >nul 2>nul
timeout /t 10 /nobreak >nul

echo   [INFO] 检查服务状态...
docker ps --filter "name=postgres" --format "{{.Status}}" >nul 2>nul
if %errorlevel% equ 0 (
    echo   [OK] PostgreSQL 已启动
) else (
    echo   [ERROR] PostgreSQL 启动失败
)

docker ps --filter "name=redis" --format "{{.Status}}" >nul 2>nul
if %errorlevel% equ 0 (
    echo   [OK] Redis 已启动
) else (
    echo   [ERROR] Redis 启动失败
)

echo.
echo [4/6] 初始化数据库...
if exist "database\init\init.sql" (
    docker exec -i securechat-db psql -U securechat_user -d securechat < database\init\init.sql >nul 2>nul
    if %errorlevel% equ 0 (
        echo   [OK] 数据库初始化完成
    ) else (
        echo   [ERROR] 数据库初始化失败
    )
) else (
    echo   [WARN] 数据库初始化脚本不存在
)

echo.
echo [5/6] 编译项目...
if exist "CMakeLists.txt" (
    echo   [INFO] 使用CMake编译...
    if not exist "build" mkdir build
    cd build
    cmake .. >nul 2>nul
    cmake --build . --config Release >nul 2>nul
    cd ..
    if %errorlevel% equ 0 (
        echo   [OK] CMake编译完成
        set BUILD_SUCCESS=1
    ) else (
        echo   [ERROR] CMake编译失败
    )
) else if exist "Makefile" (
    echo   [INFO] 使用Make编译...
    make clean >nul 2>nul
    make all >nul 2>nul
    if %errorlevel% equ 0 (
        echo   [OK] Make编译完成
        set BUILD_SUCCESS=1
    ) else (
        echo   [ERROR] Make编译失败
    )
) else (
    echo   [WARN] 没有找到构建文件
)

echo.
echo [6/6] 查找可执行文件...
set EXE=
if exist "bin\securechat.exe" (
    set EXE=bin\securechat.exe
) else if exist "build\Release\securechat.exe" (
    set EXE=build\Release\securechat.exe
) else if exist "build\securechat.exe" (
    set EXE=build\securechat.exe
) else if exist "securechat.exe" (
    set EXE=securechat.exe
)

if "%EXE%"=="" (
    echo   [ERROR] 没有找到可执行文件
) else (
    echo   [OK] 找到可执行文件: %EXE%
    echo.
    echo ============================================
    echo     服务信息
    echo ============================================
    echo PostgreSQL: localhost:5432
    echo Redis: localhost:6379
    echo Adminer: http://localhost:8082
    echo.
    echo 运行服务器: %EXE%
    echo.
    set /p RUN=是否立即启动服务器? (y/n): 
    if /i "%RUN%"=="y" (
        echo 启动服务器...
        %EXE%
    )
)

echo.
echo 按任意键退出...
pause >nul