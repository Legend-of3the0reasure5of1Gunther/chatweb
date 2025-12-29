// 文件: src/core/websocket-client.ts
/**
 * WebSocket客户端 - 与后端WebSocket服务器通信
 */

import { ProtocolCodec, MessageHeader, MessageType, MessageFlags, ErrorCode, UserInfo, MessageContent, FileMetadata } from './protocol';

export interface WebSocketConfig {
    url: string;
    reconnectAttempts: number;
    reconnectDelay: number;
    heartbeatInterval: number;
    timeout: number;
    maxMessageSize: number;
}

export interface MessageCallback {
    (messageType: MessageType, data: ArrayBuffer, header: MessageHeader): void;
}

export interface ConnectionStateCallback {
    (state: ConnectionState, error?: Error): void;
}

export enum ConnectionState {
    DISCONNECTED = 'disconnected',
    CONNECTING = 'connecting',
    CONNECTED = 'connected',
    AUTHENTICATED = 'authenticated',
    RECONNECTING = 'reconnecting',
    ERROR = 'error'
}

export class SecureChatWebSocket {
    private socket: WebSocket | null = null;
    private config: WebSocketConfig;
    private state: ConnectionState = ConnectionState.DISCONNECTED;
    private reconnectCount = 0;
    private heartbeatTimer: NodeJS.Timeout | null = null;
    private messageCallbacks: Map<MessageType, MessageCallback[]> = new Map();
    private connectionStateCallbacks: ConnectionStateCallback[] = [];
    private pendingMessages: Array<{ type: MessageType, data: ArrayBuffer, resolve: Function, reject: Function }> = [];
    private correlationMap: Map<bigint, { resolve: Function, reject: Function }> = new Map();
    private nextMessageId = 1n;
    private nextCorrelationId = 1n;
    private isAuthenticated = false;
    private userId: bigint = 0n;
    private sessionToken: string = '';
    
    constructor(config: Partial<WebSocketConfig> = {}) {
        this.config = {
            url: config.url || `ws://${window.location.hostname}:8889`,
            reconnectAttempts: config.reconnectAttempts || 5,
            reconnectDelay: config.reconnectDelay || 3000,
            heartbeatInterval: config.heartbeatInterval || 30000,
            timeout: config.timeout || 30000,
            maxMessageSize: config.maxMessageSize || 10 * 1024 * 1024 // 10MB
        };
    }
    
    // 连接服务器
    async connect(): Promise<void> {
        return new Promise((resolve, reject) => {
            if (this.state === ConnectionState.CONNECTED || 
                this.state === ConnectionState.CONNECTING) {
                reject(new Error('Already connected or connecting'));
                return;
            }
            
            this.setState(ConnectionState.CONNECTING);
            
            try {
                this.socket = new WebSocket(this.config.url);
                
                this.socket.binaryType = 'arraybuffer';
                
                this.socket.onopen = () => {
                    console.log('WebSocket connected');
                    this.setState(ConnectionState.CONNECTED);
                    this.startHeartbeat();
                    this.flushPendingMessages();
                    resolve();
                };
                
                this.socket.onmessage = (event) => {
                    this.handleMessage(event.data);
                };
                
                this.socket.onerror = (event) => {
                    console.error('WebSocket error:', event);
                    this.setState(ConnectionState.ERROR, new Error('WebSocket error'));
                    reject(new Error('WebSocket connection error'));
                };
                
                this.socket.onclose = (event) => {
                    console.log(`WebSocket closed: ${event.code} ${event.reason}`);
                    this.stopHeartbeat();
                    
                    if (event.code === 1000 || event.code === 1001) {
                        // 正常关闭
                        this.setState(ConnectionState.DISCONNECTED);
                    } else if (this.reconnectCount < this.config.reconnectAttempts) {
                        this.reconnect();
                    } else {
                        this.setState(ConnectionState.DISCONNECTED, 
                            new Error(`Connection closed: ${event.reason || 'Unknown reason'}`));
                    }
                };
                
                // 设置连接超时
                setTimeout(() => {
                    if (this.state === ConnectionState.CONNECTING) {
                        this.socket?.close();
                        reject(new Error('Connection timeout'));
                    }
                }, this.config.timeout);
                
            } catch (error) {
                this.setState(ConnectionState.ERROR, error as Error);
                reject(error);
            }
        });
    }
    
    // 断开连接
    disconnect(): void {
        this.stopHeartbeat();
        this.pendingMessages = [];
        this.correlationMap.clear();
        
        if (this.socket) {
            this.socket.close(1000, 'Client disconnect');
            this.socket = null;
        }
        
        this.setState(ConnectionState.DISCONNECTED);
    }
    
    // 重新连接
    private reconnect(): void {
        this.reconnectCount++;
        this.setState(ConnectionState.RECONNECTING);
        
        setTimeout(() => {
            console.log(`Reconnecting (attempt ${this.reconnectCount}/${this.config.reconnectAttempts})`);
            this.connect().catch(() => {
                // 重试失败，继续重试直到达到最大次数
                if (this.reconnectCount < this.config.reconnectAttempts) {
                    this.reconnect();
                }
            });
        }, this.config.reconnectDelay);
    }
    
    // 发送消息
    async sendMessage(messageType: MessageType, data: ArrayBuffer, expectResponse = false): Promise<ArrayBuffer | null> {
        return new Promise((resolve, reject) => {
            if (!this.socket || this.state !== ConnectionState.CONNECTED) {
                // 添加到待发送队列
                this.pendingMessages.push({ type: messageType, data, resolve, reject });
                return;
            }
            
            const messageId = this.nextMessageId++;
            const correlationId = expectResponse ? this.nextCorrelationId++ : 0n;
            
            const header: MessageHeader = {
                magic: 0x53435400,
                version: 0x030000, // 3.0.0
                type: messageType,
                flags: 0,
                timestamp: BigInt(Date.now()),
                messageId,
                correlationId,
                bodyLength: data.byteLength,
                checksum: ProtocolCodec.calculateChecksum(data)
            };
            
            const headerBuffer = ProtocolCodec.serializeHeader(header);
            
            // 合并头部和主体
            const messageBuffer = new Uint8Array(headerBuffer.byteLength + data.byteLength);
            messageBuffer.set(new Uint8Array(headerBuffer), 0);
            messageBuffer.set(new Uint8Array(data), headerBuffer.byteLength);
            
            try {
                this.socket.send(messageBuffer);
                
                if (expectResponse && correlationId !== 0n) {
                    // 等待响应
                    this.correlationMap.set(correlationId, { resolve, reject });
                    
                    // 设置响应超时
                    setTimeout(() => {
                        if (this.correlationMap.has(correlationId)) {
                            this.correlationMap.delete(correlationId);
                            reject(new Error('Response timeout'));
                        }
                    }, this.config.timeout);
                } else {
                    resolve(null);
                }
            } catch (error) {
                reject(error);
            }
        });
    }
    
    // 处理接收到的消息
    private handleMessage(rawData: ArrayBuffer): void {
        try {
            const header = ProtocolCodec.deserializeHeader(rawData.slice(0, 64));
            
            if (!header || !ProtocolCodec.validateHeader(header)) {
                console.error('Invalid message header');
                return;
            }
            
            // 验证校验和
            const body = rawData.slice(64, 64 + header.bodyLength);
            const calculatedChecksum = ProtocolCodec.calculateChecksum(body);
            
            if (calculatedChecksum !== header.checksum) {
                console.error('Checksum mismatch');
                return;
            }
            
            // 处理心跳响应
            if (header.type === MessageType.MSG_SYSTEM_HEARTBEAT) {
                // 心跳确认，不需要进一步处理
                return;
            }
            
            // 处理响应消息
            if (header.flags & MessageFlags.FLAG_RESPONSE) {
                const handler = this.correlationMap.get(header.correlationId);
                if (handler) {
                    handler.resolve(body);
                    this.correlationMap.delete(header.correlationId);
                }
                return;
            }
            
            // 调用消息回调
            const callbacks = this.messageCallbacks.get(header.type);
            if (callbacks) {
                callbacks.forEach(callback => callback(header.type, body, header));
            }
            
            // 处理特定消息类型
            switch (header.type) {
                case MessageType.MSG_AUTH_LOGIN_RESPONSE:
                    this.handleLoginResponse(body);
                    break;
                case MessageType.MSG_CHAT_TEXT_RECEIVE:
                    this.handleChatMessage(body);
                    break;
                case MessageType.MSG_FILE_PROGRESS_UPDATE:
                    this.handleFileProgress(body);
                    break;
                // 添加其他消息类型的处理
            }
            
        } catch (error) {
            console.error('Error handling message:', error);
        }
    }
    
    // 处理登录响应
    private handleLoginResponse(data: ArrayBuffer): void {
        const view = new DataView(data);
        const statusCode = view.getUint32(0, false);
        const errorCode = view.getUint32(4, false);
        
        if (statusCode === 200 && errorCode === ErrorCode.ERR_SUCCESS) {
            this.isAuthenticated = true;
            this.userId = view.getBigUint64(16, false);
            
            // 提取会话令牌
            const tokenLength = view.getUint32(24, false);
            const tokenData = data.slice(28, 28 + tokenLength);
            const decoder = new TextDecoder('utf-8');
            this.sessionToken = decoder.decode(tokenData);
            
            this.setState(ConnectionState.AUTHENTICATED);
        }
    }
    
    // 处理聊天消息
    private handleChatMessage(data: ArrayBuffer): void {
        // TODO: 实现聊天消息处理
        console.log('Chat message received');
    }
    
    // 处理文件传输进度
    private handleFileProgress(data: ArrayBuffer): void {
        // TODO: 实现文件传输进度处理
        console.log('File progress update received');
    }
    
    // 启动心跳
    private startHeartbeat(): void {
        this.heartbeatTimer = setInterval(() => {
            if (this.socket && this.socket.readyState === WebSocket.OPEN) {
                this.sendHeartbeat();
            }
        }, this.config.heartbeatInterval);
    }
    
    // 停止心跳
    private stopHeartbeat(): void {
        if (this.heartbeatTimer) {
            clearInterval(this.heartbeatTimer);
            this.heartbeatTimer = null;
        }
    }
    
    // 发送心跳
    private sendHeartbeat(): void {
        const data = new ArrayBuffer(0);
        this.sendMessage(MessageType.MSG_SYSTEM_HEARTBEAT, data).catch(error => {
            console.error('Failed to send heartbeat:', error);
        });
    }
    
    // 设置连接状态
    private setState(state: ConnectionState, error?: Error): void {
        const oldState = this.state;
        this.state = state;
        
        // 重置重连计数
        if (state === ConnectionState.CONNECTED) {
            this.reconnectCount = 0;
        }
        
        // 调用状态回调
        this.connectionStateCallbacks.forEach(callback => {
            callback(state, error);
        });
        
        console.log(`Connection state changed: ${oldState} -> ${state}`);
    }
    
    // 刷新待发送消息
    private flushPendingMessages(): void {
        const messages = [...this.pendingMessages];
        this.pendingMessages = [];
        
        messages.forEach(({ type, data, resolve, reject }) => {
            this.sendMessage(type, data).then(resolve).catch(reject);
        });
    }
    
    // 注册消息回调
    registerMessageCallback(messageType: MessageType, callback: MessageCallback): void {
        if (!this.messageCallbacks.has(messageType)) {
            this.messageCallbacks.set(messageType, []);
        }
        this.messageCallbacks.get(messageType)!.push(callback);
    }
    
    // 注销消息回调
    unregisterMessageCallback(messageType: MessageType, callback: MessageCallback): void {
        const callbacks = this.messageCallbacks.get(messageType);
        if (callbacks) {
            const index = callbacks.indexOf(callback);
            if (index !== -1) {
                callbacks.splice(index, 1);
            }
        }
    }
    
    // 注册连接状态回调
    registerConnectionStateCallback(callback: ConnectionStateCallback): void {
        this.connectionStateCallbacks.push(callback);
    }
    
    // 注销连接状态回调
    unregisterConnectionStateCallback(callback: ConnectionStateCallback): void {
        const index = this.connectionStateCallbacks.indexOf(callback);
        if (index !== -1) {
            this.connectionStateCallbacks.splice(index, 1);
        }
    }
    
    // 用户认证
    async login(username: string, password: string): Promise<boolean> {
        const encoder = new TextEncoder();
        const usernameBuffer = encoder.encode(username);
        const passwordBuffer = encoder.encode(password);
        
        const data = new ArrayBuffer(128 + 128 + 16);
        const view = new DataView(data);
        
        // 用户名（128字节）
        const usernameArray = new Uint8Array(data, 0, 128);
        usernameArray.set(usernameBuffer);
        usernameArray[Math.min(usernameBuffer.length, 127)] = 0;
        
        // 密码（128字节）
        const passwordArray = new Uint8Array(data, 128, 128);
        passwordArray.set(passwordBuffer);
        passwordArray[Math.min(passwordBuffer.length, 127)] = 0;
        
        // 时间戳
        view.setBigUint64(256, BigInt(Date.now()), false);
        
        try {
            const response = await this.sendMessage(MessageType.MSG_AUTH_LOGIN, data, true);
            return response !== null;
        } catch (error) {
            console.error('Login failed:', error);
            return false;
        }
    }
    
    // 发送文本消息
    async sendTextMessage(receiverId: bigint, text: string): Promise<bigint | null> {
        const encoder = new TextEncoder();
        const textBuffer = encoder.encode(text);
        
        const data = new ArrayBuffer(8 + 8 + 8 + 4 + 8 + 1 + 64 + 4 + textBuffer.length);
        const view = new DataView(data);
        let offset = 0;
        
        // 消息ID（由服务器生成，这里先填0）
        view.setBigUint64(offset, 0n, false); offset += 8;
        
        // 发送者ID（服务器会填充）
        view.setBigUint64(offset, 0n, false); offset += 8;
        
        // 接收者ID
        view.setBigUint64(offset, receiverId, false); offset += 8;
        
        // 时间戳
        view.setBigUint64(offset, BigInt(Date.now()), false); offset += 8;
        
        // 消息类型（0=文本）
        view.setUint32(offset, 0, false); offset += 4;
        
        // 回复的消息ID
        view.setBigUint64(offset, 0n, false); offset += 8;
        
        // 是否加密
        view.setUint8(offset, 0); offset += 1;
        
        // 内容哈希（暂时为空）
        const hashArray = new Uint8Array(data, offset, 64);
        hashArray.fill(0);
        offset += 64;
        
        // 内容长度
        view.setUint32(offset, textBuffer.length, false); offset += 4;
        
        // 内容
        const contentArray = new Uint8Array(data, offset, textBuffer.length);
        contentArray.set(textBuffer);
        
        try {
            const response = await this.sendMessage(MessageType.MSG_CHAT_TEXT_SEND, data, true);
            if (response) {
                const responseView = new DataView(response);
                return responseView.getBigUint64(0, false); // 返回消息ID
            }
        } catch (error) {
            console.error('Failed to send text message:', error);
        }
        
        return null;
    }
    
    // 获取用户信息
    async getUserInfo(userId: bigint): Promise<UserInfo | null> {
        const data = new ArrayBuffer(8);
        const view = new DataView(data);
        view.setBigUint64(0, userId, false);
        
        try {
            const response = await this.sendMessage(MessageType.MSG_USER_PROFILE_GET, data, true);
            if (response) {
                return ProtocolCodec.deserializeUserInfo(response);
            }
        } catch (error) {
            console.error('Failed to get user info:', error);
        }
        
        return null;
    }
    
    // 获取连接状态
    getConnectionState(): ConnectionState {
        return this.state;
    }
    
    // 是否已认证
    isAuthenticatedUser(): boolean {
        return this.isAuthenticated;
    }
    
    // 获取用户ID
    getUserId(): bigint {
        return this.userId;
    }
    
    // 获取会话令牌
    getSessionToken(): string {
        return this.sessionToken;
    }
}