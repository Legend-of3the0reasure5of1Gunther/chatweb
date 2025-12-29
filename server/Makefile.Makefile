CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE
LDFLAGS = -lpthread -lsqlite3 -lssl -lcrypto

# 源文件
SRC = auth.c protocol.c security.c buffer.c config.c utils.c
OBJ = $(SRC:.c=.o)
TARGET = secure_chat_auth

# 默认目标
all: $(TARGET)

# 链接目标文件
$(TARGET): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

# 编译源文件
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# 清理
clean:
	rm -f $(OBJ) $(TARGET)

# 安装（可选）
install: $(TARGET)
	cp $(TARGET) /usr/local/bin/

# 测试
test: $(TARGET)
	./$(TARGET) --test

.PHONY: all clean install test