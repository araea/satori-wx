#!/data/data/com.termux/files/usr/bin/bash
# 发布：核对版本 → 生成 update.json → 创建 GitHub release 并上传安装包。
#
# 用法：tools/release.sh [--dry-run] [--notes FILE]
#   先 ./build.sh。默认说明取 changelogs/<版本>.md；
#   --dry-run 只生成并核对，不创建 release。
#
# 上传的三个文件：satori-wx-server-<版本>.zip、update.json（模块管理页与 root 管理器的模块列表读它）、
# CHANGELOG.md（模块列表的更新说明）。tag 即模块版本（v0.19.0），与历史一致。
set -euo pipefail
R=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$R"
DRY=0
NOTES=
while [ $# -gt 0 ]; do
  case $1 in
    --dry-run) DRY=1 ;;
    --notes) NOTES=$2; shift ;;
    *) echo "usage: $0 [--dry-run] [--notes FILE]" >&2; exit 2 ;;
  esac
  shift
done

VER=$(sed -n 's/^version=//p' module.prop)
CODE=$(sed -n 's/^versionCode=//p' module.prop)
ZIP=build/satori-wx-server-$VER.zip
NOTES=${NOTES:-changelogs/$VER.md}
[ -f "$NOTES" ] || { echo "缺少更新说明：$NOTES" >&2; exit 1; }
[ -f "$ZIP" ] || { echo "缺少 $ZIP，先跑 ./build.sh" >&2; exit 1; }
[ "$(unzip -p "$ZIP" module.prop | sed -n 's/^versionCode=//p')" = "$CODE" ] \
  || { echo "模块包的 versionCode 与 module.prop 不一致，重新 ./build.sh" >&2; exit 1; }
[ -z "$(git status --porcelain -- native webroot ctl.sh module.prop)" ] || echo "注意：源码有未提交的改动" >&2

mkdir -p build
cp "$NOTES" build/CHANGELOG.md
python3 tools/release-manifest.py --repo araea/satori-wx --tag "$VER" --notes "$NOTES" \
  --module "$ZIP" --out build/update.json

if [ "$DRY" = 1 ]; then echo "dry-run：不创建 release（tag $VER）"; exit 0; fi
gh release create "$VER" "$ZIP" build/update.json build/CHANGELOG.md \
  --repo araea/satori-wx --target master --title "$(sed -n "1s/^#* *//p" "$NOTES")" --notes-file "$NOTES"
echo "已发布 $VER"
