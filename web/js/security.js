// 文件：web/js/security.js
/**
 * 安全聊天系统加密模块
 * 负责端到端加密、密钥管理、安全存储
 */

class SecurityManager {
    constructor() {
        this.encryptionKeys = new Map(); // 存储对端加密密钥
        this.localKeyPair = null;
        this.keyDerivationSalt = null;
        this.keyStorage = null;
        
        // 加密算法配置
        this.encryptionConfig = {
            // AES-GCM用于消息加密
            messageEncryption: {
                name: 'AES-GCM',
                length: 256,
                ivLength: 12, // 12字节IV是AES-GCM的标准
                tagLength: 128
            },
            // ECDH用于密钥交换
            keyExchange: {
                name: 'ECDH',
                namedCurve: 'P-256'
            },
            // PBKDF2用于密码派生
            passwordDerivation: {
                name: 'PBKDF2',
                iterations: 100000,
                hash: 'SHA-256'
            },
            // Argon2id用于密码哈希（通过WebAssembly）
            passwordHashing: {
                memory: 65536,
                iterations: 3,
                parallelism: 4,
                hashLength: 32
            }
        };
    }

    async init() {
        try {
            // 检查Web Crypto API支持
            if (!window.crypto || !window.crypto.subtle) {
                throw new Error('浏览器不支持Web Crypto API');
            }
            
            // 初始化密钥对
            await this.generateKeyPair();
            
            // 初始化密钥存储
            await this.initKeyStorage();
            
            // 加载已有的加密密钥
            await this.loadStoredKeys();
            
            console.log('安全模块初始化成功');
            return true;
            
        } catch (error) {
            console.error('安全模块初始化失败:', error);
            throw error;
        }
    }

    async generateKeyPair() {
        try {
            this.localKeyPair = await crypto.subtle.generateKey(
                {
                    name: this.encryptionConfig.keyExchange.name,
                    namedCurve: this.encryptionConfig.keyExchange.namedCurve
                },
                true, // 可导出
                ['deriveKey', 'deriveBits']
            );
            
            console.log('密钥对生成成功');
            
        } catch (error) {
            console.error('密钥对生成失败:', error);
            throw error;
        }
    }

    async initKeyStorage() {
        // 使用IndexedDB存储密钥
        return new Promise((resolve, reject) => {
            const request = indexedDB.open('secure_chat_keys', 1);
            
            request.onupgradeneeded = (event) => {
                const db = event.target.result;
                
                // 创建对象存储
                if (!db.objectStoreNames.contains('encryption_keys')) {
                    const store = db.createObjectStore('encryption_keys', { keyPath: 'userId' });
                    store.createIndex('by_user', 'userId', { unique: true });
                }
                
                if (!db.objectStoreNames.contains('key_pairs')) {
                    db.createObjectStore('key_pairs', { keyPath: 'id' });
                }
                
                if (!db.objectStoreNames.contains('session_data')) {
                    db.createObjectStore('session_data', { keyPath: 'key' });
                }
            };
            
            request.onsuccess = (event) => {
                this.keyStorage = event.target.result;
                console.log('密钥存储初始化成功');
                resolve();
            };
            
            request.onerror = (event) => {
                console.error('密钥存储初始化失败:', event.target.error);
                reject(new Error('无法初始化密钥存储'));
            };
        });
    }

    async loadStoredKeys() {
        if (!this.keyStorage) {
            return;
        }
        
        return new Promise((resolve, reject) => {
            const transaction = this.keyStorage.transaction(['encryption_keys'], 'readonly');
            const store = transaction.objectStore('encryption_keys');
            const request = store.openCursor();
            
            request.onsuccess = (event) => {
                const cursor = event.target.result;
                if (cursor) {
                    this.encryptionKeys.set(cursor.value.userId, cursor.value.key);
                    cursor.continue();
                } else {
                    resolve();
                }
            };
            
            request.onerror = (event) => {
                console.error('加载存储密钥失败:', event.target.error);
                reject(event.target.error);
            };
        });
    }

    async saveEncryptionKey(userId, keyData) {
        if (!this.keyStorage) {
            return;
        }
        
        return new Promise((resolve, reject) => {
            const transaction = this.keyStorage.transaction(['encryption_keys'], 'readwrite');
            const store = transaction.objectStore('encryption_keys');
            
            const request = store.put({
                userId: userId,
                key: keyData,
                timestamp: Date.now()
            });
            
            request.onsuccess = () => {
                this.encryptionKeys.set(userId, keyData);
                resolve();
            };
            
            request.onerror = (event) => {
                console.error('保存加密密钥失败:', event.target.error);
                reject(event.target.error);
            };
        });
    }

    async getEncryptionKey(userId) {
        // 先从内存缓存获取
        if (this.encryptionKeys.has(userId)) {
            return this.encryptionKeys.get(userId);
        }
        
        // 从存储中加载
        if (this.keyStorage) {
            try {
                const keyData = await this.loadKeyFromStorage(userId);
                if (keyData) {
                    this.encryptionKeys.set(userId, keyData);
                    return keyData;
                }
            } catch (error) {
                console.warn('从存储加载密钥失败:', error);
            }
        }
        
        // 如果都没有，返回null
        return null;
    }

    async loadKeyFromStorage(userId) {
        return new Promise((resolve, reject) => {
            const transaction = this.keyStorage.transaction(['encryption_keys'], 'readonly');
            const store = transaction.objectStore('encryption_keys');
            const request = store.get(userId);
            
            request.onsuccess = (event) => {
                resolve(event.target.result?.key || null);
            };
            
            request.onerror = (event) => {
                reject(event.target.error);
            };
        });
    }

    async hashPassword(password) {
        // 使用SHA-256哈希密码
        const encoder = new TextEncoder();
        const data = encoder.encode(password);
        
        const hashBuffer = await crypto.subtle.digest('SHA-256', data);
        const hashArray = Array.from(new Uint8Array(hashBuffer));
        const hashHex = hashArray.map(b => b.toString(16).padStart(2, '0')).join('');
        
        return hashHex;
    }

    async deriveEncryptionKey(sharedSecret, salt = null) {
        try {
            if (!salt) {
                salt = crypto.getRandomValues(new Uint8Array(16));
            }
            
            const keyMaterial = await crypto.subtle.importKey(
                'raw',
                sharedSecret,
                'PBKDF2',
                false,
                ['deriveKey']
            );
            
            const encryptionKey = await crypto.subtle.deriveKey(
                {
                    name: 'PBKDF2',
                    salt: salt,
                    iterations: 100000,
                    hash: 'SHA-256'
                },
                keyMaterial,
                {
                    name: 'AES-GCM',
                    length: 256
                },
                false, // 不可导出
                ['encrypt', 'decrypt']
            );
            
            return {
                key: encryptionKey,
                salt: salt
            };
            
        } catch (error) {
            console.error('派生加密密钥失败:', error);
            throw error;
        }
    }

    async encryptMessage(message, receiverPublicKey = null) {
        try {
            let encryptionKey;
            
            if (receiverPublicKey) {
                // 使用接收者的公钥进行加密
                encryptionKey = await this.deriveKeyForUser(receiverPublicKey);
            } else {
                // 使用默认密钥
                encryptionKey = await this.getDefaultEncryptionKey();
            }
            
            if (!encryptionKey) {
                throw new Error('没有可用的加密密钥');
            }
            
            const iv = crypto.getRandomValues(
                new Uint8Array(this.encryptionConfig.messageEncryption.ivLength)
            );
            
            const encoder = new TextEncoder();
            const data = encoder.encode(message);
            
            const encryptedData = await crypto.subtle.encrypt(
                {
                    name: this.encryptionConfig.messageEncryption.name,
                    iv: iv,
                    tagLength: this.encryptionConfig.messageEncryption.tagLength
                },
                encryptionKey,
                data
            );
            
            // 组合IV和加密数据
            const result = new Uint8Array(iv.length + encryptedData.byteLength);
            result.set(iv, 0);
            result.set(new Uint8Array(encryptedData), iv.length);
            
            return this.arrayBufferToBase64(result.buffer);
            
        } catch (error) {
            console.error('消息加密失败:', error);
            throw new Error('加密失败: ' + error.message);
        }
    }

    async decryptMessage(encryptedMessage, senderPublicKey = null) {
        try {
            let decryptionKey;
            
            if (senderPublicKey) {
                // 使用发送者的公钥进行解密
                decryptionKey = await this.deriveKeyForUser(senderPublicKey);
            } else {
                // 使用默认密钥
                decryptionKey = await this.getDefaultEncryptionKey();
            }
            
            if (!decryptionKey) {
                throw new Error('没有可用的解密密钥');
            }
            
            const encryptedBuffer = this.base64ToArrayBuffer(encryptedMessage);
            const encryptedArray = new Uint8Array(encryptedBuffer);
            
            const iv = encryptedArray.slice(0, this.encryptionConfig.messageEncryption.ivLength);
            const data = encryptedArray.slice(this.encryptionConfig.messageEncryption.ivLength);
            
            const decryptedData = await crypto.subtle.decrypt(
                {
                    name: this.encryptionConfig.messageEncryption.name,
                    iv: iv,
                    tagLength: this.encryptionConfig.messageEncryption.tagLength
                },
                decryptionKey,
                data
            );
            
            const decoder = new TextDecoder();
            return decoder.decode(decryptedData);
            
        } catch (error) {
            console.error('消息解密失败:', error);
            throw new Error('解密失败: ' + error.message);
        }
    }

    async deriveKeyForUser(publicKeyJwk) {
        try {
            // 导入对方公钥
            const otherPublicKey = await crypto.subtle.importKey(
                'jwk',
                publicKeyJwk,
                {
                    name: this.encryptionConfig.keyExchange.name,
                    namedCurve: this.encryptionConfig.keyExchange.namedCurve
                },
                true,
                []
            );
            
            // 生成共享密钥
            const sharedSecret = await crypto.subtle.deriveBits(
                {
                    name: this.encryptionConfig.keyExchange.name,
                    public: otherPublicKey
                },
                this.localKeyPair.privateKey,
                256
            );
            
            // 派生加密密钥
            const derivedKey = await this.deriveEncryptionKey(sharedSecret);
            
            return derivedKey.key;
            
        } catch (error) {
            console.error('派生用户密钥失败:', error);
            throw error;
        }
    }

    async getDefaultEncryptionKey() {
        // 生成或获取默认加密密钥
        const defaultKeyId = 'default_encryption_key';
        
        // 从存储中获取
        if (this.keyStorage) {
            try {
                const keyData = await this.loadKeyFromStorage(defaultKeyId);
                if (keyData) {
                    return await crypto.subtle.importKey(
                        'jwk',
                        keyData,
                        {
                            name: this.encryptionConfig.messageEncryption.name,
                            length: this.encryptionConfig.messageEncryption.length
                        },
                        false,
                        ['encrypt', 'decrypt']
                    );
                }
            } catch (error) {
                console.warn('加载默认密钥失败:', error);
            }
        }
        
        // 生成新的默认密钥
        const key = await crypto.subtle.generateKey(
            {
                name: this.encryptionConfig.messageEncryption.name,
                length: this.encryptionConfig.messageEncryption.length
            },
            true, // 可导出
            ['encrypt', 'decrypt']
        );
        
        // 导出并保存
        const exportedKey = await crypto.subtle.exportKey('jwk', key);
        await this.saveEncryptionKey(defaultKeyId, exportedKey);
        
        return key;
    }

    async calculateFileHash(file) {
        return new Promise((resolve, reject) => {
            const reader = new FileReader();
            
            reader.onload = async (e) => {
                try {
                    const arrayBuffer = e.target.result;
                    const hashBuffer = await crypto.subtle.digest('SHA-256', arrayBuffer);
                    
                    const hashArray = Array.from(new Uint8Array(hashBuffer));
                    const hashHex = hashArray.map(b => b.toString(16).padStart(2, '0')).join('');
                    
                    resolve(hashHex);
                } catch (error) {
                    reject(error);
                }
            };
            
            reader.onerror = (error) => {
                reject(error);
            };
            
            reader.readAsArrayBuffer(file);
        });
    }

    async verifyFileIntegrity(file, expectedHash) {
        const actualHash = await this.calculateFileHash(file);
        return actualHash === expectedHash;
    }

    // 密钥交换协议
    async initiateKeyExchange(userId) {
        try {
            // 导出公钥
            const exportedPublicKey = await crypto.subtle.exportKey(
                'jwk',
                this.localKeyPair.publicKey
            );
            
            return {
                userId: userId,
                publicKey: exportedPublicKey,
                timestamp: Date.now(),
                algorithm: this.encryptionConfig.keyExchange.name
            };
            
        } catch (error) {
            console.error('初始化密钥交换失败:', error);
            throw error;
        }
    }

    async completeKeyExchange(userId, remotePublicKeyJwk) {
        try {
            // 为对方派生加密密钥
            const encryptionKey = await this.deriveKeyForUser(remotePublicKeyJwk);
            
            // 保存密钥
            const exportedKey = await crypto.subtle.exportKey('jwk', encryptionKey);
            await this.saveEncryptionKey(userId, exportedKey);
            
            return true;
            
        } catch (error) {
            console.error('完成密钥交换失败:', error);
            throw error;
        }
    }

    // 工具方法
    arrayBufferToBase64(buffer) {
        const bytes = new Uint8Array(buffer);
        let binary = '';
        for (let i = 0; i < bytes.byteLength; i++) {
            binary += String.fromCharCode(bytes[i]);
        }
        return btoa(binary);
    }

    base64ToArrayBuffer(base64) {
        const binary = atob(base64);
        const bytes = new Uint8Array(binary.length);
        for (let i = 0; i < binary.length; i++) {
            bytes[i] = binary.charCodeAt(i);
        }
        return bytes.buffer;
    }

    // 清理和重置
    async clearAllKeys() {
        this.encryptionKeys.clear();
        
        if (this.keyStorage) {
            return new Promise((resolve, reject) => {
                const transaction = this.keyStorage.transaction(
                    ['encryption_keys', 'key_pairs', 'session_data'],
                    'readwrite'
                );
                
                transaction.objectStore('encryption_keys').clear();
                transaction.objectStore('key_pairs').clear();
                transaction.objectStore('session_data').clear();
                
                transaction.oncomplete = () => {
                    resolve();
                };
                
                transaction.onerror = (event) => {
                    reject(event.target.error);
                };
            });
        }
    }

    // 公开方法
    getPublicKey() {
        if (!this.localKeyPair) {
            return null;
        }
        
        return crypto.subtle.exportKey('jwk', this.localKeyPair.publicKey);
    }

    async getPublicKeyBase64() {
        const jwk = await this.getPublicKey();
        if (!jwk) {
            return null;
        }
        
        return this.arrayBufferToBase64(
            new TextEncoder().encode(JSON.stringify(jwk))
        );
    }

    getEncryptionStatus() {
        return {
            hasKeyPair: !!this.localKeyPair,
            storedKeys: this.encryptionKeys.size,
            isInitialized: !!this.keyStorage
        };
    }
}

export default SecurityManager;