# SecureChat Makefile
.PHONY: help install build test lint clean deploy up down logs ps restart

# 默认目标
.DEFAULT_GOAL := help

# 颜色定义
RED    := $(shell tput -Txterm setaf 1)
GREEN  := $(shell tput -Txterm setaf 2)
YELLOW := $(shell tput -Txterm setaf 3)
BLUE   := $(shell tput -Txterm setaf 4)
RESET  := $(shell tput -Txterm sgr0)

# 帮助
help:
	@echo "$(BLUE)SecureChat 构建系统$(RESET)"
	@echo ""
	@echo "$(YELLOW)可用命令:$(RESET)"
	@echo "  $(GREEN)install$(RESET)    安装所有依赖"
	@echo "  $(GREEN)build$(RESET)      构建项目"
	@echo "  $(GREEN)dev$(RESET)        启动开发环境"
	@echo "  $(GREEN)test$(RESET)       运行测试"
	@echo "  $(GREEN)lint$(RESET)       代码检查"
	@echo "  $(GREEN)clean$(RESET)      清理构建文件"
	@echo "  $(GREEN)up$(RESET)         启动所有服务"
	@echo "  $(GREEN)down$(RESET)       停止所有服务"
	@echo "  $(GREEN)logs$(RESET)       查看服务日志"
	@echo "  $(GREEN)ps$(RESET)         查看服务状态"
	@echo "  $(GREEN)deploy$(RESET)     部署到生产环境"
	@echo "  $(GREEN)backup$(RESET)     备份数据库"
	@echo "  $(GREEN)restore$(RESET)    恢复数据库"
	@echo "  $(GREEN)ssl$(RESET)        生成SSL证书"

# 环境检查
check-env:
	@if [ -z "$(ENV)" ]; then \
		echo "$(RED)错误: 请设置 ENV 环境变量$(RESET)"; \
		echo "用法: make <command> ENV=production"; \
		exit 1; \
	fi

# 安装依赖
install:
	@echo "$(BLUE)安装后端依赖...$(RESET)"
	cd backend && npm install
	@echo "$(BLUE)安装前端依赖...$(RESET)"
	cd frontend && npm install
	@echo "$(GREEN)依赖安装完成$(RESET)"

# 构建项目
build:
	@echo "$(BLUE)构建后端...$(RESET)"
	cd backend && npm run build
	@echo "$(BLUE)构建前端...$(RESET)"
	cd frontend && npm run build
	@echo "$(GREEN)构建完成$(RESET)"

# 开发环境
dev:
	@echo "$(BLUE)启动开发环境...$(RESET)"
	docker-compose -f docker-compose.dev.yml up -d
	@echo "$(GREEN)开发环境已启动$(RESET)"

# 运行测试
test:
	@echo "$(BLUE)运行测试...$(RESET)"
	cd backend && npm test
	cd frontend && npm test
	@echo "$(GREEN)测试完成$(RESET)"

# 代码检查
lint:
	@echo "$(BLUE)检查后端代码...$(RESET)"
	cd backend && npm run lint
	@echo "$(BLUE)检查前端代码...$(RESET)"
	cd frontend && npm run lint
	@echo "$(GREEN)代码检查完成$(RESET)"

# 清理
clean:
	@echo "$(BLUE)清理构建文件...$(RESET)"
	rm -rf backend/dist backend/node_modules backend/coverage
	rm -rf frontend/dist frontend/node_modules frontend/.nuxt
	rm -rf logs/* uploads/*
	docker system prune -f
	@echo "$(GREEN)清理完成$(RESET)"

# 启动服务
up: check-env
	@echo "$(BLUE)启动 $(ENV) 环境...$(RESET)"
	docker-compose -f docker-compose.$(ENV).yml up -d
	@echo "$(GREEN)服务已启动$(RESET)"

# 停止服务
down: check-env
	@echo "$(BLUE)停止 $(ENV) 环境...$(RESET)"
	docker-compose -f docker-compose.$(ENV).yml down
	@echo "$(GREEN)服务已停止$(RESET)"

# 查看日志
logs: check-env
	@echo "$(BLUE)查看 $(ENV) 环境日志...$(RESET)"
	docker-compose -f docker-compose.$(ENV).yml logs -f

# 查看服务状态
ps: check-env
	@echo "$(BLUE)$(ENV) 环境服务状态:$(RESET)"
	docker-compose -f docker-compose.$(ENV).yml ps

# 重启服务
restart: check-env
	@echo "$(BLUE)重启 $(ENV) 环境...$(RESET)"
	docker-compose -f docker-compose.$(ENV).yml restart
	@echo "$(GREEN)服务已重启$(RESET)"

# 部署到生产环境
deploy:
	@echo "$(BLUE)开始部署...$(RESET)"
	git pull origin main
	make install
	make build
	make test
	make up ENV=production
	@echo "$(GREEN)部署完成$(RESET)"

# 备份数据库
backup:
	@echo "$(BLUE)备份数据库...$(RESET)"
	mkdir -p backups
	docker exec securechat-db pg_dump -U securechat_user securechat > backups/backup_$(shell date +%Y%m%d_%H%M%S).sql
	@echo "$(GREEN)备份完成$(RESET)"

# 恢复数据库
restore:
	@if [ -z "$(FILE)" ]; then \
		echo "$(RED)错误: 请指定备份文件$(RESET)"; \
		echo "用法: make restore FILE=backups/backup.sql"; \
		exit 1; \
	fi
	@echo "$(BLUE)恢复数据库...$(RESET)"
	docker exec -i securechat-db psql -U securechat_user securechat < $(FILE)
	@echo "$(GREEN)恢复完成$(RESET)"

# 生成SSL证书
ssl:
	@echo "$(BLUE)生成SSL证书...$(RESET)"
	mkdir -p nginx/ssl
	openssl req -x509 -nodes -days 365 -newkey rsa:2048 \
		-keyout nginx/ssl/private.key \
		-out nginx/ssl/certificate.pem \
		-subj "/C=CN/ST=Beijing/L=Beijing/O=SecureChat/CN=securechat.example.com"
	@echo "$(GREEN)SSL证书生成完成$(RESET)"

# 数据库迁移
migrate:
	@echo "$(BLUE)运行数据库迁移...$(RESET)"
	docker exec securechat-backend npm run migrate
	@echo "$(GREEN)迁移完成$(RESET)"

# 查看监控
monitor:
	@echo "$(BLUE)打开监控面板...$(RESET)"
	open http://localhost:9090

# 安全检查
security-check:
	@echo "$(BLUE)运行安全检查...$(RESET)"
	./scripts/security_check.sh
	npm audit
	@echo "$(GREEN)安全检查完成$(RESET)"

# 性能测试
benchmark:
	@echo "$(BLUE)运行性能测试...$(RESET)"
	./scripts/benchmark.sh
	@echo "$(GREEN)性能测试完成$(RESET)"