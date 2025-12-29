# 编译器设置
CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -pthread -std=c99
LDFLAGS = -pthread -lm

# 目标文件
TARGET = secure_chat_server
WEB_TARGET = web_interface.html

# 源文件
SRCS = threadpool.c file_transfer.c server_main.c protocol.c
OBJS = $(SRCS:.c=.o)
HEADERS = protocol.h threadpool.h

# 默认目标
all: $(TARGET) $(WEB_TARGET)

# 主服务器程序
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# 编译规则
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Web界面
$(WEB_TARGET): web_interface.html
	cp web_interface.html $(WEB_TARGET)

# 清理
clean:
	rm -f $(TARGET) $(OBJS) $(WEB_TARGET)
	rm -f *.log *.part downloaded_file.txt

# 运行服务器
run: $(TARGET)
	./$(TARGET)

# 运行示例
example: threadpool.c file_transfer.c
	$(CC) $(CFLAGS) -o example example.c threadpool.c file_transfer.c $(LDFLAGS)
	./example

# 测试线程池
test_threadpool: threadpool.c test_threadpool.c
	$(CC) $(CFLAGS) -o test_threadpool test_threadpool.c threadpool.c $(LDFLAGS)
	./test_threadpool

# 代码检查
check:
	cppcheck --enable=all --suppress=missingIncludeSystem $(SRCS)

# 内存检查
valgrind: $(TARGET)
	valgrind --leak-check=full --show-leak-kinds=all ./$(TARGET)

# 安装依赖 (Ubuntu/Debian)
install_deps:
	sudo apt-get update
	sudo apt-get install -y build-essential valgrind cppcheck

.PHONY: all clean run example test check valgrind install_deps