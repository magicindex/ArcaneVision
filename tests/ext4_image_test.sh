#!/bin/bash
# 小 ext4 镜像正确性测试：同一棵树分别用 getdents 与裸设备读取，比较完整路径列表。
set -euo pipefail
cd "$(dirname "$0")/.."

TMP="$PWD/build/ext4test"
IMG="$TMP/xl_ext4_test.img"
FIND="$TMP/find.txt"
IDX_G="$TMP/getdents.idx"
IDX_R="$TMP/raw.idx"

rm -rf "$TMP"
mkdir -p "$TMP"
truncate -s 192M "$IMG"
mkfs.ext4 -q -F -b 4096 -O bigalloc -C 65536 "$IMG"

populate() {
    local root="$1" out="$2"
    mkdir -p "$root/a/b/c" "$root/中文目录/深层" "$root/bigdir" "$root/empty"
    local d="$root"
    local i=1
    while [ "$i" -le 20 ]; do d="$d/d$i"; mkdir -p "$d"; i=$((i+1)); done
    echo deep > "$d/深度文件.txt"
    for i in $(seq 1 1500); do : > "$root/bigdir/file_$i.txt"; done
    echo hello > "$root/with space.txt"
    echo hi > "$root/a/b/c/deep.txt"
    ln -s /etc/passwd "$root/link_to_passwd"
    ln -s . "$root/loop_self"
    # 隐藏项：默认应被跳过（--include-hidden 时与 find 对齐）
    mkdir -p "$root/@eaDir" "$root/中文目录/深层/.cache"
    echo meta > "$root/@eaDir/meta.txt"
    echo hidden > "$root/中文目录/深层/.cache/x.txt"
    echo dot > "$root/.hiddenfile"
    # 系统杂项：默认也应跳过
    echo t > "$root/Thumbs.db"
    echo t > "$root/中文目录/深层/desktop.ini"
    mkdir -p "$root/中文目录/深层/__MACOSX"
    echo m > "$root/中文目录/深层/__MACOSX/a.txt"
    find "$root" | sort > "$out"
}

if [ "${XL_EXT4_HOST:-0}" = "1" ]; then
    sudo mkdir -p /mnt/xl
    sudo mount -o loop "$IMG" /mnt/xl
    trap 'sudo umount /mnt/xl 2>/dev/null || true' EXIT
    sudo chmod 777 /mnt/xl
    populate /mnt/xl "$FIND"
    ./bin/xuanlan build --root /mnt/xl --out "$IDX_G" --include-hidden >/dev/null 2>&1
    sudo umount /mnt/xl
    trap - EXIT
else
    docker run --rm --privileged -v "$PWD:/w" -v /tmp:/tmp subnetdesk-build:bookworm bash -c "
$(declare -f populate)
set -e
mkdir -p /mnt/xl
mount -o loop /w/build/ext4test/xl_ext4_test.img /mnt/xl
populate /mnt/xl /w/build/ext4test/find.txt
/w/bin/xuanlan build --root /mnt/xl --out /w/build/ext4test/getdents.idx --include-hidden >/dev/null 2>&1
umount /mnt/xl
"
fi

./bin/xuanlan build --device "$IMG" --root /mnt/xl --out "$IDX_R" --include-hidden -v
./bin/xuanlan dump --index "$IDX_G" | sort > "$TMP/getdents.dump"
./bin/xuanlan dump --index "$IDX_R" | sort > "$TMP/raw.dump"

n_find=$(wc -l < "$FIND")
n_get=$(wc -l < "$TMP/getdents.dump")
n_raw=$(wc -l < "$TMP/raw.dump")
echo "find=$n_find getdents=$n_get raw=$n_raw"
if ! diff -q "$FIND" "$TMP/getdents.dump" >/dev/null; then echo "getdents 与 find 不一致"; diff "$FIND" "$TMP/getdents.dump" | head; exit 1; fi
if ! diff -q "$TMP/getdents.dump" "$TMP/raw.dump" >/dev/null; then echo "裸设备与 getdents 不一致"; diff "$TMP/getdents.dump" "$TMP/raw.dump" | head -40; exit 1; fi
# 裸扫必须从 inode 表带回真实 size/mtime（deep.txt 内容 "hi\n"）
raw_meta=$("$PWD/bin/xuanlan" query --index "$IDX_R" --json -n 5 deep.txt 2>/dev/null)
case "$raw_meta" in
    *'"size":3'*) ;;
    *) echo "FAIL raw size: $raw_meta"; exit 1 ;;
esac
case "$raw_meta" in
    *'"mtime":0'*) echo "FAIL raw mtime missing"; exit 1 ;;
esac

# 默认（skipHidden=on）：裸扫也不应出现 . / @ 隐藏项
IDX_S="$TMP/skip.idx"
./bin/xuanlan build --device "$IMG" --root /mnt/xl --out "$IDX_S" >/dev/null 2>&1
./bin/xuanlan dump --index "$IDX_S" > "$TMP/skip.dump"
if grep -qiE '/(@eaDir|\.cache|\.hiddenfile|Thumbs\.db|desktop\.ini|__MACOSX)' "$TMP/skip.dump"; then
    echo "FAIL: 默认未跳过隐藏/杂项"; grep -iE '/(@eaDir|\.cache|\.hiddenfile|Thumbs\.db|desktop\.ini|__MACOSX)' "$TMP/skip.dump" | head; exit 1
fi
n_skip=$(wc -l < "$TMP/skip.dump")
echo "skip-hidden=$n_skip full=$n_raw"
[ "$n_skip" -lt "$n_raw" ] || { echo "FAIL: 跳过隐藏后记录数未减少"; exit 1; }

echo "OK: 裸设备扫描结果与 getdents 完全一致（--include-hidden 全量对齐）"
