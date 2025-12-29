// 文件: src/core/group-encryption.ts
/**
 * 群组端到端加密模块
 * 支持群组消息的端到端加密、密钥轮换、成员管理
 */

import { GroupManager, GroupMember, GroupInfo } from './group-manager';

export interface GroupKeyBundle {
    keyId: string;
    groupId: bigint;
    keyVersion: number;
    encryptedKey: string; // 加密的群组密钥
    publicKey: string;    // 用于验证签名的公钥
    signature: string;    // 签名
    timestamp: number;
    expiresAt: number;
}

export interface EncryptedGroupMessage {
    version: number;
    groupId: bigint;
    keyVersion: number;
    keyId: string;
    iv: string;          // 初始化向量
    ciphertext: string;  // 加密的消息内容
    tag: string;         // 认证标签
    metadata: Record<string, any>;
}

export class GroupEncryptionService {
    private static readonly KEY_SIZE = 256;
    private static readonly IV_SIZE = 12;
    private static readonly TAG_SIZE = 16;
    
    private groupManager: GroupManager;
    private keyBundles: Map<string, GroupKeyBundle> = new Map();
    private groupKeys: Map<bigint, Map<number, CryptoKey>> = new Map();
    
    constructor(groupManager: GroupManager) {
        this.groupManager = groupManager;
        
        // 加载存储的密钥
        this.loadStoredKeys();
    }
    
    // 生成群组加密密钥
    async generateGroupKey(groupId: bigint, keyVersion: number): Promise<CryptoKey> {
        const key = await crypto.subtle.generateKey(
            {
                name: 'AES-GCM',
                length: GroupEncryptionService.KEY_SIZE
            },
            true, // 可导出
            ['encrypt', 'decrypt']
        );
        
        // 保存到内存
        let groupKeyMap = this.groupKeys.get(groupId);
        if (!groupKeyMap) {
            groupKeyMap = new Map();
            this.groupKeys.set(groupId, groupKeyMap);
        }
        groupKeyMap.set(keyVersion, key);
        
        // 保存到存储
        await this.storeGroupKey(groupId, keyVersion, key);
        
        return key;
    }
    
    // 为成员创建密钥包
    async createKeyBundleForMember(
        groupId: bigint,
        member: GroupMember,
        groupKey: CryptoKey,
        keyVersion: number
    ): Promise<GroupKeyBundle> {
        if (!member.publicKey) {
            throw new Error('成员没有公钥');
        }
        
        // 导出群组密钥
        const exportedKey = await crypto.subtle.exportKey('raw', groupKey);
        
        // 使用成员公钥加密群组密钥
        const encryptedKey = await this.encryptWithPublicKey(
            exportedKey,
            member.publicKey
        );
        
        // 创建密钥ID
        const keyId = `group_${groupId}_v${keyVersion}_${member.userId}`;
        
        // 签名密钥包
        const signature = await this.signKeyBundle({
            keyId,
            groupId,
            keyVersion,
            encryptedKey,
            publicKey: member.publicKey,
            timestamp: Date.now(),
            expiresAt: Date.now() + (30 * 24 * 60 * 60 * 1000) // 30天过期
        });
        
        const bundle: GroupKeyBundle = {
            keyId,
            groupId,
            keyVersion,
            encryptedKey,
            publicKey: member.publicKey,
            signature,
            timestamp: Date.now(),
            expiresAt: Date.now() + (30 * 24 * 60 * 60 * 1000)
        };
        
        // 保存密钥包
        this.keyBundles.set(keyId, bundle);
        await this.storeKeyBundle(bundle);
        
        return bundle;
    }
    
    // 加密群组消息
    async encryptGroupMessage(
        groupId: bigint,
        keyVersion: number,
        message: string | ArrayBuffer
    ): Promise<EncryptedGroupMessage> {
        // 获取群组密钥
        const groupKey = await this.getGroupKey(groupId, keyVersion);
        if (!groupKey) {
            throw new Error(`群组密钥不存在: ${groupId} v${keyVersion}`);
        }
        
        // 准备数据
        let data: ArrayBuffer;
        if (typeof message === 'string') {
            const encoder = new TextEncoder();
            data = encoder.encode(message).buffer;
        } else {
            data = message;
        }
        
        // 生成随机IV
        const iv = crypto.getRandomValues(new Uint8Array(GroupEncryptionService.IV_SIZE));
        
        // 加密数据
        const encrypted = await crypto.subtle.encrypt(
            {
                name: 'AES-GCM',
                iv: iv,
                tagLength: GroupEncryptionService.TAG_SIZE * 8
            },
            groupKey,
            data
        );
        
        // 分离密文和标签
        const ciphertext = new Uint8Array(encrypted, 0, encrypted.byteLength - GroupEncryptionService.TAG_SIZE);
        const tag = new Uint8Array(encrypted, encrypted.byteLength - GroupEncryptionService.TAG_SIZE, GroupEncryptionService.TAG_SIZE);
        
        // 创建密钥ID
        const keyId = `group_${groupId}_v${keyVersion}`;
        
        return {
            version: 1,
            groupId,
            keyVersion,
            keyId,
            iv: this.arrayBufferToBase64(iv.buffer),
            ciphertext: this.arrayBufferToBase64(ciphertext.buffer),
            tag: this.arrayBufferToBase64(tag.buffer),
            metadata: {
                timestamp: Date.now(),
                algorithm: 'AES-GCM-256'
            }
        };
    }
    
    // 解密群组消息
    async decryptGroupMessage(encryptedMessage: EncryptedGroupMessage): Promise<string | ArrayBuffer> {
        // 获取群组密钥
        const groupKey = await this.getGroupKey(encryptedMessage.groupId, encryptedMessage.keyVersion);
        if (!groupKey) {
            throw new Error(`群组密钥不存在: ${encryptedMessage.groupId} v${encryptedMessage.keyVersion}`);
        }
        
        // 准备数据
        const iv = this.base64ToArrayBuffer(encryptedMessage.iv);
        const ciphertext = this.base64ToArrayBuffer(encryptedMessage.ciphertext);
        const tag = this.base64ToArrayBuffer(encryptedMessage.tag);
        
        // 合并密文和标签
        const combined = new Uint8Array(ciphertext.byteLength + tag.byteLength);
        combined.set(new Uint8Array(ciphertext), 0);
        combined.set(new Uint8Array(tag), ciphertext.byteLength);
        
        // 解密数据
        const decrypted = await crypto.subtle.decrypt(
            {
                name: 'AES-GCM',
                iv: iv,
                tagLength: GroupEncryptionService.TAG_SIZE * 8
            },
            groupKey,
            combined
        );
        
        // 根据元数据决定返回类型
        if (encryptedMessage.metadata?.isText !== false) {
            const decoder = new TextDecoder();
            return decoder.decode(decrypted);
        } else {
            return decrypted;
        }
    }
    
    // 轮换群组密钥
    async rotateGroupKey(groupId: bigint): Promise<number> {
        // 获取当前密钥版本
        const groupInfo = await this.groupManager.getGroupInfo(groupId);
        if (!groupInfo) {
            throw new Error('群组不存在');
        }
        
        const newKeyVersion = groupInfo.encryptionKeyVersion + 1;
        
        // 生成新密钥
        const newKey = await this.generateGroupKey(groupId, newKeyVersion);
        
        // 为所有成员创建新密钥包
        const members = await this.groupManager.getGroupMembers(groupId);
        
        for (const member of members) {
            if (member.publicKey) {
                await this.createKeyBundleForMember(groupId, member, newKey, newKeyVersion);
            }
        }
        
        // 归档旧密钥
        await this.archiveOldKey(groupId, groupInfo.encryptionKeyVersion);
        
        return newKeyVersion;
    }
    
    // 添加新成员到加密群组
    async addMemberToEncryptedGroup(groupId: bigint, member: GroupMember): Promise<void> {
        const groupInfo = await this.groupManager.getGroupInfo(groupId);
        if (!groupInfo) {
            throw new Error('群组不存在');
        }
        
        if (!groupInfo.encryptionEnabled) {
            return; // 群组未启用加密
        }
        
        if (!member.publicKey) {
            throw new Error('新成员没有公钥，无法加入加密群组');
        }
        
        // 获取当前群组密钥
        const currentKeyVersion = groupInfo.encryptionKeyVersion;
        const groupKey = await this.getGroupKey(groupId, currentKeyVersion);
        
        if (!groupKey) {
            throw new Error('群组密钥不存在');
        }
        
        // 为新成员创建密钥包
        await this.createKeyBundleForMember(groupId, member, groupKey, currentKeyVersion);
    }
    
    // 移除成员从加密群组
    async removeMemberFromEncryptedGroup(groupId: bigint, userId: bigint): Promise<void> {
        // 删除该成员的所有密钥包
        const keyIdsToDelete: string[] = [];
        
        for (const [keyId, bundle] of this.keyBundles.entries()) {
            if (bundle.groupId === groupId && keyId.includes(`_${userId}`)) {
                keyIdsToDelete.push(keyId);
            }
        }
        
        // 删除密钥包
        keyIdsToDelete.forEach(keyId => {
            this.keyBundles.delete(keyId);
            localStorage.removeItem(`key_bundle_${keyId}`);
        });
        
        // 如果需要，轮换密钥以确保前成员无法解密新消息
        await this.rotateGroupKey(groupId);
    }
    
    // 获取群组密钥
    private async getGroupKey(groupId: bigint, keyVersion: number): Promise<CryptoKey | undefined> {
        // 从内存缓存获取
        const groupKeyMap = this.groupKeys.get(groupId);
        if (groupKeyMap) {
            const key = groupKeyMap.get(keyVersion);
            if (key) {
                return key;
            }
        }
        
        // 从存储加载
        return await this.loadGroupKey(groupId, keyVersion);
    }
    
    // 使用公钥加密
    private async encryptWithPublicKey(data: ArrayBuffer, publicKey: string): Promise<string> {
        // 这里需要实现使用公钥加密的逻辑
        // 简化版：返回Base64编码
        return this.arrayBufferToBase64(data);
    }
    
    // 使用私钥解密
    private async decryptWithPrivateKey(encryptedData: string, privateKey: CryptoKey): Promise<ArrayBuffer> {
        // 这里需要实现使用私钥解密的逻辑
        // 简化版：解码Base64
        return this.base64ToArrayBuffer(encryptedData);
    }
    
    // 签名密钥包
    private async signKeyBundle(bundle: Omit<GroupKeyBundle, 'signature'>): Promise<string> {
        // 创建签名数据
        const data = JSON.stringify({
            keyId: bundle.keyId,
            groupId: bundle.groupId.toString(),
            keyVersion: bundle.keyVersion,
            encryptedKey: bundle.encryptedKey,
            timestamp: bundle.timestamp,
            expiresAt: bundle.expiresAt
        });
        
        // 这里需要实现签名逻辑
        // 简化版：返回空签名
        return '';
    }
    
    // 验证密钥包签名
    private async verifyKeyBundle(bundle: GroupKeyBundle): Promise<boolean> {
        // 这里需要实现验证逻辑
        // 简化版：总是返回true
        return true;
    }
    
    // 存储群组密钥
    private async storeGroupKey(groupId: bigint, keyVersion: number, key: CryptoKey): Promise<void> {
        try {
            const exportedKey = await crypto.subtle.exportKey('jwk', key);
            
            const storageKey = `group_key_${groupId}_v${keyVersion}`;
            localStorage.setItem(storageKey, JSON.stringify(exportedKey));
        } catch (error) {
            console.error('存储群组密钥失败:', error);
        }
    }
    
    // 加载群组密钥
    private async loadGroupKey(groupId: bigint, keyVersion: number): Promise<CryptoKey | undefined> {
        try {
            const storageKey = `group_key_${groupId}_v${keyVersion}`;
            const stored = localStorage.getItem(storageKey);
            
            if (!stored) {
                return undefined;
            }
            
            const keyData = JSON.parse(stored);
            
            const key = await crypto.subtle.importKey(
                'jwk',
                keyData,
                {
                    name: 'AES-GCM',
                    length: GroupEncryptionService.KEY_SIZE
                },
                true,
                ['encrypt', 'decrypt']
            );
            
            // 保存到内存缓存
            let groupKeyMap = this.groupKeys.get(groupId);
            if (!groupKeyMap) {
                groupKeyMap = new Map();
                this.groupKeys.set(groupId, groupKeyMap);
            }
            groupKeyMap.set(keyVersion, key);
            
            return key;
        } catch (error) {
            console.error('加载群组密钥失败:', error);
            return undefined;
        }
    }
    
    // 存储密钥包
    private async storeKeyBundle(bundle: GroupKeyBundle): Promise<void> {
        try {
            const storageKey = `key_bundle_${bundle.keyId}`;
            localStorage.setItem(storageKey, JSON.stringify(bundle));
        } catch (error) {
            console.error('存储密钥包失败:', error);
        }
    }
    
    // 加载存储的密钥
    private async loadStoredKeys(): Promise<void> {
        try {
            // 加载所有密钥包
            for (let i = 0; i < localStorage.length; i++) {
                const key = localStorage.key(i);
                
                if (key?.startsWith('key_bundle_')) {
                    const stored = localStorage.getItem(key);
                    if (stored) {
                        const bundle = JSON.parse(stored) as GroupKeyBundle;
                        
                        // 验证签名
                        const valid = await this.verifyKeyBundle(bundle);
                        if (valid && bundle.expiresAt > Date.now()) {
                            this.keyBundles.set(bundle.keyId, bundle);
                        } else {
                            // 删除过期或无效的密钥包
                            localStorage.removeItem(key);
                        }
                    }
                }
            }
        } catch (error) {
            console.error('加载存储的密钥失败:', error);
        }
    }
    
    // 归档旧密钥
    private async archiveOldKey(groupId: bigint, keyVersion: number): Promise<void> {
        const storageKey = `group_key_${groupId}_v${keyVersion}`;
        const archivedKey = `archived_group_key_${groupId}_v${keyVersion}_${Date.now()}`;
        
        const stored = localStorage.getItem(storageKey);
        if (stored) {
            // 移动到归档存储
            localStorage.setItem(archivedKey, stored);
            localStorage.removeItem(storageKey);
            
            // 从内存缓存中移除
            const groupKeyMap = this.groupKeys.get(groupId);
            if (groupKeyMap) {
                groupKeyMap.delete(keyVersion);
            }
        }
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
        this.keyBundles.clear();
        this.groupKeys.clear();
    }
}