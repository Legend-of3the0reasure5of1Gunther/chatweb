// 文件：web/js/chat-client.js
/**
 * 安全聊天系统WebSocket客户端
 * 负责连接管理、协议处理和消息路由
 */

import { ProtocolCodec, MessageType, ErrorCode, MessageFlags } from './protocol.js';

class ChatClient {
    constructor(uiEngine, securityManager) {
        this.uiEngine = uiEngine;
        this.securityManager = securityManager;
        this.ws = null;
        this.isConnected = false;
        this.reconnectAttempts = 0;
        this.maxReconnectAttempts = 5;
        this.reconnectDelay = 1000;
        this.heartbeatInterval = null;
        this.messageQueue = [];
        this.pendingRequests = new Map();
        this.sessionInfo = {
            userId: null,
            username: null,
            token: null,
            refreshToken: null,
            sessionId: null
        };
        
        // 事件监听器
        this.eventListeners = new Map();
        
        // 初始化
        this.init();
    }

    async init() {
        // 从本地存储恢复会话
        await this.restoreSession();
        
        // 设置事件监听
        this.setupEventListeners();
        
        // 尝试自动连接
        if (this.sessionInfo.token) {
            this.connect();
        }
    }

    async restoreSession() {
        try {
            const sessionData = localStorage.getItem('secure_chat_session');
            if (sessionData) {
                const data = JSON.parse(sessionData);
                if (data.expiresAt > Date.now()) {
                    this.sessionInfo = data;
                    return true;
                }
            }
        } catch (error) {
            console.warn('恢复会话失败:', error);
        }
        return false;
    }

    saveSession() {
        try {
            const sessionData = {
                ...this.sessionInfo,
                expiresAt: Date.now() + 7 * 24 * 60 * 60 * 1000 // 7天有效期
            };
            localStorage.setItem('secure_chat_session', JSON.stringify(sessionData));
        } catch (error) {
            console.error('保存会话失败:', error);
        }
    }

    clearSession() {
        this.sessionInfo = {
            userId: null,
            username: null,
            token: null,
            refreshToken: null,
            sessionId: null
        };
        localStorage.removeItem('secure_chat_session');
    }

    async connect() {
        if (this.isConnected) {
            return;
        }

        try {
            const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
            const host = window.location.hostname || 'localhost';
            const port = window.location.port || 8889;
            
            const wsUrl = `${protocol}//${host}:${port}/chat`;
            
            this.ws = new WebSocket(wsUrl);
            
            this.ws.onopen = this.handleOpen.bind(this);
            this.ws.onmessage = this.handleMessage.bind(this);
            this.ws.onerror = this.handleError.bind(this);
            this.ws.onclose = this.handleClose.bind(this);
            
            // 设置连接超时
            setTimeout(() => {
                if (this.ws.readyState === WebSocket.CONNECTING) {
                    this.ws.close();
                    this.handleError(new Error('连接超时'));
                }
            }, 10000);
            
        } catch (error) {
            this.handleError(error);
        }
    }

    disconnect() {
        if (this.ws) {
            this.ws.close(1000, '用户主动断开');
            this.ws = null;
        }
        this.isConnected = false;
        this.reconnectAttempts = 0;
        
        if (this.heartbeatInterval) {
            clearInterval(this.heartbeatInterval);
            this.heartbeatInterval = null;
        }
        
        this.emit('disconnected');
    }

    async sendMessage(type, data, options = {}) {
        return new Promise((resolve, reject) => {
            const messageId = this.generateMessageId();
            const timestamp = Date.now();
            
            const header = {
                magic: 0x53435400, // "SCT\0"
                version: (3 << 16) | (0 << 8) | 0, // 3.0.0
                type: type,
                flags: options.flags || 0,
                timestamp: BigInt(timestamp),
                messageId: BigInt(messageId),
                correlationId: BigInt(options.correlationId || 0),
                bodyLength: 0,
                checksum: 0
            };
            
            // 序列化消息体
            let bodyData = null;
            if (data) {
                if (options.encrypt && this.securityManager) {
                    try {
                        const encrypted = this.securityManager.encryptMessage(JSON.stringify(data));
                        bodyData = ProtocolCodec.serializeString(encrypted, 65535);
                    } catch (error) {
                        reject(new Error('加密失败: ' + error.message));
                        return;
                    }
                } else {
                    bodyData = ProtocolCodec.serializeString(JSON.stringify(data), 65535);
                }
                header.bodyLength = bodyData.byteLength;
            }
            
            // 计算校验和
            const headerBuffer = ProtocolCodec.serializeHeader(header);
            if (bodyData) {
                const fullData = new Uint8Array(headerBuffer.byteLength + bodyData.byteLength);
                fullData.set(new Uint8Array(headerBuffer), 0);
                fullData.set(new Uint8Array(bodyData), headerBuffer.byteLength);
                header.checksum = ProtocolCodec.calculateChecksum(fullData);
            } else {
                header.checksum = ProtocolCodec.calculateChecksum(headerBuffer);
            }
            
            // 更新header
            const finalHeader = ProtocolCodec.serializeHeader(header);
            
            // 发送消息
            if (this.isConnected && this.ws && this.ws.readyState === WebSocket.OPEN) {
                try {
                    if (bodyData) {
                        const fullMessage = new Blob([finalHeader, bodyData]);
                        this.ws.send(fullMessage);
                    } else {
                        this.ws.send(finalHeader);
                    }
                    
                    // 如果需要响应，添加到pending队列
                    if (options.expectResponse) {
                        const timeout = setTimeout(() => {
                            if (this.pendingRequests.has(messageId)) {
                                this.pendingRequests.delete(messageId);
                                reject(new Error('请求超时'));
                            }
                        }, options.timeout || 10000);
                        
                        this.pendingRequests.set(messageId, {
                            resolve,
                            reject,
                            timeout
                        });
                    } else {
                        resolve({ messageId, timestamp });
                    }
                    
                } catch (error) {
                    reject(error);
                }
            } else {
                // 如果未连接，添加到队列
                this.messageQueue.push({
                    header,
                    bodyData,
                    resolve,
                    reject,
                    options
                });
                reject(new Error('未连接到服务器'));
            }
        });
    }

    async login(username, password) {
        try {
            // 密码哈希处理
            const passwordHash = await this.securityManager.hashPassword(password);
            
            const response = await this.sendMessage(
                MessageType.MSG_AUTH_LOGIN,
                {
                    username,
                    password_hash: passwordHash,
                    client_version: '3.0.0-web',
                    client_type: 'web'
                },
                {
                    expectResponse: true,
                    timeout: 15000
                }
            );
            
            if (response.status === 'success') {
                this.sessionInfo = {
                    userId: response.data.user_id,
                    username: response.data.username,
                    token: response.data.access_token,
                    refreshToken: response.data.refresh_token,
                    sessionId: response.data.session_id
                };
                
                this.saveSession();
                this.emit('login_success', response.data);
                return response.data;
            } else {
                throw new Error(response.error_message || '登录失败');
            }
            
        } catch (error) {
            this.emit('login_failed', error);
            throw error;
        }
    }

    async loginWithToken(token) {
        try {
            const response = await this.sendMessage(
                MessageType.MSG_AUTH_TOKEN_REFRESH,
                { refresh_token: token },
                { expectResponse: true }
            );
            
            if (response.status === 'success') {
                this.sessionInfo = {
                    userId: response.data.user_id,
                    username: response.data.username,
                    token: response.data.access_token,
                    refreshToken: response.data.refresh_token,
                    sessionId: response.data.session_id
                };
                
                this.saveSession();
                this.emit('login_success', response.data);
                return true;
            }
            
        } catch (error) {
            console.warn('Token登录失败:', error);
        }
        return false;
    }

    async logout() {
        try {
            await this.sendMessage(
                MessageType.MSG_AUTH_LOGOUT,
                { session_id: this.sessionInfo.sessionId },
                { expectResponse: true }
            );
        } catch (error) {
            // 忽略登出错误
        } finally {
            this.clearSession();
            this.disconnect();
            this.emit('logout');
        }
    }

    async sendTextMessage(receiverId, content, options = {}) {
        const encrypted = options.encrypt !== false && this.securityManager;
        
        const messageData = {
            receiver_id: receiverId,
            content: content,
            timestamp: Date.now(),
            reply_to: options.replyTo || 0,
            encrypted: encrypted
        };
        
        if (encrypted) {
            try {
                const encryptedContent = await this.securityManager.encryptMessage(content);
                messageData.content = encryptedContent;
                messageData.encryption_key = await this.securityManager.getEncryptionKey(receiverId);
            } catch (error) {
                throw new Error('消息加密失败: ' + error.message);
            }
        }
        
        return await this.sendMessage(
            MessageType.MSG_CHAT_TEXT_SEND,
            messageData,
            {
                expectResponse: true,
                encrypt: encrypted
            }
        );
    }

    async sendFile(file, receiverId, options = {}) {
        if (!file || !file.size) {
            throw new Error('无效的文件');
        }
        
        const maxSize = 100 * 1024 * 1024; // 100MB
        if (file.size > maxSize) {
            throw new Error(`文件大小超过限制 (${maxSize / 1024 / 1024}MB)`);
        }
        
        // 计算文件哈希
        const fileHash = await this.securityManager.calculateFileHash(file);
        
        // 请求上传许可
        const uploadResponse = await this.sendMessage(
            MessageType.MSG_FILE_UPLOAD_REQUEST,
            {
                receiver_id: receiverId,
                filename: file.name,
                file_size: file.size,
                file_hash: fileHash,
                mime_type: file.type || 'application/octet-stream',
                chunk_size: 16384,
                encrypt: options.encrypt !== false,
                compress: options.compress || false
            },
            { expectResponse: true }
        );
        
        if (uploadResponse.status !== 'success') {
            throw new Error(uploadResponse.error_message || '上传请求被拒绝');
        }
        
        const transferId = uploadResponse.data.transfer_id;
        const chunkSize = uploadResponse.data.chunk_size;
        const totalChunks = Math.ceil(file.size / chunkSize);
        
        // 分片上传
        const reader = new FileReader();
        let currentChunk = 0;
        
        return new Promise((resolve, reject) => {
            reader.onload = async (e) => {
                try {
                    const chunkData = e.target.result;
                    
                    const response = await this.sendMessage(
                        MessageType.MSG_FILE_UPLOAD_CHUNK,
                        {
                            transfer_id: transferId,
                            chunk_index: currentChunk,
                            chunk_data: chunkData,
                            total_chunks: totalChunks
                        },
                        { expectResponse: true }
                    );
                    
                    // 更新进度
                    const progress = ((currentChunk + 1) / totalChunks) * 100;
                    this.emit('file_transfer_progress', {
                        transferId,
                        progress,
                        currentChunk: currentChunk + 1,
                        totalChunks
                    });
                    
                    if (currentChunk < totalChunks - 1) {
                        currentChunk++;
                        readNextChunk();
                    } else {
                        // 上传完成
                        const completeResponse = await this.sendMessage(
                            MessageType.MSG_FILE_UPLOAD_COMPLETE,
                            { transfer_id: transferId },
                            { expectResponse: true }
                        );
                        
                        resolve(completeResponse.data);
                    }
                    
                } catch (error) {
                    reject(error);
                }
            };
            
            reader.onerror = (error) => {
                reject(new Error('文件读取失败: ' + error));
            };
            
            const readNextChunk = () => {
                const start = currentChunk * chunkSize;
                const end = Math.min(start + chunkSize, file.size);
                const slice = file.slice(start, end);
                reader.readAsArrayBuffer(slice);
            };
            
            readNextChunk();
        });
    }

    handleOpen(event) {
        console.log('WebSocket连接已建立');
        this.isConnected = true;
        this.reconnectAttempts = 0;
        
        // 发送握手消息
        this.sendMessage(MessageType.MSG_SYSTEM_HANDSHAKE, {
            client_version: '3.0.0-web',
            protocol_version: '3.0.0',
            capabilities: ['encryption', 'compression', 'file_transfer']
        }).catch(console.error);
        
        // 开始心跳
        this.startHeartbeat();
        
        // 处理消息队列
        this.processMessageQueue();
        
        this.emit('connected', event);
    }

    handleMessage(event) {
        try {
            const data = event.data;
            
            if (data instanceof Blob) {
                this.handleBinaryMessage(data);
            } else if (typeof data === 'string') {
                this.handleTextMessage(data);
            }
        } catch (error) {
            console.error('消息处理失败:', error);
            this.emit('message_error', error);
        }
    }

    async handleBinaryMessage(blob) {
        const buffer = await blob.arrayBuffer();
        const header = ProtocolCodec.deserializeHeader(buffer.slice(0, 64));
        
        if (!header || !ProtocolCodec.validateHeader(header)) {
            console.error('无效的消息头');
            return;
        }
        
        // 验证校验和
        const checksum = ProtocolCodec.calculateChecksum(buffer);
        if (checksum !== header.checksum) {
            console.error('消息校验和错误');
            return;
        }
        
        // 提取消息体
        let bodyData = null;
        if (header.bodyLength > 0) {
            bodyData = buffer.slice(64, 64 + header.bodyLength);
        }
        
        // 处理消息
        await this.processMessage(header, bodyData);
    }

    handleTextMessage(text) {
        try {
            const data = JSON.parse(text);
            // 处理文本消息（如错误信息）
            if (data.type === 'error') {
                this.emit('server_error', data);
            }
        } catch (error) {
            console.warn('文本消息解析失败:', error);
        }
    }

    async processMessage(header, bodyData) {
        let body = null;
        
        if (bodyData) {
            const textDecoder = new TextDecoder('utf-8');
            let bodyText = textDecoder.decode(bodyData);
            
            // 检查是否需要解密
            if (header.flags & MessageFlags.FLAG_ENCRYPTED) {
                try {
                    bodyText = await this.securityManager.decryptMessage(bodyText);
                } catch (error) {
                    console.error('消息解密失败:', error);
                    this.emit('decryption_error', { header, error });
                    return;
                }
            }
            
            try {
                body = JSON.parse(bodyText);
            } catch (error) {
                console.error('消息体解析失败:', error);
                return;
            }
        }
        
        // 处理响应消息
        if (header.flags & MessageFlags.FLAG_RESPONSE) {
            const pending = this.pendingRequests.get(Number(header.correlationId));
            if (pending) {
                clearTimeout(pending.timeout);
                
                if (body && body.error_code !== ErrorCode.ERR_SUCCESS) {
                    pending.reject(new Error(body.error_message || '请求失败'));
                } else {
                    pending.resolve({
                        status: 'success',
                        data: body,
                        header: header
                    });
                }
                
                this.pendingRequests.delete(Number(header.correlationId));
            }
            return;
        }
        
        // 处理不同类型的消息
        switch (header.type) {
            case MessageType.MSG_CHAT_TEXT_RECEIVE:
                await this.handleIncomingMessage(body);
                break;
                
            case MessageType.MSG_CHAT_FILE_RECEIVE:
                await this.handleFileMessage(body);
                break;
                
            case MessageType.MSG_CHAT_TYPING_NOTIFY:
                this.emit('typing', body);
                break;
                
            case MessageType.MSG_CHAT_READ_RECEIPT:
                this.emit('message_read', body);
                break;
                
            case MessageType.MSG_SYSTEM_HEARTBEAT:
                // 心跳响应
                break;
                
            case MessageType.MSG_SYSTEM_NOTIFICATION:
                this.emit('notification', body);
                break;
                
            case MessageType.MSG_SYSTEM_MAINTENANCE:
                this.emit('maintenance', body);
                break;
                
            default:
                console.warn('未知消息类型:', header.type);
        }
    }

    async handleIncomingMessage(message) {
        // 解密消息内容
        let content = message.content;
        if (message.encrypted && this.securityManager) {
            try {
                content = await this.securityManager.decryptMessage(
                    message.content,
                    message.encryption_key
                );
            } catch (error) {
                console.error('接收消息解密失败:', error);
                content = '[加密消息无法解密]';
            }
        }
        
        // 发送已读回执
        this.sendMessage(
            MessageType.MSG_CHAT_TEXT_ACK,
            { message_id: message.message_id },
            { expectResponse: false }
        ).catch(console.error);
        
        this.emit('message_received', {
            ...message,
            content: content,
            decrypted: true
        });
    }

    async handleFileMessage(message) {
        this.emit('file_received', message);
    }

    handleError(error) {
        console.error('WebSocket错误:', error);
        this.emit('connection_error', error);
        
        // 尝试重连
        this.scheduleReconnect();
    }

    handleClose(event) {
        console.log('WebSocket连接关闭:', event.code, event.reason);
        this.isConnected = false;
        
        if (this.heartbeatInterval) {
            clearInterval(this.heartbeatInterval);
            this.heartbeatInterval = null;
        }
        
        this.emit('disconnected', event);
        
        // 如果不是正常关闭，尝试重连
        if (event.code !== 1000 && event.code !== 1001) {
            this.scheduleReconnect();
        }
    }

    scheduleReconnect() {
        if (this.reconnectAttempts >= this.maxReconnectAttempts) {
            console.log('达到最大重连次数');
            this.emit('reconnect_failed');
            return;
        }
        
        this.reconnectAttempts++;
        const delay = this.reconnectDelay * Math.pow(2, this.reconnectAttempts - 1);
        
        console.log(`将在 ${delay}ms 后尝试重连 (${this.reconnectAttempts}/${this.maxReconnectAttempts})`);
        
        setTimeout(() => {
            if (!this.isConnected) {
                this.connect();
            }
        }, Math.min(delay, 30000)); // 最大延迟30秒
    }

    startHeartbeat() {
        if (this.heartbeatInterval) {
            clearInterval(this.heartbeatInterval);
        }
        
        this.heartbeatInterval = setInterval(() => {
            if (this.isConnected && this.ws.readyState === WebSocket.OPEN) {
                this.sendMessage(
                    MessageType.MSG_SYSTEM_HEARTBEAT,
                    { timestamp: Date.now() },
                    { expectResponse: false }
                ).catch(() => {
                    // 心跳失败，可能连接已断开
                    console.warn('心跳发送失败');
                });
            }
        }, 30000); // 每30秒发送一次心跳
    }

    processMessageQueue() {
        while (this.messageQueue.length > 0) {
            const queuedMessage = this.messageQueue.shift();
            
            // 重新发送消息
            this.sendMessage(
                queuedMessage.header.type,
                queuedMessage.bodyData ? JSON.parse(
                    new TextDecoder('utf-8').decode(queuedMessage.bodyData)
                ) : null,
                queuedMessage.options
            ).then(queuedMessage.resolve)
             .catch(queuedMessage.reject);
        }
    }

    generateMessageId() {
        return Date.now() * 1000 + Math.floor(Math.random() * 1000);
    }

    setupEventListeners() {
        // 设置默认事件监听
        this.on('connected', () => {
            this.uiEngine?.updateConnectionStatus(true);
        });
        
        this.on('disconnected', () => {
            this.uiEngine?.updateConnectionStatus(false);
        });
        
        this.on('login_success', (data) => {
            this.uiEngine?.showMainInterface();
        });
        
        this.on('logout', () => {
            this.uiEngine?.showLoginInterface();
        });
        
        this.on('message_received', (message) => {
            this.uiEngine?.addMessage(message);
        });
    }

    on(event, callback) {
        if (!this.eventListeners.has(event)) {
            this.eventListeners.set(event, []);
        }
        this.eventListeners.get(event).push(callback);
    }

    off(event, callback) {
        if (this.eventListeners.has(event)) {
            const listeners = this.eventListeners.get(event);
            const index = listeners.indexOf(callback);
            if (index > -1) {
                listeners.splice(index, 1);
            }
        }
    }

    emit(event, data) {
        if (this.eventListeners.has(event)) {
            this.eventListeners.get(event).forEach(callback => {
                try {
                    callback(data);
                } catch (error) {
                    console.error(`事件 ${event} 监听器错误:`, error);
                }
            });
        }
    }

    // 公开方法
    getSessionInfo() {
        return { ...this.sessionInfo };
    }

    isAuthenticated() {
        return !!this.sessionInfo.token;
    }

    getConnectionStatus() {
        return {
            isConnected: this.isConnected,
            reconnectAttempts: this.reconnectAttempts,
            maxReconnectAttempts: this.maxReconnectAttempts
        };
    }
}

export default ChatClient;