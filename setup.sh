#!/bin/bash

set -e

SQLITE_VERSION="${SQLITE_VERSION:-3510300}"
GROONGA_VERSION="${GROONGA_VERSION:-16.0.1-1}"

echo "开始安装开发环境依赖..."

# 检测操作系统类型
detect_os() {
	if [ -f /etc/os-release ]; then
		. /etc/os-release
		OS=$ID
		VER=$VERSION_ID
	elif type lsb_release >/dev/null 2>&1; then
		OS=$(lsb_release -si | tr '[:upper:]' '[:lower:]')
		VER=$(lsb_release -sr)
	else
		echo "无法检测操作系统类型"
		exit 1
	fi

	# 只支持Ubuntu和Debian
	if [ "$OS" != "ubuntu" ] && [ "$OS" != "debian" ]; then
		echo "错误: 此脚本只支持Ubuntu和Debian系统"
		exit 1
	fi
}

# https://groonga.org/docs/install/ubuntu.html#register-groonga-apt-repository
register_groonga_apt_repository() {
	echo "注册groonga apt仓库..."
	sudo apt update
	sudo apt install -y -V ca-certificates lsb-release wget
	codename=$(lsb_release --codename --short)
	deb="/tmp/groonga-apt-source-latest-${codename}.deb"
	wget -O "$deb" https://packages.groonga.org/ubuntu/groonga-apt-source-latest-${codename}.deb
	sudo apt install -y -V "$deb"
	rm -f "$deb"
}

# 安装依赖
install_deps() {
	echo "检测到 $OS $VER"
	echo "安装依赖..."
	sudo apt update
	sudo apt -V -y install gcc make pkg-config groonga=${GROONGA_VERSION} libgroonga-dev=${GROONGA_VERSION}
}

# 从源码编译安装 SQLite
install_sqlite() {
	echo "从源码编译安装 SQLite ${SQLITE_VERSION}..."

	local SQLITE_AUTOCONF_URL="https://www.sqlite.org/2026/sqlite-autoconf-${SQLITE_VERSION}.tar.gz"

	cd /tmp

	# 下载源码
	echo "下载 SQLite..."
	wget -q "${SQLITE_AUTOCONF_URL}" -O sqlite-autoconf.tar.gz
	tar xzf sqlite-autoconf.tar.gz
	cd sqlite-autoconf-*

	# 配置并编译
	echo "配置并编译 SQLite..."
	./configure --prefix=/usr/local --enable-fts5
	make -j$(nproc)

	# 安装
	sudo make install
	sudo ldconfig

	# 清理
	cd /tmp
	rm -rf sqlite-autoconf-*

	echo "SQLite ${NEW_VER} 安装完成!"
}

# 验证安装
verify_install() {
	echo "验证依赖安装..."

	# 检查gcc
	if ! command -v gcc &>/dev/null; then
		echo "错误: gcc未安装"
		exit 1
	fi

	# 检查pkg-config
	if ! command -v pkg-config &>/dev/null; then
		echo "错误: pkg-config未安装"
		exit 1
	fi

	# 检查sqlite3
	if ! command -v sqlite3 &>/dev/null; then
		echo "错误: sqlite3未安装"
		exit 1
	fi

	# 检查groonga
	if ! command -v groonga &>/dev/null; then
		echo "错误: groonga未安装"
		exit 1
	fi

	# 检查sqlite3 pkg-config配置
	if ! pkg-config --exists sqlite3 2>&1; then
		echo "警告: sqlite3 pkg-config未找到"
	else
		echo "sqlite3 pkg-config配置正常"
	fi

	# 检查groonga pkg-config配置
	if ! pkg-config --exists groonga 2>&1; then
		echo "警告: groonga pkg-config未找到"
	else
		echo "groonga pkg-config配置正常"
	fi

	echo "所有依赖验证通过!"
}

# 主函数
main() {
	detect_os
	register_groonga_apt_repository
	install_deps
	install_sqlite
	verify_install
	echo ""
	echo "开发环境安装完成!"
	echo "运行 'make all' 构建项目"
}

main
