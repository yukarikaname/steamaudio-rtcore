#!/usr/bin/env bash
# Fast macOS dependency bootstrap for the pinned Steam Audio submodule.
#
# The upstream script (external/steam-audio/core/build/get_dependencies.py)
# clones several large repositories from GitHub. This script uses Homebrew for
# flatbuffers and builds only pffft + libmysofa from pinned sources fetched via
# fast mirrors, so the universal phonon build can link them.
#
# It writes only into external/steam-audio/core/deps (untracked build input).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORE="$ROOT/external/steam-audio/core"
DEPS="$CORE/deps"
PFFFT_REV="e0bf595c98ded55cc457a371c1b29c8cab552628"
MYSOFA_URL="${MYSOFA_URL:-https://mirrors.tuna.tsinghua.edu.cn/debian/pool/main/libm/libmysofa/libmysofa_1.3.3+dfsg.orig.tar.xz}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if ! brew list --formula flatbuffers >/dev/null 2>&1; then
    brew install flatbuffers
fi

mkdir -p "$DEPS/flatbuffers/include" "$DEPS/flatbuffers/bin/osx" \
         "$DEPS/mysofa/include" "$DEPS/mysofa/lib/osx/release" \
         "$DEPS/pffft/include" "$DEPS/pffft/lib/osx/release" \
         "$DEPS/zlib/include"

ln -sfn /opt/homebrew/include/flatbuffers "$DEPS/flatbuffers/include/flatbuffers"
ln -sf /opt/homebrew/bin/flatc "$DEPS/flatbuffers/bin/osx/flatc"

SDK="$(xcrun --sdk macosx --show-sdk-path)"
ln -sf "$SDK/usr/include/zlib.h" "$DEPS/zlib/include/zlib.h"
ln -sf "$SDK/usr/include/zconf.h" "$DEPS/zlib/include/zconf.h"

fetch_pffft() {
    curl -fsSL --retry 3 --max-time 120 \
        "https://raw.githubusercontent.com/marton78/pffft/$PFFFT_REV/$1" \
        -o "$WORK/pffft/$1"
}

mkdir -p "$WORK/pffft/simd"
for file in pffft.c pffft.h pffft_common.c pffft_priv_impl.h; do
    fetch_pffft "$file"
done
for header in pf_float.h pf_scalar_float.h pf_neon_float.h pf_sse1_float.h pf_altivec_float.h; do
    fetch_pffft "simd/$header"
done

cp "$WORK/pffft/pffft.h" "$DEPS/pffft/include/pffft.h"
clang -arch arm64 -arch x86_64 -mmacosx-version-min=10.13 -O3 -fPIC -shared \
    -I"$WORK/pffft" -o "$DEPS/pffft/lib/osx/release/libpffft.dylib" \
    "$WORK/pffft/pffft.c" "$WORK/pffft/pffft_common.c"
install_name_tool -id @rpath/libpffft.dylib "$DEPS/pffft/lib/osx/release/libpffft.dylib"

curl -fsSL --retry 3 --max-time 300 -o "$WORK/libmysofa.tar.xz" "$MYSOFA_URL"
mkdir -p "$WORK/mysofa"
tar xJf "$WORK/libmysofa.tar.xz" -C "$WORK/mysofa" --strip-components=1

cmake -S "$WORK/mysofa" -B "$WORK/mysofa-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=10.13 \
    -DBUILD_TESTS=OFF -DBUILD_STATIC_LIBS=OFF
cmake --build "$WORK/mysofa-build" -j "$(sysctl -n hw.ncpu)"

MYSOFA_LIB="$(find "$WORK/mysofa-build/src" -maxdepth 1 -name 'libmysofa.*.dylib' ! -type l | head -1)"
cp "$MYSOFA_LIB" "$DEPS/mysofa/lib/osx/release/libmysofa.dylib"
ln -sf libmysofa.dylib "$DEPS/mysofa/lib/osx/release/libmysofa.1.dylib"
cp "$WORK/mysofa/src/mysofa.h" "$DEPS/mysofa/include/mysofa.h"

echo "Steam Audio macOS deps ready under $DEPS"
