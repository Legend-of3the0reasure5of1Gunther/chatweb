# 监控模块Makefile
CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -pthread -std=c99 -I. -D_GNU_SOURCE
LDFLAGS = -pthread -lm -lz -ljansson

# 目标
TARGET = monitoring_server
WEB_TARGET = monitoring_dashboard.html

# 源文件
SRCS = monitoring.c monitoring_server.c protocol.c threadpool.c utils.c
OBJS = $(SRCS:.c=.o)
HEADERS = monitoring_protocol.h monitoring.h protocol.h threadpool.h

# 默认目标
all: $(TARGET) $(WEB_TARGET)

# 监控服务器程序
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# 编译规则
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Web界面
$(WEB_TARGET): monitoring_dashboard.html
	cp monitoring_dashboard.html $(WEB_TARGET)

# 清理
clean:
	rm -f $(TARGET) $(OBJS) $(WEB_TARGET)
	rm -f *.log *.json

# 运行监控服务器
run: $(TARGET)
	./$(TARGET)

# 运行示例
example: monitoring.c monitoring_server.c
	$(CC) $(CFLAGS) -o example example.c monitoring.c monitoring_server.c threadpool.c $(LDFLAGS)
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
	sudo apt-get install -y build-essential libjansson-dev valgrind cppcheck

.PHONY: all clean run example check valgrind install_deps