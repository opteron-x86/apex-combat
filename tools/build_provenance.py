#!/usr/bin/env python3
"""Record and verify the source inputs and artifacts of a completed build."""
import argparse
import hashlib
import json
import os
from pathlib import Path

SCHEMA = 1
SOURCE_SUFFIXES = {'.c', '.cpp', '.h', '.hpp', '.inl'}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def input_hashes(root):
    root = Path(root).resolve()
    files = []
    for folder in ('src', 'third_party'):
        files.extend(path for path in (root / folder).rglob('*')
                     if path.is_file() and path.suffix in SOURCE_SUFFIXES)
    files.extend(path for path in (root / 'tools/cmake').rglob('*.cmake') if path.is_file())
    files.extend(root / name for name in ('CMakeLists.txt', 'CMakePresets.json', 'vcpkg.json', 'tools/build_provenance.py'))
    return {path.relative_to(root).as_posix(): digest(path.read_bytes()) for path in sorted(set(files))}


def json_bytes(value):
    return (json.dumps(value, indent=2, sort_keys=True) + '\n').encode()


def write_if_changed(path, value):
    path = Path(path)
    data = json_bytes(value)
    if path.exists() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_bytes(data)
    os.replace(temporary, path)


def assert_inputs(expected, actual):
    changed = sorted(name for name in expected.keys() | actual.keys() if expected.get(name) != actual.get(name))
    if changed:
        raise ValueError('Build inputs changed; rebuild before packaging: ' + ', '.join(changed[:8]))


def snapshot(root, output):
    value = {'schema': SCHEMA, 'inputs': input_hashes(root)}
    write_if_changed(output, value)
    return value


def record(root, snapshot_file, dll, pdb, cache, output):
    captured = json.loads(Path(snapshot_file).read_text())
    if captured.get('schema') != SCHEMA:
        raise ValueError('Unsupported build-input snapshot')
    inputs = input_hashes(root)
    assert_inputs(captured['inputs'], inputs)
    value = {'schema': SCHEMA, 'inputs': inputs,
             'source_inputs_sha256': digest(json_bytes(inputs)),
             'artifacts': {'ApexCombat.dll': digest(Path(dll).read_bytes()),
                           'ApexCombat.pdb': digest(Path(pdb).read_bytes())},
             'cmake_cache_sha256': digest(Path(cache).read_bytes())}
    write_if_changed(output, value)
    return value


def verify(root, build, dll, pdb):
    build = Path(build)
    path = build / 'build-provenance.json'
    if not path.is_file():
        raise ValueError('No completed-build provenance; rebuild the plugin before packaging.')
    value = json.loads(path.read_text())
    if value.get('schema') != SCHEMA:
        raise ValueError('Unsupported completed-build provenance')
    assert_inputs(value['inputs'], input_hashes(root))
    if value['source_inputs_sha256'] != digest(json_bytes(value['inputs'])):
        raise ValueError('Build-input manifest checksum failed')
    if value['cmake_cache_sha256'] != digest((build / 'CMakeCache.txt').read_bytes()):
        raise ValueError('CMake configuration changed; rebuild before packaging.')
    for name, file in (('ApexCombat.dll', dll), ('ApexCombat.pdb', pdb)):
        if value['artifacts'].get(name) != digest(Path(file).read_bytes()):
            raise ValueError(f'{name} does not match the completed build.')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest='command', required=True)
    capture = subparsers.add_parser('snapshot')
    capture.add_argument('--root', type=Path, required=True)
    capture.add_argument('--output', type=Path, required=True)
    completed = subparsers.add_parser('record')
    for name in ('root', 'snapshot', 'dll', 'pdb', 'cache', 'output'):
        completed.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == 'snapshot':
            snapshot(args.root, args.output)
        else:
            record(args.root, args.snapshot, args.dll, args.pdb, args.cache, args.output)
    except (ValueError, OSError, KeyError) as error:
        parser.exit(1, f'Build provenance failed: {error}\n')


if __name__ == '__main__':
    main()
