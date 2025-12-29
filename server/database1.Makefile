# 文件：server/database.Makefile
# 安全聊天系统数据库模块编译配置

# 编译器设置
CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror \
         -O2 -g -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 \
         -I./include -I../common/include -I/usr/local/include

# 链接器设置
LDFLAGS = -L/usr/local/lib -L../common/lib
LDLIBS = -lsqlite3 -lpthread -ldl -lm

# 数据库模块特定标志
DB_CFLAGS = -DSQLITE_ENABLE_DBSTAT_VTAB \
            -DSQLITE_ENABLE_MEMORY_MANAGEMENT \
            -DSQLITE_ENABLE_COLUMN_METADATA \
            -DSQLITE_THREADSAFE=1 \
            -DSQLITE_DEFAULT_MEMSTATUS=0 \
            -DSQLITE_MAX_MMAP_SIZE=0 \
            -DSQLITE_OMIT_DEPRECATED \
            -DSQLITE_OMIT_SHARED_CACHE \
            -DSQLITE_USE_URI=0 \
            -DSQLITE_DEFAULT_AUTOVACUUM=1 \
            -DSQLITE_DEFAULT_CACHE_SIZE=-2000 \
            -DSQLITE_DEFAULT_FOREIGN_KEYS=1 \
            -DSQLITE_DEFAULT_LOCKING_MODE=1 \
            -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
            -DSQLITE_DEFAULT_WAL_AUTOCHECKPOINT=1000 \
            -DSQLITE_LIKE_DOESNT_MATCH_BLOBS

# 目标文件
DB_OBJS = src/database.o \
          src/database_backup.o \
          src/database_migration.o \
          src/database_monitor.o \
          src/database_pool.o \
          src/database_transaction.o

# 测试文件
TEST_OBJS = tests/test_database.o \
            tests/test_schema.o \
            tests/test_performance.o

# 输出目录
OBJ_DIR = obj
BIN_DIR = bin
LIB_DIR = ../lib

# 默认目标
all: prepare $(LIB_DIR)/libdatabase.a $(BIN_DIR)/database_test

# 准备工作目录
prepare:
	@mkdir -p $(OBJ_DIR) $(BIN_DIR) $(LIB_DIR)
	@mkdir -p tests/data

# 静态库
$(LIB_DIR)/libdatabase.a: $(addprefix $(OBJ_DIR)/, $(DB_OBJS))
	ar rcs $@ $^
	ranlib $@

# 测试程序
$(BIN_DIR)/database_test: $(addprefix $(OBJ_DIR)/, $(TEST_OBJS)) $(LIB_DIR)/libdatabase.a
	$(CC) $(CFLAGS) $(DB_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# 数据库架构初始化
$(BIN_DIR)/init_database: tools/init_database.c $(LIB_DIR)/libdatabase.a
	$(CC) $(CFLAGS) $(DB_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# 数据库迁移工具
$(BIN_DIR)/migrate_database: tools/migrate_database.c $(LIB_DIR)/libdatabase.a
	$(CC) $(CFLAGS) $(DB_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# 数据库备份工具
$(BIN_DIR)/backup_database: tools/backup_database.c $(LIB_DIR)/libdatabase.a
	$(CC) $(CFLAGS) $(DB_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# 数据库监控工具
$(BIN_DIR)/monitor_database: tools/monitor_database.c $(LIB_DIR)/libdatabase.a
	$(CC) $(CFLAGS) $(DB_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# 编译规则
$(OBJ_DIR)/%.o: %.c
	$(CC) $(CFLAGS) $(DB_CFLAGS) -c $< -o $@

# 清理
clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR) $(LIB_DIR)/libdatabase.a
	rm -f securechat.db securechat.db-wal securechat.db-shm
	rm -rf backup/*.db

# 数据库清理
clean-db:
	rm -f securechat.db securechat.db-wal securechat.db-shm

# 完全清理
distclean: clean clean-db
	rm -rf coverage_report
	rm -f *.gcda *.gcno gmon.out
	rm -f valgrind-out.txt

# 架构初始化
init-schema: $(BIN_DIR)/init_database
	$(BIN_DIR)/init_database database_schema.sql

# 备份数据库
backup: $(BIN_DIR)/backup_database
	$(BIN_DIR)/backup_database securechat.db backup/$(shell date +%Y%m%d_%H%M%S).db

# 运行测试
test: $(BIN_DIR)/database_test
	$(BIN_DIR)/database_test

# 压力测试
stress-test: $(BIN_DIR)/database_test
	$(BIN_DIR)/database_test --stress

# 内存检查
memcheck: $(BIN_DIR)/database_test
	valgrind --leak-check=full \
	         --show-leak-kinds=all \
	         --track-origins=yes \
	         --verbose \
	         --log-file=valgrind-out.txt \
	         $(BIN_DIR)/database_test

# 覆盖率测试
coverage: CFLAGS += -fprofile-arcs -ftest-coverage
coverage: LDFLAGS += -lgcov
coverage: clean $(BIN_DIR)/database_test
	$(BIN_DIR)/database_test
	@mkdir -p coverage_report
	gcov -b -c -p $(addprefix $(OBJ_DIR)/, $(DB_OBJS)) > coverage_report/coverage.txt
	lcov --capture --directory . --output-file coverage_report/coverage.info
	genhtml coverage_report/coverage.info --output-directory coverage_report

# 性能分析
profile: CFLAGS += -pg
profile: LDFLAGS += -pg
profile: clean $(BIN_DIR)/database_test
	$(BIN_DIR)/database_test --performance
	gprof $(BIN_DIR)/database_test gmon.out > profile.txt

# 安装
install: $(LIB_DIR)/libdatabase.a
	cp $(LIB_DIR)/libdatabase.a /usr/local/lib/
	cp include/database.h /usr/local/include/

# 卸载
uninstall:
	rm -f /usr/local/lib/libdatabase.a
	rm -f /usr/local/include/database.h

# 依赖检查
deps:
	@echo "检查依赖..."
	@which $(CC) >/dev/null || (echo "GCC未安装" && exit 1)
	@ldconfig -p | grep libsqlite3 >/dev/null || (echo "SQLite3库未安装" && exit 1)
	@echo "所有依赖已安装"

# 帮助
help:
	@echo "可用目标:"
	@echo "  all              - 编译所有目标"
	@echo "  clean            - 清理构建文件"
	@echo "  test             - 运行单元测试"
	@echo "  stress-test      - 运行压力测试"
	@echo "  memcheck         - 运行内存检查"
	@echo "  coverage         - 生成覆盖率报告"
	@echo "  profile          - 性能分析"
	@echo "  init-schema      - 初始化数据库架构"
	@echo "  backup           - 备份数据库"
	@echo "  install          - 安装库和头文件"
	@echo "  uninstall        - 卸载库和头文件"
	@echo "  deps             - 检查依赖"
	@echo "  help             - 显示此帮助"

.PHONY: all prepare clean clean-db distclean init-schema backup test \
        stress-test memcheck coverage profile install uninstall deps help