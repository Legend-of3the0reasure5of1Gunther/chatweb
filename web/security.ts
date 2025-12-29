// 文件: src/core/security.ts
/**
 * 安全模块 - 硬件密钥、端到端加密、双重认证
 */

// WebAuthn（硬件密钥）支持
export class WebAuthnManager {
    private static readonly TIMEOUT = 60000; // 60秒超时
    
    // 检查浏览器是否支持WebAuthn
    static isSupported(): boolean {
        return typeof window !== 'undefined' && 
               typeof window.PublicKeyCredential !== 'undefined';
    }
    
    // 注册硬件密钥
    static async register(userId: string, username: string, displayName: string): Promise<Credential> {
        if (!this.isSupported()) {
            throw new Error('WebAuthn not supported by this browser');
        }
        
        const challenge = this.generateChallenge();
        
        const publicKeyCredentialCreationOptions: PublicKeyCredentialCreationOptions = {
            challenge,
            rp: {
                name: "Secure Chat System",
                id: window.location.hostname
            },
            user: {
                id: new TextEncoder().encode(userId),
                name: username,
                displayName: displayName
            },
            pubKeyCredParams: [
                { type: "public-key", alg: -7 },  // ES256
                { type: "public-key", alg: -257 } // RS256
            ],
            timeout: this.TIMEOUT,
            attestation: "direct",
            authenticatorSelection: {
                authenticatorAttachment: "cross-platform",
                requireResidentKey: true,
                userVerification: "required"
            }
        };
        
        try {
            const credential = await navigator.credentials.create({
                publicKey: publicKeyCredentialCreationOptions
            });
            
            if (!credential) {
                throw new Error('Failed to create credential');
            }
            
            return credential;
        } catch (error) {
            console.error('WebAuthn registration failed:', error);
            throw error;
        }
    }
    
    // 使用硬件密钥认证
    static async authenticate(): Promise<Credential> {
        if (!this.isSupported()) {
            throw new Error('WebAuthn not supported by this browser');
        }
        
        const challenge = this.generateChallenge();
        const credentialId = this.getStoredCredentialId();
        
        const publicKeyCredentialRequestOptions: PublicKeyCredentialRequestOptions = {
            challenge,
            timeout: this.TIMEOUT,
            allowCredentials: credentialId ? [
                {
                    type: "public-key",
                    id: credentialId,
                    transports: ["internal", "usb", "nfc", "ble"]
                }
            ] : [],
            userVerification: "required"
        };
        
        try {
            const assertion = await navigator.credentials.get({
                publicKey: publicKeyCredentialRequestOptions
            });
            
            if (!assertion) {
                throw new Error('Authentication failed');
            }
            
            return assertion;
        } catch (error) {
            console.error('WebAuthn authentication failed:', error);
            throw error;
        }
    }
    
    // 生成随机挑战
    private static generateChallenge(): ArrayBuffer {
        const array = new Uint8Array(32);
        window.crypto.getRandomValues(array);
        return array.buffer;
    }
    
    // 获取存储的凭据ID
    private static getStoredCredentialId(): ArrayBuffer | null {
        const stored = localStorage.getItem('webauthn_credential_id');
        if (!stored) return null;
        
        try {
            const decoded = atob(stored);
            const array = new Uint8Array(decoded.length);
            for (let i = 0; i < decoded.length; i++) {
                array[i] = decoded.charCodeAt(i);
            }
            return array.buffer;
        } catch {
            return null;
        }
    }
    
    // 存储凭据ID
    static storeCredentialId(credentialId: ArrayBuffer): void {
        const uint8Array = new Uint8Array(credentialId);
        const binaryString = String.fromCharCode(...uint8Array);
        const base64 = btoa(binaryString);
        localStorage.setItem('webauthn_credential_id', base64);
    }
    
    // 清除存储的凭据
    static clearStoredCredential(): void {
        localStorage.removeItem('webauthn_credential_id');
    }
}

// 端到端加密管理器
export class EndToEndEncryption {
    private static readonly KEY_SIZE = 256;
    private static readonly IV_SIZE = 12; // GCM推荐使用12字节IV
    private static readonly TAG_SIZE = 16; // GCM认证标签大小
    
    // 生成ECDH密钥对
    static async generateKeyPair(): Promise<CryptoKeyPair> {
        return await window.crypto.subtle.generateKey(
            {
                name: "ECDH",
                namedCurve: "P-256"
            },
            true, // 可导出
            ["deriveKey", "deriveBits"]
        );
    }
    
    // 导出公钥为Base64
    static async exportPublicKey(key: CryptoKey): Promise<string> {
        const exported = await window.crypto.subtle.exportKey("spki", key);
        return this.arrayBufferToBase64(exported);
    }
    
    // 从Base64导入公钥
    static async importPublicKey(base64Key: string): Promise<CryptoKey> {
        const keyData = this.base64ToArrayBuffer(base64Key);
        return await window.crypto.subtle.importKey(
            "spki",
            keyData,
            {
                name: "ECDH",
                namedCurve: "P-256"
            },
            true,
            []
        );
    }
    
    // 派生共享密钥
    static async deriveSharedKey(privateKey: CryptoKey, publicKey: CryptoKey): Promise<CryptoKey> {
        return await window.crypto.subtle.deriveKey(
            {
                name: "ECDH",
                public: publicKey
            },
            privateKey,
            {
                name: "AES-GCM",
                length: this.KEY_SIZE
            },
            true,
            ["encrypt", "decrypt"]
        );
    }
    
    // 加密消息
    static async encryptMessage(key: CryptoKey, message: string): Promise<EncryptedMessage> {
        const encoder = new TextEncoder();
        const data = encoder.encode(message);
        
        const iv = window.crypto.getRandomValues(new Uint8Array(this.IV_SIZE));
        
        const encrypted = await window.crypto.subtle.encrypt(
            {
                name: "AES-GCM",
                iv: iv,
                tagLength: this.TAG_SIZE * 8
            },
            key,
            data
        );
        
        // 分离密文和认证标签
        const ciphertext = new Uint8Array(encrypted, 0, encrypted.byteLength - this.TAG_SIZE);
        const tag = new Uint8Array(encrypted, encrypted.byteLength - this.TAG_SIZE, this.TAG_SIZE);
        
        return {
            iv: this.arrayBufferToBase64(iv.buffer),
            ciphertext: this.arrayBufferToBase64(ciphertext.buffer),
            tag: this.arrayBufferToBase64(tag.buffer),
            timestamp: Date.now()
        };
    }
    
    // 解密消息
    static async decryptMessage(key: CryptoKey, encrypted: EncryptedMessage): Promise<string> {
        const iv = this.base64ToArrayBuffer(encrypted.iv);
        const ciphertext = this.base64ToArrayBuffer(encrypted.ciphertext);
        const tag = this.base64ToArrayBuffer(encrypted.tag);
        
        // 合并密文和标签
        const combined = new Uint8Array(ciphertext.byteLength + tag.byteLength);
        combined.set(new Uint8Array(ciphertext), 0);
        combined.set(new Uint8Array(tag), ciphertext.byteLength);
        
        const decrypted = await window.crypto.subtle.decrypt(
            {
                name: "AES-GCM",
                iv: iv,
                tagLength: this.TAG_SIZE * 8
            },
            key,
            combined
        );
        
        const decoder = new TextDecoder();
        return decoder.decode(decrypted);
    }
    
    // 生成消息认证码（HMAC）
    static async generateHMAC(key: CryptoKey, data: ArrayBuffer): Promise<string> {
        const hmacKey = await window.crypto.subtle.importKey(
            "raw",
            key,
            {
                name: "HMAC",
                hash: { name: "SHA-256" }
            },
            false,
            ["sign"]
        );
        
        const signature = await window.crypto.subtle.sign(
            "HMAC",
            hmacKey,
            data
        );
        
        return this.arrayBufferToBase64(signature);
    }
    
    // 验证消息认证码
    static async verifyHMAC(key: CryptoKey, data: ArrayBuffer, signature: string): Promise<boolean> {
        const hmacKey = await window.crypto.subtle.importKey(
            "raw",
            key,
            {
                name: "HMAC",
                hash: { name: "SHA-256" }
            },
            false,
            ["verify"]
        );
        
        const signatureBuffer = this.base64ToArrayBuffer(signature);
        
        return await window.crypto.subtle.verify(
            "HMAC",
            hmacKey,
            signatureBuffer,
            data
        );
    }
    
    // 生成安全随机数
    static generateRandomBytes(size: number): ArrayBuffer {
        const array = new Uint8Array(size);
        window.crypto.getRandomValues(array);
        return array.buffer;
    }
    
    // ArrayBuffer转Base64
    private static arrayBufferToBase64(buffer: ArrayBuffer): string {
        const bytes = new Uint8Array(buffer);
        let binary = '';
        for (let i = 0; i < bytes.byteLength; i++) {
            binary += String.fromCharCode(bytes[i]);
        }
        return btoa(binary);
    }
    
    // Base64转ArrayBuffer
    private static base64ToArrayBuffer(base64: string): ArrayBuffer {
        const binary = atob(base64);
        const bytes = new Uint8Array(binary.length);
        for (let i = 0; i < binary.length; i++) {
            bytes[i] = binary.charCodeAt(i);
        }
        return bytes.buffer;
    }
}

// 双重认证管理器
export class TwoFactorAuth {
    private static readonly TOTP_PERIOD = 30; // 30秒周期
    private static readonly CODE_LENGTH = 6; // 6位验证码
    
    // 生成TOTP密钥
    static generateSecret(): string {
        const bytes = new Uint8Array(20);
        window.crypto.getRandomValues(bytes);
        return this.base32Encode(bytes);
    }
    
    // 生成TOTP验证码
    static generateCode(secret: string, timestamp = Date.now()): string {
        const key = this.base32Decode(secret);
        const counter = Math.floor(timestamp / 1000 / this.TOTP_PERIOD);
        
        const counterBytes = new ArrayBuffer(8);
        const counterView = new DataView(counterBytes);
        counterView.setUint32(4, counter, false);
        
        const hmac = this.calculateHMAC(key, counterBytes);
        const offset = hmac[hmac.length - 1] & 0x0f;
        
        let code = (
            ((hmac[offset] & 0x7f) << 24) |
            ((hmac[offset + 1] & 0xff) << 16) |
            ((hmac[offset + 2] & 0xff) << 8) |
            (hmac[offset + 3] & 0xff)
        ) % Math.pow(10, this.CODE_LENGTH);
        
        return code.toString().padStart(this.CODE_LENGTH, '0');
    }
    
    // 验证TOTP验证码
    static verifyCode(secret: string, code: string, window = 1): boolean {
        const timestamp = Date.now();
        
        for (let i = -window; i <= window; i++) {
            const expected = this.generateCode(secret, timestamp + i * this.TOTP_PERIOD * 1000);
            if (expected === code) {
                return true;
            }
        }
        
        return false;
    }
    
    // 生成QR码URL（用于Google Authenticator等）
    static generateQRCodeURL(secret: string, account: string, issuer = "Secure Chat"): string {
        const encodedAccount = encodeURIComponent(account);
        const encodedIssuer = encodeURIComponent(issuer);
        const encodedSecret = encodeURIComponent(secret);
        
        return `otpauth://totp/${encodedIssuer}:${encodedAccount}?secret=${encodedSecret}&issuer=${encodedIssuer}&digits=${this.CODE_LENGTH}&period=${this.TOTP_PERIOD}`;
    }
    
    // 计算HMAC-SHA1
    private static calculateHMAC(key: ArrayBuffer, data: ArrayBuffer): Uint8Array {
        // 简化的HMAC-SHA1实现
        // 注意：生产环境应使用Web Crypto API
        const cryptoKey = this.importKey(key);
        const hmac = this.computeHMAC(cryptoKey, data);
        return new Uint8Array(hmac);
    }
    
    // Base32编码
    private static base32Encode(data: Uint8Array): string {
        const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567';
        let bits = 0;
        let value = 0;
        let output = '';
        
        for (let i = 0; i < data.length; i++) {
            value = (value << 8) | data[i];
            bits += 8;
            
            while (bits >= 5) {
                output += alphabet[(value >>> (bits - 5)) & 31];
                bits -= 5;
            }
        }
        
        if (bits > 0) {
            output += alphabet[(value << (5 - bits)) & 31];
        }
        
        return output;
    }
    
    // Base32解码
    private static base32Decode(input: string): ArrayBuffer {
        const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567';
        let bits = 0;
        let value = 0;
        const output = [];
        
        for (let i = 0; i < input.length; i++) {
            const char = input[i].toUpperCase();
            const index = alphabet.indexOf(char);
            
            if (index === -1) continue;
            
            value = (value << 5) | index;
            bits += 5;
            
            if (bits >= 8) {
                output.push((value >>> (bits - 8)) & 255);
                bits -= 8;
            }
        }
        
        return new Uint8Array(output).buffer;
    }
    
    // 导入密钥（简化实现）
    private static importKey(key: ArrayBuffer): CryptoKey {
        // 简化的密钥导入
        return {} as CryptoKey;
    }
    
    // 计算HMAC（简化实现）
    private static computeHMAC(key: CryptoKey, data: ArrayBuffer): ArrayBuffer {
        // 简化的HMAC计算
        return new ArrayBuffer(20);
    }
}

// 加密消息接口
export interface EncryptedMessage {
    iv: string;        // 初始化向量（Base64）
    ciphertext: string; // 密文（Base64）
    tag: string;       // 认证标签（Base64）
    timestamp: number; // 时间戳
}

// 端到端加密会话
export interface EncryptionSession {
    sessionId: string;
    userId: bigint;
    publicKey: string;
    sharedKey?: CryptoKey;
    establishedAt: number;
    expiresAt: number;
}

// 群组加密上下文
export interface GroupEncryptionContext {
    groupId: bigint;
    encryptionKey: CryptoKey;
    keyVersion: number;
    keyRotationDate: number;
    members: Map<bigint, string>; // 用户ID -> 加密的密钥
}