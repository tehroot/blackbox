#!/bin/bash
# Build both firmware images and copy them to the repository root.
#   glide_pico.uf2       development build (CDC serial log, 1200-baud reboot to UF2)
#   glide_pico_work.uf2  work build (keyboard + mouse + pointer only)
# First run tools/setup-pico-toolchain.sh.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
PICO_HOME="${PICO_HOME:-$HOME/pico}"
export PICO_SDK_PATH="${PICO_SDK_PATH:-$PICO_HOME/pico-sdk}"
if [[ -z "${PICO_TOOLCHAIN_PATH:-}" ]]; then
  PICO_TOOLCHAIN_PATH="$(ls -d "$PICO_HOME"/toolchain/arm-gnu-toolchain-*-arm-none-eabi 2>/dev/null | head -1)"
  export PICO_TOOLCHAIN_PATH
fi
[[ -d "$PICO_SDK_PATH" && -d "$PICO_TOOLCHAIN_PATH" ]] || { echo "run tools/setup-pico-toolchain.sh first"; exit 1; }

cmake -S "$here" -B "$here/build" -DCMAKE_BUILD_TYPE=Release
make -C "$here/build" -j8
cp "$here/build/glide_pico.uf2" "$here/build/glide_pico_work.uf2" "$here/../../"
echo "copied glide_pico.uf2 and glide_pico_work.uf2 to the repository root"
