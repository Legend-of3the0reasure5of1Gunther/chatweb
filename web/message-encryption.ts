// 文件: src/core/message-encryption.ts
/**
 * 消息端到端加密模块
 * 支持离线消息的端到端加密存储
 */

import { EndToEndEncryption, EncryptionSession, GroupEncryptionContext } from './security';

export interface EncryptedMessageData {
    version: number;
    algorithm: string;
    iv: string;        // Base64编码的初始化向量
    ciphertext: string; // Base64编码的密文
    tag: string;       // Base64编码的认证标签
    keyId?: string;    // 使用的密钥ID
    timestamp: number;
    metadata?: Record<string, any>;
}

export interface MessageEncryptionKey {
    keyId: string;
    keyData: CryptoKey;
    createdAt: number;
    expiresAt: number;
    userIds: bigint[]; // 可以访问此密钥的用户
    groupId?: bigint;  // 如果是群组密钥
}

export class MessageEncryptionManager {
    private static readonly ENCRYPTION_VERSION = 1;
    private static readonly ENCRYPTION_ALGORITHM = 'AES-GCM';
    private static readonly KEY_SIZE = 256;
    private static readonly IV_SIZE = 12;
    private static readonly TAG_SIZE = 16;
    
    private encryptionSessions: Map<bigint, EncryptionSession> = new Map();
    private messageKeys: Map<string, MessageEncryptionKey> = new Map();
    private groupContexts: Map<bigint, GroupEncryptionContext> = new Map();
    private keyRotationInterval: NodeJS.Timeout | null = null;
    
    // 初始化
    async initialize(): Promise<void> {
        // 加载存储的密钥
        await this.loadStoredKeys();
        
        // 启动密钥轮换检查
        this.startKeyRotationCheck();
    }
    
    // 加密消息
    async encryptMessage(
        message: string | ArrayBuffer,
        receiverId: bigint,
        groupId?: bigint
    ): Promise<EncryptedMessageData> {
        // 1. 获取加密密钥
        const encryptionKey = await this.getEncryptionKey(receiverId, groupId);
        
        // 2. 准备数据
        let data: ArrayBuffer;
        if (typeof message === 'string') {
            const encoder = new TextEncoder();
            data = encoder.encode(message).buffer;
        } else {
            data = message;
        }
        
        // 3. 生成随机IV
        const iv = crypto.getRandomValues(new Uint8Array(MessageEncryptionManager.IV_SIZE));
        
        // 4. 加密数据
        const encrypted = await crypto.subtle.encrypt(
            {
                name: MessageEncryptionManager.ENCRYPTION_ALGORITHM,
                iv: iv,
                tagLength: MessageEncryptionManager.TAG_SIZE * 8
            },
            encryptionKey.keyData,
            data
        );
        
        // 5. 分离密文和认证标签
        const ciphertext = new Uint8Array(encrypted, 0, encrypted.byteLength - MessageEncryptionManager.TAG_SIZE);
        const tag = new Uint8Array(encrypted, encrypted.byteLength - MessageEncryptionManager.TAG_SIZE, MessageEncryptionManager.TAG_SIZE);
        
        // 6. 创建加密消息数据
        return {
            version: MessageEncryptionManager.ENCRYPTION_VERSION,
            algorithm: MessageEncryptionManager.ENCRYPTION_ALGORITHM,
            iv: this.arrayBufferToBase64(iv.buffer),
            ciphertext: this.arrayBufferToBase64(ciphertext.buffer),
            tag: this.arrayBufferToBase64(tag.buffer),
            keyId: encryptionKey.keyId,
            timestamp: Date.now(),
            metadata: {
                receiverId: receiverId.toString(),
                groupId: groupId?.toString()
            }
        };
    }
    
    // 解密消息
    async decryptMessage(encryptedData: EncryptedMessageData): Promise<string | ArrayBuffer> {
        // 1. 验证版本
        if (encryptedData.version !== MessageEncryptionManager.ENCRYPTION_VERSION) {
            throw new Error(`Unsupported encryption version: ${encryptedData.version}`);
        }
        
        // 2. 验证算法
        if (encryptedData.algorithm !== MessageEncryptionManager.ENCRYPTION_ALGORITHM) {
            throw new Error(`Unsupported encryption algorithm: ${encryptedData.algorithm}`);
        }
        
        // 3. 获取解密密钥
        const encryptionKey = await this.getDecryptionKey(encryptedData.keyId);
        if (!encryptionKey) {
            throw new Error(`Encryption key not found: ${encryptedData.keyId}`);
        }
        
        // 4. 准备数据
        const iv = this.base64ToArrayBuffer(encryptedData.iv);
        const ciphertext = this.base64ToArrayBuffer(encryptedData.ciphertext);
        const tag = this.base64ToArrayBuffer(encryptedData.tag);
        
        // 5. 合并密文和标签
        const combined = new Uint8Array(ciphertext.byteLength + tag.byteLength);
        combined.set(new Uint8Array(ciphertext), 0);
        combined.set(new Uint8Array(tag), ciphertext.byteLength);
        
        // 6. 解密数据
        const decrypted = await crypto.subtle.decrypt(
            {
                name: MessageEncryptionManager.ENCRYPTION_ALGORITHM,
                iv: iv,
                tagLength: MessageEncryptionManager.TAG_SIZE * 8
            },
            encryptionKey.keyData,
            combined
        );
        
        // 7. 根据元数据决定返回类型
        if (encryptedData.metadata?.isText !== false) {
            const decoder = new TextDecoder();
            return decoder.decode(decrypted);
        } else {
            return decrypted;
        }
    }
    
    // 获取加密密钥
    private async getEncryptionKey(receiverId: bigint, groupId?: bigint): Promise<MessageEncryptionKey> {
        // 1. 如果是群组消息，使用群组加密上下文
        if (groupId) {
            const groupContext = this.groupContexts.get(groupId);
            if (!groupContext) {
                throw new Error(`No encryption context for group: ${groupId}`);
            }
            
            return {
                keyId: `group_${groupId}_v${groupContext.keyVersion}`,
                keyData: groupContext.encryptionKey,
                createdAt: groupContext.keyRotationDate,
                expiresAt: groupContext.keyRotationDate + (7 * 24 * 60 * 60 * 1000), // 7天后过期
                userIds: Array.from(groupContext.members.keys()),
                groupId
            };
        }
        
        // 2. 查找现有的会话密钥
        const sessionKeyId = `session_${receiverId}`;
        let encryptionKey = this.messageKeys.get(sessionKeyId);
        
        // 3. 如果不存在或已过期，创建新密钥
        if (!encryptionKey || encryptionKey.expiresAt < Date.now()) {
            encryptionKey = await this.generateMessageKey([receiverId]);
            this.messageKeys.set(sessionKeyId, encryptionKey);
            await this.storeKey(encryptionKey);
        }
        
        return encryptionKey;
    }
    
    // 获取解密密钥
    private async getDecryptionKey(keyId?: string): Promise<MessageEncryptionKey | undefined> {
        if (!keyId) {
            return undefined;
        }
        
        // 1. 从内存缓存中查找
        let encryptionKey = this.messageKeys.get(keyId);
        
        // 2. 如果内存中不存在，从存储中加载
        if (!encryptionKey) {
            encryptionKey = await this.loadKey(keyId);
            if (encryptionKey) {
                this.messageKeys.set(keyId, encryptionKey);
            }
        }
        
        return encryptionKey;
    }
    
    // 生成消息密钥
    private async generateMessageKey(userIds: bigint[], groupId?: bigint): Promise<MessageEncryptionKey> {
        const key = await crypto.subtle.generateKey(
            {
                name: MessageEncryptionManager.ENCRYPTION_ALGORITHM,
                length: MessageEncryptionManager.KEY_SIZE
            },
            true, // 可导出（用于存储）
            ['encrypt', 'decrypt']
        );
        
        const keyId = `key_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`;
        const now = Date.now();
        
        return {
            keyId,
            keyData: key,
            createdAt: now,
            expiresAt: now + (30 * 24 * 60 * 60 * 1000), // 30天后过期
            userIds,
            groupId
        };
    }
    
    // 生成群组加密上下文
    async generateGroupEncryptionContext(groupId: bigint, memberIds: bigint[]): Promise<GroupEncryptionContext> {
        // 1. 生成群组加密密钥
        const groupKey = await crypto.subtle.generateKey(
            {
                name: MessageEncryptionManager.ENCRYPTION_ALGORITHM,
                length: MessageEncryptionManager.KEY_SIZE
            },
            true,
            ['encrypt', 'decrypt']
        );
        
        // 2. 为每个成员加密群组密钥
        const encryptedMembers = new Map<bigint, string>();
        
        for (const memberId of memberIds) {
            // 这里需要实现：使用每个成员的公钥加密群组密钥
            // 简化版：直接存储（生产环境必须加密）
            encryptedMembers.set(memberId, 'encrypted_key_placeholder');
        }
        
        const context: GroupEncryptionContext = {
            groupId,
            encryptionKey: groupKey,
            keyVersion: 1,
            keyRotationDate: Date.now(),
            members: encryptedMembers
        };
        
        this.groupContexts.set(groupId, context);
        await this.storeGroupContext(context);
        
        return context;
    }
    
    // 轮换群组密钥
    async rotateGroupKey(groupId: bigint): Promise<GroupEncryptionContext> {
        const oldContext = this.groupContexts.get(groupId);
        if (!oldContext) {
            throw new Error(`No encryption context for group: ${groupId}`);
        }
        
        // 生成新上下文，版本号+1
        const newContext = await this.generateGroupEncryptionContext(
            groupId,
            Array.from(oldContext.members.keys())
        );
        
        newContext.keyVersion = oldContext.keyVersion + 1;
        
        // 存储旧密钥的引用，用于解密旧消息
        await this.archiveOldGroupKey(oldContext);
        
        return newContext;
    }
    
    // 添加成员到群组加密上下文
    async addMemberToGroup(groupId: bigint, memberId: bigint, publicKey: string): Promise<void> {
        const context = this.groupContexts.get(groupId);
        if (!context) {
            throw new Error(`No encryption context for group: ${groupId}`);
        }
        
        // 使用新成员的公钥加密群组密钥
        const encryptedKey = await this.encryptGroupKeyForMember(
            context.encryptionKey,
            publicKey
        );
        
        context.members.set(memberId, encryptedKey);
        await this.storeGroupContext(context);
    }
    
    // 从群组加密上下文中移除成员
    async removeMemberFromGroup(groupId: bigint, memberId: bigint): Promise<void> {
        const context = this.groupContexts.get(groupId);
        if (!context) {
            throw new Error(`No encryption context for group: ${groupId}`);
        }
        
        // 移除成员
        context.members.delete(memberId);
        
        // 如果移除了成员，应该轮换密钥
        await this.rotateGroupKey(groupId);
    }
    
    // 加密群组密钥给成员
    private async encryptGroupKeyForMember(groupKey: CryptoKey, memberPublicKey: string): Promise<string> {
        // 这里需要实现使用成员公钥加密群组密钥的逻辑
        // 简化版：返回占位符
        return `encrypted_group_key_for_member_${Date.now()}`;
    }
    
    // 存储密钥
    private async storeKey(key: MessageEncryptionKey): Promise<void> {
        try {
            // 导出密钥
            const exported = await crypto.subtle.exportKey('jwk', key.keyData);
            
            const storageKey = `encryption_key_${key.keyId}`;
            const data = {
                ...key,
                keyData: exported
            };
            
            localStorage.setItem(storageKey, JSON.stringify(data));
        } catch (error) {
            console.error('Failed to store encryption key:', error);
        }
    }
    
    // 加载密钥
    private async loadKey(keyId: string): Promise<MessageEncryptionKey | undefined> {
        try {
            const storageKey = `encryption_key_${keyId}`;
            const stored = localStorage.getItem(storageKey);
            
            if (!stored) {
                return undefined;
            }
            
            const data = JSON.parse(stored);
            
            // 导入密钥
            const keyData = await crypto.subtle.importKey(
                'jwk',
                data.keyData,
                {
                    name: MessageEncryptionManager.ENCRYPTION_ALGORITHM,
                    length: MessageEncryptionManager.KEY_SIZE
                },
                true,
                ['encrypt', 'decrypt']
            );
            
            return {
                ...data,
                keyData
            };
        } catch (error) {
            console.error('Failed to load encryption key:', error);
            return undefined;
        }
    }
    
    // 存储群组上下文
    private async storeGroupContext(context: GroupEncryptionContext): Promise<void> {
        try {
            // 导出群组密钥
            const exportedKey = await crypto.subtle.exportKey('jwk', context.encryptionKey);
            
            const storageKey = `group_context_${context.groupId}_v${context.keyVersion}`;
            const data = {
                ...context,
                encryptionKey: exportedKey,
                members: Array.from(context.members.entries())
            };
            
            localStorage.setItem(storageKey, JSON.stringify(data));
        } catch (error) {
            console.error('Failed to store group context:', error);
        }
    }
    
    // 加载群组上下文
    private async loadGroupContext(groupId: bigint, version: number): Promise<GroupEncryptionContext | undefined> {
        try {
            const storageKey = `group_context_${groupId}_v${version}`;
            const stored = localStorage.getItem(storageKey);
            
            if (!stored) {
                return undefined;
            }
            
            const data = JSON.parse(stored);
            
            // 导入群组密钥
            const encryptionKey = await crypto.subtle.importKey(
                'jwk',
                data.encryptionKey,
                {
                    name: MessageEncryptionManager.ENCRYPTION_ALGORITHM,
                    length: MessageEncryptionManager.KEY_SIZE
                },
                true,
                ['encrypt', 'decrypt']
            );
            
            return {
                ...data,
                encryptionKey,
                members: new Map(data.members)
            };
        } catch (error) {
            console.error('Failed to load group context:', error);
            return undefined;
        }
    }
    
    // 加载存储的密钥
    private async loadStoredKeys(): Promise<void> {
        try {
            // 加载所有密钥
            for (let i = 0; i < localStorage.length; i++) {
                const key = localStorage.key(i);
                if (key?.startsWith('encryption_key_')) {
                    const keyId = key.replace('encryption_key_', '');
                    const loadedKey = await this.loadKey(keyId);
                    if (loadedKey) {
                        this.messageKeys.set(keyId, loadedKey);
                    }
                } else if (key?.startsWith('group_context_')) {
                    // 解析群组ID和版本
                    const match = key.match(/group_context_(\d+)_v(\d+)/);
                    if (match) {
                        const groupId = BigInt(match[1]);
                        const version = parseInt(match[2]);
                        
                        const context = await this.loadGroupContext(groupId, version);
                        if (context) {
                            this.groupContexts.set(groupId, context);
                        }
                    }
                }
            }
        } catch (error) {
            console.error('Failed to load stored keys:', error);
        }
    }
    
    // 启动密钥轮换检查
    private startKeyRotationCheck(): void {
        // 每小时检查一次密钥过期
        this.keyRotationInterval = setInterval(() => {
            this.checkKeyRotation().catch(console.error);
        }, 60 * 60 * 1000); // 1小时
    }
    
    // 检查密钥轮换
    private async checkKeyRotation(): Promise<void> {
        const now = Date.now();
        
        // 检查消息密钥
        for (const [keyId, key] of this.messageKeys.entries()) {
            // 如果密钥即将过期（7天内），重新生成
            if (key.expiresAt < now + (7 * 24 * 60 * 60 * 1000)) {
                console.log(`Key ${keyId} is expiring soon, regenerating...`);
                await this.rotateMessageKey(keyId);
            }
        }
        
        // 检查群组密钥
        for (const [groupId, context] of this.groupContexts.entries()) {
            // 群组密钥30天后轮换
            if (context.keyRotationDate < now - (30 * 24 * 60 * 60 * 1000)) {
                console.log(`Group ${groupId} key is old, rotating...`);
                await this.rotateGroupKey(groupId);
            }
        }
    }
    
    // 轮换消息密钥
    private async rotateMessageKey(keyId: string): Promise<void> {
        const oldKey = this.messageKeys.get(keyId);
        if (!oldKey) {
            return;
        }
        
        // 生成新密钥
        const newKey = await this.generateMessageKey(oldKey.userIds, oldKey.groupId);
        
        // 更新映射
        this.messageKeys.delete(keyId);
        this.messageKeys.set(newKey.keyId, newKey);
        
        // 存储新密钥
        await this.storeKey(newKey);
        
        // 归档旧密钥
        await this.archiveOldKey(oldKey);
    }
    
    // 归档旧密钥
    private async archiveOldKey(key: MessageEncryptionKey): Promise<void> {
        const archiveKey = `archived_key_${key.keyId}_${Date.now()}`;
        const exported = await crypto.subtle.exportKey('jwk', key.keyData);
        
        const data = {
            ...key,
            keyData: exported,
            archivedAt: Date.now()
        };
        
        localStorage.setItem(archiveKey, JSON.stringify(data));
    }
    
    // 归档旧群组密钥
    private async archiveOldGroupKey(context: GroupEncryptionContext): Promise<void> {
        const archiveKey = `archived_group_key_${context.groupId}_v${context.keyVersion}_${Date.now()}`;
        const exportedKey = await crypto.subtle.exportKey('jwk', context.encryptionKey);
        
        const data = {
            ...context,
            encryptionKey: exportedKey,
            members: Array.from(context.members.entries()),
            archivedAt: Date.now()
        };
        
        localStorage.setItem(archiveKey, JSON.stringify(data));
    }
    
    // ArrayBuffer转Base64
    private arrayBufferToBase64(buffer: ArrayBuffer): string {
        const bytes = new Uint8Array(buffer);
        let binary = '';
        for (let i = 0; i < bytes.byteLength; i++) {
            binary += String.fromCharCode(bytes[i]);
        }
        return btoa(binary);
    }
    
    // Base64转ArrayBuffer
    private base64ToArrayBuffer(base64: string): ArrayBuffer {
        const binary = atob(base64);
        const bytes = new Uint8Array(binary.length);
        for (let i = 0; i < binary.length; i++) {
            bytes[i] = binary.charCodeAt(i);
        }
        return bytes.buffer;
    }
    
    // 清理
    async destroy(): Promise<void> {
        if (this.keyRotationInterval) {
            clearInterval(this.keyRotationInterval);
            this.keyRotationInterval = null;
        }
        
        this.encryptionSessions.clear();
        this.messageKeys.clear();
        this.groupContexts.clear();
    }
}