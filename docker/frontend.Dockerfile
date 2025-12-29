# 构建阶段
FROM node:18-alpine AS builder

WORKDIR /app

# 复制package文件
COPY package*.json ./

# 安装依赖
RUN npm ci --only=production

# 复制源代码
COPY . .

# 构建应用
RUN npm run build

# 运行阶段
FROM nginx:alpine AS runner

# 复制nginx配置
COPY nginx/nginx.conf /etc/nginx/nginx.conf
COPY nginx/ssl /etc/nginx/ssl

# 复制构建文件
COPY --from=builder /app/dist /usr/share/nginx/html

# 创建nginx缓存目录
RUN mkdir -p /var/cache/nginx && \
    chown -R nginx:nginx /var/cache/nginx && \
    chown -R nginx:nginx /var/log/nginx && \
    chown -R nginx:nginx /etc/nginx/ssl

# 切换用户
USER nginx

# 暴露端口
EXPOSE 80 443

# 启动nginx
CMD ["nginx", "-g", "daemon off;"]