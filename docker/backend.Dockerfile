# 构建阶段
FROM node:18-alpine AS builder

# 安装构建依赖
RUN apk add --no-cache \
    python3 \
    make \
    g++ \
    git \
    openssl

# 设置工作目录
WORKDIR /app

# 复制package文件
COPY package*.json ./

# 安装依赖（包括开发依赖）
RUN npm ci --only=production && npm cache clean --force

# 复制源代码
COPY . .

# 编译TypeScript
RUN npm run build

# 运行阶段
FROM node:18-alpine AS runner

# 创建非root用户
RUN addgroup -g 1001 -S nodejs && \
    adduser -S nodejs -u 1001

# 安装运行时依赖
RUN apk add --no-cache \
    curl \
    tzdata \
    openssl

# 设置时区
ENV TZ=Asia/Shanghai

# 设置工作目录
WORKDIR /app

# 复制必要的文件
COPY --from=builder /app/package*.json ./
COPY --from=builder /app/node_modules ./node_modules
COPY --from=builder /app/dist ./dist
COPY --from=builder /app/config ./config
COPY --from=builder /app/scripts ./scripts

# 创建必要的目录
RUN mkdir -p \
    /app/logs \
    /app/uploads \
    /app/ssl \
    && chown -R nodejs:nodejs /app

# 切换用户
USER nodejs

# 健康检查
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD curl -f http://localhost:8080/health || exit 1

# 暴露端口
EXPOSE 8080 8081

# 设置环境变量
ENV NODE_ENV=production
ENV CONFIG_PATH=/app/config

# 启动命令
CMD ["node", "dist/server.js"]