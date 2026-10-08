#!/bin/bash
# Install the Pico build toolchain into ~/pico (no admin rights needed).
#   - Arm GNU Toolchain 14.2.rel1 (tarball; the Homebrew cask needs admin rights)
#   - pico-sdk 2.3.1 with its TinyUSB submodule
#   - TinyUSB dependencies for rp2040, including Pico-PIO-USB
# Versions used for the baseline build (2026-10-06):
#   pico-sdk 2.3.1 (079c6f3), TinyUSB 86ad6e5, Pico-PIO-USB fe9133f, CMake 4.4.2
#   picotool 2.3.1 (2041936) with libusb 1.0.29 (static), for tools/pico-log.sh
# Needs: git, cmake, python3, curl, a C compiler (Xcode command line tools). About 1-2 GB.
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

# picotool with USB support, to read the flash log (tools/pico-log.sh). The picotool
# that the SDK builds for the firmware has no USB support. libusb is built static from
# source: a Homebrew libusb can have the wrong architecture.
LIBUSB_VER=1.0.29
LIBUSB_SHA256=5977fc950f8d1395ccea9bd48c06b3f808fd3c2c961b44b0c2e6e29fc3a70a85
PICOTOOL_BIN="$PICO_HOME/picotool/picotool/picotool"
if [[ ! -x "$PICOTOOL_BIN" ]]; then
  mkdir -p "$PICO_HOME/src"
  cd "$PICO_HOME/src"
  if [[ ! -f "$PICO_HOME/libusb/lib/libusb-1.0.a" ]]; then
    curl -fsSL -o "libusb-$LIBUSB_VER.tar.bz2" \
      "https://github.com/libusb/libusb/releases/download/v$LIBUSB_VER/libusb-$LIBUSB_VER.tar.bz2"
    echo "$LIBUSB_SHA256  libusb-$LIBUSB_VER.tar.bz2" | shasum -a 256 -c -
    rm -rf "libusb-$LIBUSB_VER" && tar -xjf "libusb-$LIBUSB_VER.tar.bz2"
    (cd "libusb-$LIBUSB_VER" && ./configure --prefix="$PICO_HOME/libusb" --disable-shared --enable-static \
      && make -j8 && make install)
  fi
  rm -rf picotool
  git clone --depth 1 --branch 2.3.1 https://github.com/raspberrypi/picotool.git
  # CMAKE_OSX_ARCHITECTURES: an x86_64 CMake (Homebrew under Rosetta) otherwise builds x86_64.
  PICO_SDK_PATH="$PICO_HOME/pico-sdk" cmake -S picotool -B picotool/build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" \
    -DCMAKE_INSTALL_PREFIX="$PICO_HOME/picotool" -DPICOTOOL_FLAT_INSTALL=1 \
    -DLIBUSB_INCLUDE_DIR="$PICO_HOME/libusb/include/libusb-1.0" \
    "-DLIBUSB_LIBRARIES=$PICO_HOME/libusb/lib/libusb-1.0.a;-framework IOKit;-framework CoreFoundation;-framework Security"
  cmake --build picotool/build -j8
  cmake --install picotool/build
fi

echo
echo "toolchain ready. Use:"
echo "  export PICO_SDK_PATH=$PICO_HOME/pico-sdk"
echo "  export PICO_TOOLCHAIN_PATH=$ARM_DIR"
