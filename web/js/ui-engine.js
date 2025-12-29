// 文件：web/js/ui-engine.js
/**
 * 安全聊天系统UI引擎
 * 负责界面状态管理、消息渲染、用户交互
 */

class UIEngine {
    constructor() {
        this.currentState = 'login';
        this.currentChatPartner = null;
        this.messages = new Map(); // userId -> message array
        this.contacts = new Map();
        this.groups = new Map();
        this.userInfo = null;
        
        // UI元素缓存
        this.elements = {};
        
        // 事件监听器
        this.eventListeners = new Map();
        
        // 主题配置
        this.theme = 'dark';
        
        // 配置
        this.config = {
            autoScroll: true,
            showReadReceipts: true,
            showTypingIndicator: true,
            messageSound: true,
            notificationSound: true,
            enableAnimations: true
        };
    }

    async init() {
        try {
            // 缓存DOM元素
            this.cacheElements();
            
            // 加载配置
            await this.loadConfig();
            
            // 设置事件监听
            this.setupEventListeners();
            
            // 应用主题
            this.applyTheme();
            
            // 初始化界面
            this.initInterface();
            
            console.log('UI引擎初始化成功');
            return true;
            
        } catch (error) {
            console.error('UI引擎初始化失败:', error);
            throw error;
        }
    }

    cacheElements() {
        // 登录界面
        this.elements.loginScreen = document.getElementById('login-screen');
        this.elements.registerScreen = document.getElementById('register-screen');
        this.elements.chatScreen = document.getElementById('chat-screen');
        
        // 登录表单
        this.elements.loginForm = document.getElementById('login-form');
        this.elements.registerForm = document.getElementById('register-form');
        this.elements.usernameInput = document.getElementById('username');
        this.elements.passwordInput = document.getElementById('password');
        
        // 聊天界面
        this.elements.userAvatar = document.getElementById('user-avatar');
        this.elements.userName = document.getElementById('display-name');
        this.elements.userStatus = document.getElementById('user-status');
        
        this.elements.partnerAvatar = document.getElementById('partner-avatar');
        this.elements.partnerName = document.getElementById('partner-name');
        this.elements.partnerStatus = document.getElementById('partner-status');
        
        this.elements.messageContainer = document.getElementById('messages-container');
        this.elements.messagesList = document.getElementById('messages');
        this.elements.messageInput = document.getElementById('message-input');
        this.elements.sendButton = document.getElementById('send-btn');
        
        this.elements.contactsList = document.getElementById('contacts');
        this.elements.groupsList = document.getElementById('groups');
        
        this.elements.searchInput = document.getElementById('search-input');
        
        // 文件传输
        this.elements.fileSidebar = document.getElementById('file-sidebar');
        this.elements.fileTransfers = document.getElementById('file-transfers');
        
        // 连接状态
        this.elements.connectionIndicator = document.getElementById('connection-indicator');
        this.elements.connectionText = document.getElementById('connection-text');
    }

    async loadConfig() {
        try {
            const savedConfig = localStorage.getItem('secure_chat_ui_config');
            if (savedConfig) {
                this.config = { ...this.config, ...JSON.parse(savedConfig) };
            }
        } catch (error) {
            console.warn('加载配置失败:', error);
        }
    }

    saveConfig() {
        try {
            localStorage.setItem('secure_chat_ui_config', JSON.stringify(this.config));
        } catch (error) {
            console.error('保存配置失败:', error);
        }
    }

    setupEventListeners() {
        // 登录/注册切换
        const registerBtn = document.getElementById('register-btn');
        const backToLoginBtn = document.getElementById('back-to-login');
        
        if (registerBtn) {
            registerBtn.addEventListener('click', () => {
                this.showRegisterInterface();
            });
        }
        
        if (backToLoginBtn) {
            backToLoginBtn.addEventListener('click', () => {
                this.showLoginInterface();
            });
        }
        
        // 消息发送
        if (this.elements.sendButton) {
            this.elements.sendButton.addEventListener('click', () => {
                this.handleSendMessage();
            });
        }
        
        if (this.elements.messageInput) {
            this.elements.messageInput.addEventListener('keydown', (e) => {
                if (e.key === 'Enter' && !e.shiftKey) {
                    e.preventDefault();
                    this.handleSendMessage();
                }
            });
            
            this.elements.messageInput.addEventListener('input', () => {
                this.handleTyping();
            });
        }
        
        // 搜索
        if (this.elements.searchInput) {
            this.elements.searchInput.addEventListener('input', () => {
                this.handleSearch();
            });
        }
        
        // 文件传输
        const attachBtn = document.getElementById('attach-btn');
        const fileTransferBtn = document.getElementById('file-transfer');
        
        if (attachBtn) {
            attachBtn.addEventListener('click', () => {
                this.showFileSelector();
            });
        }
        
        if (fileTransferBtn) {
            fileTransferBtn.addEventListener('click', () => {
                this.toggleFileSidebar();
            });
        }
        
        // 设置
        const settingsBtn = document.getElementById('chat-settings');
        if (settingsBtn) {
            settingsBtn.addEventListener('click', () => {
                this.showSettings();
            });
        }
    }

    initInterface() {
        // 初始显示登录界面
        this.showLoginInterface();
        
        // 应用配置
        this.applyConfig();
    }

    showLoginInterface() {
        this.currentState = 'login';
        
        this.hideAllScreens();
        this.showElement(this.elements.loginScreen);
        
        // 清空表单
        if (this.elements.usernameInput) this.elements.usernameInput.value = '';
        if (this.elements.passwordInput) this.elements.passwordInput.value = '';
        
        this.emit('interface_changed', 'login');
    }

    showRegisterInterface() {
        this.hideAllScreens();
        this.showElement(this.elements.registerScreen);
        
        this.emit('interface_changed', 'register');
    }

    showMainInterface() {
        this.currentState = 'chat';
        
        this.hideAllScreens();
        this.showElement(this.elements.chatScreen);
        
        // 初始化聊天界面
        this.initChatInterface();
        
        this.emit('interface_changed', 'chat');
    }

    hideAllScreens() {
        [this.elements.loginScreen, 
         this.elements.registerScreen, 
         this.elements.chatScreen].forEach(screen => {
            if (screen) screen.classList.remove('active');
        });
    }

    showElement(element) {
        if (element) {
            element.classList.add('active');
        }
    }

    initChatInterface() {
        // 更新用户信息
        this.updateUserInfo();
        
        // 清空消息列表
        this.clearMessages();
        
        // 加载联系人
        this.loadContacts();
        
        // 设置初始状态
        this.updateConnectionStatus(false);
    }

    updateUserInfo(info = null) {
        if (info) {
            this.userInfo = info;
        }
        
        if (this.userInfo && this.elements.userName && this.elements.userStatus) {
            this.elements.userName.textContent = this.userInfo.nickname || this.userInfo.username;
            this.elements.userStatus.textContent = this.getStatusText(this.userInfo.status);
            this.elements.userStatus.className = `status-${this.getStatusClass(this.userInfo.status)}`;
            
            // 更新头像
            if (this.userInfo.avatar_id && this.elements.userAvatar) {
                this.elements.userAvatar.src = `/avatars/${this.userInfo.avatar_id}.png`;
            }
        }
    }

    updateChatPartner(partner) {
        this.currentChatPartner = partner;
        
        if (partner && this.elements.partnerName && this.elements.partnerStatus) {
            this.elements.partnerName.textContent = partner.nickname || partner.username;
            this.elements.partnerStatus.textContent = this.getStatusText(partner.status);
            this.elements.partnerStatus.className = `status-${this.getStatusClass(partner.status)}`;
            
            // 更新头像
            if (partner.avatar_id && this.elements.partnerAvatar) {
                this.elements.partnerAvatar.src = `/avatars/${partner.avatar_id}.png`;
            }
            
            // 启用发送按钮
            if (this.elements.sendButton) {
                this.elements.sendButton.disabled = false;
            }
            
            // 加载消息历史
            this.loadMessageHistory(partner.userId);
        }
    }

    async loadContacts() {
        try {
            // 这里应该从服务器获取联系人列表
            // 暂时使用模拟数据
            const mockContacts = [
                { userId: 2, username: 'alice', nickname: 'Alice', status: 1, avatar_id: 2 },
                { userId: 3, username: 'bob', nickname: 'Bob', status: 2, avatar_id: 3 },
                { userId: 4, username: 'charlie', nickname: 'Charlie', status: 1, avatar_id: 4 }
            ];
            
            this.renderContacts(mockContacts);
            
        } catch (error) {
            console.error('加载联系人失败:', error);
            this.showError('加载联系人失败', error.message);
        }
    }

    renderContacts(contacts) {
        if (!this.elements.contactsList) {
            return;
        }
        
        this.elements.contactsList.innerHTML = '';
        
        contacts.forEach(contact => {
            const contactElement = this.createContactElement(contact);
            this.elements.contactsList.appendChild(contactElement);
            
            // 存储联系人
            this.contacts.set(contact.userId, contact);
        });
    }

    createContactElement(contact) {
        const li = document.createElement('li');
        li.className = 'contact-item';
        li.dataset.userId = contact.userId;
        
        const statusClass = this.getStatusClass(contact.status);
        const statusText = this.getStatusText(contact.status);
        
        li.innerHTML = `
            <img src="/avatars/${contact.avatar_id || 0}.png" alt="${contact.username}" class="contact-avatar">
            <div class="contact-info">
                <span class="contact-name">${contact.nickname || contact.username}</span>
                <span class="contact-status status-${statusClass}">${statusText}</span>
            </div>
            <span class="unread-count hidden">0</span>
        `;
        
        // 点击事件
        li.addEventListener('click', () => {
            this.selectContact(contact.userId);
        });
        
        return li;
    }

    selectContact(userId) {
        const contact = this.contacts.get(userId);
        if (contact) {
            this.updateChatPartner(contact);
            
            // 更新选中状态
            document.querySelectorAll('.contact-item').forEach(item => {
                item.classList.remove('active');
            });
            
            const contactElement = document.querySelector(`.contact-item[data-user-id="${userId}"]`);
            if (contactElement) {
                contactElement.classList.add('active');
            }
        }
    }

    addMessage(message) {
        if (!this.currentChatPartner || message.sender_id !== this.currentChatPartner.userId) {
            // 如果不是当前聊天对象的消息，显示通知
            this.showMessageNotification(message);
            return;
        }
        
        // 添加到消息列表
        if (!this.messages.has(message.sender_id)) {
            this.messages.set(message.sender_id, []);
        }
        
        this.messages.get(message.sender_id).push(message);
        
        // 渲染消息
        this.renderMessage(message);
        
        // 滚动到底部
        if (this.config.autoScroll && this.elements.messageContainer) {
            this.elements.messageContainer.scrollTop = this.elements.messageContainer.scrollHeight;
        }
        
        // 播放消息声音
        if (this.config.messageSound && message.sender_id !== this.userInfo?.userId) {
            this.playMessageSound();
        }
    }

    renderMessage(message) {
        if (!this.elements.messagesList) {
            return;
        }
        
        const messageElement = this.createMessageElement(message);
        
        // 添加消息到列表
        this.elements.messagesList.appendChild(messageElement);
        
        // 添加动画
        if (this.config.enableAnimations) {
            messageElement.classList.add('message-enter');
            setTimeout(() => {
                messageElement.classList.remove('message-enter');
            }, 300);
        }
    }

    createMessageElement(message) {
        const isOwnMessage = message.sender_id === this.userInfo?.userId;
        const messageClass = isOwnMessage ? 'message outgoing' : 'message incoming';
        
        const div = document.createElement('div');
        div.className = messageClass;
        div.dataset.messageId = message.message_id;
        
        const time = new Date(message.timestamp).toLocaleTimeString([], { 
            hour: '2-digit', 
            minute: '2-digit' 
        });
        
        let statusIndicator = '';
        if (isOwnMessage && this.config.showReadReceipts) {
            statusIndicator = `<span class="message-status">${this.getMessageStatusIcon(message.status)}</span>`;
        }
        
        div.innerHTML = `
            <div class="message-content">
                ${message.encrypted ? '<i class="fas fa-lock message-encrypted-icon"></i>' : ''}
                <p>${this.escapeHtml(message.content)}</p>
                <div class="message-meta">
                    <span class="message-time">${time}</span>
                    ${statusIndicator}
                </div>
            </div>
        `;
        
        return div;
    }

    clearMessages() {
        if (this.elements.messagesList) {
            this.elements.messagesList.innerHTML = '';
        }
    }

    async handleSendMessage() {
        if (!this.elements.messageInput || !this.currentChatPartner) {
            return;
        }
        
        const content = this.elements.messageInput.value.trim();
        if (!content) {
            return;
        }
        
        try {
            this.emit('send_message', {
                receiverId: this.currentChatPartner.userId,
                content: content
            });
            
            // 清空输入框
            this.elements.messageInput.value = '';
            
            // 重置输入框高度
            this.elements.messageInput.style.height = 'auto';
            
        } catch (error) {
            this.showError('发送消息失败', error.message);
        }
    }

    handleTyping() {
        if (this.currentChatPartner) {
            this.emit('typing', {
                receiverId: this.currentChatPartner.userId
            });
        }
    }

    handleSearch() {
        const query = this.elements.searchInput?.value.toLowerCase() || '';
        
        if (!query) {
            // 显示所有联系人
            document.querySelectorAll('.contact-item').forEach(item => {
                item.style.display = '';
            });
            return;
        }
        
        // 过滤联系人
        document.querySelectorAll('.contact-item').forEach(item => {
            const name = item.querySelector('.contact-name')?.textContent.toLowerCase() || '';
            const visible = name.includes(query);
            item.style.display = visible ? '' : 'none';
        });
    }

    updateConnectionStatus(connected) {
        if (this.elements.connectionIndicator && this.elements.connectionText) {
            if (connected) {
                this.elements.connectionIndicator.className = 'status-online';
                this.elements.connectionText.textContent = '已连接';
            } else {
                this.elements.connectionIndicator.className = 'status-offline';
                this.elements.connectionText.textContent = '未连接';
            }
        }
    }

    async showFileSelector() {
        const input = document.createElement('input');
        input.type = 'file';
        input.multiple = false;
        
        input.addEventListener('change', async (event) => {
            const file = event.target.files[0];
            if (file && this.currentChatPartner) {
                try {
                    this.emit('send_file', {
                        file: file,
                        receiverId: this.currentChatPartner.userId
                    });
                } catch (error) {
                    this.showError('文件发送失败', error.message);
                }
            }
        });
        
        input.click();
    }

    toggleFileSidebar() {
        if (this.elements.fileSidebar) {
            const isVisible = !this.elements.fileSidebar.classList.contains('hidden');
            if (isVisible) {
                this.elements.fileSidebar.classList.add('hidden');
            } else {
                this.elements.fileSidebar.classList.remove('hidden');
            }
        }
    }

    addFileTransfer(transfer) {
        if (!this.elements.fileTransfers) {
            return;
        }
        
        const transferElement = this.createFileTransferElement(transfer);
        this.elements.fileTransfers.appendChild(transferElement);
        
        // 显示侧边栏
        this.elements.fileSidebar?.classList.remove('hidden');
    }

    createFileTransferElement(transfer) {
        const div = document.createElement('div');
        div.className = 'transfer-item';
        div.dataset.transferId = transfer.transferId;
        
        const progress = transfer.progress || 0;
        const isUpload = transfer.direction === 'upload';
        
        div.innerHTML = `
            <div class="transfer-info">
                <div class="transfer-filename">${transfer.filename}</div>
                <div class="transfer-size">${this.formatFileSize(transfer.fileSize)}</div>
            </div>
            <div class="transfer-progress">
                <div class="progress-bar">
                    <div class="progress-fill" style="width: ${progress}%"></div>
                </div>
                <div class="transfer-status">${isUpload ? '上传中' : '下载中'} ${progress.toFixed(1)}%</div>
            </div>
            <div class="transfer-controls">
                <button class="btn-icon transfer-cancel" title="取消">
                    <i class="fas fa-times"></i>
                </button>
            </div>
        `;
        
        // 取消按钮事件
        const cancelBtn = div.querySelector('.transfer-cancel');
        if (cancelBtn) {
            cancelBtn.addEventListener('click', () => {
                this.emit('cancel_transfer', transfer.transferId);
            });
        }
        
        return div;
    }

    updateFileTransferProgress(transferId, progress) {
        const transferElement = document.querySelector(`.transfer-item[data-transfer-id="${transferId}"]`);
        if (transferElement) {
            const progressFill = transferElement.querySelector('.progress-fill');
            const statusText = transferElement.querySelector('.transfer-status');
            
            if (progressFill) {
                progressFill.style.width = `${progress}%`;
            }
            
            if (statusText) {
                const isUpload = statusText.textContent.includes('上传');
                statusText.textContent = `${isUpload ? '上传中' : '下载中'} ${progress.toFixed(1)}%`;
            }
        }
    }

    completeFileTransfer(transferId, success) {
        const transferElement = document.querySelector(`.transfer-item[data-transfer-id="${transferId}"]`);
        if (transferElement) {
            const statusText = transferElement.querySelector('.transfer-status');
            if (statusText) {
                statusText.textContent = success ? '传输完成' : '传输失败';
                statusText.className = `transfer-status ${success ? 'success' : 'error'}`;
            }
            
            // 3秒后移除
            setTimeout(() => {
                transferElement.remove();
            }, 3000);
        }
    }

    showSettings() {
        // 创建设置模态框
        const modal = this.createSettingsModal();
        document.body.appendChild(modal);
        
        // 显示模态框
        setTimeout(() => {
            modal.classList.add('show');
        }, 10);
    }

    createSettingsModal() {
        const modal = document.createElement('div');
        modal.className = 'modal-overlay';
        modal.innerHTML = `
            <div class="modal settings-modal">
                <div class="modal-header">
                    <h3>设置</h3>
                    <button class="close-modal">&times;</button>
                </div>
                <div class="modal-content">
                    <div class="settings-section">
                        <h4>通知设置</h4>
                        <div class="setting-item">
                            <label>
                                <input type="checkbox" id="setting-message-sound" ${this.config.messageSound ? 'checked' : ''}>
                                消息提示音
                            </label>
                        </div>
                        <div class="setting-item">
                            <label>
                                <input type="checkbox" id="setting-notification-sound" ${this.config.notificationSound ? 'checked' : ''}>
                                通知提示音
                            </label>
                        </div>
                        <div class="setting-item">
                            <label>
                                <input type="checkbox" id="setting-show-typing" ${this.config.showTypingIndicator ? 'checked' : ''}>
                                显示正在输入提示
                            </label>
                        </div>
                        <div class="setting-item">
                            <label>
                                <input type="checkbox" id="setting-show-read-receipts" ${this.config.showReadReceipts ? 'checked' : ''}>
                                显示已读回执
                            </label>
                        </div>
                    </div>
                    
                    <div class="settings-section">
                        <h4>界面设置</h4>
                        <div class="setting-item">
                            <label>
                                <input type="checkbox" id="setting-auto-scroll" ${this.config.autoScroll ? 'checked' : ''}>
                                自动滚动到最新消息
                            </label>
                        </div>
                        <div class="setting-item">
                            <label>
                                <input type="checkbox" id="setting-enable-animations" ${this.config.enableAnimations ? 'checked' : ''}>
                                启用动画效果
                            </label>
                        </div>
                        <div class="setting-item">
                            <label>主题:</label>
                            <select id="setting-theme">
                                <option value="dark" ${this.theme === 'dark' ? 'selected' : ''}>深色</option>
                                <option value="light" ${this.theme === 'light' ? 'selected' : ''}>浅色</option>
                                <option value="auto" ${this.theme === 'auto' ? 'selected' : ''}>自动</option>
                            </select>
                        </div>
                    </div>
                    
                    <div class="settings-section">
                        <h4>隐私与安全</h4>
                        <div class="setting-item">
                            <button id="setting-clear-data" class="btn btn-danger">清除本地数据</button>
                        </div>
                        <div class="setting-item">
                            <button id="setting-export-data" class="btn btn-secondary">导出聊天记录</button>
                        </div>
                    </div>
                </div>
                <div class="modal-footer">
                    <button id="setting-save" class="btn btn-primary">保存</button>
                    <button class="btn btn-secondary close-modal">取消</button>
                </div>
            </div>
        `;
        
        // 关闭事件
        modal.querySelectorAll('.close-modal').forEach(btn => {
            btn.addEventListener('click', () => {
                modal.classList.remove('show');
                setTimeout(() => {
                    document.body.removeChild(modal);
                }, 300);
            });
        });
        
        // 保存事件
        const saveBtn = modal.querySelector('#setting-save');
        if (saveBtn) {
            saveBtn.addEventListener('click', () => {
                this.saveSettingsFromModal(modal);
                modal.classList.remove('show');
                setTimeout(() => {
                    document.body.removeChild(modal);
                }, 300);
            });
        }
        
        // 清除数据
        const clearBtn = modal.querySelector('#setting-clear-data');
        if (clearBtn) {
            clearBtn.addEventListener('click', () => {
                if (confirm('确定要清除所有本地数据吗？此操作不可撤销。')) {
                    this.clearLocalData();
                }
            });
        }
        
        // 导出数据
        const exportBtn = modal.querySelector('#setting-export-data');
        if (exportBtn) {
            exportBtn.addEventListener('click', () => {
                this.exportChatData();
            });
        }
        
        return modal;
    }

    saveSettingsFromModal(modal) {
        this.config.messageSound = modal.querySelector('#setting-message-sound').checked;
        this.config.notificationSound = modal.querySelector('#setting-notification-sound').checked;
        this.config.showTypingIndicator = modal.querySelector('#setting-show-typing').checked;
        this.config.showReadReceipts = modal.querySelector('#setting-show-read-receipts').checked;
        this.config.autoScroll = modal.querySelector('#setting-auto-scroll').checked;
        this.config.enableAnimations = modal.querySelector('#setting-enable-animations').checked;
        
        this.theme = modal.querySelector('#setting-theme').value;
        
        this.saveConfig();
        this.applyConfig();
        this.applyTheme();
        
        this.showNotification('设置已保存');
    }

    applyConfig() {
        // 应用配置到UI
        // 这里可以根据配置更新UI状态
    }

    applyTheme() {
        document.body.setAttribute('data-theme', this.theme);
        
        // 保存主题
        try {
            localStorage.setItem('secure_chat_theme', this.theme);
        } catch (error) {
            console.warn('保存主题失败:', error);
        }
    }

    clearLocalData() {
        try {
            // 清除聊天数据
            this.messages.clear();
            this.contacts.clear();
            this.groups.clear();
            this.userInfo = null;
            
            // 清除本地存储
            localStorage.removeItem('secure_chat_session');
            localStorage.removeItem('secure_chat_ui_config');
            localStorage.removeItem('secure_chat_theme');
            
            // 清除IndexedDB
            this.clearIndexedDB();
            
            this.showNotification('本地数据已清除');
            this.showLoginInterface();
            
        } catch (error) {
            this.showError('清除数据失败', error.message);
        }
    }

    async clearIndexedDB() {
        return new Promise((resolve, reject) => {
            const request = indexedDB.deleteDatabase('secure_chat_keys');
            
            request.onsuccess = () => resolve();
            request.onerror = () => reject();
            request.onblocked = () => reject();
        });
    }

    exportChatData() {
        try {
            const exportData = {
                timestamp: Date.now(),
                userInfo: this.userInfo,
                contacts: Array.from(this.contacts.values()),
                messages: Array.from(this.messages.entries()),
                groups: Array.from(this.groups.values()),
                config: this.config
            };
            
            const json = JSON.stringify(exportData, null, 2);
            const blob = new Blob([json], { type: 'application/json' });
            const url = URL.createObjectURL(blob);
            
            const a = document.createElement('a');
            a.href = url;
            a.download = `secure-chat-backup-${Date.now()}.json`;
            document.body.appendChild(a);
            a.click();
            document.body.removeChild(a);
            URL.revokeObjectURL(url);
            
            this.showNotification('数据导出成功');
            
        } catch (error) {
            this.showError('数据导出失败', error.message);
        }
    }

    // 工具方法
    getStatusText(status) {
        const statusMap = {
            0: '离线',
            1: '在线',
            2: '离开',
            3: '忙碌',
            4: '隐身',
            5: '请勿打扰'
        };
        return statusMap[status] || '未知';
    }

    getStatusClass(status) {
        const classMap = {
            0: 'offline',
            1: 'online',
            2: 'away',
            3: 'busy',
            4: 'invisible',
            5: 'dnd'
        };
        return classMap[status] || 'offline';
    }

    getMessageStatusIcon(status) {
        const iconMap = {
            0: '🕐', // 发送中
            1: '✓',  // 已发送
            2: '✓✓', // 已送达
            3: '✓✓'  // 已读
        };
        return iconMap[status] || '';
    }

    escapeHtml(text) {
        const div = document.createElement('div');
        div.textContent = text;
        return div.innerHTML;
    }

    formatFileSize(bytes) {
        if (bytes === 0) return '0 Bytes';
        
        const k = 1024;
        const sizes = ['Bytes', 'KB', 'MB', 'GB'];
        const i = Math.floor(Math.log(bytes) / Math.log(k));
        
        return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
    }

    playMessageSound() {
        if (!this.config.messageSound) {
            return;
        }
        
        try {
            // 创建简短的提示音
            const audioContext = new (window.AudioContext || window.webkitAudioContext)();
            const oscillator = audioContext.createOscillator();
            const gainNode = audioContext.createGain();
            
            oscillator.connect(gainNode);
            gainNode.connect(audioContext.destination);
            
            oscillator.frequency.value = 800;
            oscillator.type = 'sine';
            
            gainNode.gain.setValueAtTime(0.1, audioContext.currentTime);
            gainNode.gain.exponentialRampToValueAtTime(0.01, audioContext.currentTime + 0.1);
            
            oscillator.start(audioContext.currentTime);
            oscillator.stop(audioContext.currentTime + 0.1);
            
        } catch (error) {
            console.warn('播放提示音失败:', error);
        }
    }

    showMessageNotification(message) {
        if (!this.config.notificationSound) {
            return;
        }
        
        // 显示桌面通知
        if ('Notification' in window && Notification.permission === 'granted') {
            const sender = this.contacts.get(message.sender_id) || { nickname: '未知用户' };
            new Notification(`新消息 - ${sender.nickname}`, {
                body: message.content.substring(0, 100) + (message.content.length > 100 ? '...' : ''),
                icon: '/notification-icon.png'
            });
        }
        
        // 在联系人列表显示未读计数
        const contactElement = document.querySelector(`.contact-item[data-user-id="${message.sender_id}"]`);
        if (contactElement) {
            const unreadSpan = contactElement.querySelector('.unread-count');
            if (unreadSpan) {
                let count = parseInt(unreadSpan.textContent) || 0;
                count++;
                unreadSpan.textContent = count;
                unreadSpan.classList.remove('hidden');
            }
        }
    }

    showError(title, message) {
        // 创建错误提示
        const errorDiv = document.createElement('div');
        errorDiv.className = 'error-toast';
        errorDiv.innerHTML = `
            <div class="error-icon">⚠️</div>
            <div class="error-content">
                <div class="error-title">${title}</div>
                <div class="error-message">${message}</div>
            </div>
            <button class="error-close">&times;</button>
        `;
        
        document.body.appendChild(errorDiv);
        
        // 显示动画
        setTimeout(() => {
            errorDiv.classList.add('show');
        }, 10);
        
        // 关闭事件
        const closeBtn = errorDiv.querySelector('.error-close');
        if (closeBtn) {
            closeBtn.addEventListener('click', () => {
                errorDiv.classList.remove('show');
                setTimeout(() => {
                    document.body.removeChild(errorDiv);
                }, 300);
            });
        }
        
        // 3秒后自动关闭
        setTimeout(() => {
            errorDiv.classList.remove('show');
            setTimeout(() => {
                document.body.removeChild(errorDiv);
            }, 300);
        }, 3000);
    }

    showNotification(message, type = 'info') {
        const notificationDiv = document.createElement('div');
        notificationDiv.className = `notification-toast notification-${type}`;
        notificationDiv.textContent = message;
        
        document.body.appendChild(notificationDiv);
        
        setTimeout(() => {
            notificationDiv.classList.add('show');
        }, 10);
        
        setTimeout(() => {
            notificationDiv.classList.remove('show');
            setTimeout(() => {
                document.body.removeChild(notificationDiv);
            }, 300);
        }, 3000);
    }

    async loadMessageHistory(userId) {
        try {
            // 这里应该从服务器或本地存储加载历史消息
            const messages = this.messages.get(userId) || [];
            
            // 清空当前消息列表
            this.clearMessages();
            
            // 渲染历史消息
            messages.forEach(message => {
                this.renderMessage(message);
            });
            
            // 滚动到底部
            if (this.config.autoScroll && this.elements.messageContainer) {
                this.elements.messageContainer.scrollTop = this.elements.messageContainer.scrollHeight;
            }
            
        } catch (error) {
            console.error('加载消息历史失败:', error);
        }
    }

    // 事件系统
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
                    console.error(`UI事件 ${event} 监听器错误:`, error);
                }
            });
        }
    }
}

export default UIEngine;