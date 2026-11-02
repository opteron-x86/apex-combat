import os
from pathlib import Path
import tempfile
import unittest

import build_provenance as provenance


class ProvenanceTests(unittest.TestCase):
    def fixture(self, root):
        (root / 'src').mkdir()
        (root / 'src/runtime.cpp').write_text('int version = 1;\n')
        (root / 'third_party').mkdir()
        (root / 'third_party/api.h').write_text('struct Api {};\n')
        (root / 'tools').mkdir()
        for name in ('CMakeLists.txt', 'CMakePresets.json', 'vcpkg.json', 'tools/build_provenance.py'):
            (root / name).write_text(name + '\n')
        build = root / 'build'; build.mkdir()
        dll = build / 'ApexCombat.dll'; dll.write_bytes(b'DLL from build A')
        pdb = build / 'ApexCombat.pdb'; pdb.write_bytes(b'PDB from build A')
        cache = build / 'CMakeCache.txt'; cache.write_text('ENABLE_SKYRIM_VR:BOOL=ON\n')
        snapshot = build / 'build-inputs.json'
        provenance.snapshot(root, snapshot)
        provenance.record(root, snapshot, dll, pdb, cache, build / 'build-provenance.json')
        return build, dll, pdb, snapshot

    def test_valid_build_and_document_only_changes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build, dll, pdb, _ = self.fixture(root)
            (root / 'README.md').write_text('Updated documentation\n')
            recorded = provenance.verify(root, build, dll, pdb)
            self.assertEqual(recorded['artifacts']['ApexCombat.dll'], provenance.digest(dll.read_bytes()))

    def test_source_change_detected_even_with_restored_mtime(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build, dll, pdb, _ = self.fixture(root)
            source = root / 'src/runtime.cpp'; previous = source.stat()
            source.write_text('int version = 2;\n')
            os.utime(source, ns=(previous.st_atime_ns, previous.st_mtime_ns))
            with self.assertRaisesRegex(ValueError, 'Build inputs changed'):
                provenance.verify(root, build, dll, pdb)

    def test_added_source_is_detected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build, dll, pdb, _ = self.fixture(root)
            (root / 'src/new.cpp').write_text('int extra = 0;\n')
            with self.assertRaisesRegex(ValueError, 'new.cpp'):
                provenance.verify(root, build, dll, pdb)

    def test_rejects_edits_during_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build, dll, pdb, snapshot = self.fixture(root)
            (root / 'third_party/api.h').write_text('struct Api { int changed; };\n')
            with self.assertRaisesRegex(ValueError, 'Build inputs changed'):
                provenance.record(root, snapshot, dll, pdb, build / 'CMakeCache.txt', build / 'build-provenance.json')

    def test_modified_dll_and_mismatched_symbols_are_rejected(self):
        for target in ('ApexCombat.dll', 'ApexCombat.pdb'):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                build, dll, pdb, _ = self.fixture(root)
                (build / target).write_bytes(b'Artifact from a different build')
                with self.assertRaisesRegex(ValueError, 'does not match the completed build'):
                    provenance.verify(root, build, dll, pdb)

    def test_changed_configuration_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build, dll, pdb, _ = self.fixture(root)
            (build / 'CMakeCache.txt').write_text('ENABLE_SKYRIM_VR:BOOL=OFF\n')
            with self.assertRaisesRegex(ValueError, 'CMake configuration changed'):
                provenance.verify(root, build, dll, pdb)

    def test_missing_record_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build, dll, pdb, _ = self.fixture(root)
            (build / 'build-provenance.json').unlink()
            with self.assertRaisesRegex(ValueError, 'No completed-build provenance'):
                provenance.verify(root, build, dll, pdb)


if __name__ == '__main__':
    unittest.main()
