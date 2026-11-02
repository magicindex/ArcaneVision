#!/bin/bash
# 构建并安装/升级到飞牛：VOLUME_ID=<卷id> ./scripts/deploy.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CLI="${TRIM_CLI:-trim-cli}"
VOL="${VOLUME_ID:?用法: VOLUME_ID=<卷id> $0}"

bash "$ROOT/scripts/build-fpk.sh"
FPK=$(ls -t "$ROOT"/dist/*.fpk | head -1)
echo "→ $FPK (volume $VOL)"
"$CLI" app install-fpk "$FPK" --volume-id "$VOL" --accept-license --yes
"$CLI" app status xuanlan || true
