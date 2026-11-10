#!/bin/bash
# 玄览服务启动器：systemd 开机自启与应用中心启动共用同一套参数。
#
# 原先 xuanlan.service 的 ExecStart 把 --port 8971 和 --roots /vol1,/vol2,/vol3,/vol4
# 写死，安装向导改了端口、或管理员改了索引卷后，重启仍按旧参数启动。这里统一从
#   ${TRIM_PKGETC}/service_port   端口（单行数字）
#   ${TRIM_PKGETC}/xuanlan.conf   可选，shell 片段，可覆盖 ROOTS
# 读取配置后再 exec 主程序。
# 未配置 ROOTS 时自动探测 /vol1–/vol8 中实际存在的目录（GitHub #2）。
set -u

SELF_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
APP_DIR="${TRIM_APPDEST:-${SELF_DIR}}"
DATA_DIR="${TRIM_PKGVAR:-/vol1/@appdata/xuanlan}"
ETC_DIR="${TRIM_PKGETC:-/vol1/@appconf/xuanlan}"

PORT="${TRIM_SERVICE_PORT:-8971}"
if [ -r "${ETC_DIR}/service_port" ]; then
    P="$(head -n 1 "${ETC_DIR}/service_port" | tr -d '[:space:]')"
    case "$P" in
        ''|*[!0-9]*) ;;
        *)
            if [ "$P" -ge 1024 ] 2>/dev/null && [ "$P" -le 65535 ] 2>/dev/null; then
                PORT="$P"
            fi
            ;;
    esac
fi

# 自动探测 /vol1–/vol8，避免只认前四个卷；xuanlan.conf 里的 ROOTS 优先级更高
ROOTS=""
for i in 1 2 3 4 5 6 7 8; do
    # 只认真实挂载点；/vol6、/vol7 这类残留空目录不算卷
    if [ -d "/vol${i}" ] && grep -q " /vol${i} " /proc/self/mounts; then
        ROOTS="${ROOTS:+${ROOTS},}/vol${i}"
    fi
done
[ -n "${ROOTS}" ] || ROOTS="/vol1,/vol2,/vol3,/vol4"

if [ -r "${ETC_DIR}/xuanlan.conf" ]; then
    # shellcheck disable=SC1090
    . "${ETC_DIR}/xuanlan.conf"
fi

mkdir -p "${DATA_DIR}/idx" "${DATA_DIR}/logs" || true
echo "xuanlan: launching port=${PORT} roots=${ROOTS}" >&2

exec "${APP_DIR}/bin/xuanlan" serve \
    --port "${PORT}" --bind 127.0.0.1 \
    --socket "${APP_DIR}/app.sock" \
    --ui "${APP_DIR}/ui" \
    --index-dir "${DATA_DIR}/idx" \
    --roots "${ROOTS}"
