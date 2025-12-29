@echo off
echo SecureChat Simple Launcher
echo.
echo 1. Starting Docker services...
docker-compose up -d
timeout /t 5 /nobreak >nul
echo.
echo 2. Building project...
make clean >nul 2>nul
make all >nul 2>nul
echo.
echo 3. Ready!
echo.
echo PostgreSQL: localhost:5432
echo Redis: localhost:6379
echo.
if exist securechat.exe (
    echo Run: securechat.exe
) else if exist bin\securechat.exe (
    echo Run: bin\securechat.exe
)
echo.
pause
