// 文件: src/core/group-manager.ts
/**
 * 群组聊天管理器
 * 支持端到端加密、离线消息、权限管理、多设备同步
 */

import { SecureChatWebSocket, MessageType } from './websocket-client';
import { OfflineSyncManager } from './offline-sync';
import { EndToEndEncryption, TwoFactorAuth, WebAuthnManager } from './security';
import { SyncEngine } from './sync-engine';

// 群组角色和权限
export enum GroupRole {
    OWNER = 'owner',        // 群主
    ADMIN = 'admin',        // 管理员
    MODERATOR = 'moderator', // 审核员
    MEMBER = 'member',      // 普通成员
    GUEST = 'guest'         // 访客
}

export enum GroupPermission {
    // 消息权限
    SEND_MESSAGE = 'send_message',
    SEND_FILE = 'send_file',
    SEND_IMAGE = 'send_image',
    SEND_VOICE = 'send_voice',
    DELETE_MESSAGE = 'delete_message',
    EDIT_MESSAGE = 'edit_message',
    PIN_MESSAGE = 'pin_message',
    
    // 成员管理
    INVITE_MEMBER = 'invite_member',
    REMOVE_MEMBER = 'remove_member',
    PROMOTE_MEMBER = 'promote_member',
    DEMOTE_MEMBER = 'demote_member',
    MUTE_MEMBER = 'mute_member',
    BAN_MEMBER = 'ban_member',
    
    // 群组管理
    EDIT_GROUP_INFO = 'edit_group_info',
    CHANGE_GROUP_AVATAR = 'change_group_avatar',
    MANAGE_ROLES = 'manage_roles',
    MANAGE_PERMISSIONS = 'manage_permissions',
    TRANSFER_OWNERSHIP = 'transfer_ownership',
    DELETE_GROUP = 'delete_group',
    EXPORT_GROUP_DATA = 'export_group_data',
    
    // 安全权限
    VIEW_AUDIT_LOG = 'view_audit_log',
    MANAGE_ENCRYPTION = 'manage_encryption',
    MANAGE_2FA = 'manage_2fa',
    VIEW_MEMBER_INFO = 'view_member_info'
}

// 群组类型
export enum GroupType {
    PUBLIC = 'public',      // 公开群，任何人可加入
    PRIVATE = 'private',    // 私密群，需要邀请
    SECRET = 'secret',      // 秘密群，不显示在搜索结果
    CHANNEL = 'channel',    // 频道，只有管理员可以发言
    SUPPORT = 'support'     // 客服群
}

// 群组状态
export enum GroupStatus {
    ACTIVE = 'active',
    INACTIVE = 'inactive',
    ARCHIVED = 'archived',
    BANNED = 'banned',
    DELETED = 'deleted'
}

// 群组信息接口
export interface GroupInfo {
    groupId: bigint;
    name: string;
    description: string;
    avatarUrl: string;
    avatarHash: string;
    type: GroupType;
    status: GroupStatus;
    
    ownerId: bigint;
    createdAt: number;
    updatedAt: number;
    lastActivity: number;
    
    // 统计信息
    memberCount: number;
    maxMembers: number;
    messageCount: number;
    fileCount: number;
    
    // 设置
    encryptionEnabled: boolean;
    encryptionKeyId?: string;
    encryptionKeyVersion: number;
    
    twoFactorRequired: boolean;
    joinApprovalRequired: boolean;
    memberInviteAllowed: boolean;
    messageHistoryVisible: boolean;
    
    // 元数据
    tags: string[];
    customData: Record<string, any>;
}

// 群组成员信息
export interface GroupMember {
    groupId: bigint;
    userId: bigint;
    role: GroupRole;
    joinedAt: number;
    lastSeen: number;
    messageCount: number;
    
    // 成员设置
    nickname: string;
    title: string;
    permissions: GroupPermission[];
    mutedUntil?: number;
    bannedUntil?: number;
    
    // 加密信息
    publicKey?: string;
    encryptionKey?: string;
    keyVersion: number;
    
    // 状态
    online: boolean;
    typing: boolean;
    active: boolean;
}

// 群组消息
export interface GroupMessage {
    messageId: bigint;
    groupId: bigint;
    senderId: bigint;
    senderName: string;
    senderRole: GroupRole;
    
    content: string | ArrayBuffer;
    type: MessageType;
    encrypted: boolean;
    encryptionKeyId?: string;
    
    timestamp: number;
    editedAt?: number;
    deletedAt?: number;
    
    // 消息状态
    readBy: bigint[];
    deliveredTo: bigint[];
    reactions: Record<string, bigint[]>; // emoji -> 用户ID列表
    
    // 元数据
    replyTo?: bigint;
    mentions: bigint[];
    attachments: GroupAttachment[];
    metadata: Record<string, any>;
}

// 群组附件
export interface GroupAttachment {
    attachmentId: string;
    type: 'image' | 'file' | 'audio' | 'video' | 'link';
    url: string;
    name: string;
    size: number;
    mimeType: string;
    thumbnail?: string;
    encrypted: boolean;
    
    uploadedBy: bigint;
    uploadedAt: number;
    expiresAt?: number;
    
    // 安全信息
    hash: string;
    signature?: string;
    encryptionKeyId?: string;
}

// 群组邀请
export interface GroupInvitation {
    invitationId: string;
    groupId: bigint;
    inviterId: bigint;
    inviteeId?: bigint;
    inviteeEmail?: string;
    inviteePhone?: string;
    
    code: string;
    expiresAt: number;
    usesLeft: number;
    maxUses: number;
    
    // 权限
    autoApprove: boolean;
    role: GroupRole;
    
    // 状态
    used: boolean;
    usedBy?: bigint;
    usedAt?: number;
    revoked: boolean;
}

// 群组审计日志
export interface GroupAuditLog {
    logId: string;
    groupId: bigint;
    userId: bigint;
    action: string;
    targetType: string;
    targetId?: string;
    
    details: Record<string, any>;
    ipAddress: string;
    userAgent: string;
    timestamp: number;
    
    // 安全信息
    signature?: string;
    verified: boolean;
}

// 群组加密上下文
export interface GroupEncryptionContext {
    groupId: bigint;
    keyVersion: number;
    encryptionKey: CryptoKey;
    keyRotationDate: number;
    nextRotationDate: number;
    
    // 成员密钥映射
    memberKeys: Map<bigint, {
        publicKey: string;
        encryptedGroupKey: string;
        keyVersion: number;
        updatedAt: number;
    }>;
    
    // 历史密钥（用于解密旧消息）
    historicalKeys: Map<number, {
        keyId: string;
        encryptionKey: CryptoKey;
        rotationDate: number;
        archivedAt: number;
    }>;
}

// 群组管理器
export class GroupManager {
    private static readonly MAX_GROUP_MEMBERS = 5000;
    private static readonly MAX_GROUP_ADMINS = 50;
    private static readonly MAX_GROUP_MODERATORS = 100;
    
    private webSocket: SecureChatWebSocket;
    private syncEngine: SyncEngine;
    private encryptionManager: any; // 使用之前的加密管理器
    
    private groups: Map<bigint, GroupInfo> = new Map();
    private groupMembers: Map<bigint, Map<bigint, GroupMember>> = new Map();
    private groupMessages: Map<bigint, GroupMessage[]> = new Map();
    private groupInvitations: Map<string, GroupInvitation> = new Map();
    private encryptionContexts: Map<bigint, GroupEncryptionContext> = new Map();
    
    // 权限映射
    private rolePermissions: Map<GroupRole, Set<GroupPermission>>;
    
    // 事件监听器
    private eventListeners: Map<string, ((...args: any[]) => void)[]> = new Map();
    
    constructor(webSocket: SecureChatWebSocket, syncEngine: SyncEngine) {
        this.webSocket = webSocket;
        this.syncEngine = syncEngine;
        
        // 初始化权限映射
        this.rolePermissions = this.initializeRolePermissions();
        
        // 注册消息处理器
        this.registerMessageHandlers();
        
        // 加载本地群组数据
        this.loadLocalGroups();
    }
    
    // 初始化角色权限映射
    private initializeRolePermissions(): Map<GroupRole, Set<GroupPermission>> {
        const permissions = new Map<GroupRole, Set<GroupPermission>>();
        
        // 群主权限
        const ownerPermissions = new Set([
            GroupPermission.SEND_MESSAGE,
            GroupPermission.SEND_FILE,
            GroupPermission.SEND_IMAGE,
            GroupPermission.SEND_VOICE,
            GroupPermission.DELETE_MESSAGE,
            GroupPermission.EDIT_MESSAGE,
            GroupPermission.PIN_MESSAGE,
            GroupPermission.INVITE_MEMBER,
            GroupPermission.REMOVE_MEMBER,
            GroupPermission.PROMOTE_MEMBER,
            GroupPermission.DEMOTE_MEMBER,
            GroupPermission.MUTE_MEMBER,
            GroupPermission.BAN_MEMBER,
            GroupPermission.EDIT_GROUP_INFO,
            GroupPermission.CHANGE_GROUP_AVATAR,
            GroupPermission.MANAGE_ROLES,
            GroupPermission.MANAGE_PERMISSIONS,
            GroupPermission.TRANSFER_OWNERSHIP,
            GroupPermission.DELETE_GROUP,
            GroupPermission.EXPORT_GROUP_DATA,
            GroupPermission.VIEW_AUDIT_LOG,
            GroupPermission.MANAGE_ENCRYPTION,
            GroupPermission.MANAGE_2FA,
            GroupPermission.VIEW_MEMBER_INFO
        ]);
        permissions.set(GroupRole.OWNER, ownerPermissions);
        
        // 管理员权限
        const adminPermissions = new Set([
            GroupPermission.SEND_MESSAGE,
            GroupPermission.SEND_FILE,
            GroupPermission.SEND_IMAGE,
            GroupPermission.SEND_VOICE,
            GroupPermission.DELETE_MESSAGE,
            GroupPermission.EDIT_MESSAGE,
            GroupPermission.PIN_MESSAGE,
            GroupPermission.INVITE_MEMBER,
            GroupPermission.REMOVE_MEMBER,
            GroupPermission.PROMOTE_MEMBER,
            GroupPermission.DEMOTE_MEMBER,
            GroupPermission.MUTE_MEMBER,
            GroupPermission.BAN_MEMBER,
            GroupPermission.EDIT_GROUP_INFO,
            GroupPermission.CHANGE_GROUP_AVATAR,
            GroupPermission.MANAGE_ROLES,
            GroupPermission.VIEW_AUDIT_LOG,
            GroupPermission.VIEW_MEMBER_INFO
        ]);
        permissions.set(GroupRole.ADMIN, adminPermissions);
        
        // 审核员权限
        const moderatorPermissions = new Set([
            GroupPermission.SEND_MESSAGE,
            GroupPermission.SEND_FILE,
            GroupPermission.SEND_IMAGE,
            GroupPermission.SEND_VOICE,
            GroupPermission.DELETE_MESSAGE,
            GroupPermission.MUTE_MEMBER,
            GroupPermission.VIEW_AUDIT_LOG
        ]);
        permissions.set(GroupRole.MODERATOR, moderatorPermissions);
        
        // 普通成员权限
        const memberPermissions = new Set([
            GroupPermission.SEND_MESSAGE,
            GroupPermission.SEND_FILE,
            GroupPermission.SEND_IMAGE,
            GroupPermission.SEND_VOICE
        ]);
        permissions.set(GroupRole.MEMBER, memberPermissions);
        
        // 访客权限
        const guestPermissions = new Set([
            GroupPermission.SEND_MESSAGE
        ]);
        permissions.set(GroupRole.GUEST, guestPermissions);
        
        return permissions;
    }
    
    // 注册消息处理器
    private registerMessageHandlers(): void {
        // 群组创建响应
        this.webSocket.registerMessageCallback(
            MessageType.MSG_GROUP_CREATE_RESPONSE,
            (type, data) => this.handleGroupCreateResponse(data)
        );
        
        // 群组消息接收
        this.webSocket.registerMessageCallback(
            MessageType.MSG_GROUP_MESSAGE_RECEIVE,
            (type, data) => this.handleGroupMessage(data)
        );
        
        // 群组信息响应
        this.webSocket.registerMessageCallback(
            MessageType.MSG_GROUP_INFO_GET_RESPONSE,
            (type, data) => this.handleGroupInfoResponse(data)
        );
        
        // 群组成员响应
        this.webSocket.registerMessageCallback(
            MessageType.MSG_GROUP_MEMBERS_GET_RESPONSE,
            (type, data) => this.handleGroupMembersResponse(data)
        );
        
        // 群组加入响应
        this.webSocket.registerMessageCallback(
            MessageType.MSG_GROUP_JOIN_RESPONSE,
            (type, data) => this.handleGroupJoinResponse(data)
        );
        
        // 群组离开响应
        this.webSocket.registerMessageCallback(
            MessageType.MSG_GROUP_LEAVE_RESPONSE,
            (type, data) => this.handleGroupLeaveResponse(data)
        );
    }
    
    // === 群组创建和管理 ===
    
    // 创建群组
    async createGroup(options: {
        name: string;
        description?: string;
        type: GroupType;
        encryptionEnabled?: boolean;
        twoFactorRequired?: boolean;
        maxMembers?: number;
        tags?: string[];
        initialMembers?: bigint[];
    }): Promise<bigint> {
        const currentUserId = this.webSocket.getUserId();
        
        // 验证参数
        if (!options.name || options.name.length < 2 || options.name.length > 128) {
            throw new Error('群组名称必须为2-128个字符');
        }
        
        if (options.maxMembers && options.maxMembers > GroupManager.MAX_GROUP_MEMBERS) {
            throw new Error(`群组成员数不能超过${GroupManager.MAX_GROUP_MEMBERS}`);
        }
        
        // 准备群组数据
        const groupData = {
            name: options.name,
            description: options.description || '',
            type: options.type,
            encryptionEnabled: options.encryptionEnabled || false,
            twoFactorRequired: options.twoFactorRequired || false,
            maxMembers: options.maxMembers || 100,
            tags: options.tags || [],
            initialMembers: options.initialMembers || []
        };
        
        // 发送创建请求
        const data = this.serializeGroupCreateData(groupData);
        
        try {
            const response = await this.webSocket.sendMessage(
                MessageType.MSG_GROUP_CREATE,
                data,
                true
            );
            
            if (!response) {
                throw new Error('创建群组失败');
            }
            
            // 响应处理将在handleGroupCreateResponse中进行
            // 暂时返回一个虚拟ID，实际ID由服务器返回
            return 0n;
            
        } catch (error) {
            console.error('创建群组失败:', error);
            throw error;
        }
    }
    
    // 序列化群组创建数据
    private serializeGroupCreateData(options: any): ArrayBuffer {
        const encoder = new TextEncoder();
        
        // 计算总长度
        const nameBuffer = encoder.encode(options.name);
        const descBuffer = encoder.encode(options.description);
        
        // 序列化初始成员
        const membersBuffer = new ArrayBuffer(options.initialMembers.length * 8);
        const membersView = new DataView(membersBuffer);
        options.initialMembers.forEach((memberId: bigint, index: number) => {
            membersView.setBigUint64(index * 8, memberId, false);
        });
        
        // 创建数据包
        const data = new ArrayBuffer(
            128 + // 名称（最大128字节）
            512 + // 描述（最大512字节）
            1 +   // 类型
            1 +   // 加密启用
            1 +   // 2FA要求
            4 +   // 最大成员数
            4 +   // 标签数量
            (options.tags.length * 64) + // 每个标签64字节
            4 +   // 初始成员数量
            membersBuffer.byteLength
        );
        
        const view = new DataView(data);
        let offset = 0;
        
        // 名称
        const nameArray = new Uint8Array(data, offset, 128);
        nameArray.set(nameBuffer.subarray(0, 127));
        nameArray[Math.min(nameBuffer.length, 127)] = 0;
        offset += 128;
        
        // 描述
        const descArray = new Uint8Array(data, offset, 512);
        descArray.set(descBuffer.subarray(0, 511));
        descArray[Math.min(descBuffer.length, 511)] = 0;
        offset += 512;
        
        // 类型
        view.setUint8(offset, this.groupTypeToCode(options.type)); offset += 1;
        
        // 加密启用
        view.setUint8(offset, options.encryptionEnabled ? 1 : 0); offset += 1;
        
        // 2FA要求
        view.setUint8(offset, options.twoFactorRequired ? 1 : 0); offset += 1;
        
        // 最大成员数
        view.setUint32(offset, options.maxMembers, false); offset += 4;
        
        // 标签数量
        view.setUint32(offset, options.tags.length, false); offset += 4;
        
        // 标签
        options.tags.forEach((tag: string) => {
            const tagBuffer = encoder.encode(tag);
            const tagArray = new Uint8Array(data, offset, 64);
            tagArray.set(tagBuffer.subarray(0, 63));
            tagArray[Math.min(tagBuffer.length, 63)] = 0;
            offset += 64;
        });
        
        // 初始成员数量
        view.setUint32(offset, options.initialMembers.length, false); offset += 4;
        
        // 初始成员
        const membersArray = new Uint8Array(data, offset, membersBuffer.byteLength);
        membersArray.set(new Uint8Array(membersBuffer));
        
        return data;
    }
    
    // 处理群组创建响应
    private handleGroupCreateResponse(data: ArrayBuffer): void {
        const view = new DataView(data);
        const statusCode = view.getUint32(0, false);
        const groupId = view.getBigUint64(4, false);
        
        if (statusCode === 200) {
            // 创建成功，更新本地缓存
            console.log(`群组创建成功，ID: ${groupId}`);
            
            // 触发事件
            this.emit('groupCreated', { groupId });
        } else {
            console.error('群组创建失败');
            this.emit('groupCreateFailed', { statusCode });
        }
    }
    
    // 更新群组信息
    async updateGroupInfo(groupId: bigint, updates: Partial<GroupInfo>): Promise<boolean> {
        // 检查权限
        if (!await this.hasPermission(groupId, GroupPermission.EDIT_GROUP_INFO)) {
            throw new Error('没有权限更新群组信息');
        }
        
        // 序列化更新数据
        const data = this.serializeGroupUpdateData(groupId, updates);
        
        try {
            const response = await this.webSocket.sendMessage(
                MessageType.MSG_USER_PROFILE_UPDATE, // 使用用户资料更新消息类型
                data,
                true
            );
            
            return response !== null;
        } catch (error) {
            console.error('更新群组信息失败:', error);
            return false;
        }
    }
    
    // 获取群组信息
    async getGroupInfo(groupId: bigint): Promise<GroupInfo | null> {
        // 先从缓存获取
        const cachedGroup = this.groups.get(groupId);
        if (cachedGroup) {
            return cachedGroup;
        }
        
        // 从服务器获取
        const data = new ArrayBuffer(8);
        const view = new DataView(data);
        view.setBigUint64(0, groupId, false);
        
        try {
            await this.webSocket.sendMessage(
                MessageType.MSG_GROUP_INFO_GET,
                data,
                true
            );
            
            // 响应将在handleGroupInfoResponse中处理
            // 这里需要等待响应
            return null;
        } catch (error) {
            console.error('获取群组信息失败:', error);
            return null;
        }
    }
    
    // 处理群组信息响应
    private handleGroupInfoResponse(data: ArrayBuffer): void {
        const groupInfo = this.deserializeGroupInfo(data);
        if (groupInfo) {
            this.groups.set(groupInfo.groupId, groupInfo);
            this.emit('groupInfoUpdated', { groupInfo });
        }
    }
    
    // 序列化群组信息
    private deserializeGroupInfo(data: ArrayBuffer): GroupInfo | null {
        try {
            const view = new DataView(data);
            let offset = 0;
            
            const groupId = view.getBigUint64(offset, false); offset += 8;
            
            // 名称
            const name = this.readString(data, offset, 128); offset += 128;
            
            // 描述
            const description = this.readString(data, offset, 512); offset += 512;
            
            // 类型
            const typeCode = view.getUint8(offset); offset += 1;
            const type = this.codeToGroupType(typeCode);
            
            // 状态
            const statusCode = view.getUint8(offset); offset += 1;
            const status = this.codeToGroupStatus(statusCode);
            
            const ownerId = view.getBigUint64(offset, false); offset += 8;
            const createdAt = view.getBigUint64(offset, false); offset += 8;
            const updatedAt = view.getBigUint64(offset, false); offset += 8;
            const lastActivity = view.getBigUint64(offset, false); offset += 8;
            
            const memberCount = view.getUint32(offset, false); offset += 4;
            const maxMembers = view.getUint32(offset, false); offset += 4;
            const messageCount = view.getUint32(offset, false); offset += 4;
            const fileCount = view.getUint32(offset, false); offset += 4;
            
            const encryptionEnabled = view.getUint8(offset) !== 0; offset += 1;
            const encryptionKeyId = this.readString(data, offset, 64); offset += 64;
            const encryptionKeyVersion = view.getUint32(offset, false); offset += 4;
            
            const twoFactorRequired = view.getUint8(offset) !== 0; offset += 1;
            const joinApprovalRequired = view.getUint8(offset) !== 0; offset += 1;
            const memberInviteAllowed = view.getUint8(offset) !== 0; offset += 1;
            const messageHistoryVisible = view.getUint8(offset) !== 0; offset += 1;
            
            // 读取标签
            const tagCount = view.getUint32(offset, false); offset += 4;
            const tags: string[] = [];
            for (let i = 0; i < tagCount; i++) {
                tags.push(this.readString(data, offset, 64));
                offset += 64;
            }
            
            return {
                groupId,
                name,
                description,
                avatarUrl: '',
                avatarHash: '',
                type,
                status,
                ownerId,
                createdAt: Number(createdAt),
                updatedAt: Number(updatedAt),
                lastActivity: Number(lastActivity),
                memberCount,
                maxMembers,
                messageCount,
                fileCount,
                encryptionEnabled,
                encryptionKeyId: encryptionKeyId || undefined,
                encryptionKeyVersion,
                twoFactorRequired,
                joinApprovalRequired,
                memberInviteAllowed,
                messageHistoryVisible,
                tags,
                customData: {}
            };
        } catch (error) {
            console.error('反序列化群组信息失败:', error);
            return null;
        }
    }
    
    // === 群组成员管理 ===
    
    // 邀请成员加入群组
    async inviteMember(groupId: bigint, userId: bigint, role: GroupRole = GroupRole.MEMBER): Promise<boolean> {
        // 检查权限
        if (!await this.hasPermission(groupId, GroupPermission.INVITE_MEMBER)) {
            throw new Error('没有权限邀请成员');
        }
        
        // 检查群组是否已满
        const groupInfo = this.groups.get(groupId);
        if (groupInfo && groupInfo.memberCount >= groupInfo.maxMembers) {
            throw new Error('群组已满');
        }
        
        // 准备邀请数据
        const data = new ArrayBuffer(8 + 8 + 1);
        const view = new DataView(data);
        view.setBigUint64(0, groupId, false);
        view.setBigUint64(8, userId, false);
        view.setUint8(16, this.groupRoleToCode(role));
        
        try {
            const response = await this.webSocket.sendMessage(
                MessageType.MSG_GROUP_JOIN, // 使用加入消息类型
                data,
                true
            );
            
            return response !== null;
        } catch (error) {
            console.error('邀请成员失败:', error);
            return false;
        }
    }
    
    // 移除成员
    async removeMember(groupId: bigint, userId: bigint, reason?: string): Promise<boolean> {
        // 检查权限
        if (!await this.hasPermission(groupId, GroupPermission.REMOVE_MEMBER)) {
            throw new Error('没有权限移除成员');
        }
        
        // 不能移除群主
        const groupInfo = this.groups.get(groupId);
        if (groupInfo?.ownerId === userId) {
            throw new Error('不能移除群主');
        }
        
        // 不能移除自己（除非是群主）
        const currentUserId = this.webSocket.getUserId();
        if (userId === currentUserId && groupInfo?.ownerId !== currentUserId) {
            throw new Error('不能移除自己');
        }
        
        // 准备移除数据
        const encoder = new TextEncoder();
        const reasonBuffer = encoder.encode(reason || '');
        
        const data = new ArrayBuffer(8 + 8 + 4 + reasonBuffer.length);
        const view = new DataView(data);
        let offset = 0;
        
        view.setBigUint64(offset, groupId, false); offset += 8;
        view.setBigUint64(offset, userId, false); offset += 8;
        view.setUint32(offset, reasonBuffer.length, false); offset += 4;
        
        const reasonArray = new Uint8Array(data, offset, reasonBuffer.length);
        reasonArray.set(reasonBuffer);
        
        try {
            // 发送移除请求
            // 注意：需要定义MSG_GROUP_REMOVE_MEMBER消息类型
            const response = await this.webSocket.sendMessage(
                0x0400000D, // 假设的消息类型
                data,
                true
            );
            
            return response !== null;
        } catch (error) {
            console.error('移除成员失败:', error);
            return false;
        }
    }
    
    // 设置成员角色
    async setMemberRole(groupId: bigint, userId: bigint, role: GroupRole): Promise<boolean> {
        // 检查权限
        if (!await this.hasPermission(groupId, GroupPermission.PROMOTE_MEMBER)) {
            throw new Error('没有权限设置成员角色');
        }
        
        // 检查角色限制
        const memberMap = this.groupMembers.get(groupId);
        const member = memberMap?.get(userId);
        if (!member) {
            throw new Error('成员不存在');
        }
        
        // 管理员数量限制
        if (role === GroupRole.ADMIN) {
            const adminCount = await this.getRoleCount(groupId, GroupRole.ADMIN);
            if (adminCount >= GroupManager.MAX_GROUP_ADMINS) {
                throw new Error(`管理员数量不能超过${GroupManager.MAX_GROUP_ADMINS}`);
            }
        }
        
        // 审核员数量限制
        if (role === GroupRole.MODERATOR) {
            const moderatorCount = await this.getRoleCount(groupId, GroupRole.MODERATOR);
            if (moderatorCount >= GroupManager.MAX_GROUP_MODERATORS) {
                throw new Error(`审核员数量不能超过${GroupManager.MAX_GROUP_MODERATORS}`);
            }
        }
        
        // 发送角色设置请求
        const data = new ArrayBuffer(8 + 8 + 1);
        const view = new DataView(data);
        view.setBigUint64(0, groupId, false);
        view.setBigUint64(8, userId, false);
        view.setUint8(16, this.groupRoleToCode(role));
        
        try {
            // 发送设置请求
            // 注意：需要定义MSG_GROUP_SET_ROLE消息类型
            const response = await this.webSocket.sendMessage(
                0x0400000E, // 假设的消息类型
                data,
                true
            );
            
            return response !== null;
        } catch (error) {
            console.error('设置成员角色失败:', error);
            return false;
        }
    }
    
    // 获取成员列表
    async getGroupMembers(groupId: bigint, limit = 100, offset = 0): Promise<GroupMember[]> {
        const memberMap = this.groupMembers.get(groupId);
        if (memberMap) {
            // 从缓存获取
            const members = Array.from(memberMap.values());
            return members.slice(offset, offset + limit);
        }
        
        // 从服务器获取
        const data = new ArrayBuffer(8 + 4 + 4);
        const view = new DataView(data);
        view.setBigUint64(0, groupId, false);
        view.setUint32(8, limit, false);
        view.setUint32(12, offset, false);
        
        try {
            await this.webSocket.sendMessage(
                MessageType.MSG_GROUP_MEMBERS_GET,
                data,
                true
            );
            
            // 响应将在handleGroupMembersResponse中处理
            return [];
        } catch (error) {
            console.error('获取成员列表失败:', error);
            return [];
        }
    }
    
    // 处理群组成员响应
    private handleGroupMembersResponse(data: ArrayBuffer): void {
        const members = this.deserializeGroupMembers(data);
        if (members.length > 0) {
            const groupId = members[0].groupId;
            
            // 更新缓存
            let memberMap = this.groupMembers.get(groupId);
            if (!memberMap) {
                memberMap = new Map();
                this.groupMembers.set(groupId, memberMap);
            }
            
            members.forEach(member => {
                memberMap!.set(member.userId, member);
            });
            
            this.emit('groupMembersUpdated', { groupId, members });
        }
    }
    
    // 反序列化群组成员
    private deserializeGroupMembers(data: ArrayBuffer): GroupMember[] {
        const members: GroupMember[] = [];
        const view = new DataView(data);
        let offset = 0;
        
        const memberCount = view.getUint32(offset, false); offset += 4;
        
        for (let i = 0; i < memberCount; i++) {
            const groupId = view.getBigUint64(offset, false); offset += 8;
            const userId = view.getBigUint64(offset, false); offset += 8;
            
            const roleCode = view.getUint8(offset); offset += 1;
            const role = this.codeToGroupRole(roleCode);
            
            const joinedAt = view.getBigUint64(offset, false); offset += 8;
            const lastSeen = view.getBigUint64(offset, false); offset += 8;
            const messageCount = view.getUint32(offset, false); offset += 4;
            
            // 昵称
            const nickname = this.readString(data, offset, 64); offset += 64;
            
            // 权限位图
            const permissionsBitmap = view.getBigUint64(offset, false); offset += 8;
            const permissions = this.bitmapToPermissions(permissionsBitmap);
            
            const mutedUntil = view.getBigUint64(offset, false); offset += 8;
            const bannedUntil = view.getBigUint64(offset, false); offset += 8;
            
            // 公钥
            const publicKey = this.readString(data, offset, 512); offset += 512;
            
            // 加密密钥
            const encryptionKey = this.readString(data, offset, 256); offset += 256;
            const keyVersion = view.getUint32(offset, false); offset += 4;
            
            const online = view.getUint8(offset) !== 0; offset += 1;
            const typing = view.getUint8(offset) !== 0; offset += 1;
            const active = view.getUint8(offset) !== 0; offset += 1;
            
            members.push({
                groupId,
                userId,
                role,
                joinedAt: Number(joinedAt),
                lastSeen: Number(lastSeen),
                messageCount,
                nickname,
                title: '',
                permissions,
                mutedUntil: mutedUntil !== 0n ? Number(mutedUntil) : undefined,
                bannedUntil: bannedUntil !== 0n ? Number(bannedUntil) : undefined,
                publicKey: publicKey || undefined,
                encryptionKey: encryptionKey || undefined,
                keyVersion,
                online,
                typing,
                active
            });
        }
        
        return members;
    }
    
    // === 群组消息 ===
    
    // 发送群组消息
    async sendGroupMessage(groupId: bigint, content: string, options: {
        encrypted?: boolean;
        replyTo?: bigint;
        mentions?: bigint[];
        attachments?: GroupAttachment[];
    } = {}): Promise<bigint | null> {
        // 检查权限
        if (!await this.hasPermission(groupId, GroupPermission.SEND_MESSAGE)) {
            throw new Error('没有权限发送消息');
        }
        
        // 检查是否被禁言
        const member = await this.getGroupMember(groupId, this.webSocket.getUserId());
        if (member?.mutedUntil && member.mutedUntil > Date.now()) {
            throw new Error('您已被禁言');
        }
        
        // 使用同步引擎发送消息
        try {
            const messageId = await this.syncEngine.sendMessage(
                0n, // 接收者ID为0，表示群组消息
                content,
                {
                    groupId,
                    encrypted: options.encrypted,
                    replyToId: options.replyTo,
                    metadata: {
                        mentions: options.mentions || [],
                        attachments: options.attachments || []
                    }
                }
            );
            
            return messageId ? BigInt(messageId) : null;
        } catch (error) {
            console.error('发送群组消息失败:', error);
            return null;
        }
    }
    
    // 获取群组消息历史
    async getGroupMessages(groupId: bigint, limit = 50, before?: bigint): Promise<GroupMessage[]> {
        // 从本地缓存获取
        const cachedMessages = this.groupMessages.get(groupId) || [];
        
        if (cachedMessages.length >= limit) {
            // 根据before参数过滤
            if (before) {
                const beforeIndex = cachedMessages.findIndex(msg => msg.messageId === before);
                if (beforeIndex !== -1) {
                    return cachedMessages.slice(
                        Math.max(0, beforeIndex - limit),
                        beforeIndex
                    );
                }
            }
            
            return cachedMessages.slice(-limit);
        }
        
        // 从服务器获取更多消息
        const data = new ArrayBuffer(8 + 8 + 4);
        const view = new DataView(data);
        view.setBigUint64(0, groupId, false);
        view.setBigUint64(8, before || 0n, false);
        view.setUint32(16, limit, false);
        
        try {
            await this.webSocket.sendMessage(
                MessageType.MSG_CHAT_HISTORY_GET,
                data,
                true
            );
            
            // 响应将在其他地方处理
            return cachedMessages.slice(-limit);
        } catch (error) {
            console.error('获取群组消息历史失败:', error);
            return cachedMessages.slice(-limit);
        }
    }
    
    // 处理群组消息
    private handleGroupMessage(data: ArrayBuffer): void {
        const message = this.deserializeGroupMessage(data);
        if (message) {
            // 添加到缓存
            let messages = this.groupMessages.get(message.groupId);
            if (!messages) {
                messages = [];
                this.groupMessages.set(message.groupId, messages);
            }
            
            // 按时间顺序插入
            const index = messages.findIndex(m => m.timestamp > message.timestamp);
            if (index === -1) {
                messages.push(message);
            } else {
                messages.splice(index, 0, message);
            }
            
            // 限制缓存大小
            if (messages.length > 1000) {
                messages.splice(0, messages.length - 1000);
            }
            
            // 触发事件
            this.emit('groupMessageReceived', { message });
        }
    }
    
    // 反序列化群组消息
    private deserializeGroupMessage(data: ArrayBuffer): GroupMessage | null {
        try {
            const view = new DataView(data);
            let offset = 0;
            
            const messageId = view.getBigUint64(offset, false); offset += 8;
            const groupId = view.getBigUint64(offset, false); offset += 8;
            const senderId = view.getBigUint64(offset, false); offset += 8;
            
            // 发送者信息
            const senderName = this.readString(data, offset, 64); offset += 64;
            const senderRoleCode = view.getUint8(offset); offset += 1;
            const senderRole = this.codeToGroupRole(senderRoleCode);
            
            // 内容长度
            const contentLength = view.getUint32(offset, false); offset += 4;
            
            // 内容
            const contentBytes = new Uint8Array(data, offset, contentLength);
            const content = new TextDecoder().decode(contentBytes);
            offset += contentLength;
            
            const type = view.getUint32(offset, false); offset += 4;
            const encrypted = view.getUint8(offset) !== 0; offset += 1;
            
            // 加密密钥ID
            const encryptionKeyId = this.readString(data, offset, 64); offset += 64;
            
            const timestamp = view.getBigUint64(offset, false); offset += 8;
            const editedAt = view.getBigUint64(offset, false); offset += 8;
            const deletedAt = view.getBigUint64(offset, false); offset += 8;
            
            // 已读用户列表
            const readByCount = view.getUint32(offset, false); offset += 4;
            const readBy: bigint[] = [];
            for (let i = 0; i < readByCount; i++) {
                readBy.push(view.getBigUint64(offset, false));
                offset += 8;
            }
            
            // 已送达用户列表
            const deliveredToCount = view.getUint32(offset, false); offset += 4;
            const deliveredTo: bigint[] = [];
            for (let i = 0; i < deliveredToCount; i++) {
                deliveredTo.push(view.getBigUint64(offset, false));
                offset += 8;
            }
            
            // 反应（简化处理）
            const reactions = {};
            
            // 回复消息ID
            const replyTo = view.getBigUint64(offset, false); offset += 8;
            
            // 提及用户列表
            const mentionsCount = view.getUint32(offset, false); offset += 4;
            const mentions: bigint[] = [];
            for (let i = 0; i < mentionsCount; i++) {
                mentions.push(view.getBigUint64(offset, false));
                offset += 8;
            }
            
            // 附件列表（简化处理）
            const attachments: GroupAttachment[] = [];
            
            return {
                messageId,
                groupId,
                senderId,
                senderName,
                senderRole,
                content,
                type,
                encrypted,
                encryptionKeyId: encryptionKeyId || undefined,
                timestamp: Number(timestamp),
                editedAt: editedAt !== 0n ? Number(editedAt) : undefined,
                deletedAt: deletedAt !== 0n ? Number(deletedAt) : undefined,
                readBy,
                deliveredTo,
                reactions,
                replyTo: replyTo !== 0n ? replyTo : undefined,
                mentions,
                attachments,
                metadata: {}
            };
        } catch (error) {
            console.error('反序列化群组消息失败:', error);
            return null;
        }
    }
    
    // 删除群组消息
    async deleteGroupMessage(groupId: bigint, messageId: bigint): Promise<boolean> {
        // 检查权限
        const currentUserId = this.webSocket.getUserId();
        const message = await this.getMessage(groupId, messageId);
        
        if (!message) {
            throw new Error('消息不存在');
        }
        
        // 检查是否有删除权限
        const hasDeletePermission = await this.hasPermission(groupId, GroupPermission.DELETE_MESSAGE);
        const isMessageOwner = message.senderId === currentUserId;
        
        if (!hasDeletePermission && !isMessageOwner) {
            throw new Error('没有权限删除消息');
        }
        
        // 发送删除请求
        const data = new ArrayBuffer(8 + 8);
        const view = new DataView(data);
        view.setBigUint64(0, groupId, false);
        view.setBigUint64(8, messageId, false);
        
        try {
            // 发送删除请求
            // 注意：需要定义MSG_GROUP_DELETE_MESSAGE消息类型
            const response = await this.webSocket.sendMessage(
                0x0400000F, // 假设的消息类型
                data,
                true
            );
            
            return response !== null;
        } catch (error) {
            console.error('删除群组消息失败:', error);
            return false;
        }
    }
    
    // === 权限管理 ===
    
    // 检查权限
    async hasPermission(groupId: bigint, permission: GroupPermission): Promise<boolean> {
        const currentUserId = this.webSocket.getUserId();
        
        // 获取用户角色
        const member = await this.getGroupMember(groupId, currentUserId);
        if (!member) {
            return false;
        }
        
        // 检查角色权限
        const rolePermissions = this.rolePermissions.get(member.role);
        if (rolePermissions && rolePermissions.has(permission)) {
            return true;
        }
        
        // 检查自定义权限
        if (member.permissions.includes(permission)) {
            return true;
        }
        
        return false;
    }
    
    // 设置成员权限
    async setMemberPermissions(groupId: bigint, userId: bigint, permissions: GroupPermission[]): Promise<boolean> {
        // 检查权限
        if (!await this.hasPermission(groupId, GroupPermission.MANAGE_PERMISSIONS)) {
            throw new Error('没有权限管理权限');
        }
        
        // 不能修改群主权限
        const groupInfo = this.groups.get(groupId);
        if (groupInfo?.ownerId === userId) {
            throw new Error('不能修改群主权限');
        }
        
        // 准备权限位图
        const permissionsBitmap = this.permissionsToBitmap(permissions);
        
        const data = new ArrayBuffer(8 + 8 + 8);
        const view = new DataView(data);
        view.setBigUint64(0, groupId, false);
        view.setBigUint64(8, userId, false);
        view.setBigUint64(16, permissionsBitmap, false);
        
        try {
            // 发送权限设置请求
            // 注意：需要定义MSG_GROUP_SET_PERMISSIONS消息类型
            const response = await this.webSocket.sendMessage(
                0x04000010, // 假设的消息类型
                data,
                true
            );
            
            return response !== null;
        } catch (error) {
            console.error('设置成员权限失败:', error);
            return false;
        }
    }
    
    // === 加密支持 ===
    
    // 初始化群组加密
    async initializeGroupEncryption(groupId: bigint): Promise<boolean> {
        const groupInfo = this.groups.get(groupId);
        if (!groupInfo) {
            throw new Error('群组不存在');
        }
        
        // 检查权限
        if (!await this.hasPermission(groupId, GroupPermission.MANAGE_ENCRYPTION)) {
            throw new Error('没有权限管理加密');
        }
        
        // 获取所有成员
        const members = await this.getGroupMembers(groupId);
        
        // 生成加密上下文
        const context = await this.createEncryptionContext(groupId, members);
        
        // 保存上下文
        this.encryptionContexts.set(groupId, context);
        
        // 更新群组信息
        groupInfo.encryptionEnabled = true;
        groupInfo.encryptionKeyVersion = context.keyVersion;
        
        // 保存到本地存储
        await this.saveEncryptionContext(context);
        
        return true;
    }
    
    // 创建加密上下文
    private async createEncryptionContext(groupId: bigint, members: GroupMember[]): Promise<GroupEncryptionContext> {
        // 生成群组加密密钥
        const encryptionKey = await crypto.subtle.generateKey(
            {
                name: 'AES-GCM',
                length: 256
            },
            true,
            ['encrypt', 'decrypt']
        );
        
        // 创建成员密钥映射
        const memberKeys = new Map<bigint, any>();
        
        // 为每个成员加密群组密钥
        for (const member of members) {
            if (member.publicKey) {
                // 使用成员公钥加密群组密钥
                const encryptedKey = await this.encryptKeyForMember(encryptionKey, member.publicKey);
                
                memberKeys.set(member.userId, {
                    publicKey: member.publicKey,
                    encryptedGroupKey: encryptedKey,
                    keyVersion: 1,
                    updatedAt: Date.now()
                });
            }
        }
        
        const now = Date.now();
        
        return {
            groupId,
            keyVersion: 1,
            encryptionKey,
            keyRotationDate: now,
            nextRotationDate: now + (30 * 24 * 60 * 60 * 1000), // 30天后轮换
            memberKeys,
            historicalKeys: new Map()
        };
    }
    
    // 轮换群组加密密钥
    async rotateGroupEncryptionKey(groupId: bigint): Promise<boolean> {
        const context = this.encryptionContexts.get(groupId);
        if (!context) {
            throw new Error('加密上下文不存在');
        }
        
        // 检查权限
        if (!await this.hasPermission(groupId, GroupPermission.MANAGE_ENCRYPTION)) {
            throw new Error('没有权限轮换加密密钥');
        }
        
        // 归档旧密钥
        context.historicalKeys.set(context.keyVersion, {
            keyId: `group_${groupId}_v${context.keyVersion}`,
            encryptionKey: context.encryptionKey,
            rotationDate: context.keyRotationDate,
            archivedAt: Date.now()
        });
        
        // 生成新密钥
        const newEncryptionKey = await crypto.subtle.generateKey(
            {
                name: 'AES-GCM',
                length: 256
            },
            true,
            ['encrypt', 'decrypt']
        );
        
        // 更新上下文
        context.encryptionKey = newEncryptionKey;
        context.keyVersion++;
        context.keyRotationDate = Date.now();
        context.nextRotationDate = Date.now() + (30 * 24 * 60 * 60 * 1000);
        
        // 重新加密所有成员的密钥
        await this.reencryptMemberKeys(context);
        
        // 保存上下文
        this.encryptionContexts.set(groupId, context);
        await this.saveEncryptionContext(context);
        
        // 更新群组信息
        const groupInfo = this.groups.get(groupId);
        if (groupInfo) {
            groupInfo.encryptionKeyVersion = context.keyVersion;
        }
        
        return true;
    }
    
    // 重新加密成员密钥
    private async reencryptMemberKeys(context: GroupEncryptionContext): Promise<void> {
        for (const [userId, memberKey] of context.memberKeys.entries()) {
            const member = await this.getGroupMember(context.groupId, userId);
            if (member?.publicKey) {
                const encryptedKey = await this.encryptKeyForMember(context.encryptionKey, member.publicKey);
                
                memberKey.encryptedGroupKey = encryptedKey;
                memberKey.keyVersion = context.keyVersion;
                memberKey.updatedAt = Date.now();
            }
        }
    }
    
    // 加密密钥给成员
    private async encryptKeyForMember(key: CryptoKey, publicKey: string): Promise<string> {
        // 这里需要实现使用成员公钥加密群组密钥的逻辑
        // 简化版：返回占位符
        return `encrypted_key_${Date.now()}`;
    }
    
    // === 工具方法 ===
    
    // 读取字符串
    private readString(data: ArrayBuffer, offset: number, maxLength: number): string {
        const bytes = new Uint8Array(data, offset, maxLength);
        let length = 0;
        
        while (length < maxLength && bytes[length] !== 0) {
            length++;
        }
        
        return new TextDecoder().decode(bytes.subarray(0, length));
    }
    
    // 群组类型转编码
    private groupTypeToCode(type: GroupType): number {
        switch (type) {
            case GroupType.PUBLIC: return 0;
            case GroupType.PRIVATE: return 1;
            case GroupType.SECRET: return 2;
            case GroupType.CHANNEL: return 3;
            case GroupType.SUPPORT: return 4;
            default: return 0;
        }
    }
    
    // 编码转群组类型
    private codeToGroupType(code: number): GroupType {
        switch (code) {
            case 0: return GroupType.PUBLIC;
            case 1: return GroupType.PRIVATE;
            case 2: return GroupType.SECRET;
            case 3: return GroupType.CHANNEL;
            case 4: return GroupType.SUPPORT;
            default: return GroupType.PRIVATE;
        }
    }
    
    // 群组角色转编码
    private groupRoleToCode(role: GroupRole): number {
        switch (role) {
            case GroupRole.OWNER: return 0;
            case GroupRole.ADMIN: return 1;
            case GroupRole.MODERATOR: return 2;
            case GroupRole.MEMBER: return 3;
            case GroupRole.GUEST: return 4;
            default: return 3;
        }
    }
    
    // 编码转群组角色
    private codeToGroupRole(code: number): GroupRole {
        switch (code) {
            case 0: return GroupRole.OWNER;
            case 1: return GroupRole.ADMIN;
            case 2: return GroupRole.MODERATOR;
            case 3: return GroupRole.MEMBER;
            case 4: return GroupRole.GUEST;
            default: return GroupRole.MEMBER;
        }
    }
    
    // 群组状态转编码
    private codeToGroupStatus(code: number): GroupStatus {
        switch (code) {
            case 0: return GroupStatus.ACTIVE;
            case 1: return GroupStatus.INACTIVE;
            case 2: return GroupStatus.ARCHIVED;
            case 3: return GroupStatus.BANNED;
            case 4: return GroupStatus.DELETED;
            default: return GroupStatus.ACTIVE;
        }
    }
    
    // 权限转位图
    private permissionsToBitmap(permissions: GroupPermission[]): bigint {
        let bitmap = 0n;
        
        // 定义权限位映射
        const permissionBits: Record<GroupPermission, number> = {
            [GroupPermission.SEND_MESSAGE]: 0,
            [GroupPermission.SEND_FILE]: 1,
            [GroupPermission.SEND_IMAGE]: 2,
            [GroupPermission.SEND_VOICE]: 3,
            [GroupPermission.DELETE_MESSAGE]: 4,
            [GroupPermission.EDIT_MESSAGE]: 5,
            [GroupPermission.PIN_MESSAGE]: 6,
            [GroupPermission.INVITE_MEMBER]: 7,
            [GroupPermission.REMOVE_MEMBER]: 8,
            [GroupPermission.PROMOTE_MEMBER]: 9,
            [GroupPermission.DEMOTE_MEMBER]: 10,
            [GroupPermission.MUTE_MEMBER]: 11,
            [GroupPermission.BAN_MEMBER]: 12,
            [GroupPermission.EDIT_GROUP_INFO]: 13,
            [GroupPermission.CHANGE_GROUP_AVATAR]: 14,
            [GroupPermission.MANAGE_ROLES]: 15,
            [GroupPermission.MANAGE_PERMISSIONS]: 16,
            [GroupPermission.TRANSFER_OWNERSHIP]: 17,
            [GroupPermission.DELETE_GROUP]: 18,
            [GroupPermission.EXPORT_GROUP_DATA]: 19,
            [GroupPermission.VIEW_AUDIT_LOG]: 20,
            [GroupPermission.MANAGE_ENCRYPTION]: 21,
            [GroupPermission.MANAGE_2FA]: 22,
            [GroupPermission.VIEW_MEMBER_INFO]: 23
        };
        
        permissions.forEach(permission => {
            const bit = permissionBits[permission];
            if (bit !== undefined) {
                bitmap |= (1n << BigInt(bit));
            }
        });
        
        return bitmap;
    }
    
    // 位图转权限
    private bitmapToPermissions(bitmap: bigint): GroupPermission[] {
        const permissions: GroupPermission[] = [];
        
        // 权限位映射
        const bitPermissions: Record<number, GroupPermission> = {
            0: GroupPermission.SEND_MESSAGE,
            1: GroupPermission.SEND_FILE,
            2: GroupPermission.SEND_IMAGE,
            3: GroupPermission.SEND_VOICE,
            4: GroupPermission.DELETE_MESSAGE,
            5: GroupPermission.EDIT_MESSAGE,
            6: GroupPermission.PIN_MESSAGE,
            7: GroupPermission.INVITE_MEMBER,
            8: GroupPermission.REMOVE_MEMBER,
            9: GroupPermission.PROMOTE_MEMBER,
            10: GroupPermission.DEMOTE_MEMBER,
            11: GroupPermission.MUTE_MEMBER,
            12: GroupPermission.BAN_MEMBER,
            13: GroupPermission.EDIT_GROUP_INFO,
            14: GroupPermission.CHANGE_GROUP_AVATAR,
            15: GroupPermission.MANAGE_ROLES,
            16: GroupPermission.MANAGE_PERMISSIONS,
            17: GroupPermission.TRANSFER_OWNERSHIP,
            18: GroupPermission.DELETE_GROUP,
            19: GroupPermission.EXPORT_GROUP_DATA,
            20: GroupPermission.VIEW_AUDIT_LOG,
            21: GroupPermission.MANAGE_ENCRYPTION,
            22: GroupPermission.MANAGE_2FA,
            23: GroupPermission.VIEW_MEMBER_INFO
        };
        
        for (let i = 0; i < 64; i++) {
            if ((bitmap & (1n << BigInt(i))) !== 0n) {
                const permission = bitPermissions[i];
                if (permission) {
                    permissions.push(permission);
                }
            }
        }
        
        return permissions;
    }
    
    // 获取角色数量
    private async getRoleCount(groupId: bigint, role: GroupRole): Promise<number> {
        const members = await this.getGroupMembers(groupId);
        return members.filter(member => member.role === role).length;
    }
    
    // 获取群组成员
    private async getGroupMember(groupId: bigint, userId: bigint): Promise<GroupMember | undefined> {
        const memberMap = this.groupMembers.get(groupId);
        if (memberMap) {
            return memberMap.get(userId);
        }
        
        // 如果缓存中没有，尝试从服务器获取
        await this.getGroupMembers(groupId);
        
        const updatedMap = this.groupMembers.get(groupId);
        return updatedMap?.get(userId);
    }
    
    // 获取消息
    private async getMessage(groupId: bigint, messageId: bigint): Promise<GroupMessage | undefined> {
        const messages = this.groupMessages.get(groupId);
        return messages?.find(msg => msg.messageId === messageId);
    }
    
    // 加载本地群组数据
    private async loadLocalGroups(): Promise<void> {
        try {
            // 从localStorage加载群组数据
            const savedGroups = localStorage.getItem('secure_chat_groups');
            if (savedGroups) {
                const groups = JSON.parse(savedGroups);
                groups.forEach((group: any) => {
                    this.groups.set(BigInt(group.groupId), {
                        ...group,
                        groupId: BigInt(group.groupId),
                        ownerId: BigInt(group.ownerId)
                    });
                });
            }
        } catch (error) {
            console.error('加载本地群组数据失败:', error);
        }
    }
    
    // 保存加密上下文
    private async saveEncryptionContext(context: GroupEncryptionContext): Promise<void> {
        try {
            // 导出密钥
            const exportedKey = await crypto.subtle.exportKey('jwk', context.encryptionKey);
            
            const data = {
                ...context,
                encryptionKey: exportedKey,
                memberKeys: Array.from(context.memberKeys.entries()),
                historicalKeys: Array.from(context.historicalKeys.entries())
            };
            
            localStorage.setItem(
                `group_encryption_${context.groupId}_v${context.keyVersion}`,
                JSON.stringify(data)
            );
        } catch (error) {
            console.error('保存加密上下文失败:', error);
        }
    }
    
    // 事件系统
    private emit(event: string, data: any): void {
        const listeners = this.eventListeners.get(event);
        if (listeners) {
            listeners.forEach(listener => {
                try {
                    listener(data);
                } catch (error) {
                    console.error(`事件监听器错误 (${event}):`, error);
                }
            });
        }
    }
    
    // 添加事件监听器
    on(event: string, callback: (data: any) => void): void {
        let listeners = this.eventListeners.get(event);
        if (!listeners) {
            listeners = [];
            this.eventListeners.set(event, listeners);
        }
        listeners.push(callback);
    }
    
    // 移除事件监听器
    off(event: string, callback: (data: any) => void): void {
        const listeners = this.eventListeners.get(event);
        if (listeners) {
            const index = listeners.indexOf(callback);
            if (index !== -1) {
                listeners.splice(index, 1);
            }
        }
    }
    
    // 清理
    async destroy(): Promise<void> {
        this.groups.clear();
        this.groupMembers.clear();
        this.groupMessages.clear();
        this.groupInvitations.clear();
        this.encryptionContexts.clear();
        this.eventListeners.clear();
    }
}