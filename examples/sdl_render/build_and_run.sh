#!/usr/bin/env bash
# 一键：构建媒体库 → 构建 sdl_render → 跑基准 → 跑回归测试 → PPM 转 PNG。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
BUILD_DIR=".build-sdl-render"

echo "== 1. 构建媒体库（.build-bento-wsl） =="
cmake --build .build-bento-wsl -j"$(nproc)"

echo "== 2. 构建 sdl_render =="
cmake -S examples/sdl_render -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(nproc)"

export DISPLAY="${DISPLAY:-:0}"
echo "== 3. 跑基准（DISPLAY=$DISPLAY） =="
"$BUILD_DIR/sdl_render_bench" --artifacts examples/sdl_render/artifacts "$@"

echo "== 4. 回归测试 =="
"$BUILD_DIR/sdl_test_render"

echo "== 5. PPM -> PNG =="
python3 - <<'PY'
from pathlib import Path

try:
    from PIL import Image
except Exception as exc:  # noqa: BLE001 - 只用于可选转图
    print(f"Pillow 不可用，跳过 PNG：{exc}")
    raise SystemExit(0)

art = Path("examples/sdl_render/artifacts")
for ppm in sorted(art.glob("sdl_validation_*.ppm")):
    png = ppm.with_suffix(".png")
    Image.open(ppm).save(png)
    print(f"{ppm.name} -> {png.name}")
PY

echo "== 完成 =="
echo "数据：/tmp/sdl_render_data.md"
echo "产物：examples/sdl_render/artifacts"
