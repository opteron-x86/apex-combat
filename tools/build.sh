#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
preset="${1:-native-tests}"
case "$preset" in
  native-tests)
    cmake --preset native-tests
    cmake --build --preset native-tests
    ctest --preset native-tests
    python3 -m unittest discover -s tools -p 'test_*.py'
    ;;
  linux-cross)
    : "${XWIN_SYSROOT:?Set XWIN_SYSROOT to your Windows SDK/CRT winsysroot}"
    : "${APEX_WINDOWS_PREFIX:?Set APEX_WINDOWS_PREFIX to compatible Windows dependency packages}"
    cmake --preset linux-cross "${@:2}"
    cmake --build --preset linux-cross --parallel "${APEX_BUILD_JOBS:-4}"
    python3 tools/audit-dll.py build/linux-cross/ApexCombat.dll
    ;;
  windows-release)
    cmake --preset windows-release "${@:2}"
    cmake --build --preset windows-release
    ctest --test-dir build/windows-release --output-on-failure
    python tools/audit-dll.py build/windows-release/ApexCombat.dll
    ;;
  *) echo 'Usage: tools/build.sh {native-tests|linux-cross|windows-release} [CMake options]' >&2; exit 2 ;;
esac
