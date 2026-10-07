#!/bin/bash
# Install the Pico build toolchain into ~/pico (no admin rights needed).
#   - Arm GNU Toolchain 14.2.rel1 (tarball; the Homebrew cask needs admin rights)
#   - pico-sdk 2.3.1 with its TinyUSB submodule
#   - TinyUSB dependencies for rp2040, including Pico-PIO-USB
# Versions used for the baseline build (2026-10-06):
#   pico-sdk 2.3.1 (079c6f3), TinyUSB 86ad6e5, Pico-PIO-USB fe9133f, CMake 4.4.2
# Needs: git, cmake, python3, curl. About 1-2 GB.
set -euo pipefail
PICO_HOME="${PICO_HOME:-$HOME/pico}"
ARM_VER=14.2.rel1
case "$(uname -m)" in
  arm64)  ARM_HOST=darwin-arm64 ;;
  x86_64) ARM_HOST=darwin-x86_64 ;;
  *) echo "unsupported host $(uname -m)"; exit 1 ;;
esac
ARM_DIR="$PICO_HOME/toolchain/arm-gnu-toolchain-$ARM_VER-$ARM_HOST-arm-none-eabi"

mkdir -p "$PICO_HOME/toolchain"
if [[ ! -x "$ARM_DIR/bin/arm-none-eabi-gcc" ]]; then
  url="https://developer.arm.com/-/media/Files/downloads/gnu/$ARM_VER/binrel/arm-gnu-toolchain-$ARM_VER-$ARM_HOST-arm-none-eabi.tar.xz"
  echo "download $url"
  curl -fSL -o "$PICO_HOME/toolchain/arm.tar.xz" "$url"
  tar -xJf "$PICO_HOME/toolchain/arm.tar.xz" -C "$PICO_HOME/toolchain"
  rm "$PICO_HOME/toolchain/arm.tar.xz"
fi

if [[ ! -d "$PICO_HOME/pico-sdk" ]]; then
  git clone --depth 1 --branch 2.3.1 https://github.com/raspberrypi/pico-sdk.git "$PICO_HOME/pico-sdk"
  git -C "$PICO_HOME/pico-sdk" submodule update --init --depth 1 lib/tinyusb
fi
# Pico-PIO-USB and the other rp2040 dependencies of TinyUSB.
(cd "$PICO_HOME/pico-sdk/lib/tinyusb" && python3 -I tools/get_deps.py rp2040)

echo
echo "toolchain ready. Use:"
echo "  export PICO_SDK_PATH=$PICO_HOME/pico-sdk"
echo "  export PICO_TOOLCHAIN_PATH=$ARM_DIR"
