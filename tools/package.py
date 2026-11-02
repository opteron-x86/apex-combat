#!/usr/bin/env python3
"""Create deterministic mod-manager and symbol archives from built artifacts."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import time
import zipfile

import build_provenance

ROOT = Path(__file__).resolve().parents[1]
COMMONLIB_COMMIT = '12d61f3c39f7d4ba1b1ab543df2bca0e613dc747'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def write_zip(path, files, epoch=315532800, executable_names=()):
    timestamp = time.gmtime(max(315532800, epoch))[:6]
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo(name, timestamp)
            info.create_system = 3
            info.external_attr = (0o100755 if name in executable_names else 0o100644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data, compresslevel=9)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'dist')
    parser.add_argument('--dependency-prefix', type=Path, help='Dependency prefix for a Linux cross-build')
    args = parser.parse_args()
    build = args.build.resolve()
    dll, pdb = build / 'ApexCombat.dll', build / 'ApexCombat.pdb'
    if not dll.is_file() or not pdb.is_file():
        parser.error('Both ApexCombat.dll and its matching ApexCombat.pdb are required.')
    spec = importlib.util.spec_from_file_location('audit_dll', ROOT / 'tools/audit-dll.py')
    audit = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(audit)
    dll_info = audit.inspect(dll)
    try:
        provenance = build_provenance.verify(ROOT, build, dll, pdb)
    except (ValueError, OSError, KeyError) as error:
        parser.error(str(error))
    cache = (build / 'CMakeCache.txt').read_text()
    for runtime in ('SE', 'AE', 'VR'):
        if f'ENABLE_SKYRIM_{runtime}:BOOL=ON' not in cache:
            parser.error(f'CommonLib was not configured for {runtime}.')
    version = re.search(r'project\(ApexCombat VERSION ([\d.]+)', (ROOT / 'CMakeLists.txt').read_text())[1]
    try:
        commit = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], stderr=subprocess.DEVNULL, text=True).strip()
    except subprocess.CalledProcessError:
        commit = None
    dirty = bool(subprocess.check_output(['git', '-C', str(ROOT), 'status', '--porcelain'], text=True))
    files = {str(p.relative_to(ROOT / 'package')).replace('\\', '/'): p.read_bytes()
             for p in (ROOT / 'package').rglob('*') if p.is_file()}
    files['SKSE/Plugins/ApexCombat.dll'] = dll.read_bytes()
    for name in ('README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md'):
        file = ROOT / name
        if not file.is_file():
            parser.error(f'Missing release document: {name}')
        files[name] = file.read_bytes()
    for name in ('COPYING', 'EXCEPTIONS', 'NOTICE.md'):
        file = ROOT / 'third_party/precision' / name
        if not file.is_file(): parser.error(f'Missing Precision license notice: {name}')
        files['licenses/Precision-' + name] = file.read_bytes()
    for file in (ROOT / 'licenses').glob('*'):
        if file.is_file(): files['licenses/' + file.name] = file.read_bytes()
    prefix = args.dependency_prefix or build / 'vcpkg_installed/x64-windows-static-md'
    for name in ('fmt', 'spdlog', 'directxmath', 'directxtk', 'rapidcsv'):
        copyright_file = prefix / 'share' / name / 'copyright'
        if not copyright_file.is_file():
            parser.error(f'Missing dependency license: {copyright_file}')
        files[f'licenses/{name}.txt'] = copyright_file.read_bytes()
    source_match = re.search(r'^FETCHCONTENT_SOURCE_DIR_COMMONLIBSSE:PATH=(.+)$', cache, re.M)
    commonlib = Path(source_match[1]) if source_match and source_match[1] else build / '_deps/commonlibsse-src'
    actual_commit = subprocess.check_output(['git', '-C', str(commonlib), 'rev-parse', 'HEAD'], text=True).strip()
    if actual_commit != COMMONLIB_COMMIT:
        parser.error('The CommonLib source does not match the pinned commit.')
    if subprocess.check_output(['git', '-C', str(commonlib), 'status', '--porcelain', '--untracked-files=no'], text=True):
        parser.error('The pinned CommonLib source or its submodules have local changes.')
    for file in [commonlib / 'COPYING.txt', commonlib / 'EXCEPTIONS.md',
                 commonlib / 'extern/openvr/LICENSE', *sorted((commonlib / 'licenses').glob('*.txt'))]:
        if not file.is_file():
            parser.error(f'Missing CommonLib/OpenVR license: {file}')
        name = 'OpenVR.txt' if '/openvr/' in file.as_posix() else 'CommonLib-' + file.name
        files['licenses/' + name] = file.read_bytes()
    metadata = {'version': version, 'commit': commit, 'dirty': dirty,
                'runtime_layouts': ['SE', 'AE', 'VR'], 'commonlib_commit': COMMONLIB_COMMIT,
                'dll_sha256': digest(dll.read_bytes()), 'pdb_sha256': digest(pdb.read_bytes()),
                'dll': dll_info, 'source_inputs_sha256': provenance['source_inputs_sha256']}
    files['build-provenance.json'] = build_provenance.json_bytes(provenance)
    files['build-info.json'] = (json.dumps(metadata, indent=2, sort_keys=True) + '\n').encode()
    files['SHA256SUMS.txt'] = ''.join(f'{digest(data)}  {name}\n' for name, data in sorted(files.items())).encode()
    args.output.mkdir(parents=True, exist_ok=True)
    name = f'Apex-Combat-{version}'
    archive = args.output / (name + '.zip')
    symbols = args.output / (name + '-symbols.zip')
    sources = args.output / (name + '-source.zip')
    epoch = int(os.environ.get('SOURCE_DATE_EPOCH', '315532800'))
    write_zip(archive, files, epoch)
    write_zip(symbols, {'ApexCombat.pdb': pdb.read_bytes()}, epoch)
    source_names = subprocess.check_output(['git', '-C', str(ROOT), 'ls-files', '--cached', '--others', '--exclude-standard', '-z']).decode().split('\0')
    source_files = {entry: (ROOT / entry).read_bytes() for entry in sorted(set(source_names)) if entry and (ROOT / entry).is_file()}
    source_files['build-provenance.json'] = build_provenance.json_bytes(provenance)
    source_files['SOURCE-INFO.json'] = (json.dumps({'version': version, 'commit': commit, 'dirty': dirty, 'commonlib_commit': COMMONLIB_COMMIT}, indent=2) + '\n').encode()
    executable_names = {name for name in source_files if (ROOT / name).is_file() and (ROOT / name).stat().st_mode & 0o111}
    write_zip(sources, source_files, epoch, executable_names)
    manifest = args.output / (name + '-SHA256SUMS.txt')
    manifest.write_text(''.join(f'{digest(p.read_bytes())}  {p.name}\n' for p in (archive, symbols, sources)), encoding='utf-8', newline='\n')
    print(json.dumps({'archives': [str(archive), str(symbols), str(sources)], 'manifest': str(manifest), **metadata}, indent=2))


if __name__ == '__main__':
    main()
