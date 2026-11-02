#!/bin/sh
# 冒烟测试：在临时目录造树 → 建索引 → 校验条数与查询结果
set -eu
BIN=${BIN:-$(dirname "$0")/../bin/xuanlan}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/root/电影/2024" "$TMP/root/work/deep/nest"
: > "$TMP/root/电影/2024/星际穿越.mkv"
: > "$TMP/root/电影/2024/Interstellar.mkv"
: > "$TMP/root/work/report-q3.pdf"
: > "$TMP/root/work/deep/nest/docker-compose.yaml"
: > "$TMP/root/README.md"

"$BIN" build --root "$TMP/root" --out "$TMP/i.xl" 2>/dev/null

have=$("$BIN" info --index "$TMP/i.xl" | awk '/records/{print $2}')
want=$(find "$TMP/root" | wc -l)
[ "$have" = "$want" ] || { echo "FAIL records $have != $want"; exit 1; }

check() {
    n=$("$BIN" query --index "$TMP/i.xl" -n 100 "$1" 2>&1 >/dev/null | awk '/matches/{gsub(">=","",$1); print $1}')
    [ "${n:-0}" = "$2" ] || { echo "FAIL query '$1': got ${n:-0}, want $2"; exit 1; }
}

check interstellar 1
check "docker-compose" 1
check ".mkv" 2
check "电影" 1
check "不存在的东西" 0

# 输出路径必须真实存在
"$BIN" query --index "$TMP/i.xl" -n 50 .mkv 2>/dev/null | while read -r p; do
    [ -e "${p%/}" ] || { echo "FAIL path $p"; exit 1; }
done

echo "smoke: OK ($have records)"
