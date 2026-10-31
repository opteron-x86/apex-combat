#!/usr/bin/env python3
"""Preview or apply an explicit mod-manager deployment plan, with verified rollback."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import tempfile
from datetime import datetime, timezone
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PROFILE_FILES = ('modlist.txt', 'plugins.txt', 'loadorder.txt')


def sha(data): return hashlib.sha256(data).hexdigest()


def files_in(path):
    if path.is_symlink(): raise ValueError(f'Symlink target is not supported: {path}')
    if not path.exists(): return None
    if path.is_file(): return {'': path.read_bytes()}
    result = {}
    for file in sorted(path.rglob('*')):
        if file.is_symlink(): raise ValueError(f'Symlink target is not supported: {file}')
        if file.is_file(): result[file.relative_to(path).as_posix()] = file.read_bytes()
    return result


def state_hash(files):
    if files is None: return None
    return sha(json.dumps({name: sha(data) for name, data in sorted(files.items())}, sort_keys=True).encode())


def validate_name(name):
    if not name or name in ('.', '..') or any(c in name for c in '/\\\r\n\0'):
        raise ValueError(f'Invalid mod/plugin name: {name!r}')
    return name


def edit_modlist(data, enable, disable):
    bom = b'\xef\xbb\xbf' if data.startswith(b'\xef\xbb\xbf') else b''
    body = data[len(bom):]
    lines = body.splitlines(keepends=True)
    newline = b'\r\n' if b'\r\n' in body else b'\n'
    wanted = {validate_name(name): b'-' for name in disable}
    if enable in wanted: raise ValueError('The deployed mod cannot also be disabled.')
    wanted[validate_name(enable)] = b'+'
    counts = {name: 0 for name in wanted}
    for i, line in enumerate(lines):
        stripped = line.rstrip(b'\r\n')
        if stripped[:1] in (b'+', b'-'):
            name = stripped[1:].decode('utf-8')
            if name in wanted:
                counts[name] += 1
                lines[i] = wanted[name] + line[1:]
    for name, count in counts.items():
        if count > 1 or (count == 0 and name != enable):
            raise ValueError(f'Mod must appear exactly once in modlist: {name}')
    if not counts[enable]:
        index = 0
        while index < len(lines) and lines[index].startswith(b'#'): index += 1
        if index and not lines[index - 1].endswith((b'\r', b'\n')): lines[index - 1] += newline
        lines.insert(index, b'+' + enable.encode('utf-8') + newline)
    return bom + b''.join(lines)


def edit_plugins(data, disabled):
    bom = b'\xef\xbb\xbf' if data.startswith(b'\xef\xbb\xbf') else b''
    lines = data[len(bom):].splitlines(keepends=True)
    wanted = {validate_name(name).casefold(): 0 for name in disabled}
    for i, line in enumerate(lines):
        name = line.rstrip(b'\r\n').lstrip(b'*').decode('utf-8').casefold()
        if name in wanted:
            wanted[name] += 1
            if line.startswith(b'*'): lines[i] = line[1:]
    for name, count in wanted.items():
        if count != 1: raise ValueError(f'Plugin must appear exactly once in plugins.txt: {name}')
    return bom + b''.join(lines)


def package_files(path):
    result = {}
    with zipfile.ZipFile(path) as archive:
        for info in archive.infolist():
            if info.is_dir(): continue
            name = info.filename
            parts = PurePosixPath(name).parts
            if name.startswith('/') or '\\' in name or any(part in ('.', '..') or ':' in part for part in parts):
                raise ValueError(f'Unsafe archive member: {name}')
            if stat.S_ISLNK(info.external_attr >> 16): raise ValueError(f'Archive symlink: {name}')
            if name.casefold() in {entry.casefold() for entry in result}: raise ValueError(f'Duplicate archive member: {name}')
            result[name] = archive.read(info)
    if 'SKSE/Plugins/ApexCombat.dll' not in result: raise ValueError('Archive does not contain ApexCombat.dll.')
    manifest = result.get('SHA256SUMS.txt')
    if manifest is None: raise ValueError('Archive lacks SHA256SUMS.txt.')
    covered = set()
    for line in manifest.decode('utf-8').splitlines():
        digest, name = line.split('  ', 1)
        if name not in result or sha(result[name]) != digest: raise ValueError(f'Package checksum failed: {name}')
        if name in covered: raise ValueError(f'Duplicate package checksum: {name}')
        covered.add(name)
    if covered != set(result) - {'SHA256SUMS.txt'}: raise ValueError('Package checksum coverage is incomplete.')
    return result


def prepare(plan):
    profile = Path(plan['profile_dir']).resolve(strict=True)
    mods = Path(plan['mods_dir']).resolve(strict=True)
    mod_name = validate_name(plan.get('mod_name', 'Apex Combat'))
    target = mods / mod_name
    changes = []
    def add(path, after, kind='file'):
        if any(Path(x['path']) == path for x in changes): raise ValueError(f'Duplicate target: {path}')
        if path.is_relative_to(ROOT): raise ValueError('Deployment targets must be outside the repository.')
        before = files_in(path)
        changes.append({'path': str(path), 'kind': kind, 'before': before, 'after': after})
    for name in PROFILE_FILES:
        path = profile / name
        original = path.read_bytes() if path.exists() else None
        if name in ('modlist.txt', 'plugins.txt') and original is None: raise ValueError(f'Missing profile file: {path}')
        after = original
        if name == 'modlist.txt': after = edit_modlist(original, mod_name, plan.get('disable_mods', []))
        elif name == 'plugins.txt': after = edit_plugins(original, plan.get('disable_plugins', []))
        add(path, None if after is None else {'': after})
    for update in plan.get('config_updates', []):
        path = Path(update['path']).absolute()
        # Resolve the parent, but never follow a symlink at the file itself.
        path = path.parent.resolve(strict=True) / path.name
        before = files_in(path)
        expected = update['expected_sha256']
        if (None if before is None else sha(before[''])) != expected:
            raise ValueError(f'Configuration precondition failed: {path}')
        add(path, {'': Path(update['source']).read_bytes()})
    add(target, package_files(Path(plan['package'])), 'directory')
    for i, change in enumerate(changes):
        path = Path(change['path'])
        for other in changes[i + 1:]:
            peer = Path(other['path'])
            if path.is_relative_to(peer) or peer.is_relative_to(path): raise ValueError('Deployment targets overlap.')
    return changes


def preview(changes):
    return [{'path': change['path'], 'kind': change['kind'], 'changed': change['before'] != change['after'],
             'before_sha256': state_hash(change['before']), 'after_sha256': state_hash(change['after']),
             'files_after': len(change['after'] or {})} for change in changes]


def atomic_file(path, data):
    mode = stat.S_IMODE(path.stat().st_mode) if path.exists() else 0o644
    fd, temporary = tempfile.mkstemp(prefix='.apex-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary): os.unlink(temporary)


def replace(path, files, kind):
    if kind == 'file':
        if files is None:
            if path.exists(): path.unlink()
        else: atomic_file(path, files[''])
        return
    temporary = Path(tempfile.mkdtemp(prefix='.apex-stage-', dir=path.parent))
    old = path.parent / ('.apex-old-' + uuid.uuid4().hex)
    moved = False
    try:
        if files is not None:
            for name, data in files.items():
                file = temporary / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_bytes(data)
        if path.exists():
            os.replace(path, old)
            moved = True
        if files is not None: os.replace(temporary, path)
        if moved: shutil.rmtree(old)
    except BaseException:
        if moved and old.exists() and not path.exists(): os.replace(old, path)
        raise
    finally:
        if temporary.exists(): shutil.rmtree(temporary)


def apply(changes, backup_dir):
    backup_dir = backup_dir.resolve()
    if backup_dir.is_relative_to(ROOT): raise ValueError('Backup directory must be outside the repository.')
    if any(backup_dir.is_relative_to(Path(c['path'])) for c in changes): raise ValueError('Backup directory overlaps a deployment target.')
    for change in changes:
        if files_in(Path(change['path'])) != change['before']: raise ValueError(f'Target changed after preview: {change["path"]}')
    snapshot = backup_dir / (datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ-') + uuid.uuid4().hex[:8])
    snapshot.mkdir(parents=True)
    manifest = {'schema': 1, 'state': 'prepared', 'changes': preview(changes)}
    for index, change in enumerate(changes):
        if change['before'] is not None:
            folder = snapshot / str(index)
            folder.mkdir()
            for name, data in change['before'].items():
                file = folder / (name or '__file__')
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_bytes(data)
    manifest_path = snapshot / 'manifest.json'
    atomic_file(manifest_path, (json.dumps(manifest, indent=2) + '\n').encode())
    completed = []
    try:
        for change in changes:
            if change['before'] != change['after']:
                if files_in(Path(change['path'])) != change['before']: raise ValueError(f'Target changed during deployment: {change["path"]}')
                replace(Path(change['path']), change['after'], change['kind'])
                completed.append(change)
        manifest['state'] = 'applied'
    except BaseException:
        for change in reversed(completed): replace(Path(change['path']), change['before'], change['kind'])
        manifest['state'] = 'rolled-back-after-error'
        atomic_file(manifest_path, (json.dumps(manifest, indent=2) + '\n').encode())
        raise
    atomic_file(manifest_path, (json.dumps(manifest, indent=2) + '\n').encode())
    return manifest_path


def rollback(manifest_path, perform):
    manifest_path = manifest_path.resolve()
    manifest = json.loads(manifest_path.read_text())
    if manifest.get('schema') != 1 or manifest.get('state') != 'applied': raise ValueError('Snapshot is not an applied deployment.')
    changes = []
    for index, entry in enumerate(manifest['changes']):
        path = Path(entry['path'])
        current = files_in(path)
        if state_hash(current) != entry['after_sha256']: raise ValueError(f'Refusing to overwrite later changes: {path}')
        saved = None
        if entry['before_sha256'] is not None:
            saved = files_in(manifest_path.parent / str(index))
            if entry['kind'] == 'file': saved = {'': saved['__file__']}
            if state_hash(saved) != entry['before_sha256']: raise ValueError(f'Backup checksum failed: {path}')
        changes.append({'path': str(path), 'kind': entry['kind'], 'before': current, 'after': saved})
    if perform:
        for change in reversed(changes):
            if change['before'] != change['after']: replace(Path(change['path']), change['after'], change['kind'])
        manifest['state'] = 'rolled-back'
        atomic_file(manifest_path, (json.dumps(manifest, indent=2) + '\n').encode())
    return preview(changes)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument('--plan', type=Path, help='JSON plan with profile_dir, mods_dir, package, and optional disable_mods/disable_plugins/config_updates')
    selection.add_argument('--rollback', type=Path, help='Snapshot manifest to restore')
    parser.add_argument('--backup-dir', type=Path, help='External snapshot directory; required to apply a deployment')
    parser.add_argument('--apply', action='store_true', help='Perform the previewed changes; default is read-only')
    args = parser.parse_args()
    try:
        if args.rollback:
            result = {'mode': 'rollback' if args.apply else 'dry-run', 'changes': rollback(args.rollback, args.apply)}
        else:
            changes = prepare(json.loads(args.plan.read_text()))
            result = {'mode': 'apply' if args.apply else 'dry-run', 'changes': preview(changes)}
            if args.apply:
                if args.backup_dir is None: parser.error('--apply requires --backup-dir outside the repository.')
                result['snapshot'] = str(apply(changes, args.backup_dir))
        print(json.dumps(result, indent=2))
    except (ValueError, OSError, KeyError, zipfile.BadZipFile) as error:
        parser.exit(1, f'Deployment stopped: {error}\n')


if __name__ == '__main__':
    main()
