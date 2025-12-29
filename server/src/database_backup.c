// 文件：server/src/database_backup.c
#include "database.h"
#include "../common/logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_BACKUP_FILES 30
#define BACKUP_DIR "backups"

// 创建备份目录
static bool create_backup_dir(void) {
    struct stat st = {0};
    if (stat(BACKUP_DIR, &st) == -1) {
        if (mkdir(BACKUP_DIR, 0755) != 0) {
            LOG_ERROR("无法创建备份目录: %s", BACKUP_DIR);
            return false;
        }
    }
    return true;
}

// 生成备份文件名
static char *generate_backup_filename(void) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    
    static char filename[256];
    snprintf(filename, sizeof(filename), 
             "%s/securechat_backup_%04d%02d%02d_%02d%02d%02d.db",
             BACKUP_DIR,
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
    
    return filename;
}

// 清理旧备份文件
static void cleanup_old_backups(void) {
    DIR *dir = opendir(BACKUP_DIR);
    if (!dir) {
        return;
    }
    
    struct dirent **namelist;
    int n = scandir(BACKUP_DIR, &namelist, NULL, alphasort);
    
    if (n < 0) {
        closedir(dir);
        return;
    }
    
    // 如果备份文件超过限制，删除最旧的
    if (n > MAX_BACKUP_FILES) {
        int files_to_remove = n - MAX_BACKUP_FILES;
        for (int i = 0; i < files_to_remove; i++) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", BACKUP_DIR, namelist[i]->d_name);
            unlink(path);
            LOG_INFO("删除旧备份: %s", namelist[i]->d_name);
        }
    }
    
    // 清理内存
    for (int i = 0; i < n; i++) {
        free(namelist[i]);
    }
    free(namelist);
    
    closedir(dir);
}

// 执行数据库备份
bool database_perform_backup(database_pool_t *pool) {
    if (!create_backup_dir()) {
        return false;
    }
    
    char *backup_file = generate_backup_filename();
    
    LOG_INFO("开始数据库备份: %s", backup_file);
    
    bool success = database_backup(pool, backup_file);
    
    if (success) {
        LOG_INFO("数据库备份成功: %s", backup_file);
        cleanup_old_backups();
    } else {
        LOG_ERROR("数据库备份失败");
    }
    
    return success;
}

// 列出所有备份文件
void database_list_backups(void) {
    DIR *dir = opendir(BACKUP_DIR);
    if (!dir) {
        printf("备份目录不存在\n");
        return;
    }
    
    printf("可用的数据库备份:\n");
    printf("--------------------------------------------------\n");
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG && 
            strstr(entry->d_name, "securechat_backup_") == entry->d_name) {
            
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", BACKUP_DIR, entry->d_name);
            
            struct stat st;
            if (stat(path, &st) == 0) {
                struct tm *tm = localtime(&st.st_mtime);
                double size_mb = st.st_size / (1024.0 * 1024.0);
                
                printf("%-35s %5.1f MB  %04d-%02d-%02d %02d:%02d\n",
                       entry->d_name, size_mb,
                       tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                       tm->tm_hour, tm->tm_min);
            }
        }
    }
    
    closedir(dir);
    printf("--------------------------------------------------\n");
}

// 从备份恢复数据库
bool database_restore_from_backup(database_pool_t *pool, const char *backup_file) {
    if (!backup_file) {
        LOG_ERROR("未指定备份文件");
        return false;
    }
    
    char full_path[512];
    if (backup_file[0] != '/') {
        snprintf(full_path, sizeof(full_path), "%s/%s", BACKUP_DIR, backup_file);
    } else {
        strncpy(full_path, backup_file, sizeof(full_path) - 1);
    }
    
    struct stat st;
    if (stat(full_path, &st) != 0) {
        LOG_ERROR("备份文件不存在: %s", full_path);
        return false;
    }
    
    LOG_INFO("开始从备份恢复: %s", full_path);
    
    // 关闭现有数据库连接
    database_shutdown(pool);
    
    // 备份当前数据库（以防万一）
    char temp_backup[512];
    snprintf(temp_backup, sizeof(temp_backup), "%s.old", pool->config.database_path);
    rename(pool->config.database_path, temp_backup);
    
    // 复制备份文件
    FILE *src = fopen(full_path, "rb");
    FILE *dst = fopen(pool->config.database_path, "wb");
    
    if (!src || !dst) {
        LOG_ERROR("无法打开文件进行复制");
        if (src) fclose(src);
        if (dst) fclose(dst);
        rename(temp_backup, pool->config.database_path);
        return false;
    }
    
    char buffer[8192];
    size_t bytes;
    bool success = true;
    
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, bytes, dst) != bytes) {
            success = false;
            break;
        }
    }
    
    fclose(src);
    fclose(dst);
    
    if (success) {
        LOG_INFO("数据库恢复成功");
        unlink(temp_backup);
    } else {
        LOG_ERROR("数据库恢复失败");
        unlink(pool->config.database_path);
        rename(temp_backup, pool->config.database_path);
    }
    
    // 重新初始化数据库
    database_init(&pool->config);
    
    return success;
}