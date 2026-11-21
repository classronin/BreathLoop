"""导出等价基准：用权威 Python 版渲染同一场景，产出 out\\export_py_breath.webp。

场景与 C++ 侧 `BreathLoop.exe --exporttest` 完全一致（同一张测试图、同两个 BBOX、
同一套套索与导出参数）。用法：

    E:\\Games\\Python\\BreathLoop\\.venv\\Scripts\\python.exe tools\\gen_export_golden.py
"""

import os
import sys
import warnings

import numpy as np
import pyvips

sys.path.insert(0, r'E:\Games\Python\BreathLoop')

warnings.filterwarnings('ignore')

OUT = r'E:\GitHub\C\BreathLoop\out'
ASSET = os.path.join(OUT, 'parity_asset.png')

FRAMES = 12
MS_PER_FRAME = 33.3
OUT_W = 128
OUT_H = 0
FMT = 'webp'


def make_asset(path: str) -> None:
    """120×160 的 RGBA：渐变 rgb + 椭圆软边 alpha（四周留透明边，考验内容框）。"""
    w, h = 120, 160
    xs = np.arange(w, dtype=np.float64)[None, :]
    ys = np.arange(h, dtype=np.float64)[:, None]
    rgb = np.stack([
        np.clip(xs * 2.0, 0, 255) * np.ones((h, 1)),
        np.clip(ys * 1.4, 0, 255) * np.ones((1, w)),
        np.full((h, w), 128.0),
    ], axis=-1)
    d = np.sqrt(((xs - 60.0) / 45.0) ** 2 + ((ys - 80.0) / 65.0) ** 2)
    alpha = np.clip((1.0 - d) * 6.0, 0.0, 1.0) * 255.0
    rgba = np.dstack([rgb, alpha]).astype(np.uint8)
    image = pyvips.Image.new_from_memory(
        rgba.tobytes(), w, h, 4, 'uchar')
    image.pngsave(path)


def build_doc(asset_store):
    """图层、BBOX 与套索：数值在 C++ 侧逐一对齐。"""
    from breathloop.domain.layers import Document
    from breathloop.domain.lasso import Lasso

    asset = asset_store.load(ASSET)
    doc = Document()
    layer = doc.new_layer(asset.name, asset.path, asset.key,
                          asset.width, asset.height, 200.0, 150.0)
    layer.scale = 1.2
    doc.add_layer_top(layer)

    specs = [
        dict(cx=45.0, cy=70.0, rx=30.0, ry=40.0, rot=15.0,
             amp=14.0, speed=100.0, aniso_x=0.35, aniso_y=1.0,
             zeta=0.2, freq=2.5),
        dict(cx=85.0, cy=95.0, rx=22.0, ry=18.0, rot=-30.0,
             amp=8.0, speed=50.0, aniso_x=0.8, aniso_y=0.6,
             zeta=0.35, freq=3.5),
    ]
    for spec in specs:
        region = doc.add_bbox(layer, spec['cx'], spec['cy'])
        for key, value in spec.items():
            if key not in ('cx', 'cy'):
                setattr(region, key, value)

    layer.lasso = Lasso([
        (20.0, 30.0), (100.0, 25.0), (110.0, 120.0), (40.0, 140.0), (15.0, 80.0),
    ])
    return doc


def main() -> int:
    make_asset(ASSET)

    import pyglet
    from passer.app import App
    from breathloop.render.assets import AssetStore
    from breathloop.render.exporter import PasserExporter

    app = App(64, 64, 'parity', scale_mode='stretch', resizable=False)
    app.window.set_visible(False)

    assets = AssetStore()
    doc = build_doc(assets)

    exporter = PasserExporter(
        doc, assets, FRAMES, os.path.join(OUT, 'export_py'),
        lambda index, total: None, lambda path, error: None,
        True, 100, 0, 100, MS_PER_FRAME, OUT_W, OUT_H, FMT)
    assert exporter.step() is False or exporter.step() is True
    while not exporter.step():
        pass
    path, error = exporter.result
    if error is not None:
        raise error
    print(f'python export: {path}')
    print(f'frame={exporter.frame} out={exporter.out_w}x{exporter.out_h} '
          f'ss={exporter.render_ss} n={exporter.n} dt={exporter.dt:.17g}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
