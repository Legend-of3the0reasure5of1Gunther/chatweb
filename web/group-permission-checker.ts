// 文件: src/core/group-permission-checker.ts
/**
 * 群组权限检查器
 * 实时验证用户权限，支持复杂的权限逻辑
 */

import { GroupManager, GroupRole, GroupPermission } from './group-manager';

export interface PermissionRule {
    name: string;
    condition: (context: PermissionContext) => boolean | Promise<boolean>;
    priority: number;
}

export interface PermissionContext {
    groupId: bigint;
    userId: bigint;
    targetUserId?: bigint;
    action: string;
    resource?: any;
    timestamp: number;
    ipAddress?: string;
    userAgent?: string;
}

export class GroupPermissionChecker {
    private groupManager: GroupManager;
    private rules: PermissionRule[] = [];
    
    constructor(groupManager: GroupManager) {
        this.groupManager = groupManager;
        
        // 初始化默认规则
        this.initializeDefaultRules();
    }
    
    // 初始化默认规则
    private initializeDefaultRules(): void {
        // 规则1: 群主有所有权限
        this.rules.push({
            name: 'owner_has_all_permissions',
            condition: async (context) => {
                const groupInfo = await this.groupManager.getGroupInfo(context.groupId);
                return groupInfo?.ownerId === context.userId;
            },
            priority: 100
        });
        
        // 规则2: 不能修改群主角色
        this.rules.push({
            name: 'cannot_modify_owner',
            condition: async (context) => {
                if (context.action === 'set_role' || context.action === 'remove_member') {
                    const groupInfo = await this.groupManager.getGroupInfo(context.groupId);
                    return context.targetUserId !== groupInfo?.ownerId;
                }
                return true;
            },
            priority: 90
        });
        
        // 规则3: 管理员不能修改其他管理员（除非是群主）
        this.rules.push({
            name: 'admin_cannot_modify_other_admins',
            condition: async (context) => {
                if (context.action === 'set_role' || context.action === 'remove_member') {
                    const userRole = await this.getUserRole(context.groupId, context.userId);
                    const targetRole = await this.getUserRole(context.groupId, context.targetUserId!);
                    
                    if (userRole === GroupRole.ADMIN && targetRole === GroupRole.ADMIN) {
                        // 管理员不能修改其他管理员
                        return false;
                    }
                }
                return true;
            },
            priority: 80
        });
        
        // 规则4: 被禁言的用户不能发送消息
        this.rules.push({
            name: 'muted_users_cannot_send_messages',
            condition: async (context) => {
                if (context.action === 'send_message') {
                    const member = await this.groupManager.getGroupMember(context.groupId, context.userId);
                    if (member?.mutedUntil && member.mutedUntil > Date.now()) {
                        return false;
                    }
                }
                return true;
            },
            priority: 70
        });
        
        // 规则5: 只能删除自己的消息（除非有删除权限）
        this.rules.push({
            name: 'can_only_delete_own_messages',
            condition: async (context) => {
                if (context.action === 'delete_message') {
                    const message = context.resource;
                    if (message?.senderId !== context.userId) {
                        // 检查是否有删除权限
                        const hasPermission = await this.groupManager.hasPermission(
                            context.groupId,
                            GroupPermission.DELETE_MESSAGE
                        );
                        return hasPermission;
                    }
                }
                return true;
            },
            priority: 60
        });
        
        // 规则6: 2FA要求
        this.rules.push({
            name: 'two_factor_required',
            condition: async (context) => {
                const groupInfo = await this.groupManager.getGroupInfo(context.groupId);
                if (groupInfo?.twoFactorRequired) {
                    // 检查用户是否启用了2FA
                    const has2FA = await this.checkUser2FA(context.userId);
                    return has2FA;
                }
                return true;
            },
            priority: 50
        });
        
        // 规则7: 加密群组的密钥要求
        this.rules.push({
            name: 'encryption_key_required',
            condition: async (context) => {
                const groupInfo = await this.groupManager.getGroupInfo(context.groupId);
                if (groupInfo?.encryptionEnabled) {
                    // 检查用户是否有有效的密钥
                    const hasValidKey = await this.checkUserEncryptionKey(context.groupId, context.userId);
                    return hasValidKey;
                }
                return true;
            },
            priority: 40
        });
    }
    
    // 检查权限
    async checkPermission(context: PermissionContext): Promise<{
        allowed: boolean;
        reason?: string;
        rules: string[];
    }> {
        const results: boolean[] = [];
        const appliedRules: string[] = [];
        
        // 按优先级排序规则
        const sortedRules = [...this.rules].sort((a, b) => b.priority - a.priority);
        
        // 应用所有规则
        for (const rule of sortedRules) {
            try {
                const result = await rule.condition(context);
                results.push(result);
                appliedRules.push(rule.name);
                
                // 如果规则返回false，立即拒绝
                if (!result) {
                    return {
                        allowed: false,
                        reason: `规则 "${rule.name}" 拒绝访问`,
                        rules: appliedRules
                    };
                }
            } catch (error) {
                console.error(`规则 "${rule.name}" 执行错误:`, error);
                // 规则执行错误时，默认为拒绝
                results.push(false);
                appliedRules.push(rule.name);
                
                return {
                    allowed: false,
                    reason: `规则 "${rule.name}" 执行错误`,
                    rules: appliedRules
                };
            }
        }
        
        // 所有规则都通过
        return {
            allowed: true,
            rules: appliedRules
        };
    }
    
    // 添加自定义规则
    addRule(rule: PermissionRule): void {
        this.rules.push(rule);
    }
    
    // 移除规则
    removeRule(ruleName: string): void {
        this.rules = this.rules.filter(rule => rule.name !== ruleName);
    }
    
    // 获取用户角色
    private async getUserRole(groupId: bigint, userId: bigint): Promise<GroupRole | null> {
        const member = await this.groupManager.getGroupMember(groupId, userId);
        return member?.role || null;
    }
    
    // 检查用户2FA状态
    private async checkUser2FA(userId: bigint): Promise<boolean> {
        // 这里需要实现2FA检查逻辑
        // 简化版：总是返回true
        return true;
    }
    
    // 检查用户加密密钥
    private async checkUserEncryptionKey(groupId: bigint, userId: bigint): Promise<boolean> {
        // 这里需要实现密钥检查逻辑
        // 简化版：总是返回true
        return true;
    }
    
    // 批量检查权限
    async checkPermissionsBatch(contexts: PermissionContext[]): Promise<Map<string, boolean>> {
        const results = new Map<string, boolean>();
        
        for (const context of contexts) {
            const result = await this.checkPermission(context);
            results.set(this.getContextKey(context), result.allowed);
        }
        
        return results;
    }
    
    // 获取上下文键
    private getContextKey(context: PermissionContext): string {
        return `${context.groupId}_${context.userId}_${context.action}_${context.timestamp}`;
    }
    
    // 获取权限统计
    getRuleStatistics(): {
        totalRules: number;
        ruleNames: string[];
        priorities: number[];
    } {
        return {
            totalRules: this.rules.length,
            ruleNames: this.rules.map(rule => rule.name),
            priorities: this.rules.map(rule => rule.priority)
        };
    }
}