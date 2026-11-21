"""遮罩数值对照：权威 Python 版（cv2）与 C++ 自写填充/EDT 的逐字节差。

用法：E:\\Games\\Python\\BreathLoop\\.venv\\Scripts\\python.exe tools\\gen_mask_golden.py
输出：out\\mask_<case>_py.bin / out\\coverage_<case>_py.bin + out\\mask_python.txt
"""

import os
import sys

import numpy as np

sys.path.insert(0, r'E:\Games\Python\BreathLoop')

from breathloop import config as C
from breathloop.domain.lasso import Lasso
from breathloop.render import mask as mask_ui

OUT = r'E:\GitHub\C\BreathLoop\out'

CASES = [
    ('a', 40, 28, 2.0,
     [(-5.0, -3.0), (30.0, 2.0), (38.0, 20.0), (15.0, 34.0), (2.0, 10.0)]),
    ('b', 96, 64, 1.0,
     [(10.0, 8.0), (80.0, 14.0), (70.0, 55.0), (20.0, 48.0)]),
    ('c', 96, 64, 1.0, [(1.0, 1.0), (2.0, 2.0)]),
]


def main() -> int:
    lines = []
    for name, w, h, detail, points in CASES:
        lasso = Lasso(list(points))
        field = mask_ui.sdf(lasso, w, h, detail)
        cov = mask_ui.coverage(lasso, w, h)
        # Python 的 y 向下，C++ 的图层局部坐标 y 向上；两边算法一致，只差整块行镜像，
        # 所以基准先按行翻一次，才能和 C++ 的纹素逐字节对照。
        flipped_field = np.flipud(np.frombuffer(
            field.data, dtype=np.uint8).reshape(field.height, field.width))
        flipped_cov = np.flipud(np.asarray(cov))
        with open(os.path.join(OUT, f'mask_{name}_py.bin'), 'wb') as handle:
            handle.write(flipped_field.tobytes())
        with open(os.path.join(OUT, f'coverage_{name}_py.bin'), 'wb') as handle:
            handle.write(flipped_cov.tobytes())
        lines.append(f'mask.{name}.size={field.width} {field.height}')
        lines.append(f'mask.{name}.scale={field.scale:.17g}')
        lines.append(
            f'mask.{name}.feather={field.feather(C.LASSO_FEATHER):.17g}')
    with open(os.path.join(OUT, 'mask_python.txt'), 'w', encoding='utf-8') as handle:
        handle.write('\n'.join(lines) + '\n')
    print('\n'.join(lines))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
