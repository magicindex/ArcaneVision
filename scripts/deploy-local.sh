#!/bin/bash
# 本机（fnOS，需 root）直接升级玄览：
#   systemctl stop → 解包新 payload → systemctl start
# 保留 /vol1/@appdata/xuanlan（设置与索引）不动。
#
# 用法：sudo bash scripts/deploy-local.sh [fpk 路径]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FPK="${1:-$ROOT/dist/xuanlan_0.10.53_amd64.fpk}"
APPDEST="${XL_APPDEST:-/vol1/@appcenter/xuanlan}"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

[ "$(id -u)" = "0" ] || { echo "请用 root 执行：sudo bash $0"; exit 1; }
[ -f "$FPK" ] || { echo "找不到安装包：$FPK"; exit 1; }
[ -d "$APPDEST" ] || { echo "找不到安装目录：$APPDEST"; exit 1; }

tar -xzf "$FPK" -C "$STAGE"
VERSION="$(sed -n 's/^version *= *//p' "$STAGE/manifest" | head -n 1)"
[ -f "$STAGE/app.tgz" ] || { echo "安装包缺少 app.tgz"; exit 1; }

echo "→ 停止 xuanlan.service"
systemctl stop xuanlan 2>/dev/null || true

echo "→ 解包 ${VERSION:-未知版本} 到 $APPDEST"
rm -rf "$APPDEST/bin" "$APPDEST/ui" "$APPDEST/start.sh" "$APPDEST/config"
tar -xzf "$STAGE/app.tgz" -C "$APPDEST"

# 恢复应用目录 ACL（与安装器一致：xuanlan 用户可读写）
if command -v setfacl >/dev/null 2>&1 && id xuanlan >/dev/null 2>&1; then
    setfacl -m u:xuanlan:rwx "$APPDEST" "$APPDEST/bin" "$APPDEST/config" "$APPDEST/ui" 2>/dev/null || true
fi

echo "→ 重新加载并启动"
systemctl daemon-reload
systemctl start xuanlan
sleep 1
if systemctl is-active --quiet xuanlan; then
    echo "完成：玄览 ${VERSION} 已运行"
else
    echo "启动失败，查看：systemctl status xuanlan --no-pager"
    exit 1
fi
