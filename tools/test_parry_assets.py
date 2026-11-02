"""Validate the packaged parry NIFs without Skyrim or third-party Python modules.

Only the SSE NIF fields needed by these effects are decoded. Layouts follow
https://github.com/niftools/nifxml/blob/develop/nif.xml.
"""
import hashlib
import math
from pathlib import Path
import re
import struct
import unittest


ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'package'
EFFECTS = ('PerfectBlockImpact', 'PerfectRing')
# These texture paths are present in the base game's texture archives.
VANILLA_TEXTURES = {
    'textures/effects/iceshards01.dds',
    'textures/effects/gradients/gradflamethin01.dds',
}


class Reader:
    def __init__(self, data):
        self.data = data
        self.position = 0

    def take(self, size):
        if size < 0 or self.position + size > len(self.data):
            raise ValueError('Truncated NIF field')
        result = self.data[self.position:self.position + size]
        self.position += size
        return result

    def read(self, format):
        return struct.unpack('<' + format, self.take(struct.calcsize('<' + format)))

    def scalar(self, format):
        return self.read(format)[0]

    def string(self, size_type='I'):
        return self.take(self.scalar(size_type)).rstrip(b'\0').decode('utf-8')

    def done(self):
        if self.position != len(self.data):
            raise ValueError('Unexpected trailing NIF fields')


def keys(reader, width=1, boolean=False):
    count = reader.scalar('I')
    if not count:
        return []
    kind = reader.scalar('I')
    if kind not in ((5,) if boolean else (1, 2, 3)):
        raise ValueError(f'Unsupported key interpolation {kind}')
    result = []
    for _ in range(count):
        time = reader.scalar('f')
        value = reader.read('B' if boolean else 'f' * width)
        tangent_width = width * 2 if kind == 2 else 3 if kind == 3 else 0
        tangent = reader.read('f' * tangent_width)
        if not all(math.isfinite(x) for x in (time, *value, *tangent)):
            raise ValueError('Nonfinite animation key')
        if time < 0 or (result and time < result[-1][0]):
            raise ValueError('Animation keys are not in time order')
        if boolean and value[0] not in (0, 1):
            raise ValueError('Invalid boolean animation value')
        result.append((time, value, tangent))
    return result


class Nif:
    def __init__(self, path):
        self.data = path.read_bytes()
        reader = Reader(self.data)
        header_end = self.data.index(b'\n') + 1
        if reader.take(header_end) != b'Gamebryo File Format, Version 20.2.0.7\n':
            raise ValueError('Expected an SSE Gamebryo header')
        version, endian, user, count, bethesda = reader.read('IBIII')
        if (version, endian, user, bethesda) != (0x14020007, 1, 12, 100):
            raise ValueError('Expected SSE NIF version 20.2.0.7 / 12 / 100')
        for _ in range(3):
            reader.string('B')
        types = [reader.string() for _ in range(reader.scalar('H'))]
        indices = reader.read('H' * count)
        sizes = reader.read('I' * count)
        strings, longest = reader.read('II')
        self.strings = [reader.string() for _ in range(strings)]
        if any(len(value.encode('utf-8')) > longest for value in self.strings):
            raise ValueError('Incorrect string-table length')
        reader.take(reader.scalar('I') * 4)
        self.blocks = []
        for index, size in zip(indices, sizes):
            if index >= len(types):
                raise ValueError('Invalid block type index')
            self.blocks.append((types[index], reader.take(size)))
        self.roots = reader.read('i' * reader.scalar('I'))
        reader.done()
        if not self.roots or any(not 0 <= root < count for root in self.roots):
            raise ValueError('Invalid scene roots')

    def block(self, index, expected=None):
        if not 0 <= index < len(self.blocks):
            raise ValueError(f'Invalid block reference {index}')
        kind, data = self.blocks[index]
        if expected and kind != expected:
            raise ValueError(f'Expected {expected}, got {kind}')
        return Reader(data)

    def name(self, index):
        string_index = self.block(index).scalar('i')
        return self.strings[string_index] if string_index >= 0 else ''

    def curve(self, interpolator):
        kind, _ = self.blocks[interpolator]
        reader = self.block(interpolator)
        if kind == 'NiFloatInterpolator':
            _, data = reader.read('fi')
            width, boolean, expected = 1, False, 'NiFloatData'
        elif kind in ('NiBoolInterpolator', 'NiBoolTimelineInterpolator'):
            _, data = reader.read('Bi')
            width, boolean, expected = 1, True, 'NiBoolData'
        elif kind == 'NiPoint3Interpolator':
            _, _, _, data = reader.read('fffi')
            width, boolean, expected = 3, False, 'NiPosData'
        else:
            raise ValueError(f'Unsupported curve {kind}')
        reader.done()
        if data < 0:
            raise ValueError('Missing animation curve')
        reader = self.block(data, expected)
        result = keys(reader, width, boolean)
        reader.done()
        return result

    def transform_curves(self, interpolator):
        reader = self.block(interpolator, 'NiTransformInterpolator')
        reader.take(32)
        data = reader.scalar('i')
        reader.done()
        reader = self.block(data, 'NiTransformData')
        rotations = reader.scalar('I')
        if rotations:
            kind = reader.scalar('I')
            if kind == 4:
                for _ in range(3):
                    keys(reader)
            elif kind in (1, 2, 3):
                for _ in range(rotations):
                    values = reader.read('f' * (8 if kind == 3 else 5))
                    if not all(math.isfinite(value) for value in values):
                        raise ValueError('Nonfinite rotation key')
            else:
                raise ValueError('Unsupported rotation key type')
        translation, scale = keys(reader, 3), keys(reader)
        reader.done()
        return translation, scale

    def shape(self, index):
        reader, _, _, _ = self.av_object(index)
        bounds = reader.read('ffff')
        _, shader, _ = reader.read('iii')
        descriptor, triangles, count, size = reader.read('QHHI')
        stride = (descriptor & 15) * 4
        if stride < 12 or size != count * stride + triangles * 6 or not count:
            raise ValueError('Unexpected SSE vertex buffer layout')
        vertices = [struct.unpack_from('<fff', reader.take(stride)) for _ in range(count)]
        if not all(math.isfinite(value) for vertex in vertices for value in vertex):
            raise ValueError('Nonfinite vertex')
        indices = reader.read('H' * (triangles * 3))
        if any(index >= count for index in indices):
            raise ValueError('Triangle references a missing vertex')
        particle_words = reader.scalar('I')
        if particle_words:
            if particle_words * 2 != count * 12 + triangles * 6:
                raise ValueError('Unexpected particle mesh layout')
            particle_vertices = [reader.read('eee') for _ in range(count)]
            particle_normals = [reader.read('eee') for _ in range(count)]
            particle_triangles = reader.read('H' * (triangles * 3))
            if not all(math.isfinite(v) for vertex in (*particle_vertices, *particle_normals) for v in vertex):
                raise ValueError('Nonfinite particle mesh')
            if particle_triangles != indices:
                raise ValueError('Particle mesh triangle indices differ')
            if any(abs(min(v[a] for v in particle_vertices) + max(v[a] for v in particle_vertices)) > 0.06
                   for a in range(3)):
                raise ValueError('Particle emission mesh is not centered')
            if any(math.sqrt(sum(v * v for v in vertex)) > bounds[3] + 0.001
                   for vertex in particle_vertices):
                raise ValueError('Particle mesh extends beyond its bound')
        reader.done()
        return vertices, bounds, shader

    def av_object(self, index):
        reader = self.block(index)
        reader.take(4)
        reader.take(reader.scalar('I') * 4)
        reader.take(8)
        translation = reader.read('fff')
        rotation = reader.read('f' * 9)
        scale = reader.scalar('f')
        reader.take(4)
        if not all(math.isfinite(v) for v in (*translation, *rotation, scale)):
            raise ValueError('Nonfinite scene transform')
        return reader, translation, rotation, scale

    def rendered_shaders(self):
        shaders = set()
        for index, (kind, _) in enumerate(self.blocks):
            if kind == 'BSTriShape':
                reader, _, _, _ = self.av_object(index)
                reader.take(16)
                _, shader, _ = reader.read('iii')
                if shader >= 0:
                    shaders.add(shader)
        return shaders


class ParryAssetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.effects = {name: Nif(PACKAGE / 'meshes/ApexCombat/Parry' / (name + '.nif'))
                       for name in EFFECTS}

    @staticmethod
    def linked_controllers(nif):
        """Walk the actual NiObjectNET controller links, never sequence tracks."""
        owners = {'BSFadeNode', 'NiNode', 'NiBillboardNode', 'BSTriShape',
                  'NiParticleSystem', 'BSStripParticleSystem', 'BSEffectShaderProperty'}
        found = {}
        for owner, (kind, _) in enumerate(nif.blocks):
            if kind not in owners:
                continue
            reader = nif.block(owner)
            reader.take(4)
            reader.take(reader.scalar('I') * 4)
            current = reader.scalar('i')
            chain = set()
            while current >= 0:
                if current in chain:
                    raise ValueError('Controller-chain cycle')
                chain.add(current)
                controller = nif.block(current)
                next_index, flags, frequency, phase, start, stop, target = controller.read('iHffffi')
                if target != owner:
                    raise ValueError('Controller target is not its owning object')
                if current in found:
                    raise ValueError('Controller belongs to multiple objects')
                found[current] = (owner, flags, frequency, phase, start, stop)
                current = next_index
        return found

    def test_scenes_are_standalone_autonomous_sse(self):
        forbidden = {'NiControllerManager', 'NiControllerSequence', 'NiMultiTargetTransformController',
                     'NiBlendFloatInterpolator', 'NiBlendPoint3Interpolator', 'NiBlendTransformInterpolator',
                     'BSValueNode'}
        for name, nif in self.effects.items():
            with self.subTest(effect=name):
                self.assertFalse(forbidden.intersection(kind for kind, _ in nif.blocks))
                self.assertEqual([nif.blocks[root][0] for root in nif.roots], ['BSFadeNode'])
                controllers = self.linked_controllers(nif)
                self.assertTrue(controllers)
                for index, (_, flags, frequency, phase, start, stop) in controllers.items():
                    self.assertEqual(flags, 0x45, f'{name}: {index} must use native APP_INIT/clamp timing')
                    self.assertEqual(frequency, 1)
                    self.assertEqual(phase, 0)
                    self.assertTrue(math.isfinite(start) and math.isfinite(stop) and 0 <= start <= stop)
                    if nif.blocks[index][0] == 'NiPSysUpdateCtlr':
                        continue
                    reader = nif.block(index)
                    reader.take(26)
                    interpolator = reader.scalar('i')
                    self.assertGreaterEqual(interpolator, 0, f'{name}: unbound controller {index}')
                    self.assertNotIn('Blend', nif.blocks[interpolator][0])
                    self.assertTrue(nif.blocks[interpolator][0].endswith('Interpolator'))

    def test_normal_block_burst_preserves_verified_model(self):
        # The complete impact model is copied intact except for namespaced texture paths.
        nif = self.effects['PerfectBlockImpact']
        self.assertEqual(hashlib.sha256(nif.data).hexdigest(),
                         'fd35b7fbca5d7b34998f2fa05bdc1711aa5eac75550d8fc8e6d553f2a8323133')
        self.assertEqual(len(nif.blocks), 391)
        self.assertEqual(sum(kind in ('NiParticleSystem', 'BSStripParticleSystem') for kind, _ in nif.blocks), 9)

    def test_texture_dependencies_and_dds(self):
        packaged = {str(path.relative_to(PACKAGE)).replace('\\', '/').lower(): path
                    for path in (PACKAGE / 'textures').rglob('*') if path.is_file()}
        custom = set()
        for name, nif in self.effects.items():
            paths = re.findall(rb'textures[\\/][A-Za-z0-9_ .!\\/\-]+\.dds', nif.data, re.I)
            self.assertTrue(paths, name)
            for raw_path in paths:
                path = raw_path.decode('ascii').replace('\\', '/').lower()
                self.assertNotIn('..', path.split('/'))
                self.assertTrue(path in VANILLA_TEXTURES or path in packaged, f'{name}: {path}')
                if path in packaged:
                    self.assertTrue(path.startswith('textures/apexcombat/parry/'))
                    custom.add(path)
        self.assertEqual(len(custom), 11)
        for path in custom:
            data = packaged[path].read_bytes()
            self.assertEqual(data[:4], b'DDS ')
            self.assertGreaterEqual(len(data), 128)
            size, _, height, width = struct.unpack_from('<IIII', data, 4)
            self.assertEqual(size, 124)
            self.assertTrue(0 < width <= 8192 and 0 < height <= 8192)
            self.assertEqual(struct.unpack_from('<I', data, 76)[0], 32)

    def test_ring_has_direct_live_alpha_and_scale_bindings(self):
        nif = self.effects['PerfectRing']
        controllers = self.linked_controllers(nif)
        self.assertEqual(len(nif.blocks), 13)
        self.assertEqual(len(controllers), 2)
        self.assertEqual({nif.blocks[index][0] for index in controllers},
                         {'NiTransformController', 'BSEffectShaderPropertyFloatController'})
        self.assertFalse(any('Particle' in kind or 'Emitter' in kind for kind, _ in nif.blocks))
        # Spawn flag 0x4 derives lifetime from controller ranges. The caller's
        # requested duration is not a hard cap, so every retained range matters.
        all_controllers = {i for i, (kind, _) in enumerate(nif.blocks) if kind.endswith(('Controller', 'Ctlr'))}
        self.assertEqual(set(controllers), all_controllers, 'No orphan or unexamined long controller')
        for index, (owner, _, _, _, start, stop) in controllers.items():
            self.assertEqual(start, 0)
            self.assertAlmostEqual(stop, 0.2)
            self.assertLessEqual(stop - start, 0.25)
            reader = nif.block(index)
            reader.take(26)
            interpolator = reader.scalar('i')
            if nif.blocks[index][0] == 'BSEffectShaderPropertyFloatController':
                self.assertEqual(nif.blocks[owner][0], 'BSEffectShaderProperty')
                self.assertEqual(reader.scalar('I'), 5, 'Must control rendered alpha')
                self.assertEqual(nif.blocks[interpolator][0], 'NiFloatInterpolator')
                curve = nif.curve(interpolator)
                self.assertEqual(curve[0][1], (0,))
                self.assertEqual(curve[-1][1], (0,))
                self.assertGreater(max(value[0] for _, value, _ in curve), 0)
                self.assertLessEqual(curve[-1][0], stop)
                self.assertTrue(all(not tangent for _, _, tangent in curve), 'Linear alpha cannot overshoot')
            else:
                self.assertEqual(nif.blocks[owner][0], 'NiNode')
                translation, scale = nif.transform_curves(interpolator)
                self.assertFalse(translation)
                self.assertTrue(all(value[0] > 0 and not tangent for _, value, tangent in scale))
                self.assertGreater(scale[-1][1][0], scale[0][1][0])
                self.assertLessEqual(scale[-1][0], stop)
            reader.done()
        shader_index = next(i for i, (kind, _) in enumerate(nif.blocks) if kind == 'BSEffectShaderProperty')
        shader = nif.block(shader_index)
        shader.take(36)
        shader.string()
        shader.take(20)
        _, _, _, alpha = shader.read('ffff')
        self.assertEqual(alpha, 0, 'An uninitialized ring must not stay visible')
        shape_index = next(i for i, (kind, _) in enumerate(nif.blocks) if kind == 'BSTriShape')
        self.assertEqual(nif.shape(shape_index)[2], shader_index, 'Animated shader must be on the drawn shape')
        bsx = next(nif.block(i) for i, (kind, _) in enumerate(nif.blocks) if kind == 'BSXFlags')
        bsx.take(4)
        self.assertTrue(bsx.scalar('I') & 1, 'Scene must advertise animation')

    def test_ring_geometry_and_scene_are_centered_and_compact(self):
        nif = self.effects['PerfectRing']
        parents, scales, scene = {}, {}, set()
        node_types = ('BSFadeNode', 'NiNode', 'NiBillboardNode')
        def walk(index):
            self.assertNotIn(index, scene, 'Scene child reused or cyclic')
            scene.add(index)
            reader, translation, _, _ = nif.av_object(index)
            self.assertTrue(all(abs(value) < 0.001 for value in translation))
            if nif.blocks[index][0] in node_types:
                for child in reader.read('i' * reader.scalar('I')):
                    if child >= 0:
                        parents[child] = index
                        walk(child)
        for root in nif.roots:
            walk(root)
        for index, (owner, *_rest) in self.linked_controllers(nif).items():
            if nif.blocks[index][0] == 'NiTransformController':
                reader = nif.block(index)
                reader.take(26)
                _, scale = nif.transform_curves(reader.scalar('i'))
                scales[owner] = max(value[0] for _, value, _ in scale)
        shapes = [i for i in scene if nif.blocks[i][0] == 'BSTriShape']
        self.assertEqual(len(shapes), 1)
        for index in shapes:
            vertices, bounds, _ = nif.shape(index)
            low = [min(v[axis] for v in vertices) for axis in range(3)]
            high = [max(v[axis] for v in vertices) for axis in range(3)]
            self.assertTrue(all(abs((a + b) / 2) < 0.001 for a, b in zip(low, high)))
            self.assertTrue(all(abs(value) < 0.001 for value in bounds[:3]))
            self.assertGreaterEqual(bounds[3] + 0.001,
                                    max(math.sqrt(sum(v * v for v in vertex)) for vertex in vertices))
            maximum_scale, ancestor = 1, index
            while ancestor is not None:
                maximum_scale *= scales.get(ancestor, nif.av_object(ancestor)[3])
                ancestor = parents.get(ancestor)
            extent = max(b - a for a, b in zip(low, high)) * maximum_scale
            self.assertTrue(60 <= extent <= 100, f'Unexpected maximum ring extent {extent}')


if __name__ == '__main__':
    unittest.main()
