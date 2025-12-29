// 文件: src/core/sync-conflict-resolver.ts
/**
 * 同步冲突解决器
 * 处理多设备同步时的数据冲突
 */

import { OfflineMessage, SyncOperation, DeviceInfo, ConflictResolutionStrategy } from './offline-sync';

export interface ConflictResolutionRule {
    priority: number;
    condition: (conflict: SyncConflict) => boolean;
    resolution: ConflictResolutionStrategy;
    description: string;
}

export class SyncConflictResolver {
    private rules: ConflictResolutionRule[] = [];
    private manualResolutionCallback?: (conflict: SyncConflict) => Promise<ConflictResolutionStrategy>;
    
    constructor() {
        this.initializeDefaultRules();
    }
    
    // 初始化默认规则
    private initializeDefaultRules(): void {
        // 规则1: 服务器时间戳较新的消息胜出
        this.rules.push({
            priority: 100,
            condition: (conflict) => 
                conflict.type === 'message_conflict' && 
                conflict.serverValue?.timestamp &&
                conflict.clientValue?.timestamp,
            resolution: ConflictResolutionStrategy.NEWER_WINS,
            description: 'Newer message timestamp wins'
        });
        
        // 规则2: 已删除的消息优先
        this.rules.push({
            priority: 90,
            condition: (conflict) => 
                conflict.type === 'message_conflict' &&
                (conflict.serverValue?.deleted || conflict.clientValue?.deleted),
            resolution: ConflictResolutionStrategy.SERVER_WINS,
            description: 'Deleted messages take precedence'
        });
        
        // 规则3: 群组消息的服务器版本优先
        this.rules.push({
            priority: 80,
            condition: (conflict) =>
                conflict.type === 'message_conflict' &&
                conflict.serverValue?.groupId &&
                conflict.clientValue?.groupId,
            resolution: ConflictResolutionStrategy.SERVER_WINS,
            description: 'Group messages: server version wins'
        });
        
        // 规则4: 未发送的消息（客户端草稿）
        this.rules.push({
            priority: 70,
            condition: (conflict) =>
                conflict.type === 'message_conflict' &&
                conflict.clientValue?.status === 'local_draft',
            resolution: ConflictResolutionStrategy.CLIENT_WINS,
            description: 'Local drafts: client version wins'
        });
        
        // 规则5: 用户状态更新
        this.rules.push({
            priority: 60,
            condition: (conflict) =>
                conflict.type === 'operation_conflict' &&
                conflict.operation?.entityType === 'user',
            resolution: ConflictResolutionStrategy.NEWER_WINS,
            description: 'User operations: newer wins'
        });
    }
    
    // 添加自定义规则
    addRule(rule: ConflictResolutionRule): void {
        this.rules.push(rule);
        this.rules.sort((a, b) => b.priority - a.priority); // 按优先级降序排序
    }
    
    // 移除规则
    removeRule(description: string): void {
        this.rules = this.rules.filter(rule => rule.description !== description);
    }
    
    // 解决冲突
    async resolve(conflict: SyncConflict): Promise<{
        resolution: ConflictResolutionStrategy;
        resolvedValue: any;
        mergedValue?: any;
    }> {
        // 1. 查找适用的规则
        const applicableRule = this.rules.find(rule => rule.condition(conflict));
        
        // 2. 如果有适用的规则，使用规则解决
        if (applicableRule && applicableRule.resolution !== ConflictResolutionStrategy.MANUAL) {
            return this.applyRule(conflict, applicableRule.resolution);
        }
        
        // 3. 如果规则要求手动解决或有手动解决回调
        if (this.manualResolutionCallback && 
            (applicableRule?.resolution === ConflictResolutionStrategy.MANUAL || 
             !applicableRule)) {
            
            try {
                const resolution = await this.manualResolutionCallback(conflict);
                return this.applyRule(conflict, resolution);
            } catch (error) {
                console.error('Manual resolution failed:', error);
                // 回退到默认策略
                return this.applyRule(conflict, ConflictResolutionStrategy.SERVER_WINS);
            }
        }
        
        // 4. 默认策略：服务器胜出
        return this.applyRule(conflict, ConflictResolutionStrategy.SERVER_WINS);
    }
    
    // 应用解决规则
    private applyRule(
        conflict: SyncConflict, 
        strategy: ConflictResolutionStrategy
    ): {
        resolution: ConflictResolutionStrategy;
        resolvedValue: any;
        mergedValue?: any;
    } {
        switch (strategy) {
            case ConflictResolutionStrategy.SERVER_WINS:
                return {
                    resolution: strategy,
                    resolvedValue: conflict.serverValue,
                    mergedValue: this.mergeValues(conflict.serverValue, conflict.clientValue)
                };
                
            case ConflictResolutionStrategy.CLIENT_WINS:
                return {
                    resolution: strategy,
                    resolvedValue: conflict.clientValue,
                    mergedValue: this.mergeValues(conflict.clientValue, conflict.serverValue)
                };
                
            case ConflictResolutionStrategy.NEWER_WINS:
                const serverTime = this.extractTimestamp(conflict.serverValue);
                const clientTime = this.extractTimestamp(conflict.clientValue);
                
                if (clientTime > serverTime) {
                    return {
                        resolution: strategy,
                        resolvedValue: conflict.clientValue,
                        mergedValue: this.mergeValues(conflict.clientValue, conflict.serverValue)
                    };
                } else {
                    return {
                        resolution: strategy,
                        resolvedValue: conflict.serverValue,
                        mergedValue: this.mergeValues(conflict.serverValue, conflict.clientValue)
                    };
                }
                
            case ConflictResolutionStrategy.MANUAL:
                // 应该在前面的步骤中处理
                throw new Error('Manual resolution should have been handled earlier');
                
            default:
                throw new Error(`Unknown resolution strategy: ${strategy}`);
        }
    }
    
    // 合并值（智能合并）
    private mergeValues(primary: any, secondary: any): any {
        if (!primary || !secondary) {
            return primary || secondary;
        }
        
        if (typeof primary !== typeof secondary) {
            return primary;
        }
        
        if (Array.isArray(primary) && Array.isArray(secondary)) {
            // 数组合并：去重
            const merged = [...primary];
            secondary.forEach(item => {
                if (!merged.includes(item)) {
                    merged.push(item);
                }
            });
            return merged;
        }
        
        if (typeof primary === 'object' && primary !== null) {
            // 对象合并
            const merged = { ...primary };
            Object.keys(secondary).forEach(key => {
                if (!(key in merged)) {
                    merged[key] = secondary[key];
                } else if (typeof merged[key] === 'object' && merged[key] !== null) {
                    // 递归合并嵌套对象
                    merged[key] = this.mergeValues(merged[key], secondary[key]);
                }
            });
            return merged;
        }
        
        return primary;
    }
    
    // 从值中提取时间戳
    private extractTimestamp(value: any): number {
        if (!value) {
            return 0;
        }
        
        if (value.timestamp) {
            return value.timestamp;
        }
        
        if (value.updatedAt) {
            return value.updatedAt;
        }
        
        if (value.createdAt) {
            return value.createdAt;
        }
        
        if (value.lastModified) {
            return value.lastModified;
        }
        
        return 0;
    }
    
    // 批量解决冲突
    async resolveBatch(conflicts: SyncConflict[]): Promise<Map<string, any>> {
        const results = new Map<string, any>();
        const resolvedConflicts: SyncConflict[] = [];
        
        for (const conflict of conflicts) {
            try {
                const result = await this.resolve(conflict);
                results.set(this.getConflictId(conflict), result.resolvedValue);
                
                // 记录解决的冲突
                resolvedConflicts.push({
                    ...conflict,
                    resolution: result.resolution
                });
                
            } catch (error) {
                console.error(`Failed to resolve conflict:`, conflict, error);
                // 默认使用服务器值
                results.set(this.getConflictId(conflict), conflict.serverValue);
            }
        }
        
        // 记录解决结果
        await this.logResolutionBatch(resolvedConflicts);
        
        return results;
    }
    
    // 获取冲突ID
    private getConflictId(conflict: SyncConflict): string {
        switch (conflict.type) {
            case 'message_conflict':
                return `msg_${conflict.message?.id || conflict.operation?.entityId}`;
            case 'operation_conflict':
                return `op_${conflict.operation?.operationId}`;
            case 'device_conflict':
                return `dev_${conflict.device?.deviceId}`;
            default:
                return `unknown_${Date.now()}_${Math.random()}`;
        }
    }
    
    // 记录解决批次
    private async logResolutionBatch(conflicts: SyncConflict[]): Promise<void> {
        const resolutionLog = {
            timestamp: Date.now(),
            conflictsCount: conflicts.length,
            resolutions: conflicts.map(c => ({
                type: c.type,
                resolution: c.resolution,
                message: c.message
            })),
            deviceInfo: this.getDeviceInfo()
        };
        
        // 保存到本地存储
        try {
            const logs = JSON.parse(localStorage.getItem('sync_conflict_logs') || '[]');
            logs.push(resolutionLog);
            
            // 只保留最近的100条记录
            if (logs.length > 100) {
                logs.splice(0, logs.length - 100);
            }
            
            localStorage.setItem('sync_conflict_logs', JSON.stringify(logs));
        } catch (error) {
            console.error('Failed to log conflict resolution:', error);
        }
    }
    
    // 获取设备信息
    private getDeviceInfo(): any {
        return {
            userAgent: navigator.userAgent,
            platform: navigator.platform,
            screen: `${window.screen.width}x${window.screen.height}`,
            language: navigator.language,
            timezone: Intl.DateTimeFormat().resolvedOptions().timeZone,
            timestamp: Date.now()
        };
    }
    
    // 设置手动解决回调
    setManualResolutionCallback(callback: (conflict: SyncConflict) => Promise<ConflictResolutionStrategy>): void {
        this.manualResolutionCallback = callback;
    }
    
    // 获取解决统计
    getResolutionStats(): {
        totalResolved: number;
        byStrategy: Record<ConflictResolutionStrategy, number>;
        byType: Record<string, number>;
    } {
        try {
            const logs = JSON.parse(localStorage.getItem('sync_conflict_logs') || '[]');
            
            const stats = {
                totalResolved: 0,
                byStrategy: {
                    [ConflictResolutionStrategy.SERVER_WINS]: 0,
                    [ConflictResolutionStrategy.CLIENT_WINS]: 0,
                    [ConflictResolutionStrategy.NEWER_WINS]: 0,
                    [ConflictResolutionStrategy.MANUAL]: 0
                },
                byType: {
                    'message_conflict': 0,
                    'operation_conflict': 0,
                    'device_conflict': 0
                }
            };
            
            logs.forEach((log: any) => {
                log.resolutions.forEach((resolution: any) => {
                    stats.totalResolved++;
                    
                    if (resolution.resolution in stats.byStrategy) {
                        stats.byStrategy[resolution.resolution as ConflictResolutionStrategy]++;
                    }
                    
                    if (resolution.type in stats.byType) {
                        stats.byType[resolution.type]++;
                    }
                });
            });
            
            return stats;
            
        } catch (error) {
            console.error('Failed to get resolution stats:', error);
            return {
                totalResolved: 0,
                byStrategy: {
                    [ConflictResolutionStrategy.SERVER_WINS]: 0,
                    [ConflictResolutionStrategy.CLIENT_WINS]: 0,
                    [ConflictResolutionStrategy.NEWER_WINS]: 0,
                    [ConflictResolutionStrategy.MANUAL]: 0
                },
                byType: {
                    'message_conflict': 0,
                    'operation_conflict': 0,
                    'device_conflict': 0
                }
            };
        }
    }
}

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