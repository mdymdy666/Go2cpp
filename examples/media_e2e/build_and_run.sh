#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 一键构建并运行媒体端到端验证：
#   1) 构建主工程（.build-bento-wsl）的 media/Bento 库；
#   2) 构建 examples/media_e2e（独立构建目录 .build-media-e2e）；
#   3) 跑 demo（生成 PPM + /tmp/media_e2e_data.md）；
#   4) 用 Pillow 把 PPM 转 PNG，并生成「采集源 | 渲染结果」对照图。
#
# 用法：bash examples/media_e2e/build_and_run.sh [--quick]
# 产物：examples/media_e2e/artifacts/*.ppm|*.png
# ---------------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${ROOT}/.build-media-e2e"
ARTIFACTS="${SCRIPT_DIR}/artifacts"
QUICK="${1:-}"

echo "==> 主工程构建目录：${ROOT}/.build-bento-wsl"
cmake --build "${ROOT}/.build-bento-wsl" --target go2cpp_media -j"$(nproc)"

echo "==> 配置 examples/media_e2e"
cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=Release

echo "==> 构建 demo 与回归测试"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

mkdir -p "${ARTIFACTS}"

echo "==> 运行 demo"
EXTRA=()
if [[ "${QUICK}" == "--quick" ]]; then
    EXTRA+=(--quick)
fi
"${BUILD_DIR}/media_e2e_demo" --artifacts "${ARTIFACTS}" "${EXTRA[@]}"

if [[ -x "${BUILD_DIR}/media_test_e2e_jitter" ]]; then
    echo "==> 运行 media_e2e_jitter 回归测试"
    "${BUILD_DIR}/media_test_e2e_jitter"
fi

echo "==> PPM -> PNG（Pillow）"
python3 - "${ARTIFACTS}" <<'PY'
import sys
from pathlib import Path
from PIL import Image, ImageDraw

artifacts = Path(sys.argv[1])
ppms = sorted(artifacts.glob("*.ppm"))
if not ppms:
    print("没有 PPM 产物")
    sys.exit(0)

def to_png(path: Path) -> Image.Image:
    image = Image.open(path).convert("RGB")
    image.save(path.with_suffix(".png"))
    return image

# 1) 单张转换
for ppm in ppms:
    to_png(ppm)

# 2) 采集/渲染对照图（并排 + 标签）
pairs = []
for ppm in ppms:
    if ppm.stem.endswith("_capture"):
        render = ppm.with_name(ppm.stem[:-len("_capture")] + "_render.ppm")
        if render.exists():
            pairs.append((ppm, render))

bad_pairs = [(x, x.with_name(x.stem[:-3] + "expected.ppm"))
             for x in sorted(artifacts.glob("*_bad.ppm"))
             if x.with_name(x.stem[:-3] + "expected.ppm").exists()]
pairs = pairs + bad_pairs

for capture, render in pairs:
    left = Image.open(capture).convert("RGB")
    right = Image.open(render).convert("RGB")
    width = left.width + right.width + 12
    height = max(left.height, right.height) + 28
    canvas = Image.new("RGB", (width, height), (24, 24, 24))
    canvas.paste(left, (0, 28))
    canvas.paste(right, (left.width + 12, 28))
    draw = ImageDraw.Draw(canvas)
    draw.text((4, 8), f"capture: {capture.name}", fill=(255, 255, 255))
    draw.text((left.width + 16, 8), f"render: {render.name}", fill=(0, 255, 128))
    if capture.stem.endswith("_bad"):
        out = artifacts / (capture.stem + "_compare.png")
    else:
        out = artifacts / (capture.stem[:-len("_capture")] + "_compare.png")
    canvas.save(out)
    print(f"compare -> {out}")

print(f"PNG 数量：{len(list(artifacts.glob('*.png')))}")
PY

echo "==> 产物：${ARTIFACTS}"
ls -1 "${ARTIFACTS}"/*.png 2>/dev/null | head -40
echo "==> 数据：/tmp/media_e2e_data.md"
