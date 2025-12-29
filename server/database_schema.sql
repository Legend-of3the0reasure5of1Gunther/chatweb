-- 安全聊天系统数据库架构
-- 版本: 1.0.0

-- 用户表
CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT UNIQUE NOT NULL,
    password_hash TEXT NOT NULL,
    password_salt TEXT NOT NULL,
    email TEXT UNIQUE NOT NULL,
    nickname TEXT NOT NULL,
    avatar_id INTEGER DEFAULT 0,
    status INTEGER DEFAULT 0,  -- 0=离线, 1=在线, 2=离开, 3=忙碌
    status_message TEXT DEFAULT '',
    last_seen INTEGER DEFAULT 0,
    friend_count INTEGER DEFAULT 0,
    group_count INTEGER DEFAULT 0,
    is_verified INTEGER DEFAULT 0,
    is_premium INTEGER DEFAULT 0,
    is_admin INTEGER DEFAULT 0,
    login_attempts INTEGER DEFAULT 0,
    last_login_attempt INTEGER DEFAULT 0,
    account_locked_until INTEGER DEFAULT 0,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    last_login INTEGER DEFAULT 0,
    last_login_ip TEXT
);

-- 用户会话表
CREATE TABLE IF NOT EXISTS sessions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL,
    access_token TEXT UNIQUE NOT NULL,
    refresh_token TEXT UNIQUE NOT NULL,
    client_ip TEXT NOT NULL,
    user_agent TEXT,
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    refresh_expires_at INTEGER NOT NULL,
    is_valid INTEGER DEFAULT 1,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
);

-- 好友关系表
CREATE TABLE IF NOT EXISTS friends (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL,
    friend_id INTEGER NOT NULL,
    relationship INTEGER DEFAULT 0,  -- 0=好友, 1=特别关心, 2=拉黑
    alias TEXT,  -- 好友备注
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    UNIQUE(user_id, friend_id),
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,
    FOREIGN KEY (friend_id) REFERENCES users(id) ON DELETE CASCADE
);

-- 群组表
CREATE TABLE IF NOT EXISTS groups (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    description TEXT,
    owner_id INTEGER NOT NULL,
    avatar_id INTEGER DEFAULT 0,
    max_members INTEGER DEFAULT 500,
    is_public INTEGER DEFAULT 1,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    FOREIGN KEY (owner_id) REFERENCES users(id) ON DELETE CASCADE
);

-- 群组成员表
CREATE TABLE IF NOT EXISTS group_members (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    group_id INTEGER NOT NULL,
    user_id INTEGER NOT NULL,
    role INTEGER DEFAULT 0,  -- 0=成员, 1=管理员, 2=群主
    join_time INTEGER NOT NULL,
    last_active INTEGER DEFAULT 0,
    UNIQUE(group_id, user_id),
    FOREIGN KEY (group_id) REFERENCES groups(id) ON DELETE CASCADE,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
);

-- 私聊消息表
CREATE TABLE IF NOT EXISTS messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    sender_id INTEGER NOT NULL,
    receiver_id INTEGER NOT NULL,
    message_type INTEGER NOT NULL,  -- 0=文本, 1=图片, 2=语音, 3=文件
    content TEXT NOT NULL,
    content_hash TEXT,
    timestamp INTEGER NOT NULL,
    status INTEGER DEFAULT 0,  -- 0=发送中, 1=已发送, 2=已送达, 3=已读
    is_encrypted INTEGER DEFAULT 0,
    reply_to_id INTEGER DEFAULT 0,
    deleted INTEGER DEFAULT 0,
    FOREIGN KEY (sender_id) REFERENCES users(id) ON DELETE CASCADE,
    FOREIGN KEY (receiver_id) REFERENCES users(id) ON DELETE CASCADE
);

-- 群聊消息表
CREATE TABLE IF NOT EXISTS group_messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    group_id INTEGER NOT NULL,
    sender_id INTEGER NOT NULL,
    message_type INTEGER NOT NULL,
    content TEXT NOT NULL,
    content_hash TEXT,
    timestamp INTEGER NOT NULL,
    status INTEGER DEFAULT 0,
    is_encrypted INTEGER DEFAULT 0,
    reply_to_id INTEGER DEFAULT 0,
    deleted INTEGER DEFAULT 0,
    FOREIGN KEY (group_id) REFERENCES groups(id) ON DELETE CASCADE,
    FOREIGN KEY (sender_id) REFERENCES users(id) ON DELETE CASCADE
);

-- 文件元数据表
CREATE TABLE IF NOT EXISTS files (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL,
    filename TEXT NOT NULL,
    original_name TEXT NOT NULL,
    file_path TEXT NOT NULL,
    mime_type TEXT,
    file_size INTEGER NOT NULL,
    file_type INTEGER DEFAULT 0,  -- 0=未知, 1=图片, 2=音频, 3=视频, 4=文档
    file_hash TEXT NOT NULL,
    is_encrypted INTEGER DEFAULT 0,
    is_compressed INTEGER DEFAULT 0,
    uploaded_at INTEGER NOT NULL,
    expires_at INTEGER DEFAULT 0,
    download_count INTEGER DEFAULT 0,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
);

-- 文件共享表
CREATE TABLE IF NOT EXISTS file_shares (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    file_id INTEGER NOT NULL,
    user_id INTEGER NOT NULL,
    share_type INTEGER DEFAULT 0,  -- 0=公开, 1=私密, 2=密码保护
    share_token TEXT UNIQUE NOT NULL,
    expires_at INTEGER DEFAULT 0,
    max_downloads INTEGER DEFAULT 0,
    download_count INTEGER DEFAULT 0,
    created_at INTEGER NOT NULL,
    FOREIGN KEY (file_id) REFERENCES files(id) ON DELETE CASCADE,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
);

-- 消息已读状态表
CREATE TABLE IF NOT EXISTS message_read_status (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    message_id INTEGER NOT NULL,
    user_id INTEGER NOT NULL,
    read_time INTEGER NOT NULL,
    UNIQUE(message_id, user_id),
    FOREIGN KEY (message_id) REFERENCES messages(id) ON DELETE CASCADE,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
);

-- 用户设置表
CREATE TABLE IF NOT EXISTS user_settings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER UNIQUE NOT NULL,
    theme TEXT DEFAULT 'light',
    language TEXT DEFAULT 'zh-CN',
    notification_enabled INTEGER DEFAULT 1,
    sound_enabled INTEGER DEFAULT 1,
    auto_download_files INTEGER DEFAULT 0,
    file_size_limit INTEGER DEFAULT 10485760,  -- 10MB
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
);

-- 安全日志表
CREATE TABLE IF NOT EXISTS security_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER,
    event_type TEXT NOT NULL,
    details TEXT,
    client_ip TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE SET NULL
);

-- 系统配置表
CREATE TABLE IF NOT EXISTS system_config (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    config_key TEXT UNIQUE NOT NULL,
    config_value TEXT NOT NULL,
    description TEXT,
    updated_at INTEGER NOT NULL
);

-- 创建索引以提高查询性能

-- 用户表索引
CREATE INDEX IF NOT EXISTS idx_users_username ON users(username);
CREATE INDEX IF NOT EXISTS idx_users_email ON users(email);
CREATE INDEX IF NOT EXISTS idx_users_status ON users(status);

-- 会话表索引
CREATE INDEX IF NOT EXISTS idx_sessions_user_id ON sessions(user_id);
CREATE INDEX IF NOT EXISTS idx_sessions_access_token ON sessions(access_token);
CREATE INDEX IF NOT EXISTS idx_sessions_expires_at ON sessions(expires_at);

-- 好友关系索引
CREATE INDEX IF NOT EXISTS idx_friends_user_id ON friends(user_id);
CREATE INDEX IF NOT EXISTS idx_friends_friend_id ON friends(friend_id);

-- 消息表索引
CREATE INDEX IF NOT EXISTS idx_messages_sender_id ON messages(sender_id);
CREATE INDEX IF NOT EXISTS idx_messages_receiver_id ON messages(receiver_id);
CREATE INDEX IF NOT EXISTS idx_messages_timestamp ON messages(timestamp);
CREATE INDEX IF NOT EXISTS idx_messages_status ON messages(status);

-- 群组消息索引
CREATE INDEX IF NOT EXISTS idx_group_messages_group_id ON group_messages(group_id);
CREATE INDEX IF NOT EXISTS idx_group_messages_sender_id ON group_messages(sender_id);
CREATE INDEX IF NOT EXISTS idx_group_messages_timestamp ON group_messages(timestamp);

-- 文件表索引
CREATE INDEX IF NOT EXISTS idx_files_user_id ON files(user_id);
CREATE INDEX IF NOT EXISTS idx_files_file_hash ON files(file_hash);
CREATE INDEX IF NOT EXISTS idx_files_expires_at ON files(expires_at);

-- 安全日志索引
CREATE INDEX IF NOT EXISTS idx_security_logs_user_id ON security_logs(user_id);
CREATE INDEX IF NOT EXISTS idx_security_logs_created_at ON security_logs(created_at);

-- 插入默认系统配置
INSERT OR IGNORE INTO system_config (config_key, config_value, description, updated_at) VALUES
('system_name', 'Secure Chat System', '系统名称', strftime('%s', 'now')),
('max_file_size', '104857600', '最大文件大小(100MB)', strftime('%s', 'now')),
('max_users', '10000', '最大用户数', strftime('%s', 'now')),
('message_retention_days', '365', '消息保留天数', strftime('%s', 'now')),
('backup_interval_hours', '24', '备份间隔小时数', strftime('%s', 'now')),
('maintenance_mode', '0', '维护模式', strftime('%s', 'now'));

-- 创建触发器：更新用户更新时间戳
CREATE TRIGGER IF NOT EXISTS update_user_timestamp
AFTER UPDATE ON users
FOR EACH ROW
BEGIN
    UPDATE users SET updated_at = strftime('%s', 'now') WHERE id = OLD.id;
END;

-- 创建触发器：自动更新好友计数
CREATE TRIGGER IF NOT EXISTS update_friend_count_insert
AFTER INSERT ON friends
FOR EACH ROW
BEGIN
    UPDATE users SET friend_count = friend_count + 1 WHERE id = NEW.user_id;
    UPDATE users SET friend_count = friend_count + 1 WHERE id = NEW.friend_id;
END;

CREATE TRIGGER IF NOT EXISTS update_friend_count_delete
AFTER DELETE ON friends
FOR EACH ROW
BEGIN
    UPDATE users SET friend_count = friend_count - 1 WHERE id = OLD.user_id;
    UPDATE users SET friend_count = friend_count - 1 WHERE id = OLD.friend_id;
END;

-- 创建触发器：自动更新群组成员计数
CREATE TRIGGER IF NOT EXISTS update_group_member_count_insert
AFTER INSERT ON group_members
FOR EACH ROW
BEGIN
    UPDATE groups SET max_members = max_members + 1 WHERE id = NEW.group_id;
    UPDATE users SET group_count = group_count + 1 WHERE id = NEW.user_id;
END;

CREATE TRIGGER IF NOT EXISTS update_group_member_count_delete
AFTER DELETE ON group_members
FOR EACH ROW
BEGIN
    UPDATE groups SET max_members = max_members - 1 WHERE id = OLD.group_id;
    UPDATE users SET group_count = group_count - 1 WHERE id = OLD.user_id;
END;

-- 创建视图：在线用户视图
CREATE VIEW IF NOT EXISTS online_users AS
SELECT id, username, nickname, status, status_message, last_seen
FROM users
WHERE status = 1 AND last_seen > strftime('%s', 'now') - 300;  -- 最近5分钟活跃

-- 创建视图：最近活跃用户
CREATE VIEW IF NOT EXISTS recent_active_users AS
SELECT id, username, nickname, status, last_seen,
       CASE 
           WHEN last_seen > strftime('%s', 'now') - 300 THEN '刚刚'
           WHEN last_seen > strftime('%s', 'now') - 3600 THEN '1小时内'
           WHEN last_seen > strftime('%s', 'now') - 86400 THEN '今天'
           WHEN last_seen > strftime('%s', 'now') - 604800 THEN '本周'
           ELSE '更早'
       END as activity_level
FROM users
ORDER BY last_seen DESC;