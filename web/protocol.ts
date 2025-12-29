// 文件: src/core/protocol.ts
/**
 * Web前端协议模块 - 与C后端完全兼容
 */

// 协议版本
export const PROTOCOL_VERSION_MAJOR = 3;
export const PROTOCOL_VERSION_MINOR = 0;
export const PROTOCOL_VERSION_PATCH = 0;
export const PROTOCOL_VERSION = (PROTOCOL_VERSION_MAJOR << 16) | (PROTOCOL_VERSION_MINOR << 8) | PROTOCOL_VERSION_PATCH;
export const PROTOCOL_MAGIC = 0x53435400; // "SCT\0"

// 错误码枚举
export enum ErrorCode {
    ERR_SUCCESS = 0,
    ERR_INVALID_REQUEST = 1001,
    ERR_AUTH_FAILED = 1002,
    ERR_INVALID_TOKEN = 1003,
    ERR_PERMISSION_DENIED = 1004,
    ERR_USER_NOT_FOUND = 1005,
    ERR_USER_ALREADY_EXISTS = 1006,
    ERR_INVALID_PARAMETERS = 1007,
    ERR_RATE_LIMIT_EXCEEDED = 1008,
    ERR_SERVER_ERROR = 2001,
    ERR_DATABASE_ERROR = 2002,
    ERR_FILESYSTEM_ERROR = 2003,
    ERR_SERVICE_UNAVAILABLE = 2004,
    ERR_MAINTENANCE_MODE = 2005,
    ERR_NETWORK_ERROR = 3001,
    ERR_CONNECTION_TIMEOUT = 3002,
    ERR_PROTOCOL_ERROR = 3003,
    ERR_FILE_TOO_LARGE = 4001,
    ERR_FILE_TYPE_NOT_ALLOWED = 4002,
    ERR_FILE_UPLOAD_FAILED = 4003,
    ERR_FILE_NOT_FOUND = 4004,
    ERR_FILE_HASH_MISMATCH = 4005,
    ERR_RESOURCE_LIMIT_EXCEEDED = 5001,
    ERR_STORAGE_FULL = 5002,
    ERR_SECURITY_VIOLATION = 6001,
    ERR_IP_BLOCKED = 6002,
    ERR_ACCOUNT_LOCKED = 6003,
    ERR_UNKNOWN = 9999
}

// 消息类型枚举
export enum MessageType {
    // 系统消息
    MSG_SYSTEM_HANDSHAKE = 0x00000001,
    MSG_SYSTEM_HEARTBEAT = 0x00000002,
    MSG_SYSTEM_ERROR = 0x00000003,
    MSG_SYSTEM_NOTIFICATION = 0x00000004,
    MSG_SYSTEM_MAINTENANCE = 0x00000005,
    
    // 认证消息
    MSG_AUTH_LOGIN = 0x01000001,
    MSG_AUTH_LOGIN_RESPONSE = 0x01000002,
    MSG_AUTH_REGISTER = 0x01000003,
    MSG_AUTH_REGISTER_RESPONSE = 0x01000004,
    MSG_AUTH_LOGOUT = 0x01000005,
    MSG_AUTH_LOGOUT_RESPONSE = 0x01000006,
    MSG_AUTH_TOKEN_REFRESH = 0x01000007,
    MSG_AUTH_TOKEN_REFRESH_RESPONSE = 0x01000008,
    
    // 用户消息
    MSG_USER_PROFILE_GET = 0x02000001,
    MSG_USER_PROFILE_GET_RESPONSE = 0x02000002,
    MSG_USER_PROFILE_UPDATE = 0x02000003,
    MSG_USER_PROFILE_UPDATE_RESPONSE = 0x02000004,
    MSG_USER_STATUS_UPDATE = 0x02000005,
    MSG_USER_STATUS_UPDATE_RESPONSE = 0x02000006,
    MSG_USER_LIST_GET = 0x02000007,
    MSG_USER_LIST_GET_RESPONSE = 0x02000008,
    MSG_USER_SEARCH = 0x02000009,
    MSG_USER_SEARCH_RESPONSE = 0x0200000A,
    
    // 聊天消息
    MSG_CHAT_TEXT_SEND = 0x03000001,
    MSG_CHAT_TEXT_RECEIVE = 0x03000002,
    MSG_CHAT_TEXT_ACK = 0x03000003,
    MSG_CHAT_FILE_SEND = 0x03000004,
    MSG_CHAT_FILE_RECEIVE = 0x03000005,
    MSG_CHAT_FILE_ACK = 0x03000006,
    MSG_CHAT_IMAGE_SEND = 0x03000007,
    MSG_CHAT_IMAGE_RECEIVE = 0x03000008,
    MSG_CHAT_VOICE_SEND = 0x03000009,
    MSG_CHAT_VOICE_RECEIVE = 0x0300000A,
    MSG_CHAT_HISTORY_GET = 0x0300000B,
    MSG_CHAT_HISTORY_GET_RESPONSE = 0x0300000C,
    MSG_CHAT_TYPING_NOTIFY = 0x0300000D,
    MSG_CHAT_READ_RECEIPT = 0x0300000E,
    
    // 群组消息
    MSG_GROUP_CREATE = 0x04000001,
    MSG_GROUP_CREATE_RESPONSE = 0x04000002,
    MSG_GROUP_JOIN = 0x04000003,
    MSG_GROUP_JOIN_RESPONSE = 0x04000004,
    MSG_GROUP_LEAVE = 0x04000005,
    MSG_GROUP_LEAVE_RESPONSE = 0x04000006,
    MSG_GROUP_MESSAGE_SEND = 0x04000007,
    MSG_GROUP_MESSAGE_RECEIVE = 0x04000008,
    MSG_GROUP_INFO_GET = 0x04000009,
    MSG_GROUP_INFO_GET_RESPONSE = 0x0400000A,
    MSG_GROUP_MEMBERS_GET = 0x0400000B,
    MSG_GROUP_MEMBERS_GET_RESPONSE = 0x0400000C,
    
    // 文件传输
    MSG_FILE_UPLOAD_REQUEST = 0x05000001,
    MSG_FILE_UPLOAD_REQUEST_RESPONSE = 0x05000002,
    MSG_FILE_UPLOAD_CHUNK = 0x05000003,
    MSG_FILE_UPLOAD_CHUNK_RESPONSE = 0x05000004,
    MSG_FILE_UPLOAD_COMPLETE = 0x05000005,
    MSG_FILE_UPLOAD_COMPLETE_RESPONSE = 0x05000006,
    MSG_FILE_DOWNLOAD_REQUEST = 0x05000007,
    MSG_FILE_DOWNLOAD_REQUEST_RESPONSE = 0x05000008,
    MSG_FILE_DOWNLOAD_CHUNK = 0x05000009,
    MSG_FILE_DOWNLOAD_CHUNK_RESPONSE = 0x0500000A,
    MSG_FILE_DOWNLOAD_COMPLETE = 0x0500000B,
    MSG_FILE_DOWNLOAD_COMPLETE_RESPONSE = 0x0500000C,
    MSG_FILE_PROGRESS_UPDATE = 0x0500000D,
    
    // 加密消息
    MSG_CRYPTO_KEY_EXCHANGE = 0x06000001,
    MSG_CRYPTO_KEY_EXCHANGE_RESPONSE = 0x06000002,
    MSG_CRYPTO_MESSAGE_ENCRYPTED = 0x06000003,
    MSG_CRYPTO_MESSAGE_DECRYPTED = 0x06000004
}

// 标志位定义
export enum MessageFlags {
    FLAG_ENCRYPTED = 1 << 0,
    FLAG_COMPRESSED = 1 << 1,
    FLAG_PRIORITY_HIGH = 1 << 2,
    FLAG_PRIORITY_URGENT = 1 << 3,
    FLAG_RESPONSE = 1 << 4,
    FLAG_MULTIPART = 1 << 5,
    FLAG_SIGNED = 1 << 6
}

// 用户状态
export enum UserStatus {
    USER_STATUS_OFFLINE = 0,
    USER_STATUS_ONLINE = 1,
    USER_STATUS_AWAY = 2,
    USER_STATUS_BUSY = 3,
    USER_STATUS_INVISIBLE = 4,
    USER_STATUS_DND = 5
}

// 消息状态
export enum MessageStatus {
    MESSAGE_STATUS_SENDING = 0,
    MESSAGE_STATUS_SENT = 1,
    MESSAGE_STATUS_DELIVERED = 2,
    MESSAGE_STATUS_READ = 3,
    MESSAGE_STATUS_FAILED = 4
}

// 文件类型
export enum FileType {
    FILE_TYPE_UNKNOWN = 0,
    FILE_TYPE_IMAGE = 1,
    FILE_TYPE_AUDIO = 2,
    FILE_TYPE_VIDEO = 3,
    FILE_TYPE_DOCUMENT = 4,
    FILE_TYPE_ARCHIVE = 5,
    FILE_TYPE_EXECUTABLE = 6
}

// 数据结构接口
export interface MessageHeader {
    magic: number;
    version: number;
    type: MessageType;
    flags: number;
    timestamp: bigint;
    messageId: bigint;
    correlationId: bigint;
    bodyLength: number;
    checksum: number;
}

export interface UserInfo {
    userId: bigint;
    username: string;
    nickname: string;
    email: string;
    avatarId: number;
    status: UserStatus;
    statusMessage: string;
    lastSeen: bigint;
    friendCount: number;
    groupCount: number;
    isVerified: boolean;
    isPremium: boolean;
    createdAt: bigint;
}

export interface MessageContent {
    messageId: bigint;
    senderId: bigint;
    receiverId: bigint;
    timestamp: bigint;
    messageType: number;
    replyToId: bigint;
    encrypted: boolean;
    contentHash: string;
    contentLength: number;
    content: ArrayBuffer;
}

export interface FileMetadata {
    fileId: bigint;
    uploaderId: bigint;
    filename: string;
    originalName: string;
    mimeType: string;
    fileSize: bigint;
    fileType: FileType;
    fileHash: string;
    isEncrypted: boolean;
    isCompressed: boolean;
    uploadedAt: bigint;
    expiresAt: bigint;
}

export interface FileTransferRequest {
    transferId: bigint;
    senderId: bigint;
    receiverId: bigint;
    metadata: FileMetadata;
    chunkSize: number;
    transferMode: number;
    encryptionEnabled: boolean;
    encryptionKey: string;
}

export interface ResponseData {
    statusCode: number;
    errorCode: ErrorCode;
    requestId: bigint;
    dataLength: number;
    data: ArrayBuffer;
}

export interface ErrorInfo {
    errorCode: ErrorCode;
    requestId: bigint;
    errorMessage: string;
    debugInfo: string;
    timestamp: bigint;
}

// 协议编解码器
export class ProtocolCodec {
    private static readonly HEADER_SIZE = 64;
    private static readonly RESERVED_SIZE = 16;
    
    // 序列化消息头
    static serializeHeader(header: MessageHeader): ArrayBuffer {
        const buffer = new ArrayBuffer(this.HEADER_SIZE);
        const view = new DataView(buffer);
        
        view.setUint32(0, header.magic, false);
        view.setUint32(4, header.version, false);
        view.setUint32(8, header.type, false);
        view.setUint32(12, header.flags, false);
        view.setBigUint64(16, header.timestamp, false);
        view.setBigUint64(24, header.messageId, false);
        view.setBigUint64(32, header.correlationId, false);
        view.setUint32(40, header.bodyLength, false);
        view.setUint32(44, header.checksum, false);
        
        // 保留字段清零
        for (let i = 48; i < this.HEADER_SIZE; i++) {
            view.setUint8(i, 0);
        }
        
        return buffer;
    }
    
    // 反序列化消息头
    static deserializeHeader(buffer: ArrayBuffer): MessageHeader | null {
        if (buffer.byteLength < this.HEADER_SIZE) {
            return null;
        }
        
        const view = new DataView(buffer);
        
        return {
            magic: view.getUint32(0, false),
            version: view.getUint32(4, false),
            type: view.getUint32(8, false),
            flags: view.getUint32(12, false),
            timestamp: view.getBigUint64(16, false),
            messageId: view.getBigUint64(24, false),
            correlationId: view.getBigUint64(32, false),
            bodyLength: view.getUint32(40, false),
            checksum: view.getUint32(44, false)
        };
    }
    
    // 验证消息头
    static validateHeader(header: MessageHeader): boolean {
        if (header.magic !== PROTOCOL_MAGIC) {
            return false;
        }
        
        if (header.version !== PROTOCOL_VERSION) {
            return false;
        }
        
        return true;
    }
    
    // 计算CRC32校验和
    static calculateChecksum(data: ArrayBuffer): number {
        const view = new DataView(data);
        let crc = 0xFFFFFFFF;
        
        for (let i = 0; i < data.byteLength; i++) {
            crc ^= view.getUint8(i);
            
            for (let j = 0; j < 8; j++) {
                crc = (crc >>> 1) ^ (crc & 1 ? 0xEDB88320 : 0);
            }
        }
        
        return crc ^ 0xFFFFFFFF;
    }
    
    // 序列化字符串（UTF-8，固定长度）
    static serializeString(str: string, maxLength: number): ArrayBuffer {
        const encoder = new TextEncoder();
        const encoded = encoder.encode(str);
        const buffer = new ArrayBuffer(maxLength);
        const view = new Uint8Array(buffer);
        
        const length = Math.min(encoded.length, maxLength - 1);
        view.set(encoded.subarray(0, length));
        view[length] = 0; // Null终止
        
        return buffer;
    }
    
    // 反序列化字符串
    static deserializeString(buffer: ArrayBuffer, maxLength: number): string {
        const view = new Uint8Array(buffer);
        let length = 0;
        
        while (length < maxLength && view[length] !== 0) {
            length++;
        }
        
        const decoder = new TextDecoder('utf-8');
        return decoder.decode(view.subarray(0, length));
    }
    
    // 序列化用户信息
    static serializeUserInfo(info: UserInfo): ArrayBuffer {
        const buffer = new ArrayBuffer(512); // 估计大小
        const view = new DataView(buffer);
        let offset = 0;
        
        view.setBigUint64(offset, info.userId, false); offset += 8;
        
        // 序列化字符串字段
        const usernameBuffer = this.serializeString(info.username, 64);
        new Uint8Array(buffer, offset, 64).set(new Uint8Array(usernameBuffer));
        offset += 64;
        
        const nicknameBuffer = this.serializeString(info.nickname, 64);
        new Uint8Array(buffer, offset, 64).set(new Uint8Array(nicknameBuffer));
        offset += 64;
        
        const emailBuffer = this.serializeString(info.email, 128);
        new Uint8Array(buffer, offset, 128).set(new Uint8Array(emailBuffer));
        offset += 128;
        
        view.setUint32(offset, info.avatarId, false); offset += 4;
        view.setUint8(offset, info.status); offset += 1;
        
        const statusMsgBuffer = this.serializeString(info.statusMessage, 128);
        new Uint8Array(buffer, offset, 128).set(new Uint8Array(statusMsgBuffer));
        offset += 128;
        
        view.setBigUint64(offset, info.lastSeen, false); offset += 8;
        view.setUint32(offset, info.friendCount, false); offset += 4;
        view.setUint32(offset, info.groupCount, false); offset += 4;
        view.setUint8(offset, info.isVerified ? 1 : 0); offset += 1;
        view.setUint8(offset, info.isPremium ? 1 : 0); offset += 1;
        view.setBigUint64(offset, info.createdAt, false); offset += 8;
        
        return buffer.slice(0, offset);
    }
    
    // 反序列化用户信息
    static deserializeUserInfo(buffer: ArrayBuffer): UserInfo {
        const view = new DataView(buffer);
        let offset = 0;
        
        const userId = view.getBigUint64(offset, false); offset += 8;
        const username = this.deserializeString(buffer.slice(offset, offset + 64), 64); offset += 64;
        const nickname = this.deserializeString(buffer.slice(offset, offset + 64), 64); offset += 64;
        const email = this.deserializeString(buffer.slice(offset, offset + 128), 128); offset += 128;
        const avatarId = view.getUint32(offset, false); offset += 4;
        const status = view.getUint8(offset); offset += 1;
        const statusMessage = this.deserializeString(buffer.slice(offset, offset + 128), 128); offset += 128;
        const lastSeen = view.getBigUint64(offset, false); offset += 8;
        const friendCount = view.getUint32(offset, false); offset += 4;
        const groupCount = view.getUint32(offset, false); offset += 4;
        const isVerified = view.getUint8(offset) !== 0; offset += 1;
        const isPremium = view.getUint8(offset) !== 0; offset += 1;
        const createdAt = view.getBigUint64(offset, false); offset += 8;
        
        return {
            userId,
            username,
            nickname,
            email,
            avatarId,
            status,
            statusMessage,
            lastSeen,
            friendCount,
            groupCount,
            isVerified,
            isPremium,
            createdAt
        };
    }
}