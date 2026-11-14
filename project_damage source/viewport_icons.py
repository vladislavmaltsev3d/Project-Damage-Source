# один раз читаем формы значков и используем их при перерисовке
import math
import time
from pathlib import Path
from xml.etree import ElementTree

import gpu
from gpu_extras.batch import batch_for_shader
from mathutils import Matrix

DIRECTORY = Path(__file__).resolve().parent / 'icons' / 'viewport'
FILES = {
    'LIVE': 'live-update.svg',
    'MANUAL': 'manual-update.svg',
    'COMPUTE': 'background.svg',
    'BG_RUNNING': 'working.svg',
    'BG_READY': 'complete.svg',
    'BG_ERROR': 'error.svg',
    'CUT_THROUGH': 'knife-through.svg',
    'CUT_SURFACE': 'knife-surface.svg',
}
_points = {}
_batches = {}
_shader = None
_loaded = False


def _read(path):
    root = ElementTree.parse(path).getroot()
    x, y, width, height = map(float, root.attrib['viewBox'].split())
    if not all(math.isfinite(v) for v in (x, y, width, height)) or min(width, height) <= 0:
        raise ValueError('Invalid icon viewBox')
    segments = []
    for element in root:
        kind = element.tag.rsplit('}', 1)[-1]
        if kind not in {'polyline', 'polygon'}:
            continue
        values = tuple(map(float, element.attrib['points'].replace(',', ' ').split()))
        if len(values) % 2 or len(values) < 4 or not all(math.isfinite(v) for v in values):
            raise ValueError('Invalid icon points')
        points = [((a - x) / width, 1.0 - (b - y) / height)
                  for a, b in zip(values[::2], values[1::2])]
        if kind == 'polygon':
            points.append(points[0])
        segments.extend(point for pair in zip(points[:-1], points[1:]) for point in pair)
    if not segments:
        raise ValueError('Icon contains no line geometry')
    return tuple(segments)


def preload():
    global _loaded
    if _loaded:
        return
    for key, filename in FILES.items():
        try:
            _points[key] = _read(DIRECTORY / filename)
        except (OSError, ValueError, KeyError, ElementTree.ParseError) as exc:
            print('Project Damage: cannot load viewport icon', filename, str(exc))
    _loaded = True


def clear(unload=False):
    global _shader, _loaded
    _batches.clear()
    _shader = None
    if unload:
        _points.clear()
        _loaded = False


# используем уже подготовленную форму значка
def draw(kind, x, y, size, active=False):
    global _shader
    key = 'MANUAL' if kind == 'LIVE' and not active else kind
    if size <= 0 or key not in _points:
        return
    if _shader is None:
        _shader = gpu.shader.from_builtin('POLYLINE_UNIFORM_COLOR')
    batch = _batches.get(key)
    if batch is None:
        batch = _batches[key] = batch_for_shader(_shader, 'LINES', {'pos': _points[key]})
    with gpu.matrix.push_pop():
        gpu.matrix.translate((x, y, 0.0))
        gpu.matrix.scale((size, size, 1.0))
        if kind == 'BG_RUNNING':
            gpu.matrix.translate((0.5, 0.5, 0.0))
            gpu.matrix.multiply_matrix(Matrix.Rotation(0.45 * math.sin(time.monotonic() * 5.0), 4, 'Z'))
            gpu.matrix.translate((-0.5, -0.5, 0.0))
        _shader.bind()
        _shader.uniform_float('viewportSize', gpu.state.viewport_get()[2:])
        _shader.uniform_float('lineWidth', 1.8)
        _shader.uniform_float('color', (0.94, 0.96, 1.0, 1.0))
        batch.draw(_shader)
