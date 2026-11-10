#!/bin/bash
# 打包 fnOS 应用：dist/xuanlan_<version>_amd64.fpk
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${VERSION:-0.8.24}"
PORT="${PORT:-8971}"
STAGE="$ROOT/build/fpk"
PAYLOAD="$STAGE/payload"
FPKROOT="$STAGE/fpkroot"
DIST="$ROOT/dist"

printf '#ifndef XL_APP_VERSION\n#define XL_APP_VERSION "%s"\n#endif\n' "$VERSION" > "$ROOT/src/version.h"
make -C "$ROOT" >/dev/null

rm -rf "$STAGE"
mkdir -p "$PAYLOAD/bin" "$PAYLOAD/ui/images" "$PAYLOAD/ui/vendor" "$PAYLOAD/config" \
         "$FPKROOT/wizard" "$FPKROOT/cmd" "$FPKROOT/config" "$DIST"

# ---- payload（解到安装目录） ----
install -m 0755 "$ROOT/bin/xuanlan" "$PAYLOAD/bin/xuanlan"
install -m 0644 "$ROOT/ui/index.html" "$PAYLOAD/ui/index.html"
install -m 0644 "$ROOT/pkg/ui/config" "$PAYLOAD/ui/config"
install -m 0644 "$ROOT/pkg/ui/images/icon_64.png" "$PAYLOAD/ui/images/icon_64.png"
install -m 0644 "$ROOT/pkg/ui/images/icon_256.png" "$PAYLOAD/ui/images/icon_256.png"
install -m 0644 "$ROOT/ui/vendor/pdf.min.mjs" "$PAYLOAD/ui/vendor/pdf.min.mjs"
install -m 0644 "$ROOT/ui/vendor/pdf.worker.min.mjs" "$PAYLOAD/ui/vendor/pdf.worker.min.mjs"
install -m 0644 "$ROOT/ui/vendor/fflate.min.js" "$PAYLOAD/ui/vendor/fflate.min.js"
install -m 0644 "$ROOT/ui/vendor/LICENSE.pdfjs" "$PAYLOAD/ui/vendor/LICENSE.pdfjs"
install -m 0644 "$ROOT/ui/vendor/LICENSE.fflate.txt" "$PAYLOAD/ui/vendor/LICENSE.fflate.txt"
install -m 0644 "$ROOT/ui/vendor/LICENSE.feather-icons.txt" "$PAYLOAD/ui/vendor/LICENSE.feather-icons.txt"
install -m 0644 "$ROOT/LICENSE" "$PAYLOAD/ui/LICENSE.txt"
install -m 0644 "$ROOT/THIRD_PARTY.md" "$PAYLOAD/ui/THIRD_PARTY.md"
install -m 0644 "$ROOT/NOTICE.md" "$PAYLOAD/ui/NOTICE.md"
install -m 0644 "$ROOT/pkg/config/privilege" "$PAYLOAD/config/privilege"
install -m 0644 "$ROOT/pkg/config/resource" "$PAYLOAD/config/resource"
install -m 0644 "$ROOT/pkg/xuanlan.service" "$PAYLOAD/xuanlan.service"
install -m 0755 "$ROOT/pkg/start.sh" "$PAYLOAD/start.sh"

tar -czf "$STAGE/app.tgz" -C "$PAYLOAD" bin ui config xuanlan.service start.sh
CHECKSUM="$(md5sum "$STAGE/app.tgz" | cut -d' ' -f1)"

# ---- 包根 ----
cp "$STAGE/app.tgz" "$FPKROOT/app.tgz"
install -m 0644 "$ROOT/pkg/ICON.PNG" "$FPKROOT/ICON.PNG"
install -m 0644 "$ROOT/pkg/ICON_256.PNG" "$FPKROOT/ICON_256.PNG"
install -m 0644 "$ROOT/pkg/config/privilege" "$FPKROOT/config/privilege"
install -m 0644 "$ROOT/pkg/config/resource" "$FPKROOT/config/resource"
for f in main port_config install_init install_callback upgrade_init upgrade_callback \
         uninstall_init uninstall_callback config_init config_callback; do
    install -m 0755 "$ROOT/pkg/cmd/$f" "$FPKROOT/cmd/$f"
done
for f in install uninstall config; do
    install -m 0644 "$ROOT/pkg/wizard/$f" "$FPKROOT/wizard/$f"
done
sed -e "s/@VERSION@/$VERSION/g" -e "s/@PORT@/$PORT/g" -e "s/@CHECKSUM@/$CHECKSUM/g" \
    "$ROOT/pkg/manifest.in" > "$FPKROOT/manifest"

OUT="$DIST/xuanlan_${VERSION}_amd64.fpk"
rm -f "$OUT"
tar -czf "$OUT" -C "$FPKROOT" .
echo "built: $OUT"
echo "checksum(app.tgz)=$CHECKSUM"
tar -tzf "$OUT" | sed 's/^/  /'
