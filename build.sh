#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
cd "$PROJECT_ROOT"

JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
STATIC="${STATIC:-}"
DEBUG="${DEBUG:-}"

# ── Platform ──
UNAME=$(uname -s 2>/dev/null || echo Windows_NT)
case "$UNAME" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        PLATFORM=windows; export PATH="/mingw64/bin:$PATH"; EXE=".exe"
        RELEASE="$PROJECT_ROOT/release/win64"
        for d in "/c/Program Files/CMake/bin" "/mingw64/bin"; do
            [ -x "$d/cmake.exe" ] && export PATH="$d:$PATH" && break
        done
        ;;
    *)  PLATFORM=unix; EXE=""; RELEASE="$PROJECT_ROOT/release/linux" ;;
esac

needs_rebuild() {
    [ ! -f "$1" ] && return 0
    local m; m=$(ar p "$1" 2>/dev/null | dd bs=4 count=1 2>/dev/null | od -A n -t x1 | tr -d ' \n')
    [ -z "$m" ] && return 0
    [ "$PLATFORM" = "unix" ] && [ "$m" != "7f454c46" ] && return 0   # not ELF
    [ "$PLATFORM" = "windows" ] && [ "$m" = "7f454c46" ] && return 0  # ELF on win
    return 1
}

# ── Build deps if wrong platform or missing ──
BUILD_DEPS=0
for lib in zlib/libz.a flac/src/libFLAC/libFLAC-static.a xz/build/liblzma.a wavpack/libwavpack.a; do
    needs_rebuild "$lib" && BUILD_DEPS=1 && break
done

if [ "$BUILD_DEPS" -eq 1 ]; then
    echo "=== Building native $PLATFORM dependencies ==="

    # zlib
    echo "--- zlib ---"
    cp makefile_zlib/Makefile.linux zlib/Makefile.linux
    make -C zlib -f Makefile.linux clean 2>/dev/null || true
    make -C zlib -f Makefile.linux -j"$JOBS"
    [ -f zlib/libzlinux.a ] && cp zlib/libzlinux.a zlib/libz.a
    echo "  OK"

    if [ "$PLATFORM" = "unix" ]; then
        echo "--- apt packages ---"
        APT_PKGS=""
        for p in libflac-dev liblzma-dev libzstd-dev libogg-dev; do
            dpkg -s "$p" >/dev/null 2>&1 || APT_PKGS="$APT_PKGS $p"
        done
        [ -n "$APT_PKGS" ] && sudo apt-get install -y $APT_PKGS || echo "  All installed"

        for pair in "xz/build/liblzma.a|/usr/lib/x86_64-linux-gnu/liblzma.a /usr/lib/liblzma.a" \
                     "flac/src/libFLAC/libFLAC-static.a|/usr/lib/x86_64-linux-gnu/libFLAC.a /usr/lib/libFLAC.a"; do
            t="${pair%%|*}"; needs_rebuild "$t" || continue
            for p in ${pair#*|}; do [ -f "$p" ] && { mkdir -p "$(dirname "$t")"; cp "$p" "$t"; break; }; done
        done

        if needs_rebuild wavpack/libwavpack.a; then
            echo "--- wavpack (from source) ---"
            rm -rf wavpack/build-linux
            cmake -S wavpack -B wavpack/build-linux -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DCMAKE_C_FLAGS="-fPIC"
            cmake --build wavpack/build-linux -j"$JOBS"
            cp "$(find wavpack/build-linux -name libwavpack.a | head -1)" wavpack/libwavpack.a
            echo "  OK"
        fi

    elif [ "$PLATFORM" = "windows" ]; then
        for pair in \
            "xz/build/liblzma.a|xz/build-windows|cmake -S xz -B xz/build-windows -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -G 'MinGW Makefiles'" \
            "flac/src/libFLAC/libFLAC-static.a|flac/build-windows|cmake -S flac -B flac/build-windows -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DFLAC_BUILD_PROGRAMS=OFF -DFLAC_BUILD_EXAMPLES=OFF -DFLAC_BUILD_TESTS=OFF -DINSTALL_MANPAGES=OFF -G 'MinGW Makefiles'" \
            "wavpack/libwavpack.a|wavpack/build-windows|cmake -S wavpack -B wavpack/build-windows -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -G 'MinGW Makefiles'"; do
            TARGET="${pair%%|*}"; rest="${pair#*|}"; BDIR="${rest%%|*}"; CMD="${rest#*|}"
            needs_rebuild "$TARGET" || continue
            echo "--- $(basename "$TARGET" .a) (from source) ---"
            rm -rf "$BDIR"; eval "$CMD"
            cmake --build "$BDIR" -j"$JOBS"
            # lib name may differ from target name (e.g., FLAC produces libFLAC.a not libFLAC-static.a)
            LIB=$(find "$BDIR" -name "$(basename "$TARGET")" 2>/dev/null | head -1)
            [ -z "$LIB" ] && LIB=$(find "$BDIR" -name 'lib*.a' ! -path '*/CMakeFiles/*' 2>/dev/null | head -1)
            [ -z "$LIB" ] && LIB=$(find "$BDIR" -name '*.a' ! -path '*/CMakeFiles/*' ! -name 'objects.a' 2>/dev/null | head -1)
            if [ -n "$LIB" ]; then
                mkdir -p "$(dirname "$TARGET")" && cp "$LIB" "$TARGET"
                echo "  OK"
            else
                echo "  ERROR: build produced no static library" >&2; exit 1
            fi
        done
    fi
    echo ""
fi

# ── Final check ──
MISSING=0
for lib in zlib/libz.a flac/src/libFLAC/libFLAC-static.a xz/build/liblzma.a wavpack/libwavpack.a; do
    [ ! -f "$lib" ] && echo "ERROR: Missing $lib" >&2 && MISSING=1
    needs_rebuild "$lib" && echo "ERROR: Wrong platform $lib" >&2 && MISSING=1
done
[ "$MISSING" -ne 0 ] && exit 1

# ── Compile ──
rm -rf "$RELEASE"; mkdir -p "$RELEASE"
OUTPUT="$RELEASE/ecm3${EXE}"

echo "Compiling ecm3..."
[ -n "$STATIC" ] && echo "  Static linking"
[ -n "$DEBUG" ]  && echo "  Debug mode"

WIN_FLAG=""; [ "$PLATFORM" = "windows" ] && WIN_FLAG="-D_WIN32_WINNT=0x0602"
OPT_FLAGS="-Os -s -ffunction-sections -Wl,-gc-sections"
[ -n "$DEBUG" ] && OPT_FLAGS="-g -O0 -DDEBUG"

# Find ogg static lib
OGG_LIB=""
[ "$PLATFORM" = "windows" ] && [ -f /mingw64/lib/libogg.a ] && OGG_LIB=/mingw64/lib/libogg.a
[ "$PLATFORM" = "unix" ] && for p in /usr/lib/x86_64-linux-gnu/libogg.a /usr/lib/libogg.a; do [ -f "$p" ] && { OGG_LIB="$p"; break; }; done

# ── Compile .rc (Windows only) ──
RES=""
if [ "$PLATFORM" = "windows" ] && command -v windres >/dev/null 2>&1; then
    windres -O coff resources.rc resources.o && RES="resources.o"
fi

g++ $OPT_FLAGS \
    -std=c++17 -Wno-deprecated-declarations ${STATIC:+-static} \
    -Izlib -Ixz/src/liblzma/api -Ilz4/lib -Ilzlib4 -Iflaczlib \
    -Iflac/include -Izstd/lib -Izstd/lib/common -Iwavpack/include -Iwavpackzlib \
    -DZSTD_DISABLE_ASM -DFLAC__NO_DLL $WIN_FLAG \
    -o "$OUTPUT" \
    ecm3.cpp ecm3_core.cpp cue_gen.cpp compressor.cpp sector_tools.cpp cue_parser.cpp cuesplit.cpp \
    lz4/lib/lz4hc.c lz4/lib/lz4.c lzlib4/lzlib4.cpp flaczlib/flaczlib.cpp wavpackzlib/wavpackzlib.cpp \
    $(find zstd/lib -name '*.c' ! -path '*/legacy/*' ! -path '*/deprecated/*') \
    $RES \
    zlib/libz.a flac/src/libFLAC/libFLAC-static.a xz/build/liblzma.a wavpack/libwavpack.a $OGG_LIB

echo "Build succeeded: $OUTPUT"
