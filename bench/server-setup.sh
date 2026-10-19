#!/usr/bin/env bash
# One-time setup for the marina benchmark box (Ubuntu 24.04, 1 vCPU, 1GB RAM)
set -euo pipefail

echo "=== swap (needed for cargo on 1GB)"
if ! swapon --show | grep -q /swapfile; then
  fallocate -l 2G /swapfile && chmod 600 /swapfile && mkswap /swapfile && swapon /swapfile
fi
free -m | head -2

echo "=== apt packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential clang libsqlite3-dev sqlite3 curl git rsync > /dev/null
clang --version | head -1

echo "=== rustup (minimal profile)"
if [ ! -x /root/.cargo/bin/cargo ]; then
  curl -4 --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain stable
fi
source /root/.cargo/env
rustc --version

echo "=== node 22 (NodeSource)"
if ! command -v node > /dev/null; then
  curl -4 -fsSL https://deb.nodesource.com/setup_22.x | bash - > /dev/null
  apt-get install -y -qq nodejs > /dev/null
fi
node --version

echo "=== bun"
if ! command -v bun > /dev/null; then
  curl -4 -fsSL https://bun.sh/install | bash > /dev/null
fi
export BUN_INSTALL="/root/.bun"
export PATH="$BUN_INSTALL/bin:$PATH"
bun --version

echo "=== go 1.23"
if ! command -v go > /dev/null; then
  curl -4 -fsSL https://go.dev/dl/go1.23.3.linux-amd64.tar.gz -o /tmp/go.tgz
  tar -C /usr/local -xzf /tmp/go.tgz
fi
export PATH=$PATH:/usr/local/go/bin
go version

echo "=== done"
