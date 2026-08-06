#!/usr/bin/env bash
set -euo pipefail

test_root="${CROSSINK_SIMULATOR_TEST_FS:-/crossink-test-fs}"
test -d "${test_root}"
test -w "${test_root}"
test -f /usr/include/openssl/md5.h

sdl_version="$(sdl2-config --version)"
compiler_version="$(c++ --version | head -n 1)"
cmake_version="$(cmake --version | head -n 1)"
ninja_version="$(ninja --version)"
format_version="$(clang-format-21 --version)"
pio_version="$(pio --version)"
python_version="$(python3 --version)"
font_match="$(fc-match --format '%{family}\n' sans-serif | head -n 1)"

case "${sdl_version}" in
  2.*) ;;
  *) echo "Simulator self-test: unexpected SDL2 version ${sdl_version}" >&2; exit 1 ;;
esac
echo "${format_version}" | grep -Eq 'version 21([.]| )'
echo "${pio_version}" | grep -Fq '6.1.19'
test -n "${compiler_version}"
test -n "${cmake_version}"
test -n "${ninja_version}"
test -n "${python_version}"
test -n "${font_match}"

probe="${test_root}/crossink-rw-probe"
printf 'crossink-simulator-rw\n' > "${probe}"
grep -Fxq 'crossink-simulator-rw' "${probe}"
rm -f "${probe}"

if find /dev -maxdepth 1 \( -name 'ttyUSB*' -o -name 'ttyACM*' \) -print -quit | grep -q .; then
  echo "Simulator self-test: host serial device is visible" >&2
  exit 1
fi

printf 'SIMULATOR_CONTAINER_PASS sdl=%s compiler=%s cmake=%s ninja=%s clang_format=%s pio=%s python=%s font=%s\n' \
  "${sdl_version}" "${compiler_version}" "${cmake_version}" "${ninja_version}" \
  "${format_version}" "${pio_version}" "${python_version}" "${font_match}"
