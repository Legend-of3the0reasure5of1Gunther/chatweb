-- SecureChat 数据库初始化脚本
-- 版本: 1.0.0
-- 创建时间: $(date)

-- 创建数据库
CREATE DATABASE securechat
    WITH 
    OWNER = securechat_user
    ENCODING = 'UTF8'
    LC_COLLATE = 'en_US.utf8'
    LC_CTYPE = 'en_US.utf8'
    TABLESPACE = pg_default
    CONNECTION LIMIT = -1
    IS_TEMPLATE = False;

-- 连接数据库
\c securechat;

-- 创建扩展
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";
CREATE EXTENSION IF NOT EXISTS "pgcrypto";
CREATE EXTENSION IF NOT EXISTS "citext";

-- 用户表
CREATE TABLE users (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    username CITEXT UNIQUE NOT NULL,
    email CITEXT UNIQUE NOT NULL,
    email_verified BOOLEAN DEFAULT FALSE,
    phone VARCHAR(20),
    phone_verified BOOLEAN DEFAULT FALSE,
    
    -- 密码存储
    password_hash VARCHAR(255) NOT NULL,
    salt VARCHAR(255) NOT NULL,
    password_changed_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    password_reset_token VARCHAR(255),
    password_reset_expires TIMESTAMP WITH TIME ZONE,
    
    -- 个人信息
    first_name VARCHAR(100),
    last_name VARCHAR(100),
    display_name VARCHAR(200),
    avatar_url VARCHAR(500),
    bio TEXT,
    date_of_birth DATE,
    gender VARCHAR(20),
    
    -- 安全信息
    two_factor_enabled BOOLEAN DEFAULT FALSE,
    two_factor_secret VARCHAR(255),
    backup_codes TEXT[],
    last_login_at TIMESTAMP WITH TIME ZONE,
    last_login_ip INET,
    failed_login_attempts INTEGER DEFAULT 0,
    locked_until TIMESTAMP WITH TIME ZONE,
    
    -- 权限
    role VARCHAR(50) DEFAULT 'user',
    permissions JSONB DEFAULT '[]',
    
    -- 状态
    status VARCHAR(20) DEFAULT 'active',
    deactivated_at TIMESTAMP WITH TIME ZONE,
    deleted_at TIMESTAMP WITH TIME ZONE,
    
    -- 元数据
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    version INTEGER DEFAULT 1,
    
    -- 索引
    CHECK (char_length(username) >= 3 AND char_length(username) <= 50),
    CHECK (char_length(email) <= 255)
);

-- 用户会话表
CREATE TABLE user_sessions (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    user_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    session_token VARCHAR(255) UNIQUE NOT NULL,
    refresh_token VARCHAR(255) UNIQUE NOT NULL,
    device_info JSONB,
    ip_address INET,
    user_agent TEXT,
    location VARCHAR(255),
    
    -- 过期时间
    expires_at TIMESTAMP WITH TIME ZONE NOT NULL,
    refresh_expires_at TIMESTAMP WITH TIME ZONE NOT NULL,
    revoked_at TIMESTAMP WITH TIME ZONE,
    
    -- 元数据
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    
    -- 索引
    INDEX idx_user_sessions_user_id (user_id),
    INDEX idx_user_sessions_expires_at (expires_at)
);

-- 聊天室/群组表
CREATE TABLE chat_rooms (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    name VARCHAR(255) NOT NULL,
    description TEXT,
    type VARCHAR(50) NOT NULL DEFAULT 'private', -- private, group, channel
    avatar_url VARCHAR(500),
    
    -- 加密设置
    encrypted BOOLEAN DEFAULT TRUE,
    encryption_key VARCHAR(500), -- 加密的密钥
    
    -- 权限设置
    is_public BOOLEAN DEFAULT FALSE,
    join_code VARCHAR(100),
    max_members INTEGER DEFAULT 100,
    
    -- 所有者
    owner_id UUID REFERENCES users(id),
    
    -- 状态
    archived BOOLEAN DEFAULT FALSE,
    deleted_at TIMESTAMP WITH TIME ZONE,
    
    -- 元数据
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    
    -- 索引
    INDEX idx_chat_rooms_owner_id (owner_id),
    INDEX idx_chat_rooms_type (type)
);

-- 房间成员表
CREATE TABLE room_members (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    room_id UUID NOT NULL REFERENCES chat_rooms(id) ON DELETE CASCADE,
    user_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    
    -- 成员角色
    role VARCHAR(50) DEFAULT 'member', -- owner, admin, moderator, member
    permissions JSONB DEFAULT '[]',
    
    -- 通知设置
    notifications_enabled BOOLEAN DEFAULT TRUE,
    mute_until TIMESTAMP WITH TIME ZONE,
    
    -- 加入信息
    joined_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    invited_by UUID REFERENCES users(id),
    
    -- 状态
    left_at TIMESTAMP WITH TIME ZONE,
    kicked BOOLEAN DEFAULT FALSE,
    
    -- 元数据
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    
    -- 约束和索引
    UNIQUE(room_id, user_id),
    INDEX idx_room_members_user_id (user_id),
    INDEX idx_room_members_room_id (room_id)
);

-- 消息表
CREATE TABLE messages (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    room_id UUID NOT NULL REFERENCES chat_rooms(id) ON DELETE CASCADE,
    sender_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    
    -- 消息内容
    content TEXT, -- 加密的内容
    content_hash VARCHAR(255), -- 用于完整性验证
    content_type VARCHAR(50) DEFAULT 'text', -- text, image, file, system
    metadata JSONB, -- 附加元数据
    
    -- 加密信息
    encrypted BOOLEAN DEFAULT TRUE,
    encryption_iv VARCHAR(255),
    encryption_key_id UUID,
    
    -- 引用和回复
    reply_to UUID REFERENCES messages(id),
    forward_from UUID REFERENCES messages(id),
    
    -- 已读状态
    read_by JSONB DEFAULT '[]', -- 存储已读用户ID和时间
    
    -- 编辑历史
    edited BOOLEAN DEFAULT FALSE,
    edit_history JSONB DEFAULT '[]',
    
    -- 删除状态
    deleted BOOLEAN DEFAULT FALSE,
    deleted_at TIMESTAMP WITH TIME ZONE,
    deleted_by UUID REFERENCES users(id),
    
    -- 元数据
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    
    -- 索引
    INDEX idx_messages_room_id (room_id),
    INDEX idx_messages_sender_id (sender_id),
    INDEX idx_messages_created_at (created_at),
    INDEX idx_messages_room_created (room_id, created_at DESC)
) PARTITION BY RANGE (created_at);

-- 创建消息分区表（按月份分区）
CREATE TABLE messages_y2023m11 PARTITION OF messages
    FOR VALUES FROM ('2023-11-01') TO ('2023-12-01');

CREATE TABLE messages_y2023m12 PARTITION OF messages
    FOR VALUES FROM ('2023-12-01') TO ('2024-01-01');

-- 文件表
CREATE TABLE files (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    uploader_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    room_id UUID REFERENCES chat_rooms(id) ON DELETE SET NULL,
    
    -- 文件信息
    original_name VARCHAR(500) NOT NULL,
    storage_path VARCHAR(1000) NOT NULL,
    mime_type VARCHAR(255) NOT NULL,
    size BIGINT NOT NULL,
    hash VARCHAR(255) NOT NULL, -- 文件哈希值
    dimensions JSONB, -- 图片/视频尺寸
    
    -- 加密信息
    encrypted BOOLEAN DEFAULT TRUE,
    encryption_key VARCHAR(500),
    encryption_iv VARCHAR(255),
    
    -- 访问控制
    is_public BOOLEAN DEFAULT FALSE,
    access_token VARCHAR(255),
    expires_at TIMESTAMP WITH TIME ZONE,
    
    -- 状态
    uploaded_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    deleted_at TIMESTAMP WITH TIME ZONE,
    
    -- 元数据
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    
    -- 索引
    INDEX idx_files_uploader_id (uploader_id),
    INDEX idx_files_room_id (room_id),
    INDEX idx_files_created_at (created_at),
    INDEX idx_files_hash (hash)
);

-- 消息文件关联表
CREATE TABLE message_files (
    message_id UUID NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    file_id UUID NOT NULL REFERENCES files(id) ON DELETE CASCADE,
    order_index INTEGER DEFAULT 0,
    
    PRIMARY KEY (message_id, file_id),
    INDEX idx_message_files_file_id (file_id)
);

-- 加密密钥表
CREATE TABLE encryption_keys (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    user_id UUID REFERENCES users(id) ON DELETE CASCADE,
    room_id UUID REFERENCES chat_rooms(id) ON DELETE CASCADE,
    
    -- 密钥信息
    key_type VARCHAR(50) NOT NULL, -- symmetric, asymmetric
    algorithm VARCHAR(50) NOT NULL,
    public_key TEXT, -- 非对称加密公钥
    private_key_encrypted TEXT, -- 加密的私钥
    symmetric_key_encrypted TEXT, -- 加密的对称密钥
    
    -- 密钥版本和轮换
    version INTEGER DEFAULT 1,
    previous_key_id UUID REFERENCES encryption_keys(id),
    rotated_at TIMESTAMP WITH TIME ZONE,
    expires_at TIMESTAMP WITH TIME ZONE,
    
    -- 状态
    active BOOLEAN DEFAULT TRUE,
    revoked_at TIMESTAMP WITH TIME ZONE,
    
    -- 元数据
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    
    -- 索引
    INDEX idx_encryption_keys_user_id (user_id),
    INDEX idx_encryption_keys_room_id (room_id),
    INDEX idx_encryption_keys_active (active)
);

-- 审计日志表
CREATE TABLE audit_logs (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    user_id UUID REFERENCES users(id) ON DELETE SET NULL,
    action VARCHAR(100) NOT NULL,
    resource_type VARCHAR(50) NOT NULL,
    resource_id UUID,
    
    -- 详情
    details JSONB NOT NULL,
    ip_address INET,
    user_agent TEXT,
    
    -- 元数据
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    
    -- 索引
    INDEX idx_audit_logs_user_id (user_id),
    INDEX idx_audit_logs_action (action),
    INDEX idx_audit_logs_created_at (created_at),
    INDEX idx_audit_logs_resource (resource_type, resource_id)
) PARTITION BY RANGE (created_at);

-- 创建审计日志分区
CREATE TABLE audit_logs_y2023m11 PARTITION OF audit_logs
    FOR VALUES FROM ('2023-11-01') TO ('2023-12-01');

-- 通知表
CREATE TABLE notifications (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    user_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    
    -- 通知内容
    type VARCHAR(50) NOT NULL,
    title VARCHAR(255) NOT NULL,
    body TEXT NOT NULL,
    data JSONB,
    
    -- 状态
    read BOOLEAN DEFAULT FALSE,
    read_at TIMESTAMP WITH TIME ZONE,
    delivered BOOLEAN DEFAULT FALSE,
    delivered_at TIMESTAMP WITH TIME ZONE,
    
    -- 过期
    expires_at TIMESTAMP WITH TIME ZONE,
    
    -- 元数据
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    
    -- 索引
    INDEX idx_notifications_user_id (user_id),
    INDEX idx_notifications_read (read),
    INDEX idx_notifications_created_at (created_at)
);

-- 联系人表
CREATE TABLE contacts (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    user_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    contact_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    
    -- 联系人信息
    nickname VARCHAR(255),
    notes TEXT,
    favorite BOOLEAN DEFAULT FALSE,
    
    -- 关系
    relationship VARCHAR(50) DEFAULT 'friend',
    blocked BOOLEAN DEFAULT FALSE,
    blocked_at TIMESTAMP WITH TIME ZONE,
    
    -- 元数据
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    
    -- 约束和索引
    UNIQUE(user_id, contact_id),
    INDEX idx_contacts_user_id (user_id),
    INDEX idx_contacts_contact_id (contact_id)
);

-- 创建视图
CREATE VIEW user_stats AS
SELECT 
    u.id,
    u.username,
    u.email,
    COUNT(DISTINCT rm.room_id) as room_count,
    COUNT(DISTINCT c.contact_id) as contact_count,
    COUNT(DISTINCT m.id) as message_count,
    u.last_login_at
FROM users u
LEFT JOIN room_members rm ON u.id = rm.user_id AND rm.left_at IS NULL
LEFT JOIN contacts c ON u.id = c.user_id AND c.blocked = FALSE
LEFT JOIN messages m ON u.id = m.sender_id
GROUP BY u.id;

-- 创建函数
CREATE OR REPLACE FUNCTION update_updated_at_column()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = CURRENT_TIMESTAMP;
    RETURN NEW;
END;
$$ language 'plpgsql';

-- 创建触发器
CREATE TRIGGER update_users_updated_at 
    BEFORE UPDATE ON users 
    FOR EACH ROW 
    EXECUTE FUNCTION update_updated_at_column();

CREATE TRIGGER update_room_members_updated_at 
    BEFORE UPDATE ON room_members 
    FOR EACH ROW 
    EXECUTE FUNCTION update_updated_at_column();

CREATE TRIGGER update_chat_rooms_updated_at 
    BEFORE UPDATE ON chat_rooms 
    FOR EACH ROW 
    EXECUTE FUNCTION update_updated_at_column();

CREATE TRIGGER update_messages_updated_at 
    BEFORE UPDATE ON messages 
    FOR EACH ROW 
    EXECUTE FUNCTION update_updated_at_column();

-- 创建索引
CREATE INDEX idx_users_email_verified ON users(email_verified) WHERE email_verified = TRUE;
CREATE INDEX idx_users_status ON users(status);
CREATE INDEX idx_messages_read_by ON messages USING GIN(read_by);
CREATE INDEX idx_messages_encrypted ON messages(encrypted);
CREATE INDEX idx_files_expires_at ON files(expires_at) WHERE expires_at IS NOT NULL;

-- 创建全文搜索索引
CREATE INDEX idx_messages_content_search ON messages USING GIN(to_tsvector('english', content));

-- 添加注释
COMMENT ON TABLE users IS '用户表，存储所有用户信息';
COMMENT ON COLUMN users.password_hash IS '使用bcrypt加密的密码哈希';
COMMENT ON COLUMN users.two_factor_secret IS '加密存储的2FA密钥';

-- 设置表空间（如果需要）
ALTER TABLE users SET TABLESPACE pg_default;
ALTER TABLE messages SET TABLESPACE pg_default;

-- 创建只读用户（用于监控和备份）
CREATE USER securechat_monitor WITH PASSWORD '${MONITOR_PASSWORD}';
GRANT CONNECT ON DATABASE securechat TO securechat_monitor;
GRANT USAGE ON SCHEMA public TO securechat_monitor;
GRANT SELECT ON ALL TABLES IN SCHEMA public TO securechat_monitor;

-- 创建备份用户
CREATE USER securechat_backup WITH PASSWORD '${BACKUP_PASSWORD}';
GRANT CONNECT ON DATABASE securechat TO securechat_backup;
GRANT USAGE ON SCHEMA public TO securechat_backup;
GRANT SELECT ON ALL TABLES IN SCHEMA public TO securechat_backup;

-- 更新统计信息
ANALYZE;