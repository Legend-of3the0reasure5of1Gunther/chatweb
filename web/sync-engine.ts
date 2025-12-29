// 文件: src/core/sync-engine.ts
/**
 * 同步引擎 - 统一管理离线存储、消息加密和同步
 */

import { SecureChatWebSocket } from './websocket-client';
import { OfflineSyncManager, OfflineMessage, MessageStatus } from './offline-sync';
import { SyncConflictResolver, SyncConflict } from './sync-conflict-resolver';
import { MessageEncryptionManager, EncryptedMessageData } from './message-encryption';
import { EndToEndEncryption } from './security';

export interface SyncEngineConfig {
    autoSyncInterval: number; // 自动同步间隔（毫秒）
    maxRetries: number;       // 最大重试次数
    encryptionEnabled: boolean; // 启用加密
    conflictStrategy: 'auto' | 'manual' | 'server_wins' | 'client_wins';
    maxOfflineStorage: number; // 最大离线存储大小（MB）
    syncOnConnect: boolean;    // 连接时自动同步
}

export class SyncEngine {
    private webSocket: SecureChatWebSocket;
    private offlineSync: OfflineSyncManager;
    private conflictResolver: SyncConflictResolver;
    private encryptionManager: MessageEncryptionManager;
    
    private config: SyncEngineConfig;
    private isInitialized = false;
    private syncQueue: Map<string, Promise<void>> = new Map();
    
    constructor(webSocket: SecureChatWebSocket, config: Partial<SyncEngineConfig> = {}) {
        this.webSocket = webSocket;
        
        this.config = {
            autoSyncInterval: 60 * 1000, // 1分钟
            maxRetries: 3,
            encryptionEnabled: true,
            conflictStrategy: 'auto',
            maxOfflineStorage: 100, // 100MB
            syncOnConnect: true,
            ...config
        };
        
        this.offlineSync = new OfflineSyncManager(webSocket);
        this.conflictResolver = new SyncConflictResolver();
        this.encryptionManager = new MessageEncryptionManager();
        
        // 设置手动解决回调
        if (this.config.conflictStrategy === 'manual') {
            this.conflictResolver.setManualResolutionCallback(
                (conflict) => this.handleManualConflictResolution(conflict)
            );
        }
    }
    
    // 初始化
    async initialize(): Promise<void> {
        if (this.isInitialized) {
            return;
        }
        
        try {
            // 1. 初始化离线同步管理器
            await this.offlineSync.initialize();
            
            // 2. 初始化加密管理器
            await this.encryptionManager.initialize();
            
            // 3. 监听网络状态
            this.setupNetworkListeners();
            
            // 4. 如果已连接，执行初始同步
            if (this.webSocket.getConnectionState() === 'connected' && this.config.syncOnConnect) {
                await this.fullSync();
            }
            
            this.isInitialized = true;
            console.log('SyncEngine initialized');
            
        } catch (error) {
            console.error('Failed to initialize SyncEngine:', error);
            throw error;
        }
    }
    
    // 设置网络监听器
    private setupNetworkListeners(): void {
        // 网络恢复时同步
        window.addEventListener('online', () => {
            console.log('Network online, triggering sync');
            this.autoSync().catch(console.error);
        });
        
        // WebSocket连接状态变化
        this.webSocket.registerConnectionStateCallback((state) => {
            if (state === 'connected' && this.config.syncOnConnect) {
                this.autoSync().catch(console.error);
            }
        });
    }
    
    // 自动同步
    async autoSync(): Promise<void> {
        if (!this.isInitialized) {
            return;
        }
        
        const syncId = `sync_${Date.now()}`;
        
        // 防止重复同步
        if (this.syncQueue.has(syncId)) {
            return;
        }
        
        const syncPromise = this.performAutoSync().finally(() => {
            this.syncQueue.delete(syncId);
        });
        
        this.syncQueue.set(syncId, syncPromise);
        return syncPromise;
    }
    
    // 执行自动同步
    private async performAutoSync(): Promise<void> {
        try {
            // 1. 同步待发送消息
            await this.syncPendingMessages();
            
            // 2. 拉取服务器消息
            await this.pullNewMessages();
            
            // 3. 同步消息状态
            await this.syncMessageStatus();
            
            console.log('Auto sync completed');
            
        } catch (error) {
            console.error('Auto sync failed:', error);
            throw error;
        }
    }
    
    // 完整同步
    async fullSync(): Promise<void> {
        if (!this.isInitialized) {
            throw new Error('SyncEngine not initialized');
        }
        
        console.log('Starting full sync...');
        
        try {
            await this.offlineSync.forceSync();
            console.log('Full sync completed');
            
        } catch (error) {
            console.error('Full sync failed:', error);
            throw error;
        }
    }
    
    // 发送消息（支持离线）
    async sendMessage(
        receiverId: bigint,
        content: string,
        options: {
            groupId?: bigint;
            encrypted?: boolean;
            replyToId?: bigint;
            metadata?: Record<string, any>;
        } = {}
    ): Promise<string> {
        if (!this.isInitialized) {
            throw new Error('SyncEngine not initialized');
        }
        
        const messageId = `local_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`;
        
        try {
            let messageContent: string | ArrayBuffer = content;
            let encrypted = options.encrypted ?? this.config.encryptionEnabled;
            let encryptionKeyId: string | undefined;
            
            // 如果需要加密
            if (encrypted) {
                const encryptedData = await this.encryptionManager.encryptMessage(
                    content,
                    receiverId,
                    options.groupId
                );
                
                messageContent = JSON.stringify(encryptedData);
                encryptionKeyId = encryptedData.keyId;
            }
            
            // 创建离线消息
            const offlineMessage: Omit<OfflineMessage, 'id' | 'localTimestamp' | 'status' | 'sendAttempts' | 'lastAttempt' | 'synced' | 'readBy' | 'deliveredTo'> = {
                localId: messageId,
                messageId: 0n, // 服务器分配
                conversationId: options.groupId ? `group:${options.groupId}` : `user:${receiverId}`,
                senderId: this.webSocket.getUserId(),
                receiverId,
                groupId: options.groupId,
                type: options.groupId ? 0x04000007 : 0x03000001, // 群组消息或私聊消息
                content: messageContent,
                encrypted,
                encryptionKeyId,
                timestamp: Date.now(),
                metadata: {
                    ...options.metadata,
                    replyToId: options.replyToId,
                    encrypted,
                    encryptionKeyId
                }
            };
            
            // 存储到离线数据库
            const storedId = await this.offlineSync.storeMessage(offlineMessage);
            
            // 立即尝试发送
            if (navigator.onLine) {
                this.autoSync().catch(console.error);
            }
            
            return storedId;
            
        } catch (error) {
            console.error('Failed to send message:', error);
            throw error;
        }
    }
    
    // 发送文件（支持离线）
    async sendFile(
        receiverId: bigint,
        file: File,
        options: {
            groupId?: bigint;
            encrypted?: boolean;
            metadata?: Record<string, any>;
        } = {}
    ): Promise<string> {
        if (!this.isInitialized) {
            throw new Error('SyncEngine not initialized');
        }
        
        // 检查文件大小
        if (file.size > this.config.maxOfflineStorage * 1024 * 1024) {
            throw new Error(`File too large. Maximum size is ${this.config.maxOfflineStorage}MB`);
        }
        
        const messageId = `file_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`;
        
        try {
            // 读取文件内容
            const arrayBuffer = await file.arrayBuffer();
            let content: ArrayBuffer | string = arrayBuffer;
            let encrypted = options.encrypted ?? this.config.encryptionEnabled;
            let encryptionKeyId: string | undefined;
            
            // 如果需要加密
            if (encrypted) {
                const encryptedData = await this.encryptionManager.encryptMessage(
                    arrayBuffer,
                    receiverId,
                    options.groupId
                );
                
                content = JSON.stringify(encryptedData);
                encryptionKeyId = encryptedData.keyId;
            }
            
            // 创建离线消息
            const offlineMessage: Omit<OfflineMessage, 'id' | 'localTimestamp' | 'status' | 'sendAttempts' | 'lastAttempt' | 'synced' | 'readBy' | 'deliveredTo'> = {
                localId: messageId,
                messageId: 0n,
                conversationId: options.groupId ? `group:${options.groupId}` : `user:${receiverId}`,
                senderId: this.webSocket.getUserId(),
                receiverId,
                groupId: options.groupId,
                type: 0x03000004, // 文件消息
                content,
                encrypted,
                encryptionKeyId,
                timestamp: Date.now(),
                metadata: {
                    ...options.metadata,
                    fileName: file.name,
                    fileSize: file.size,
                    mimeType: file.type,
                    encrypted,
                    encryptionKeyId,
                    isText: false
                }
            };
            
            // 存储到离线数据库
            const storedId = await this.offlineSync.storeMessage(offlineMessage);
            
            // 立即尝试发送
            if (navigator.onLine) {
                this.autoSync().catch(console.error);
            }
            
            return storedId;
            
        } catch (error) {
            console.error('Failed to send file:', error);
            throw error;
        }
    }
    
    // 获取对话消息
    async getConversationMessages(
        conversationId: string,
        limit = 50,
        offset = 0
    ): Promise<OfflineMessage[]> {
        if (!this.isInitialized) {
            throw new Error('SyncEngine not initialized');
        }
        
        const messages = await this.offlineSync.getConversationMessages(conversationId, limit, offset);
        
        // 解密消息内容
        for (const message of messages) {
            if (message.encrypted && typeof message.content === 'string') {
                try {
                    const encryptedData = JSON.parse(message.content) as EncryptedMessageData;
                    const decrypted = await this.encryptionManager.decryptMessage(encryptedData);
                    
                    if (typeof decrypted === 'string') {
                        message.content = decrypted;
                    } else {
                        message.content = '[Encrypted content]';
                    }
                } catch (error) {
                    console.error('Failed to decrypt message:', error);
                    message.content = '[Decryption failed]';
                }
            }
        }
        
        return messages;
    }
    
    // 标记消息为已读
    async markMessageAsRead(messageId: string): Promise<void> {
        if (!this.isInitialized) {
            throw new Error('SyncEngine not initialized');
        }
        
        await this.offlineSync.updateMessageStatus(messageId, MessageStatus.READ);
        
        // 同步到服务器
        if (navigator.onLine) {
            await this.syncReadStatus(messageId).catch(console.error);
        }
    }
    
    // 同步已读状态
    private async syncReadStatus(messageId: string): Promise<void> {
        // 实现已读状态同步逻辑
    }
    
    // 同步待发送消息
    private async syncPendingMessages(): Promise<void> {
        // 通过离线同步管理器处理
        // 这里会触发消息的实际发送
    }
    
    // 拉取新消息
    private async pullNewMessages(): Promise<void> {
        // 从服务器拉取新消息
        // 消息会通过WebSocket处理器自动处理
    }
    
    // 同步消息状态
    private async syncMessageStatus(): Promise<void> {
        // 同步消息的发送状态
    }
    
    // 处理手动冲突解决
    private async handleManualConflictResolution(conflict: SyncConflict): Promise<'server_wins' | 'client_wins' | 'newer_wins'> {
        // 在UI中显示冲突，让用户选择
        // 这里需要集成UI组件
        
        return new Promise((resolve) => {
            // 模拟用户选择（实际需要UI交互）
            setTimeout(() => {
                resolve('server_wins');
            }, 1000);
        });
    }
    
    // 清理旧数据
    async cleanupOldData(maxAgeDays = 30): Promise<void> {
        if (!this.isInitialized) {
            throw new Error('SyncEngine not initialized');
        }
        
        await this.offlineSync.clearOldMessages(maxAgeDays);
    }
    
    // 获取同步状态
    getSyncStatus(): {
        isSyncing: boolean;
        lastSyncTime: number;
        pendingMessages: number;
        offlineStorage: number;
    } {
        // 实现状态获取逻辑
        return {
            isSyncing: this.syncQueue.size > 0,
            lastSyncTime: Date.now() - 1000 * 60, // 1分钟前
            pendingMessages: 0,
            offlineStorage: 0
        };
    }
    
    // 导出数据（用于备份）
    async exportData(): Promise<Blob> {
        if (!this.isInitialized) {
            throw new Error('SyncEngine not initialized');
        }
        
        // 实现数据导出逻辑
        const data = {
            timestamp: Date.now(),
            version: '1.0',
            messages: [] // 需要获取所有消息
        };
        
        return new Blob([JSON.stringify(data, null, 2)], {
            type: 'application/json'
        });
    }
    
    // 导入数据（从备份恢复）
    async importData(blob: Blob): Promise<void> {
        if (!this.isInitialized) {
            throw new Error('SyncEngine not initialized');
        }
        
        const text = await blob.text();
        const data = JSON.parse(text);
        
        // 验证数据格式
        if (!data.version || !data.messages) {
            throw new Error('Invalid backup format');
        }
        
        // 实现数据导入逻辑
        console.log('Importing backup data:', data);
    }
    
    // 销毁
    async destroy(): Promise<void> {
        this.isInitialized = false;
        
        await this.offlineSync.destroy();
        await this.encryptionManager.destroy();
        
        this.syncQueue.clear();
    }
}