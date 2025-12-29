CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
LDFLAGS = -lpthread -lsqlite3 -lssl -lcrypto -lz

# 源文件
SRC = database.c protocol.c buffer.c config.c utils.c
OBJ = $(SRC:.c=.o)
TARGET = secure_chat_db

EXAMPLE_SRC = database_example.c
EXAMPLE_OBJ = $(EXAMPLE_SRC:.c=.o)
EXAMPLE_TARGET = db_example

# 默认目标
all: $(TARGET) $(EXAMPLE_TARGET)

# 主目标
$(TARGET): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

# 示例程序
$(EXAMPLE_TARGET): $(EXAMPLE_OBJ) $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

# 编译源文件
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# 清理
clean:
	rm -f $(OBJ) $(EXAMPLE_OBJ) $(TARGET) $(EXAMPLE_TARGET) *.db backups/*

# 测试
test: $(EXAMPLE_TARGET)
	./$(EXAMPLE_TARGET)

# 运行内存检查
memcheck: $(EXAMPLE_TARGET)
	valgrind --leak-check=full --show-leak-kinds=all ./$(EXAMPLE_TARGET)

.PHONY: all clean test memcheck