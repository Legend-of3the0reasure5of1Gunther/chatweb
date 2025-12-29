CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
LDFLAGS = -lpthread -lssl -lcrypto -lz -ljson-c

# 源文件
SRC = file_transfer.c protocol.c security.c thread_pool.c buffer.c utils.c
OBJ = $(SRC:.c=.o)
TARGET = secure_chat_ft

EXAMPLE_SRC = file_transfer_example.c
EXAMPLE_OBJ = $(EXAMPLE_SRC:.c=.o)
EXAMPLE_TARGET = ft_example

WS_SRC = file_transfer_websocket.c websocket_server.c
WS_OBJ = $(WS_SRC:.c=.o)
WS_TARGET = ft_websocket

# 默认目标
all: $(TARGET) $(EXAMPLE_TARGET) $(WS_TARGET)

# 主目标
$(TARGET): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

# 示例程序
$(EXAMPLE_TARGET): $(EXAMPLE_OBJ) $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

# WebSocket测试程序
$(WS_TARGET): $(WS_OBJ) $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

# 编译源文件
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# 清理
clean:
	rm -f $(OBJ) $(EXAMPLE_OBJ) $(WS_OBJ) $(TARGET) $(EXAMPLE_TARGET) $(WS_TARGET)
	rm -rf test_storage test_temp

# 测试
test: $(EXAMPLE_TARGET)
	./$(EXAMPLE_TARGET)

# 运行内存检查
memcheck: $(EXAMPLE_TARGET)
	valgrind --leak-check=full --show-leak-kinds=all ./$(EXAMPLE_TARGET)

.PHONY: all clean test memcheck