"""校准遮罩光栅化：把 C++ 的填充规则在 Python 里复刻，逐条与 cv2 对拍。

用来找出"哪种端点/取整约定与 cv2.fillPoly 完全一致"，找到后再改 mask.cpp。
"""

import math

import cv2
import numpy as np

CASES = [
    ('a', 40, 28, 2.0,
     [(-5.0, -3.0), (30.0, 2.0), (38.0, 20.0), (15.0, 34.0), (2.0, 10.0)]),
    ('b', 96, 64, 1.0,
     [(10.0, 8.0), (80.0, 14.0), (70.0, 55.0), (20.0, 48.0)]),
]


def supersample(tw, th):
    return 2 if tw * th <= 1200000 else 1


def cv_buffer(points, tw, th, scale, ss, pad):
    w, h = tw + 2 * pad, th + 2 * pad
    sw, sh = w * ss, h * ss
    out = np.zeros((sh, sw), np.uint8)
    fixed = ss << 4
    verts = np.round((np.asarray(points, np.float64) * scale + pad) * fixed)
    cv2.fillPoly(out, [verts.astype(np.int32)], 255, cv2.LINE_8, 4)
    return out


def my_buffer(points, tw, th, scale, ss, pad, row_rule, col_rule):
    w, h = tw + 2 * pad, th + 2 * pad
    sw, sh = w * ss, h * ss
    grid = float(ss) * 16.0
    verts = [(math.floor((p[0] * scale + pad) * grid + 0.5) / 16.0,
              math.floor((p[1] * scale + pad) * grid + 0.5) / 16.0)
             for p in points]
    out = np.zeros((sh, sw), np.uint8)
    n = len(verts)
    for y in range(sh):
        sy = float(y)
        xs = []
        for i in range(n):
            a = verts[i]
            b = verts[(i + 1) % n]
            if a[1] == b[1]:
                continue
            lo, hi = min(a[1], b[1]), max(a[1], b[1])
            if row_rule == 'half':
                if sy < lo or sy >= hi:
                    continue
            else:
                if sy < lo or sy > hi:
                    continue
            xs.append(a[0] + (sy - a[1]) * (b[0] - a[0]) / (b[1] - a[1]))
        if len(xs) < 2:
            continue
        xs.sort()
        for k in range(0, len(xs) - 1, 2):
            if col_rule == 'round':
                first = math.floor(xs[k] + 0.5)
                last = math.floor(xs[k + 1] + 0.5)
            elif col_rule == 'halfopen':
                first = math.ceil(xs[k] - 0.5)
                last = math.ceil(xs[k + 1] - 0.5) - 1
            elif col_rule == 'fixed':
                first = int(xs[k] * 16) >> 4
                last = int(xs[k + 1] * 16) >> 4
            elif col_rule == 'fixed_round':
                first = int(xs[k] * 16 + 0.5) >> 4
                last = int(xs[k + 1] * 16 + 0.5) >> 4
            else:  # 右端含、左端取整到像素
                first = math.floor(xs[k] + 0.5)
                last = math.floor(xs[k + 1] + 0.5) - 1
            first = max(0, first)
            last = min(sw - 1, last)
            if last >= first:
                out[y, first:last + 1] = 255
    return out


def main() -> None:
    for row_rule in ('half', 'closed'):
        for col_rule in ('round', 'halfopen', 'rightopen', 'fixed',
                         'fixed_round'):
            total = 0
            detail = []
            for name, iw, ih, scale_detail, points in CASES:
                scale = min(scale_detail, 1024 / max(1, iw, ih))
                tw, th = max(1, round(iw * scale)), max(1, round(ih * scale))
                ss = supersample(tw, th)
                ref = cv_buffer(points, tw, th, scale, ss, 0)
                mine = my_buffer(points, tw, th, scale, ss, 0, row_rule,
                                 col_rule)
                diff = int((ref != mine).sum())
                total += diff
                detail.append(f'{name}:{diff}')
            print(f'row={row_rule:6s} col={col_rule:9s} total={total:5d} '
                  f'({",".join(detail)})')


if __name__ == '__main__':
    main()
