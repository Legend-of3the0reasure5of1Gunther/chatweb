# WebSocket服务器Makefile
CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -pthread -std=c99 -I.
LDFLAGS = -pthread -lssl -lcrypto -lz

# 目标
TARGET = websocket_server
WEB_TARGET = websocket_monitor.html

# 源文件
SRCS = websocket_server.c websocket_file_transfer.c protocol.c threadpool.c utils.c
OBJS = $(SRCS:.c=.o)
HEADERS = websocket_protocol.h websocket_server.h protocol.h threadpool.h

# 默认目标
all: $(TARGET) $(WEB_TARGET)

# 主服务器程序
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# 编译规则
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Web界面
$(WEB_TARGET): websocket_monitor.html
	cp websocket_monitor.html $(WEB_TARGET)

# 清理
clean:
	rm -f $(TARGET) $(OBJS) $(WEB_TARGET)
	rm -f *.log uploads/* downloads/*

# 运行服务器
run: $(TARGET)
	./$(TARGET)

# 运行示例
example: websocket_server.c websocket_file_transfer.c
	$(CC) $(CFLAGS) -o example example.c websocket_server.c websocket_file_transfer.c threadpool.c $(LDFLAGS)
	./example

# 代码检查
check:
	cppcheck --enable=all --suppress=missingIncludeSystem $(SRCS)

# 内存检查
valgrind: $(TARGET)
	valgrind --leak-check=full --show-leak-kinds=all ./$(TARGET)

# 安装依赖
install_deps:
	sudo apt-get update
	sudo apt-get install -y build-essential libssl-dev libz-dev valgrind cppcheck

.PHONY: all clean run example check valgrind install_deps