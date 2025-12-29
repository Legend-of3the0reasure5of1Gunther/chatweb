// 文件: src/core/file-transfer.ts
/**
 * 文件传输模块 - 支持大文件分片传输（最大1GB）
 */

import { SecureChatWebSocket, MessageType } from './websocket-client';
import { EndToEndEncryption } from './security';

export interface FileTransferProgress {
    transferId: bigint;
    fileName: string;
    fileSize: number;
    bytesTransferred: number;
    progress: number;
    speed: number; // 字节/秒
    estimatedTime: number; // 剩余秒数
    status: FileTransferStatus;
    error?: string;
}

export enum FileTransferStatus {
    PENDING = 'pending',
    PREPARING = 'preparing',
    TRANSFERRING = 'transferring',
    PAUSED = 'paused',
    COMPLETED = 'completed',
    FAILED = 'failed',
    CANCELLED = 'cancelled'
}

export interface FileChunk {
    transferId: bigint;
    chunkIndex: number;
    chunkSize: number;
    totalChunks: number;
    data: ArrayBuffer;
    checksum: string;
}

export class FileTransferManager {
    private static readonly MAX_FILE_SIZE = 1024 * 1024 * 1024; // 1GB
    private static readonly CHUNK_SIZE = 16 * 1024; // 16KB
    private static readonly MAX_CONCURRENT_UPLOADS = 3;
    private static readonly MAX_CONCURRENT_DOWNLOADS = 3;
    private static readonly MAX_RETRIES = 3;
    
    private webSocket: SecureChatWebSocket;
    private uploadQueue: FileUpload[] = [];
    private downloadQueue: FileDownload[] = [];
    private activeUploads: Map<bigint, FileUpload> = new Map();
    private activeDownloads: Map<bigint, FileDownload> = new Map();
    private progressCallbacks: Map<bigint, (progress: FileTransferProgress) => void> = new Map();
    
    constructor(webSocket: SecureChatWebSocket) {
        this.webSocket = webSocket;
        
        // 注册文件传输消息处理器
        this.registerMessageHandlers();
    }
    
    // 上传文件
    async uploadFile(file: File, receiverId: bigint, encryptionEnabled = false): Promise<bigint> {
        // 验证文件大小
        if (file.size > FileTransferManager.MAX_FILE_SIZE) {
            throw new Error(`File too large. Maximum size is ${FileTransferManager.MAX_FILE_SIZE / 1024 / 1024}MB`);
        }
        
        // 生成传输ID
        const transferId = this.generateTransferId();
        
        // 创建上传任务
        const upload = new FileUpload(
            transferId,
            file,
            receiverId,
            encryptionEnabled,
            this.webSocket
        );
        
        // 添加到队列
        this.uploadQueue.push(upload);
        
        // 开始处理队列
        this.processUploadQueue();
        
        return transferId;
    }
    
    // 下载文件
    async downloadFile(fileId: bigint, targetPath?: string): Promise<bigint> {
        const transferId = this.generateTransferId();
        
        // 创建下载任务
        const download = new FileDownload(
            transferId,
            fileId,
            targetPath,
            this.webSocket
        );
        
        // 添加到队列
        this.downloadQueue.push(download);
        
        // 开始处理队列
        this.processDownloadQueue();
        
        return transferId;
    }
    
    // 暂停传输
    pauseTransfer(transferId: bigint): boolean {
        const upload = this.activeUploads.get(transferId);
        const download = this.activeDownloads.get(transferId);
        
        if (upload) {
            upload.pause();
            return true;
        } else if (download) {
            download.pause();
            return true;
        }
        
        return false;
    }
    
    // 恢复传输
    resumeTransfer(transferId: bigint): boolean {
        const upload = this.activeUploads.get(transferId);
        const download = this.activeDownloads.get(transferId);
        
        if (upload) {
            upload.resume();
            return true;
        } else if (download) {
            download.resume();
            return true;
        }
        
        return false;
    }
    
    // 取消传输
    cancelTransfer(transferId: bigint): boolean {
        const upload = this.activeUploads.get(transferId);
        const download = this.activeDownloads.get(transferId);
        
        if (upload) {
            upload.cancel();
            this.activeUploads.delete(transferId);
            return true;
        } else if (download) {
            download.cancel();
            this.activeDownloads.delete(transferId);
            return true;
        }
        
        // 从队列中移除
        const uploadIndex = this.uploadQueue.findIndex(u => u.transferId === transferId);
        if (uploadIndex !== -1) {
            this.uploadQueue.splice(uploadIndex, 1);
            return true;
        }
        
        const downloadIndex = this.downloadQueue.findIndex(d => d.transferId === transferId);
        if (downloadIndex !== -1) {
            this.downloadQueue.splice(downloadIndex, 1);
            return true;
        }
        
        return false;
    }
    
    // 获取传输进度
    getTransferProgress(transferId: bigint): FileTransferProgress | null {
        const upload = this.activeUploads.get(transferId);
        const download = this.activeDownloads.get(transferId);
        
        if (upload) {
            return upload.getProgress();
        } else if (download) {
            return download.getProgress();
        }
        
        return null;
    }
    
    // 注册进度回调
    registerProgressCallback(transferId: bigint, callback: (progress: FileTransferProgress) => void): void {
        this.progressCallbacks.set(transferId, callback);
    }
    
    // 注销进度回调
    unregisterProgressCallback(transferId: bigint): void {
        this.progressCallbacks.delete(transferId);
    }
    
    // 处理上传队列
    private processUploadQueue(): void {
        while (this.activeUploads.size < FileTransferManager.MAX_CONCURRENT_UPLOADS && 
               this.uploadQueue.length > 0) {
            const upload = this.uploadQueue.shift();
            if (upload) {
                this.activeUploads.set(upload.transferId, upload);
                upload.start().then(() => {
                    this.activeUploads.delete(upload.transferId);
                    this.processUploadQueue();
                }).catch(error => {
                    console.error('Upload failed:', error);
                    this.activeUploads.delete(upload.transferId);
                    this.processUploadQueue();
                });
            }
        }
    }
    
    // 处理下载队列
    private processDownloadQueue(): void {
        while (this.activeDownloads.size < FileTransferManager.MAX_CONCURRENT_DOWNLOADS && 
               this.downloadQueue.length > 0) {
            const download = this.downloadQueue.shift();
            if (download) {
                this.activeDownloads.set(download.transferId, download);
                download.start().then(() => {
                    this.activeDownloads.delete(download.transferId);
                    this.processDownloadQueue();
                }).catch(error => {
                    console.error('Download failed:', error);
                    this.activeDownloads.delete(download.transferId);
                    this.processDownloadQueue();
                });
            }
        }
    }
    
    // 注册消息处理器
    private registerMessageHandlers(): void {
        // 文件上传响应
        this.webSocket.registerMessageCallback(
            MessageType.MSG_FILE_UPLOAD_REQUEST_RESPONSE,
            (type, data) => this.handleUploadResponse(data)
        );
        
        // 文件块响应
        this.webSocket.registerMessageCallback(
            MessageType.MSG_FILE_UPLOAD_CHUNK_RESPONSE,
            (type, data) => this.handleChunkResponse(data)
        );
        
        // 文件下载响应
        this.webSocket.registerMessageCallback(
            MessageType.MSG_FILE_DOWNLOAD_REQUEST_RESPONSE,
            (type, data) => this.handleDownloadResponse(data)
        );
        
        // 文件块数据
        this.webSocket.registerMessageCallback(
            MessageType.MSG_FILE_DOWNLOAD_CHUNK,
            (type, data) => this.handleDownloadChunk(data)
        );
        
        // 进度更新
        this.webSocket.registerMessageCallback(
            MessageType.MSG_FILE_PROGRESS_UPDATE,
            (type, data) => this.handleProgressUpdate(data)
        );
    }
    
    // 处理上传响应
    private handleUploadResponse(data: ArrayBuffer): void {
        const view = new DataView(data);
        const transferId = view.getBigUint64(0, false);
        const status = view.getUint32(8, false);
        
        const upload = this.activeUploads.get(transferId);
        if (upload) {
            upload.handleUploadResponse(status, data);
        }
    }
    
    // 处理块响应
    private handleChunkResponse(data: ArrayBuffer): void {
        const view = new DataView(data);
        const transferId = view.getBigUint64(0, false);
        const chunkIndex = view.getUint32(8, false);
        const status = view.getUint32(12, false);
        
        const upload = this.activeUploads.get(transferId);
        if (upload) {
            upload.handleChunkResponse(chunkIndex, status);
        }
    }
    
    // 处理下载响应
    private handleDownloadResponse(data: ArrayBuffer): void {
        const view = new DataView(data);
        const transferId = view.getBigUint64(0, false);
        const status = view.getUint32(8, false);
        
        const download = this.activeDownloads.get(transferId);
        if (download) {
            download.handleDownloadResponse(status, data);
        }
    }
    
    // 处理下载块
    private handleDownloadChunk(data: ArrayBuffer): void {
        const view = new DataView(data);
        const transferId = view.getBigUint64(0, false);
        const chunkIndex = view.getUint32(8, false);
        
        const download = this.activeDownloads.get(transferId);
        if (download) {
            download.handleDownloadChunk(chunkIndex, data);
        }
    }
    
    // 处理进度更新
    private handleProgressUpdate(data: ArrayBuffer): void {
        const view = new DataView(data);
        const transferId = view.getBigUint64(0, false);
        const progress = view.getUint32(8, false);
        const bytesTransferred = view.getBigUint64(12, false);
        
        const callback = this.progressCallbacks.get(transferId);
        if (callback) {
            callback({
                transferId,
                fileName: '',
                fileSize: 0,
                bytesTransferred: Number(bytesTransferred),
                progress,
                speed: 0,
                estimatedTime: 0,
                status: FileTransferStatus.TRANSFERRING
            });
        }
    }
    
    // 生成传输ID
    private generateTransferId(): bigint {
        const timestamp = BigInt(Date.now());
        const random = BigInt(Math.floor(Math.random() * 0xFFFFFFFF));
        return (timestamp << 32n) | random;
    }
}

// 文件上传类
class FileUpload {
    private static readonly CHUNK_SIZE = 16 * 1024;
    
    transferId: bigint;
    private file: File;
    private receiverId: bigint;
    private encryptionEnabled: boolean;
    private webSocket: SecureChatWebSocket;
    
    private fileSize: number;
    private totalChunks: number;
    private currentChunk: number = 0;
    private bytesTransferred: number = 0;
    private startTime: number = 0;
    private status: FileTransferStatus = FileTransferStatus.PENDING;
    private paused: boolean = false;
    private cancelled: boolean = false;
    private retryCount: number = 0;
    
    constructor(
        transferId: bigint,
        file: File,
        receiverId: bigint,
        encryptionEnabled: boolean,
        webSocket: SecureChatWebSocket
    ) {
        this.transferId = transferId;
        this.file = file;
        this.receiverId = receiverId;
        this.encryptionEnabled = encryptionEnabled;
        this.webSocket = webSocket;
        
        this.fileSize = file.size;
        this.totalChunks = Math.ceil(file.size / FileUpload.CHUNK_SIZE);
    }
    
    // 开始上传
    async start(): Promise<void> {
        this.status = FileTransferStatus.PREPARING;
        this.startTime = Date.now();
        
        try {
            // 1. 发送文件元数据
            await this.sendFileMetadata();
            
            // 2. 开始分片上传
            await this.uploadChunks();
            
            // 3. 发送完成通知
            await this.sendCompletion();
            
            this.status = FileTransferStatus.COMPLETED;
            
        } catch (error) {
            this.status = FileTransferStatus.FAILED;
            throw error;
        }
    }
    
    // 发送文件元数据
    private async sendFileMetadata(): Promise<void> {
        const encoder = new TextEncoder();
        
        // 计算文件哈希（SHA-256）
        const fileHash = await this.calculateFileHash();
        
        const metadata = {
            transferId: this.transferId,
            senderId: 0n, // 由服务器填充
            receiverId: this.receiverId,
            metadata: {
                fileId: 0n,
                uploaderId: 0n,
                filename: this.file.name,
                originalName: this.file.name,
                mimeType: this.file.type || 'application/octet-stream',
                fileSize: BigInt(this.fileSize),
                fileType: this.determineFileType(this.file.name),
                fileHash: fileHash,
                isEncrypted: this.encryptionEnabled,
                isCompressed: false,
                uploadedAt: BigInt(Date.now()),
                expiresAt: 0n
            },
            chunkSize: FileUpload.CHUNK_SIZE,
            transferMode: 0, // 直接传输
            encryptionEnabled: this.encryptionEnabled,
            encryptionKey: '' // 如果需要加密，这里应该包含加密密钥
        };
        
        // 序列化元数据
        const jsonData = JSON.stringify(metadata);
        const data = encoder.encode(jsonData);
        
        await this.webSocket.sendMessage(MessageType.MSG_FILE_UPLOAD_REQUEST, data.buffer, true);
    }
    
    // 上传文件块
    private async uploadChunks(): Promise<void> {
        this.status = FileTransferStatus.TRANSFERRING;
        
        for (let i = 0; i < this.totalChunks; i++) {
            if (this.cancelled) {
                throw new Error('Upload cancelled');
            }
            
            while (this.paused) {
                await new Promise(resolve => setTimeout(resolve, 100));
                if (this.cancelled) {
                    throw new Error('Upload cancelled');
                }
            }
            
            await this.uploadChunk(i);
            this.currentChunk = i + 1;
            this.bytesTransferred = Math.min(this.fileSize, (i + 1) * FileUpload.CHUNK_SIZE);
        }
    }
    
    // 上传单个块
    private async uploadChunk(chunkIndex: number): Promise<void> {
        const start = chunkIndex * FileUpload.CHUNK_SIZE;
        const end = Math.min(start + FileUpload.CHUNK_SIZE, this.fileSize);
        const chunkSize = end - start;
        
        // 读取文件块
        const chunk = await this.readFileChunk(start, end);
        
        // 计算块校验和
        const checksum = await this.calculateChecksum(chunk);
        
        // 准备块数据
        const data = new ArrayBuffer(8 + 4 + 4 + 4 + 4 + chunkSize + 64);
        const view = new DataView(data);
        let offset = 0;
        
        view.setBigUint64(offset, this.transferId, false); offset += 8;
        view.setUint32(offset, chunkIndex, false); offset += 4;
        view.setUint32(offset, chunkSize, false); offset += 4;
        view.setUint32(offset, this.totalChunks, false); offset += 4;
        view.setUint32(offset, chunkSize, false); offset += 4;
        
        // 复制块数据
        const chunkArray = new Uint8Array(data, offset, chunkSize);
        chunkArray.set(new Uint8Array(chunk));
        offset += chunkSize;
        
        // 复制校验和
        const checksumArray = new Uint8Array(data, offset, 64);
        const encoder = new TextEncoder();
        const checksumBytes = encoder.encode(checksum);
        checksumArray.set(checksumBytes.subarray(0, 64));
        
        // 发送块
        await this.webSocket.sendMessage(MessageType.MSG_FILE_UPLOAD_CHUNK, data, true);
    }
    
    // 发送完成通知
    private async sendCompletion(): Promise<void> {
        const data = new ArrayBuffer(8);
        const view = new DataView(data);
        view.setBigUint64(0, this.transferId, false);
        
        await this.webSocket.sendMessage(MessageType.MSG_FILE_UPLOAD_COMPLETE, data, true);
    }
    
    // 暂停上传
    pause(): void {
        this.paused = true;
        this.status = FileTransferStatus.PAUSED;
    }
    
    // 恢复上传
    resume(): void {
        this.paused = false;
        this.status = FileTransferStatus.TRANSFERRING;
    }
    
    // 取消上传
    cancel(): void {
        this.cancelled = true;
        this.paused = false;
        this.status = FileTransferStatus.CANCELLED;
    }
    
    // 处理上传响应
    handleUploadResponse(status: number, data: ArrayBuffer): void {
        if (status !== 200) {
            throw new Error(`Upload request failed with status: ${status}`);
        }
    }
    
    // 处理块响应
    handleChunkResponse(chunkIndex: number, status: number): void {
        if (status !== 200) {
            this.retryCount++;
            if (this.retryCount <= 3) {
                // 重试上传当前块
                this.uploadChunk(chunkIndex).catch(error => {
                    console.error('Chunk retry failed:', error);
                });
            } else {
                throw new Error(`Chunk ${chunkIndex} upload failed after ${this.retryCount} retries`);
            }
        } else {
            this.retryCount = 0;
        }
    }
    
    // 获取进度信息
    getProgress(): FileTransferProgress {
        const elapsed = (Date.now() - this.startTime) / 1000;
        const speed = elapsed > 0 ? this.bytesTransferred / elapsed : 0;
        const remainingBytes = this.fileSize - this.bytesTransferred;
        const estimatedTime = speed > 0 ? remainingBytes / speed : 0;
        
        return {
            transferId: this.transferId,
            fileName: this.file.name,
            fileSize: this.fileSize,
            bytesTransferred: this.bytesTransferred,
            progress: this.fileSize > 0 ? (this.bytesTransferred / this.fileSize) * 100 : 0,
            speed: speed,
            estimatedTime: estimatedTime,
            status: this.status
        };
    }
    
    // 计算文件哈希
    private async calculateFileHash(): Promise<string> {
        const arrayBuffer = await this.file.arrayBuffer();
        const hashBuffer = await crypto.subtle.digest('SHA-256', arrayBuffer);
        const hashArray = Array.from(new Uint8Array(hashBuffer));
        return hashArray.map(b => b.toString(16).padStart(2, '0')).join('');
    }
    
    // 计算校验和
    private async calculateChecksum(data: ArrayBuffer): Promise<string> {
        const hashBuffer = await crypto.subtle.digest('SHA-256', data);
        const hashArray = Array.from(new Uint8Array(hashBuffer));
        return hashArray.map(b => b.toString(16).padStart(2, '0')).join('');
    }
    
    // 读取文件块
    private async readFileChunk(start: number, end: number): Promise<ArrayBuffer> {
        const slice = this.file.slice(start, end);
        return await slice.arrayBuffer();
    }
    
    // 确定文件类型
    private determineFileType(filename: string): number {
        const extension = filename.toLowerCase().split('.').pop() || '';
        
        const imageExtensions = ['jpg', 'jpeg', 'png', 'gif', 'bmp', 'webp'];
        const audioExtensions = ['mp3', 'wav', 'ogg', 'flac', 'm4a'];
        const videoExtensions = ['mp4', 'avi', 'mov', 'wmv', 'flv', 'webm'];
        const documentExtensions = ['pdf', 'doc', 'docx', 'txt', 'rtf', 'odt'];
        const archiveExtensions = ['zip', 'rar', '7z', 'tar', 'gz'];
        const executableExtensions = ['exe', 'app', 'bat', 'sh'];
        
        if (imageExtensions.includes(extension)) return 1; // FILE_TYPE_IMAGE
        if (audioExtensions.includes(extension)) return 2; // FILE_TYPE_AUDIO
        if (videoExtensions.includes(extension)) return 3; // FILE_TYPE_VIDEO
        if (documentExtensions.includes(extension)) return 4; // FILE_TYPE_DOCUMENT
        if (archiveExtensions.includes(extension)) return 5; // FILE_TYPE_ARCHIVE
        if (executableExtensions.includes(extension)) return 6; // FILE_TYPE_EXECUTABLE
        
        return 0; // FILE_TYPE_UNKNOWN
    }
}

// 文件下载类
class FileDownload {
    transferId: bigint;
    private fileId: bigint;
    private targetPath?: string;
    private webSocket: SecureChatWebSocket;
    
    private fileSize: number = 0;
    private bytesTransferred: number = 0;
    private status: FileTransferStatus = FileTransferStatus.PENDING;
    private chunks: Map<number, ArrayBuffer> = new Map();
    private receivedChunks: Set<number> = new Set();
    private totalChunks: number = 0;
    
    constructor(
        transferId: bigint,
        fileId: bigint,
        targetPath: string | undefined,
        webSocket: SecureChatWebSocket
    ) {
        this.transferId = transferId;
        this.fileId = fileId;
        this.targetPath = targetPath;
        this.webSocket = webSocket;
    }
    
    // 开始下载
    async start(): Promise<void> {
        this.status = FileTransferStatus.PREPARING;
        
        try {
            // 1. 请求文件下载
            await this.requestDownload();
            
            // 2. 等待所有块接收完成
            await this.waitForCompletion();
            
            // 3. 组装文件
            await this.assembleFile();
            
            this.status = FileTransferStatus.COMPLETED;
            
        } catch (error) {
            this.status = FileTransferStatus.FAILED;
            throw error;
        }
    }
    
    // 请求下载
    private async requestDownload(): Promise<void> {
        const data = new ArrayBuffer(8);
        const view = new DataView(data);
        view.setBigUint64(0, this.fileId, false);
        
        await this.webSocket.sendMessage(MessageType.MSG_FILE_DOWNLOAD_REQUEST, data, true);
    }
    
    // 等待完成
    private async waitForCompletion(): Promise<void> {
        this.status = FileTransferStatus.TRANSFERRING;
        
        return new Promise((resolve, reject) => {
            const checkInterval = setInterval(() => {
                if (this.receivedChunks.size === this.totalChunks && this.totalChunks > 0) {
                    clearInterval(checkInterval);
                    resolve();
                }
            }, 100);
            
            // 设置超时
            setTimeout(() => {
                clearInterval(checkInterval);
                reject(new Error('Download timeout'));
            }, 5 * 60 * 1000); // 5分钟超时
        });
    }
    
    // 组装文件
    private async assembleFile(): Promise<void> {
        // 按顺序合并所有块
        const chunks: ArrayBuffer[] = [];
        for (let i = 0; i < this.totalChunks; i++) {
            const chunk = this.chunks.get(i);
            if (!chunk) {
                throw new Error(`Missing chunk ${i}`);
            }
            chunks.push(chunk);
        }
        
        // 合并为单个ArrayBuffer
        const totalSize = chunks.reduce((sum, chunk) => sum + chunk.byteLength, 0);
        const result = new Uint8Array(totalSize);
        let offset = 0;
        
        for (const chunk of chunks) {
            result.set(new Uint8Array(chunk), offset);
            offset += chunk.byteLength;
        }
        
        // 保存文件
        await this.saveFile(result.buffer);
    }
    
    // 保存文件
    private async saveFile(data: ArrayBuffer): Promise<void> {
        if (this.targetPath) {
            // 在浏览器环境中，我们使用下载方式
            const blob = new Blob([data]);
            const url = URL.createObjectURL(blob);
            const a = document.createElement('a');
            a.href = url;
            a.download = `downloaded_file_${this.fileId.toString()}`;
            document.body.appendChild(a);
            a.click();
            document.body.removeChild(a);
            URL.revokeObjectURL(url);
        }
    }
    
    // 暂停下载
    pause(): void {
        this.status = FileTransferStatus.PAUSED;
    }
    
    // 恢复下载
    resume(): void {
        this.status = FileTransferStatus.TRANSFERRING;
    }
    
    // 取消下载
    cancel(): void {
        this.status = FileTransferStatus.CANCELLED;
    }
    
    // 处理下载响应
    handleDownloadResponse(status: number, data: ArrayBuffer): void {
        if (status !== 200) {
            throw new Error(`Download request failed with status: ${status}`);
        }
        
        const view = new DataView(data);
        this.fileSize = Number(view.getBigUint64(8, false));
        this.totalChunks = view.getUint32(16, false);
    }
    
    // 处理下载块
    handleDownloadChunk(chunkIndex: number, data: ArrayBuffer): void {
        const view = new DataView(data);
        const chunkSize = view.getUint32(12, false);
        const totalChunks = view.getUint32(16, false);
        
        // 提取块数据
        const chunkData = data.slice(20, 20 + chunkSize);
        
        // 存储块
        this.chunks.set(chunkIndex, chunkData);
        this.receivedChunks.add(chunkIndex);
        this.bytesTransferred += chunkSize;
        
        // 更新总块数（如果未设置）
        if (this.totalChunks === 0) {
            this.totalChunks = totalChunks;
        }
    }
    
    // 获取进度信息
    getProgress(): FileTransferProgress {
        const progress = this.fileSize > 0 ? (this.bytesTransferred / this.fileSize) * 100 : 0;
        
        return {
            transferId: this.transferId,
            fileName: `File ${this.fileId.toString()}`,
            fileSize: this.fileSize,
            bytesTransferred: this.bytesTransferred,
            progress: progress,
            speed: 0, // 需要实现速度计算
            estimatedTime: 0, // 需要实现时间估计
            status: this.status
        };
    }
}