import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

HERE = Path(__file__).resolve().parent

def load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

deploy = load('deploy')
package = load('package')


class DeploymentTests(unittest.TestCase):
    def test_preserves_bom_crlf_and_unrelated_bytes(self):
        data = b'\xef\xbb\xbf# profile\r\n+Combat A\r\n-Unrelated\r\n+Combat B'
        changed = deploy.edit_modlist(data, 'Apex Combat', ['Combat A'])
        self.assertEqual(changed, b'\xef\xbb\xbf# profile\r\n+Apex Combat\r\n-Combat A\r\n-Unrelated\r\n+Combat B')
        plugins = b'\xef\xbb\xbf# plugins\r\n*Combat.esp\r\n*Keep.esm\r\n'
        self.assertEqual(deploy.edit_plugins(plugins, ['combat.esp']), b'\xef\xbb\xbf# plugins\r\nCombat.esp\r\n*Keep.esm\r\n')

    def test_missing_or_duplicate_names_stop(self):
        with self.assertRaises(ValueError): deploy.edit_modlist(b'+Combat\n+Combat\n', 'Apex Combat', ['Combat'])
        with self.assertRaises(ValueError): deploy.edit_plugins(b'*Keep.esp\n', ['Missing.esp'])

    def fixture(self, folder):
        profile, mods = folder / 'profile', folder / 'mods'
        profile.mkdir(); mods.mkdir()
        original = {'modlist.txt': b'\xef\xbb\xbf# test\r\n+Old Combat\r\n', 'plugins.txt': b'*Old.esp\r\n*Keep.esp\r\n', 'loadorder.txt': b'Old.esp\r\nKeep.esp\r\n'}
        for name, data in original.items(): (profile / name).write_bytes(data)
        config = folder / 'settings.ini'; config.write_bytes(b'old\r\n')
        new = folder / 'new.ini'; new.write_bytes(b'new\r\n')
        old_mod = mods / 'Apex Combat'; old_mod.mkdir(); (old_mod / 'old.txt').write_bytes(b'original')
        archive = folder / 'package.zip'
        files = {'SKSE/Plugins/ApexCombat.dll': b'test-dll', 'SKSE/Plugins/ApexCombat.ini': b'[General]\n'}
        files['SHA256SUMS.txt'] = ''.join(f'{deploy.sha(data)}  {name}\n' for name, data in sorted(files.items())).encode()
        package.write_zip(archive, files)
        plan = {'profile_dir': str(profile), 'mods_dir': str(mods), 'package': str(archive), 'disable_mods': ['Old Combat'], 'disable_plugins': ['Old.esp'], 'config_updates': [{'path': str(config), 'source': str(new), 'expected_sha256': deploy.sha(config.read_bytes())}]}
        return plan, profile, mods, config, original

    def test_apply_and_verified_rollback(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            plan, profile, mods, config, originals = self.fixture(folder)
            changes = deploy.prepare(plan)
            snapshot = deploy.apply(changes, folder / 'backups')
            self.assertEqual(config.read_bytes(), b'new\r\n')
            self.assertFalse((mods / 'Apex Combat/old.txt').exists())
            self.assertTrue((mods / 'Apex Combat/SKSE/Plugins/ApexCombat.dll').exists())
            deploy.rollback(snapshot, False)
            self.assertEqual(config.read_bytes(), b'new\r\n')
            deploy.rollback(snapshot, True)
            self.assertEqual(config.read_bytes(), b'old\r\n')
            for name, data in originals.items(): self.assertEqual((profile / name).read_bytes(), data)
            self.assertEqual((mods / 'Apex Combat/old.txt').read_bytes(), b'original')
            self.assertFalse((mods / 'Apex Combat/SKSE').exists())

    def test_refuses_to_overwrite_changes_after_deploy(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            plan, _, _, config, _ = self.fixture(folder)
            snapshot = deploy.apply(deploy.prepare(plan), folder / 'backups')
            config.write_bytes(b'user changed this')
            with self.assertRaisesRegex(ValueError, 'later changes'): deploy.rollback(snapshot, True)
            self.assertEqual(config.read_bytes(), b'user changed this')

    def test_cli_dry_run_is_read_only(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            plan, profile, _, config, originals = self.fixture(folder)
            plan_file = folder / 'plan.json'; plan_file.write_text(json.dumps(plan))
            before = deploy.files_in(folder)
            completed = subprocess.run([sys.executable, str(HERE / 'deploy.py'), '--plan', str(plan_file)], capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(completed.stdout)['mode'], 'dry-run')
            self.assertEqual(deploy.files_in(folder), before)
            self.assertEqual(config.read_bytes(), b'old\r\n')

    def test_rejects_unsafe_archive(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'bad.zip'
            with zipfile.ZipFile(path, 'w') as archive: archive.writestr('../outside.txt', b'bad')
            with self.assertRaisesRegex(ValueError, 'Unsafe'): deploy.package_files(path)

    def test_archive_is_deterministic(self):
        with tempfile.TemporaryDirectory() as temporary:
            one, two = Path(temporary) / 'one.zip', Path(temporary) / 'two.zip'
            package.write_zip(one, {'b.txt': b'B', 'a.txt': b'A'})
            package.write_zip(two, {'a.txt': b'A', 'b.txt': b'B'})
            self.assertEqual(one.read_bytes(), two.read_bytes())


if __name__ == '__main__':
    unittest.main()
