"""生成 domain 层的黄金数值：Python 权威版算一遍，C++ 版照同一组输入算一遍。

只用于开发期比对（附属于 tools/，不随 exe 交付）。
用法：<BreathLoop 的 venv python> tools/gen_golden.py
"""
import sys

sys.path.insert(0, r'E:\Games\Python\BreathLoop')

from breathloop.domain import seamless
from breathloop.domain.layers import Document
from breathloop.domain.region import Region

FMT = '{:.17g}'


def emit(name, value):
    print(f'{name}={FMT.format(value)}')


def main():
    regions = []
    for amp, speed, ax, ay, zeta, freq in [
        (10.0, 20.0, 0.3, 1.0, 0.15, 2.5),
        (20.0, 12.0, 0.8, 1.0, 0.25, 1.8),
        (30.0, 8.0, 0.5, 0.5, 0.4, 4.0),
    ]:
        r = Region()
        r.amp, r.speed = amp, speed
        r.aniso_x, r.aniso_y = ax, ay
        r.zeta, r.freq = zeta, freq
        regions.append(r)

    speeds = [r.speed for r in regions]
    emit('common_period', seamless.common_period(speeds))

    plan = seamless.plan_loop(60, 33.3, speeds)
    emit('plan.n', plan['n'])
    emit('plan.per_cycle', plan['per_cycle'])
    emit('plan.period', plan['period'])
    emit('plan.duration', plan['duration'])

    table = seamless.build_table(regions, plan['period'])
    emit('table.frames', len(table))

    for t in (0.5, 3.2, 4.9):
        for i, (dx, dy) in enumerate(seamless.sample_table(table, plan['period'], t)):
            emit(f'sample.{t}.{i}.dx', dx)
            emit(f'sample.{t}.{i}.dy', dy)

    doc = Document()
    layer = doc.new_layer('a.png', 'a.png', 1, 100, 200, 50.0, 60.0)
    layer.scale = 1.5
    doc.add_layer_top(layer)
    wx, wy = layer.to_world(10.0, 20.0)
    emit('layer.world.x', wx)
    emit('layer.world.y', wy)
    lx, ly = layer.from_world(wx, wy)
    emit('layer.local.x', lx)
    emit('layer.local.y', ly)

    child = doc.new_layer('b.png', 'b.png', 2, 40, 80, 120.0, 90.0)
    child.parent = layer
    layer.children.append(child)
    mx, my = child.map_point_from(layer, 10.0, 20.0)
    emit('map.x', mx)
    emit('map.y', my)
    emit('len_factor', child.len_factor_from(layer))


if __name__ == '__main__':
    main()
