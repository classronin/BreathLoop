"""逐帧比对两份 WebP 动画，给出硬数字与判定。

用法（在 Python 权威版的 venv 里跑，那里有 pyvips）：
    python tools/verify_export.py <a.webp> <b.webp>

判定口径来自 `项目导航.md` 的验收基线：产物等价——元数据必须完全一致，
像素差落进阈值（见 THRESHOLDS）。阈值不是拍脑袋定的，是 10-27 那次
`--exporttest` 的实测值留了余量（内部 0.55 / 全帧 12.95 / alpha 3.64 / max 24）。
"""
import sys

import numpy as np
import pyvips

# (键, 上限, 说明)
THRESHOLDS = [
    ('inner_rgb_mean', 0.60, '内部（alpha≥250）RGB 平均差'),
    ('all_rgb_mean', 17.50, '全帧 RGB 平均差（含全透明区的解混差异）'),
    ('alpha_mean', 4.00, 'alpha 平均差'),
    ('alpha_max', 26.0, 'alpha 单点最大差'),
    ('px_over_8', 6200.0, '任一通道差 >8 的像素数'),
]

META_KEYS = ('width', 'height', 'n-pages', 'page-height', 'delay', 'loop')


def load_frames(path):
    """读回动画并按 page-height 切成逐帧 RGBA（float32，0..255）。"""
    image = pyvips.Image.new_from_file(path, n=-1)
    page_h = image.get('page-height')
    width = image.width
    pages = image.get('n-pages')
    frames = []
    for index in range(pages):
        area = image.crop(0, index * page_h, width, page_h)
        rgba = area.colourspace('srgb').cast('uchar')
        arr = np.ndarray(
            (rgba.height, rgba.width, 4),
            dtype=np.uint8,
            buffer=rgba.write_to_memory(),
        ).astype(np.float32)
        frames.append(arr)
    return frames, {key: image.get(key) for key in META_KEYS if image.get(key) is not None}


def compare(a_path, b_path):
    a_frames, a_meta = load_frames(a_path)
    b_frames, b_meta = load_frames(b_path)

    ok = True
    for key in META_KEYS:
        left, right = a_meta.get(key), b_meta.get(key)
        same = left == right
        ok = ok and same
        print(f'  meta {key:10s} {left} vs {right}  {"OK" if same else "DIFF"}')
    if len(a_frames) != len(b_frames):
        print(f'  frames {len(a_frames)} vs {len(b_frames)}  DIFF')
        return False

    print(f'  frames {len(a_frames)} · {a_frames[0].shape[1]}x{a_frames[0].shape[0]}')
    totals = {key: [] for key, _, _ in THRESHOLDS}
    for index, (fa, fb) in enumerate(zip(a_frames, b_frames)):
        diff = np.abs(fa - fb)
        alpha_a = fa[..., 3]
        inner = alpha_a >= 250
        inner_rgb_mean = float(diff[..., :3][inner].mean()) if inner.any() else 0.0
        alpha_diff = diff[..., 3]
        premul = np.abs(
            fa[..., :3] * fa[..., 3:4] / 255.0 - fb[..., :3] * fb[..., 3:4] / 255.0
        )
        totals['inner_rgb_mean'].append(inner_rgb_mean)
        totals['all_rgb_mean'].append(float(diff[..., :3].mean()))
        totals['alpha_mean'].append(float(alpha_diff.mean()))
        totals['alpha_max'].append(float(alpha_diff.max()))
        totals['px_over_8'].append(float((diff > 8).any(axis=2).sum()))
        print(f'  frame {index:03d} inner {inner_rgb_mean:7.3f} '
              f'all {totals["all_rgb_mean"][-1]:7.2f} '
              f'premul {float(premul.mean()):5.2f} '
              f'alpha {totals["alpha_mean"][-1]:6.2f}/{totals["alpha_max"][-1]:5.1f} '
              f'px>8 {int(totals["px_over_8"][-1]):5d}')

    print()
    for key, limit, label in THRESHOLDS:
        worst = max(totals[key]) if key == 'alpha_max' else max(totals[key])
        passed = worst <= limit
        ok = ok and passed
        print(f'  {label:28s} 最差 {worst:8.3f} ≤ {limit:7.2f}  {"PASS" if passed else "FAIL"}')
    return ok


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    print(f'A = {argv[1]}\nB = {argv[2]}')
    return 0 if compare(argv[1], argv[2]) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
