// 文件：server/src/database_monitor.c
#include "database.h"
#include "../common/logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// 监控数据结构
typedef struct {
    time_t timestamp;
    int64_t total_queries;
    int64_t total_transactions;
    int active_connections;
    int64_t cache_hits;
    int64_t cache_misses;
    double query_time_avg;
    double database_size_mb;
} db_monitor_point_t;

// 监控历史记录
#define MONITOR_HISTORY_SIZE 1440 // 24小时，每分钟一个点
static db_monitor_point_t monitor_history[MONITOR_HISTORY_SIZE];
static int monitor_index = 0;
static pthread_mutex_t monitor_mutex = PTHREAD_MUTEX_INITIALIZER;

// 记录监控点
void database_record_monitor_point(database_pool_t *pool) {
    db_monitor_point_t point;
    point.timestamp = time(NULL);
    
    pthread_mutex_lock(&pool->mutex);
    point.total_queries = pool->query_counter;
    point.total_transactions = pool->transaction_counter;
    point.active_connections = pool->active_connections;
    pthread_mutex_unlock(&pool->mutex);
    
    // 获取数据库统计
    sqlite3 *conn = database_get_write_connection(pool);
    if (conn) {
        // 获取缓存命中率
        const char *sql = "SELECT * FROM pragma_cache_stats;";
        sqlite3_stmt *stmt;
        
        if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                point.cache_hits = sqlite3_column_int64(stmt, 1);
                point.cache_misses = sqlite3_column_int64(stmt, 2);
            }
            sqlite3_finalize(stmt);
        }
        
        // 获取数据库大小
        sql = "SELECT page_count * page_size FROM pragma_page_count(), pragma_page_size();";
        if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                point.database_size_mb = sqlite3_column_double(stmt, 0) / (1024.0 * 1024.0);
            }
            sqlite3_finalize(stmt);
        }
        
        database_release_connection(pool, conn);
    }
    
    // 保存监控点
    pthread_mutex_lock(&monitor_mutex);
    monitor_history[monitor_index] = point;
    monitor_index = (monitor_index + 1) % MONITOR_HISTORY_SIZE;
    pthread_mutex_unlock(&monitor_mutex);
}

// 获取监控统计
db_monitor_point_t *database_get_monitor_statistics(int *count) {
    pthread_mutex_lock(&monitor_mutex);
    
    static db_monitor_point_t stats[MONITOR_HISTORY_SIZE];
    int actual_count = 0;
    
    for (int i = 0; i < MONITOR_HISTORY_SIZE; i++) {
        int idx = (monitor_index + i) % MONITOR_HISTORY_SIZE;
        if (monitor_history[idx].timestamp > 0) {
            stats[actual_count++] = monitor_history[idx];
        }
    }
    
    *count = actual_count;
    pthread_mutex_unlock(&monitor_mutex);
    
    return stats;
}

// 生成监控报告
void database_generate_monitor_report(database_pool_t *pool, FILE *output) {
    fprintf(output, "=== 数据库监控报告 ===\n");
    fprintf(output, "生成时间: %s\n", ctime(&(time_t){time(NULL)}));
    fprintf(output, "----------------------\n\n");
    
    // 基本统计
    database_stats_t *stats = database_get_statistics(pool);
    if (stats) {
        fprintf(output, "基本统计:\n");
        fprintf(output, "  - 总用户数: %ld\n", stats->total_users);
        fprintf(output, "  - 总消息数: %ld\n", stats->total_messages);
        fprintf(output, "  - 总群组数: %ld\n", stats->total_groups);
        fprintf(output, "  - 总文件数: %ld\n", stats->total_files);
        fprintf(output, "  - 数据库大小: %.2f MB\n", stats->database_size_mb);
        fprintf(output, "  - 总查询次数: %ld\n", stats->query_count);
        fprintf(output, "  - 总事务次数: %ld\n", stats->transaction_count);
        fprintf(output, "\n");
        
        free(stats);
    }
    
    // 连接池状态
    pthread_mutex_lock(&pool->mutex);
    fprintf(output, "连接池状态:\n");
    fprintf(output, "  - 最大连接数: %d\n", pool->max_connections);
    fprintf(output, "  - 活跃连接数: %d\n", pool->active_connections);
    fprintf(output, "  - 总查询次数: %ld\n", pool->query_counter);
    fprintf(output, "  - 总事务次数: %ld\n", pool->transaction_counter);
    pthread_mutex_unlock(&pool->mutex);
    fprintf(output, "\n");
    
    // 表大小统计
    const char *table_sizes_sql = 
        "SELECT name, pgsize "
        "FROM dbstat "
        "WHERE aggregate = TRUE "
        "ORDER BY pgsize DESC "
        "LIMIT 10;";
    
    query_result_t *result = database_execute_query(pool, table_sizes_sql, NULL, 0);
    if (result && result->row_count > 0) {
        fprintf(output, "表大小排名 (前10):\n");
        for (int i = 0; i < result->row_count; i++) {
            double size_mb = atof(result->rows[i][1]) / (1024.0 * 1024.0);
            fprintf(output, "  %2d. %-20s %6.2f MB\n", 
                    i + 1, result->rows[i][0], size_mb);
        }
        fprintf(output, "\n");
        database_free_result(result);
    }
    
    // 查询性能统计
    const char *slow_queries_sql = 
        "SELECT sql, COUNT(*) as executions, AVG(duration) as avg_duration "
        "FROM query_log "
        "WHERE timestamp > unix_timestamp() - 3600 "
        "GROUP BY sql "
        "ORDER BY avg_duration DESC "
        "LIMIT 5;";
    
    result = database_execute_query(pool, slow_queries_sql, NULL, 0);
    if (result && result->row_count > 0) {
        fprintf(output, "最慢查询 (最近1小时):\n");
        for (int i = 0; i < result->row_count; i++) {
            fprintf(output, "  %2d. 执行次数: %s, 平均耗时: %s ms\n", 
                    i + 1, result->rows[i][1], result->rows[i][2]);
            fprintf(output, "      SQL: %s\n", result->rows[i][0]);
        }
        fprintf(output, "\n");
        database_free_result(result);
    }
    
    // 索引使用统计
    const char *index_stats_sql = 
        "SELECT name, COUNT(*) as uses "
        "FROM sqlite_stat1 "
        "GROUP BY name "
        "ORDER BY uses DESC "
        "LIMIT 10;";
    
    result = database_execute_query(pool, index_stats_sql, NULL, 0);
    if (result && result->row_count > 0) {
        fprintf(output, "索引使用统计:\n");
        for (int i = 0; i < result->row_count; i++) {
            fprintf(output, "  %2d. %-30s %8s 次\n", 
                    i + 1, result->rows[i][0], result->rows[i][1]);
        }
        fprintf(output, "\n");
        database_free_result(result);
    }
    
    fprintf(output, "=== 报告结束 ===\n");
}

// 检查数据库健康状况
bool database_check_health(database_pool_t *pool) {
    bool healthy = true;
    
    // 检查连接池
    pthread_mutex_lock(&pool->mutex);
    if (pool->active_connections >= pool->max_connections * 0.9) {
        LOG_WARN("连接池使用率过高: %d/%d", 
                pool->active_connections, pool->max_connections);
        healthy = false;
    }
    pthread_mutex_unlock(&pool->mutex);
    
    // 检查数据库文件大小
    struct stat st;
    if (stat(pool->config.database_path, &st) == 0) {
        double size_mb = st.st_size / (1024.0 * 1024.0);
        if (size_mb > pool->config.vacuum_threshold_mb) {
            LOG_WARN("数据库文件过大: %.2f MB (阈值: %d MB)", 
                    size_mb, pool->config.vacuum_threshold_mb);
            healthy = false;
        }
    }
    
    // 检查WAL文件大小
    char wal_file[1024];
    snprintf(wal_file, sizeof(wal_file), "%s-wal", pool->config.database_path);
    if (stat(wal_file, &st) == 0) {
        double wal_size_mb = st.st_size / (1024.0 * 1024.0);
        if (wal_size_mb > 100) { // 100MB WAL文件过大
            LOG_WARN("WAL文件过大: %.2f MB", wal_size_mb);
            healthy = false;
        }
    }
    
    // 检查备份状态
    if (pool->config.backup_interval_hours > 0) {
        time_t now = time(NULL);
        char backup_pattern[256];
        snprintf(backup_pattern, sizeof(backup_pattern), 
                "%s/securechat_backup_*.db", BACKUP_DIR);
        
        // 查找最新备份
        DIR *dir = opendir(BACKUP_DIR);
        if (dir) {
            time_t latest_backup = 0;
            struct dirent *entry;
            
            while ((entry = readdir(dir)) != NULL) {
                if (strstr(entry->d_name, "securechat_backup_") == entry->d_name) {
                    char path[512];
                    snprintf(path, sizeof(path), "%s/%s", BACKUP_DIR, entry->d_name);
                    
                    struct stat backup_stat;
                    if (stat(path, &backup_stat) == 0) {
                        if (backup_stat.st_mtime > latest_backup) {
                            latest_backup = backup_stat.st_mtime;
                        }
                    }
                }
            }
            
            closedir(dir);
            
            if (latest_backup > 0) {
                double hours_since_backup = difftime(now, latest_backup) / 3600.0;
                if (hours_since_backup > pool->config.backup_interval_hours) {
                    LOG_WARN("备份已过期: %.1f 小时 (阈值: %d 小时)", 
                            hours_since_backup, pool->config.backup_interval_hours);
                    healthy = false;
                }
            }
        }
    }
    
    return healthy;
}