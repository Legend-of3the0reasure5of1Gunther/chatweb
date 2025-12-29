// 文件: src/core/offline-sync.ts
/**
 * 离线消息与多设备同步模块
 * 功能：离线消息存储、消息同步、冲突解决、多设备状态一致性
 */

import { SecureChatWebSocket, MessageType } from './websocket-client';
import { EndToEndEncryption, EncryptionSession } from './security';

export interface OfflineMessage {
    id: string;
    localId?: string; // 客户端生成的临时ID
    messageId: bigint; // 服务器分配的ID
    conversationId: string; // 会话ID (user:123 或 group:456)
    senderId: bigint;
    receiverId: bigint;
    groupId?: bigint;
    type: MessageType;
    content: ArrayBuffer | string;
    encrypted: boolean;
    encryptionKeyId?: string;
    timestamp: number;
    localTimestamp: number;
    status: MessageStatus;
    sendAttempts: number;
    lastAttempt: number;
    synced: boolean;
    readBy: bigint[]; // 已读用户列表
    deliveredTo: bigint[]; // 已送达用户列表
    metadata: Record<string, any>;
}

export enum MessageStatus {
    LOCAL_DRAFT = 'local_draft',     // 本地草稿
    SENDING = 'sending',             // 发送中
    SENT = 'sent',                   // 已发送到服务器
    DELIVERED = 'delivered',         // 已送达对方设备
    READ = 'read',                   // 已读
    FAILED = 'failed',               // 发送失败
    PENDING_RETRY = 'pending_retry', // 等待重试
    CONFLICT = 'conflict'            // 冲突状态
}

export interface SyncSession {
    sessionId: string;
    deviceId: string;
    userId: bigint;
    lastSyncTime: number;
    syncToken: string;
    pendingOperations: number;
    isSyncing: boolean;
    conflictResolution: ConflictResolutionStrategy;
}

export enum ConflictResolutionStrategy {
    SERVER_WINS = 'server_wins',
    CLIENT_WINS = 'client_wins',
    NEWER_WINS = 'newer_wins',
    MANUAL = 'manual'
}

export interface SyncOperation {
    operationId: string;
    type: 'create' | 'update' | 'delete';
    entityType: 'message' | 'user' | 'group' | 'read_status';
    entityId: string;
    data: any;
    timestamp: number;
    deviceId: string;
    resolved: boolean;
    retryCount: number;
}

export interface DeviceInfo {
    deviceId: string;
    deviceName: string;
    deviceType: string; // 'web', 'mobile', 'desktop'
    platform: string;
    lastSeen: number;
    syncEnabled: boolean;
    encryptionSupported: boolean;
    publicKey?: string;
}

export class OfflineSyncManager {
    private static readonly DB_NAME = 'SecureChatOfflineDB';
    private static readonly DB_VERSION = 3;
    private static readonly MESSAGE_STORE = 'messages';
    private static readonly OPERATION_STORE = 'operations';
    private static readonly DEVICE_STORE = 'devices';
    private static readonly SYNC_STORE = 'sync_sessions';
    
    private webSocket: SecureChatWebSocket;
    private db: IDBDatabase | null = null;
    private syncSession: SyncSession | null = null;
    private deviceInfo: DeviceInfo;
    private isInitialized = false;
    private syncInterval: NodeJS.Timeout | null = null;
    private syncInProgress = false;
    private pendingOperations: Map<string, SyncOperation> = new Map();
    
    // 事件回调
    private messageStatusCallbacks: Map<string, (message: OfflineMessage) => void> = new Map();
    private syncProgressCallbacks: ((progress: SyncProgress) => void)[] = [];
    private conflictCallbacks: ((conflict: SyncConflict) => void)[] = [];
    
    constructor(webSocket: SecureChatWebSocket) {
        this.webSocket = webSocket;
        this.deviceInfo = this.generateDeviceInfo();
        
        // 注册WebSocket消息处理器
        this.registerMessageHandlers();
    }
    
    // 初始化离线数据库
    async initialize(): Promise<void> {
        if (this.isInitialized) {
            return;
        }
        
        return new Promise((resolve, reject) => {
            const request = indexedDB.open(
                OfflineSyncManager.DB_NAME,
                OfflineSyncManager.DB_VERSION
            );
            
            request.onerror = () => {
                reject(new Error('Failed to open IndexedDB'));
            };
            
            request.onsuccess = (event) => {
                this.db = (event.target as IDBOpenDBRequest).result;
                this.isInitialized = true;
                this.setupAutoSync();
                resolve();
            };
            
            request.onupgradeneeded = (event) => {
                const db = (event.target as IDBOpenDBRequest).result;
                
                // 创建消息存储
                if (!db.objectStoreNames.contains(OfflineSyncManager.MESSAGE_STORE)) {
                    const messageStore = db.createObjectStore(
                        OfflineSyncManager.MESSAGE_STORE,
                        { keyPath: 'id' }
                    );
                    
                    // 创建索引
                    messageStore.createIndex('conversationId', 'conversationId', { unique: false });
                    messageStore.createIndex('timestamp', 'timestamp', { unique: false });
                    messageStore.createIndex('status', 'status', { unique: false });
                    messageStore.createIndex('synced', 'synced', { unique: false });
                    messageStore.createIndex('localId', 'localId', { unique: true });
                }
                
                // 创建操作存储
                if (!db.objectStoreNames.contains(OfflineSyncManager.OPERATION_STORE)) {
                    const operationStore = db.createObjectStore(
                        OfflineSyncManager.OPERATION_STORE,
                        { keyPath: 'operationId' }
                    );
                    
                    operationStore.createIndex('timestamp', 'timestamp', { unique: false });
                    operationStore.createIndex('resolved', 'resolved', { unique: false });
                }
                
                // 创建设备存储
                if (!db.objectStoreNames.contains(OfflineSyncManager.DEVICE_STORE)) {
                    const deviceStore = db.createObjectStore(
                        OfflineSyncManager.DEVICE_STORE,
                        { keyPath: 'deviceId' }
                    );
                    
                    deviceStore.createIndex('lastSeen', 'lastSeen', { unique: false });
                }
                
                // 创建同步会话存储
                if (!db.objectStoreNames.contains(OfflineSyncManager.SYNC_STORE)) {
                    db.createObjectStore(
                        OfflineSyncManager.SYNC_STORE,
                        { keyPath: 'sessionId' }
                    );
                }
            };
        });
    }
    
    // 生成设备信息
    private generateDeviceInfo(): DeviceInfo {
        const deviceId = this.getDeviceId();
        
        return {
            deviceId,
            deviceName: this.getDeviceName(),
            deviceType: 'web',
            platform: navigator.platform,
            lastSeen: Date.now(),
            syncEnabled: true,
            encryptionSupported: true,
            publicKey: undefined
        };
    }
    
    // 获取或生成设备ID
    private getDeviceId(): string {
        let deviceId = localStorage.getItem('secure_chat_device_id');
        
        if (!deviceId) {
            deviceId = `web_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`;
            localStorage.setItem('secure_chat_device_id', deviceId);
        }
        
        return deviceId;
    }
    
    // 获取设备名称
    private getDeviceName(): string {
        const savedName = localStorage.getItem('secure_chat_device_name');
        if (savedName) {
            return savedName;
        }
        
        // 尝试从浏览器获取设备信息
        const userAgent = navigator.userAgent;
        let deviceName = 'Web Browser';
        
        if (userAgent.includes('Windows')) {
            deviceName = 'Windows PC';
        } else if (userAgent.includes('Mac')) {
            deviceName = 'Mac';
        } else if (userAgent.includes('Linux')) {
            deviceName = 'Linux PC';
        } else if (userAgent.includes('Android')) {
            deviceName = 'Android Device';
        } else if (userAgent.includes('iPhone') || userAgent.includes('iPad')) {
            deviceName = 'iOS Device';
        }
        
        localStorage.setItem('secure_chat_device_name', deviceName);
        return deviceName;
    }
    
    // 设置自动同步
    private setupAutoSync(): void {
        // 每分钟检查一次同步
        this.syncInterval = setInterval(() => {
            if (!this.syncInProgress) {
                this.autoSync().catch(console.error);
            }
        }, 60 * 1000); // 1分钟
        
        // 监听网络状态变化
        window.addEventListener('online', () => {
            console.log('Network online, triggering sync');
            this.autoSync().catch(console.error);
        });
        
        window.addEventListener('offline', () => {
            console.log('Network offline');
        });
    }
    
    // 自动同步
    private async autoSync(): Promise<void> {
        if (!navigator.onLine) {
            return;
        }
        
        if (this.syncInProgress) {
            console.log('Sync already in progress');
            return;
        }
        
        try {
            this.syncInProgress = true;
            
            // 1. 同步待发送消息
            await this.syncPendingMessages();
            
            // 2. 同步操作记录
            await this.syncOperations();
            
            // 3. 同步消息状态
            await this.syncMessageStatus();
            
            // 4. 拉取服务器消息
            await this.pullServerMessages();
            
            // 5. 更新设备信息
            await this.updateDeviceInfo();
            
            console.log('Auto sync completed');
            
        } catch (error) {
            console.error('Auto sync failed:', error);
        } finally {
            this.syncInProgress = false;
        }
    }
    
    // 同步待发送消息
    private async syncPendingMessages(): Promise<void> {
        const pendingMessages = await this.getPendingMessages();
        
        for (const message of pendingMessages) {
            if (message.sendAttempts >= 3) {
                // 超过重试次数，标记为失败
                await this.updateMessageStatus(message.id, MessageStatus.FAILED);
                continue;
            }
            
            try {
                // 发送消息到服务器
                await this.sendMessageToServer(message);
                
                // 更新消息状态
                await this.updateMessageStatus(message.id, MessageStatus.SENT, {
                    sendAttempts: message.sendAttempts + 1,
                    lastAttempt: Date.now()
                });
                
            } catch (error) {
                console.error(`Failed to send message ${message.id}:`, error);
                
                // 更新重试信息
                await this.updateMessageStatus(message.id, MessageStatus.PENDING_RETRY, {
                    sendAttempts: message.sendAttempts + 1,
                    lastAttempt: Date.now()
                });
            }
        }
    }
    
    // 发送消息到服务器
    private async sendMessageToServer(message: OfflineMessage): Promise<void> {
        // 根据消息类型发送到服务器
        switch (message.type) {
            case MessageType.MSG_CHAT_TEXT_SEND:
                await this.sendTextMessage(message);
                break;
            case MessageType.MSG_CHAT_FILE_SEND:
                await this.sendFileMessage(message);
                break;
            case MessageType.MSG_GROUP_MESSAGE_SEND:
                await this.sendGroupMessage(message);
                break;
            default:
                throw new Error(`Unsupported message type: ${message.type}`);
        }
    }
    
    // 发送文本消息
    private async sendTextMessage(message: OfflineMessage): Promise<void> {
        let content: ArrayBuffer;
        
        if (typeof message.content === 'string') {
            const encoder = new TextEncoder();
            content = encoder.encode(message.content).buffer;
        } else {
            content = message.content;
        }
        
        // 准备消息数据
        const data = this.prepareTextMessageData(message, content);
        
        // 发送到服务器
        await this.webSocket.sendMessage(MessageType.MSG_CHAT_TEXT_SEND, data, true);
    }
    
    // 准备文本消息数据
    private prepareTextMessageData(message: OfflineMessage, content: ArrayBuffer): ArrayBuffer {
        const view = new DataView(new ArrayBuffer(8 + 8 + 8 + 8 + 4 + 8 + 1 + 64 + 4 + content.byteLength));
        let offset = 0;
        
        // 消息ID（服务器填充）
        view.setBigUint64(offset, 0n, false); offset += 8;
        
        // 发送者ID（服务器填充）
        view.setBigUint64(offset, 0n, false); offset += 8;
        
        // 接收者ID
        view.setBigUint64(offset, message.receiverId, false); offset += 8;
        
        // 时间戳
        view.setBigUint64(offset, BigInt(message.timestamp), false); offset += 8;
        
        // 消息类型（0=文本）
        view.setUint32(offset, 0, false); offset += 4;
        
        // 回复的消息ID
        view.setBigUint64(offset, 0n, false); offset += 8;
        
        // 是否加密
        view.setUint8(offset, message.encrypted ? 1 : 0); offset += 1;
        
        // 内容哈希
        const hashArray = new Uint8Array(view.buffer, offset, 64);
        hashArray.fill(0); // TODO: 计算实际哈希
        offset += 64;
        
        // 内容长度
        view.setUint32(offset, content.byteLength, false); offset += 4;
        
        // 内容
        const contentArray = new Uint8Array(view.buffer, offset, content.byteLength);
        contentArray.set(new Uint8Array(content));
        
        return view.buffer;
    }
    
    // 同步操作记录
    private async syncOperations(): Promise<void> {
        const pendingOperations = await this.getPendingOperations();
        
        for (const operation of pendingOperations) {
            if (operation.retryCount >= 3) {
                // 标记为需要手动解决
                await this.markOperationForManualResolution(operation);
                continue;
            }
            
            try {
                await this.executeOperation(operation);
                await this.markOperationAsResolved(operation.operationId);
            } catch (error) {
                console.error(`Operation ${operation.operationId} failed:`, error);
                await this.incrementOperationRetry(operation.operationId);
            }
        }
    }
    
    // 执行同步操作
    private async executeOperation(operation: SyncOperation): Promise<void> {
        switch (operation.entityType) {
            case 'message':
                await this.executeMessageOperation(operation);
                break;
            case 'read_status':
                await this.executeReadStatusOperation(operation);
                break;
            // 其他操作类型...
        }
    }
    
    // 执行消息操作
    private async executeMessageOperation(operation: SyncOperation): Promise<void> {
        // 将消息操作同步到服务器
        // 这里需要实现具体的服务器API调用
        console.log(`Executing message operation: ${operation.operationId}`);
    }
    
    // 同步消息状态
    private async syncMessageStatus(): Promise<void> {
        // 获取需要同步状态的消息
        const messages = await this.getMessagesByStatus([MessageStatus.SENT, MessageStatus.DELIVERED]);
        
        for (const message of messages) {
            // 向服务器查询消息状态
            const status = await this.queryMessageStatus(message.messageId);
            
            if (status) {
                // 更新本地状态
                await this.updateMessageStatus(message.id, status);
            }
        }
    }
    
    // 查询消息状态
    private async queryMessageStatus(messageId: bigint): Promise<MessageStatus | null> {
        try {
            // 发送查询请求到服务器
            const data = new ArrayBuffer(8);
            const view = new DataView(data);
            view.setBigUint64(0, messageId, false);
            
            const response = await this.webSocket.sendMessage(
                MessageType.MSG_CHAT_TEXT_ACK,
                data,
                true
            );
            
            if (response) {
                const responseView = new DataView(response);
                const statusCode = responseView.getUint32(0, false);
                
                // 根据状态码返回对应的MessageStatus
                switch (statusCode) {
                    case 1: return MessageStatus.SENT;
                    case 2: return MessageStatus.DELIVERED;
                    case 3: return MessageStatus.READ;
                    default: return null;
                }
            }
        } catch (error) {
            console.error('Failed to query message status:', error);
        }
        
        return null;
    }
    
    // 拉取服务器消息
    private async pullServerMessages(): Promise<void> {
        if (!this.syncSession) {
            return;
        }
        
        try {
            // 获取最后同步时间
            const lastSyncTime = this.syncSession.lastSyncTime;
            
            // 请求服务器上的新消息
            const newMessages = await this.fetchNewMessagesFromServer(lastSyncTime);
            
            // 处理新消息
            for (const message of newMessages) {
                await this.processIncomingMessage(message);
            }
            
            // 更新同步时间
            await this.updateSyncSession({
                lastSyncTime: Date.now()
            });
            
        } catch (error) {
            console.error('Failed to pull server messages:', error);
        }
    }
    
    // 从服务器获取新消息
    private async fetchNewMessagesFromServer(lastSyncTime: number): Promise<any[]> {
        // 这里需要实现与服务器的消息拉取协议
        // 暂时返回空数组
        return [];
    }
    
    // 处理收到的消息
    private async processIncomingMessage(messageData: any): Promise<void> {
        // 转换消息格式
        const message: OfflineMessage = {
            id: this.generateMessageId(),
            messageId: BigInt(messageData.messageId),
            conversationId: this.getConversationId(messageData),
            senderId: BigInt(messageData.senderId),
            receiverId: BigInt(messageData.receiverId),
            groupId: messageData.groupId ? BigInt(messageData.groupId) : undefined,
            type: messageData.type,
            content: messageData.content,
            encrypted: messageData.encrypted || false,
            timestamp: messageData.timestamp,
            localTimestamp: Date.now(),
            status: MessageStatus.DELIVERED,
            sendAttempts: 0,
            lastAttempt: 0,
            synced: true,
            readBy: [],
            deliveredTo: [this.deviceInfo.deviceId as any], // 类型转换，实际需要调整
            metadata: messageData.metadata || {}
        };
        
        // 保存到本地数据库
        await this.saveMessage(message);
        
        // 触发消息接收回调
        this.triggerMessageReceived(message);
    }
    
    // 更新设备信息
    private async updateDeviceInfo(): Promise<void> {
        this.deviceInfo.lastSeen = Date.now();
        await this.saveDeviceInfo(this.deviceInfo);
        
        // 同步到服务器
        await this.syncDeviceInfoToServer();
    }
    
    // 同步设备信息到服务器
    private async syncDeviceInfoToServer(): Promise<void> {
        // 实现设备信息同步逻辑
    }
    
    // 注册WebSocket消息处理器
    private registerMessageHandlers(): void {
        // 文本消息接收
        this.webSocket.registerMessageCallback(
            MessageType.MSG_CHAT_TEXT_RECEIVE,
            (type, data) => this.handleIncomingMessage(type, data)
        );
        
        // 消息确认
        this.webSocket.registerMessageCallback(
            MessageType.MSG_CHAT_TEXT_ACK,
            (type, data) => this.handleMessageAck(type, data)
        );
        
        // 已读回执
        this.webSocket.registerMessageCallback(
            MessageType.MSG_CHAT_READ_RECEIPT,
            (type, data) => this.handleReadReceipt(type, data)
        );
    }
    
    // 处理收到的消息
    private handleIncomingMessage(type: MessageType, data: ArrayBuffer): void {
        try {
            const message = this.parseIncomingMessage(data);
            this.processIncomingMessage(message).catch(console.error);
        } catch (error) {
            console.error('Failed to handle incoming message:', error);
        }
    }
    
    // 解析收到的消息
    private parseIncomingMessage(data: ArrayBuffer): any {
        const view = new DataView(data);
        let offset = 0;
        
        const messageId = view.getBigUint64(offset, false); offset += 8;
        const senderId = view.getBigUint64(offset, false); offset += 8;
        const receiverId = view.getBigUint64(offset, false); offset += 8;
        const timestamp = view.getBigUint64(offset, false); offset += 8;
        const messageType = view.getUint32(offset, false); offset += 4;
        const replyToId = view.getBigUint64(offset, false); offset += 8;
        const encrypted = view.getUint8(offset) !== 0; offset += 1;
        
        // 内容哈希
        const hashArray = new Uint8Array(data, offset, 64);
        const contentHash = Array.from(hashArray).map(b => b.toString(16).padStart(2, '0')).join('');
        offset += 64;
        
        const contentLength = view.getUint32(offset, false); offset += 4;
        const content = data.slice(offset, offset + contentLength);
        
        return {
            messageId: Number(messageId),
            senderId: Number(senderId),
            receiverId: Number(receiverId),
            timestamp: Number(timestamp),
            type: messageType,
            replyToId: Number(replyToId),
            encrypted,
            contentHash,
            content
        };
    }
    
    // 处理消息确认
    private handleMessageAck(type: MessageType, data: ArrayBuffer): void {
        const view = new DataView(data);
        const messageId = view.getBigUint64(0, false);
        const statusCode = view.getUint32(8, false);
        
        // 更新消息状态
        this.updateMessageStatusByServerId(messageId, statusCode).catch(console.error);
    }
    
    // 根据服务器消息ID更新状态
    private async updateMessageStatusByServerId(messageId: bigint, statusCode: number): Promise<void> {
        // 查找对应的本地消息
        const message = await this.getMessageByServerId(messageId);
        if (!message) {
            return;
        }
        
        let status: MessageStatus;
        switch (statusCode) {
            case 1: status = MessageStatus.SENT; break;
            case 2: status = MessageStatus.DELIVERED; break;
            case 3: status = MessageStatus.READ; break;
            default: return;
        }
        
        // 更新状态
        await this.updateMessageStatus(message.id, status);
    }
    
    // 处理已读回执
    private handleReadReceipt(type: MessageType, data: ArrayBuffer): void {
        const view = new DataView(data);
        const messageId = view.getBigUint64(0, false);
        const readerId = view.getBigUint64(8, false);
        const timestamp = view.getBigUint64(16, false);
        
        // 更新消息的已读状态
        this.markMessageAsRead(messageId, readerId, Number(timestamp)).catch(console.error);
    }
    
    // 标记消息为已读
    private async markMessageAsRead(messageId: bigint, readerId: bigint, timestamp: number): Promise<void> {
        const message = await this.getMessageByServerId(messageId);
        if (!message) {
            return;
        }
        
        // 添加已读者
        if (!message.readBy.includes(readerId)) {
            message.readBy.push(readerId);
        }
        
        // 更新状态
        message.status = MessageStatus.READ;
        message.timestamp = timestamp;
        
        // 保存更新
        await this.saveMessage(message);
    }
    
    // 数据库操作方法
    private async saveMessage(message: OfflineMessage): Promise<void> {
        return new Promise((resolve, reject) => {
            if (!this.db) {
                reject(new Error('Database not initialized'));
                return;
            }
            
            const transaction = this.db.transaction(
                [OfflineSyncManager.MESSAGE_STORE],
                'readwrite'
            );
            
            const store = transaction.objectStore(OfflineSyncManager.MESSAGE_STORE);
            const request = store.put(message);
            
            request.onerror = () => {
                reject(new Error('Failed to save message'));
            };
            
            request.onsuccess = () => {
                resolve();
            };
        });
    }
    
    private async getMessage(id: string): Promise<OfflineMessage | null> {
        return new Promise((resolve, reject) => {
            if (!this.db) {
                reject(new Error('Database not initialized'));
                return;
            }
            
            const transaction = this.db.transaction(
                [OfflineSyncManager.MESSAGE_STORE],
                'readonly'
            );
            
            const store = transaction.objectStore(OfflineSyncManager.MESSAGE_STORE);
            const request = store.get(id);
            
            request.onerror = () => {
                reject(new Error('Failed to get message'));
            };
            
            request.onsuccess = () => {
                resolve(request.result || null);
            };
        });
    }
    
    private async getMessageByServerId(messageId: bigint): Promise<OfflineMessage | null> {
        return new Promise((resolve, reject) => {
            if (!this.db) {
                reject(new Error('Database not initialized'));
                return;
            }
            
            const transaction = this.db.transaction(
                [OfflineSyncManager.MESSAGE_STORE],
                'readonly'
            );
            
            const store = transaction.objectStore(OfflineSyncManager.MESSAGE_STORE);
            const index = store.index('messageId'); // 需要创建这个索引
            
            const request = index.get(messageId);
            
            request.onerror = () => {
                reject(new Error('Failed to get message by server ID'));
            };
            
            request.onsuccess = () => {
                resolve(request.result || null);
            };
        });
    }
    
    private async getPendingMessages(): Promise<OfflineMessage[]> {
        return new Promise((resolve, reject) => {
            if (!this.db) {
                reject(new Error('Database not initialized'));
                return;
            }
            
            const transaction = this.db.transaction(
                [OfflineSyncManager.MESSAGE_STORE],
                'readonly'
            );
            
            const store = transaction.objectStore(OfflineSyncManager.MESSAGE_STORE);
            const statusIndex = store.index('status');
            
            const request = statusIndex.getAll([
                MessageStatus.SENDING,
                MessageStatus.PENDING_RETRY
            ]);
            
            request.onerror = () => {
                reject(new Error('Failed to get pending messages'));
            };
            
            request.onsuccess = () => {
                resolve(request.result || []);
            };
        });
    }
    
    private async getMessagesByStatus(statuses: MessageStatus[]): Promise<OfflineMessage[]> {
        return new Promise((resolve, reject) => {
            if (!this.db) {
                reject(new Error('Database not initialized'));
                return;
            }
            
            const transaction = this.db.transaction(
                [OfflineSyncManager.MESSAGE_STORE],
                'readonly'
            );
            
            const store = transaction.objectStore(OfflineSyncManager.MESSAGE_STORE);
            const statusIndex = store.index('status');
            
            const results: OfflineMessage[] = [];
            let completed = 0;
            
            statuses.forEach(status => {
                const request = statusIndex.getAll(status);
                
                request.onsuccess = () => {
                    if (request.result) {
                        results.push(...request.result);
                    }
                    
                    completed++;
                    if (completed === statuses.length) {
                        resolve(results);
                    }
                };
                
                request.onerror = () => {
                    completed++;
                    if (completed === statuses.length) {
                        resolve(results);
                    }
                };
            });
        });
    }
    
    private async updateMessageStatus(
        messageId: string, 
        status: MessageStatus, 
        updates: Partial<OfflineMessage> = {}
    ): Promise<void> {
        const message = await this.getMessage(messageId);
        if (!message) {
            return;
        }
        
        message.status = status;
        Object.assign(message, updates);
        
        if (status === MessageStatus.SENT && !message.synced) {
            message.synced = true;
        }
        
        await this.saveMessage(message);
        
        // 触发状态更新回调
        this.triggerMessageStatusUpdate(message);
    }
    
    private async saveDeviceInfo(deviceInfo: DeviceInfo): Promise<void> {
        return new Promise((resolve, reject) => {
            if (!this.db) {
                reject(new Error('Database not initialized'));
                return;
            }
            
            const transaction = this.db.transaction(
                [OfflineSyncManager.DEVICE_STORE],
                'readwrite'
            );
            
            const store = transaction.objectStore(OfflineSyncManager.DEVICE_STORE);
            const request = store.put(deviceInfo);
            
            request.onerror = () => {
                reject(new Error('Failed to save device info'));
            };
            
            request.onsuccess = () => {
                resolve();
            };
        });
    }
    
    private async getPendingOperations(): Promise<SyncOperation[]> {
        return new Promise((resolve, reject) => {
            if (!this.db) {
                reject(new Error('Database not initialized'));
                return;
            }
            
            const transaction = this.db.transaction(
                [OfflineSyncManager.OPERATION_STORE],
                'readonly'
            );
            
            const store = transaction.objectStore(OfflineSyncManager.OPERATION_STORE);
            const resolvedIndex = store.index('resolved');
            
            const request = resolvedIndex.getAll(false); // 获取未解决的操作
            
            request.onerror = () => {
                reject(new Error('Failed to get pending operations'));
            };
            
            request.onsuccess = () => {
                resolve(request.result || []);
            };
        });
    }
    
    private async markOperationAsResolved(operationId: string): Promise<void> {
        return new Promise((resolve, reject) => {
            if (!this.db) {
                reject(new Error('Database not initialized'));
                return;
            }
            
            const transaction = this.db.transaction(
                [OfflineSyncManager.OPERATION_STORE],
                'readwrite'
            );
            
            const store = transaction.objectStore(OfflineSyncManager.OPERATION_STORE);
            const getRequest = store.get(operationId);
            
            getRequest.onsuccess = () => {
                const operation = getRequest.result;
                if (operation) {
                    operation.resolved = true;
                    const putRequest = store.put(operation);
                    
                    putRequest.onerror = () => {
                        reject(new Error('Failed to mark operation as resolved'));
                    };
                    
                    putRequest.onsuccess = () => {
                        resolve();
                    };
                } else {
                    resolve();
                }
            };
            
            getRequest.onerror = () => {
                reject(new Error('Failed to get operation'));
            };
        });
    }
    
    private async incrementOperationRetry(operationId: string): Promise<void> {
        return new Promise((resolve, reject) => {
            if (!this.db) {
                reject(new Error('Database not initialized'));
                return;
            }
            
            const transaction = this.db.transaction(
                [OfflineSyncManager.OPERATION_STORE],
                'readwrite'
            );
            
            const store = transaction.objectStore(OfflineSyncManager.OPERATION_STORE);
            const getRequest = store.get(operationId);
            
            getRequest.onsuccess = () => {
                const operation = getRequest.result;
                if (operation) {
                    operation.retryCount = (operation.retryCount || 0) + 1;
                    const putRequest = store.put(operation);
                    
                    putRequest.onerror = () => {
                        reject(new Error('Failed to increment operation retry'));
                    };
                    
                    putRequest.onsuccess = () => {
                        resolve();
                    };
                } else {
                    resolve();
                }
            };
            
            getRequest.onerror = () => {
                reject(new Error('Failed to get operation'));
            };
        });
    }
    
    private async markOperationForManualResolution(operation: SyncOperation): Promise<void> {
        // 标记操作为需要手动解决
        operation.resolved = false;
        operation.retryCount = 3;
        
        // 保存操作
        await new Promise((resolve, reject) => {
            if (!this.db) {
                reject(new Error('Database not initialized'));
                return;
            }
            
            const transaction = this.db.transaction(
                [OfflineSyncManager.OPERATION_STORE],
                'readwrite'
            );
            
            const store = transaction.objectStore(OfflineSyncManager.OPERATION_STORE);
            const request = store.put(operation);
            
            request.onerror = () => {
                reject(new Error('Failed to save operation'));
            };
            
            request.onsuccess = () => {
                resolve();
            };
        });
        
        // 触发冲突回调
        this.triggerConflict({
            type: 'operation_conflict',
            operation,
            message: `Operation ${operation.operationId} requires manual resolution`
        });
    }
    
    private async updateSyncSession(updates: Partial<SyncSession>): Promise<void> {
        if (!this.syncSession) {
            return;
        }
        
        Object.assign(this.syncSession, updates);
        
        return new Promise((resolve, reject) => {
            if (!this.db) {
                reject(new Error('Database not initialized'));
                return;
            }
            
            const transaction = this.db.transaction(
                [OfflineSyncManager.SYNC_STORE],
                'readwrite'
            );
            
            const store = transaction.objectStore(OfflineSyncManager.SYNC_STORE);
            const request = store.put(this.syncSession);
            
            request.onerror = () => {
                reject(new Error('Failed to update sync session'));
            };
            
            request.onsuccess = () => {
                resolve();
            };
        });
    }
    
    // 公共API方法
    async storeMessage(message: Omit<OfflineMessage, 'id' | 'localTimestamp' | 'status' | 'sendAttempts' | 'lastAttempt' | 'synced' | 'readBy' | 'deliveredTo'>): Promise<string> {
        const fullMessage: OfflineMessage = {
            ...message,
            id: this.generateMessageId(),
            localTimestamp: Date.now(),
            status: MessageStatus.SENDING,
            sendAttempts: 0,
            lastAttempt: 0,
            synced: false,
            readBy: [],
            deliveredTo: []
        };
        
        await this.saveMessage(fullMessage);
        
        // 立即尝试同步
        if (navigator.onLine) {
            this.autoSync().catch(console.error);
        }
        
        return fullMessage.id;
    }
    
    async getConversationMessages(conversationId: string, limit = 50, offset = 0): Promise<OfflineMessage[]> {
        return new Promise((resolve, reject) => {
            if (!this.db) {
                reject(new Error('Database not initialized'));
                return;
            }
            
            const transaction = this.db.transaction(
                [OfflineSyncManager.MESSAGE_STORE],
                'readonly'
            );
            
            const store = transaction.objectStore(OfflineSyncManager.MESSAGE_STORE);
            const conversationIndex = store.index('conversationId');
            
            const request = conversationIndex.getAll(conversationId);
            
            request.onerror = () => {
                reject(new Error('Failed to get conversation messages'));
            };
            
            request.onsuccess = () => {
                const messages = request.result || [];
                
                // 按时间戳排序（最新的在前）
                messages.sort((a, b) => b.timestamp - a.timestamp);
                
                // 分页
                const start = offset;
                const end = offset + limit;
                const paginated = messages.slice(start, end);
                
                resolve(paginated);
            };
        });
    }
    
    async getUnreadCount(conversationId?: string): Promise<number> {
        return new Promise((resolve, reject) => {
            if (!this.db) {
                reject(new Error('Database not initialized'));
                return;
            }
            
            const transaction = this.db.transaction(
                [OfflineSyncManager.MESSAGE_STORE],
                'readonly'
            );
            
            const store = transaction.objectStore(OfflineSyncManager.MESSAGE_STORE);
            
            let request: IDBRequest<OfflineMessage[]>;
            
            if (conversationId) {
                const conversationIndex = store.index('conversationId');
                const keyRange = IDBKeyRange.only(conversationId);
                request = conversationIndex.getAll(keyRange);
            } else {
                request = store.getAll();
            }
            
            request.onsuccess = () => {
                const messages = request.result || [];
                
                // 计算未读消息数（状态为DELIVERED且当前用户未读）
                const unreadCount = messages.filter(message => 
                    message.status === MessageStatus.DELIVERED && 
                    !message.readBy.includes(BigInt(this.deviceInfo.deviceId)) // 需要调整
                ).length;
                
                resolve(unreadCount);
            };
            
            request.onerror = () => {
                reject(new Error('Failed to get unread count'));
            };
        });
    }
    
    async markConversationAsRead(conversationId: string): Promise<void> {
        const messages = await this.getConversationMessages(conversationId, 1000, 0);
        const currentUserId = await this.getCurrentUserId(); // 需要实现
        
        for (const message of messages) {
            if (!message.readBy.includes(currentUserId)) {
                message.readBy.push(currentUserId);
                message.status = MessageStatus.READ;
                await this.saveMessage(message);
            }
        }
    }
    
    async deleteMessage(messageId: string): Promise<void> {
        return new Promise((resolve, reject) => {
            if (!this.db) {
                reject(new Error('Database not initialized'));
                return;
            }
            
            const transaction = this.db.transaction(
                [OfflineSyncManager.MESSAGE_STORE],
                'readwrite'
            );
            
            const store = transaction.objectStore(OfflineSyncManager.MESSAGE_STORE);
            const request = store.delete(messageId);
            
            request.onerror = () => {
                reject(new Error('Failed to delete message'));
            };
            
            request.onsuccess = () => {
                resolve();
            };
        });
    }
    
    async clearOldMessages(maxAgeDays = 30): Promise<void> {
        const cutoffTime = Date.now() - (maxAgeDays * 24 * 60 * 60 * 1000);
        
        return new Promise((resolve, reject) => {
            if (!this.db) {
                reject(new Error('Database not initialized'));
                return;
            }
            
            const transaction = this.db.transaction(
                [OfflineSyncManager.MESSAGE_STORE],
                'readwrite'
            );
            
            const store = transaction.objectStore(OfflineSyncManager.MESSAGE_STORE);
            const timestampIndex = store.index('timestamp');
            
            const keyRange = IDBKeyRange.upperBound(cutoffTime);
            const request = timestampIndex.openCursor(keyRange);
            
            request.onerror = () => {
                reject(new Error('Failed to clear old messages'));
            };
            
            request.onsuccess = (event) => {
                const cursor = (event.target as IDBRequest<IDBCursorWithValue>).result;
                if (cursor) {
                    // 保留未发送的消息
                    const message = cursor.value as OfflineMessage;
                    if (message.synced) {
                        cursor.delete();
                    }
                    cursor.continue();
                } else {
                    resolve();
                }
            };
        });
    }
    
    async forceSync(): Promise<SyncProgress> {
        if (this.syncInProgress) {
            throw new Error('Sync already in progress');
        }
        
        this.syncInProgress = true;
        
        try {
            const startTime = Date.now();
            
            // 执行完整同步
            await this.syncPendingMessages();
            await this.syncOperations();
            await this.syncMessageStatus();
            await this.pullServerMessages();
            await this.updateDeviceInfo();
            
            const endTime = Date.now();
            
            const progress: SyncProgress = {
                totalOperations: 0,
                completedOperations: 0,
                startTime,
                endTime,
                duration: endTime - startTime,
                status: 'completed',
                errors: []
            };
            
            // 触发进度回调
            this.triggerSyncProgress(progress);
            
            return progress;
            
        } catch (error) {
            const progress: SyncProgress = {
                totalOperations: 0,
                completedOperations: 0,
                startTime: Date.now(),
                endTime: Date.now(),
                duration: 0,
                status: 'failed',
                errors: [error instanceof Error ? error.message : String(error)]
            };
            
            this.triggerSyncProgress(progress);
            throw error;
            
        } finally {
            this.syncInProgress = false;
        }
    }
    
    // 事件触发器
    private triggerMessageStatusUpdate(message: OfflineMessage): void {
        this.messageStatusCallbacks.forEach(callback => {
            try {
                callback(message);
            } catch (error) {
                console.error('Error in message status callback:', error);
            }
        });
    }
    
    private triggerMessageReceived(message: OfflineMessage): void {
        // 实现消息接收事件分发
    }
    
    private triggerSyncProgress(progress: SyncProgress): void {
        this.syncProgressCallbacks.forEach(callback => {
            try {
                callback(progress);
            } catch (error) {
                console.error('Error in sync progress callback:', error);
            }
        });
    }
    
    private triggerConflict(conflict: SyncConflict): void {
        this.conflictCallbacks.forEach(callback => {
            try {
                callback(conflict);
            } catch (error) {
                console.error('Error in conflict callback:', error);
            }
        });
    }
    
    // 工具方法
    private generateMessageId(): string {
        return `msg_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`;
    }
    
    private generateOperationId(): string {
        return `op_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`;
    }
    
    private getConversationId(messageData: any): string {
        if (messageData.groupId) {
            return `group:${messageData.groupId}`;
        } else {
            return `user:${messageData.senderId}`;
        }
    }
    
    private async getCurrentUserId(): Promise<bigint> {
        // 从WebSocket客户端获取当前用户ID
        return this.webSocket.getUserId();
    }
    
    // 注册回调
    registerMessageStatusCallback(callbackId: string, callback: (message: OfflineMessage) => void): void {
        this.messageStatusCallbacks.set(callbackId, callback);
    }
    
    unregisterMessageStatusCallback(callbackId: string): void {
        this.messageStatusCallbacks.delete(callbackId);
    }
    
    registerSyncProgressCallback(callback: (progress: SyncProgress) => void): void {
        this.syncProgressCallbacks.push(callback);
    }
    
    unregisterSyncProgressCallback(callback: (progress: SyncProgress) => void): void {
        const index = this.syncProgressCallbacks.indexOf(callback);
        if (index !== -1) {
            this.syncProgressCallbacks.splice(index, 1);
        }
    }
    
    registerConflictCallback(callback: (conflict: SyncConflict) => void): void {
        this.conflictCallbacks.push(callback);
    }
    
    unregisterConflictCallback(callback: (conflict: SyncConflict) => void): void {
        const index = this.conflictCallbacks.indexOf(callback);
        if (index !== -1) {
            this.conflictCallbacks.splice(index, 1);
        }
    }
    
    // 清理
    async destroy(): Promise<void> {
        if (this.syncInterval) {
            clearInterval(this.syncInterval);
            this.syncInterval = null;
        }
        
        this.messageStatusCallbacks.clear();
        this.syncProgressCallbacks = [];
        this.conflictCallbacks = [];
        
        if (this.db) {
            this.db.close();
            this.db = null;
        }
        
        this.isInitialized = false;
    }
}

// 同步进度接口
export interface SyncProgress {
    totalOperations: number;
    completedOperations: number;
    startTime: number;
    endTime: number;
    duration: number;
    status: 'pending' | 'in_progress' | 'completed' | 'failed';
    errors: string[];
}

// 同步冲突接口
export interface SyncConflict {
    type: 'message_conflict' | 'operation_conflict' | 'device_conflict';
    operation?: SyncOperation;
    message?: OfflineMessage;
    device?: DeviceInfo;
    serverValue?: any;
    clientValue?: any;
    message: string;
    resolution?: ConflictResolutionStrategy;
}