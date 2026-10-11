#!/usr/bin/env bash
#
# Builds the FFmpeg decoder used by milestone M3 and drops the .so files where
# Gradle expects them: app/src/main/jniLibs/<abi>/libbandbridge_ffmpeg.so
#
# WHY THIS SCRIPT EXISTS AND WHY IT IS NOT PART OF `gradlew`
# ----------------------------------------------------------
# The handover doc pins NDK r16b, because that is the last toolchain that
# produces libraries loadable on the target devices and the last one whose
# gnueabi output the 830MHz CPU's ROM accepts. AGP 8's `externalNativeBuild`
# does not support r16b, so the build cannot be driven from Gradle at all: the
# .so files are produced out of band, exactly as app/build.gradle's comment
# says. This script is that out-of-band step.
#
# CONSEQUENCE, STATED PLAINLY: without running this script the app builds and
# runs, but can only play what the platform MediaPlayer supports (mp3/m4a/aac/
# ogg/wav/mid). That is a supported configuration, not a broken one - see
# player/engine/NativeDecoders.java, which detects the library at runtime and
# adjusts the file filter accordingly.
#
# USAGE
#   scripts/build-ffmpeg.sh                 # build for both ABIs
#   ANDROID_NDK=/path scripts/build-ffmpeg.sh
#   ABIS="armeabi-v7a" scripts/build-ffmpeg.sh
#
# REQUIREMENTS
#   - NDK r16b (any r16b; the path is passed in via ANDROID_NDK or auto-detected)
#   - The project MUST be on a path with no spaces or non-ASCII characters.
#     NDK r16b's build system mishandles them; the handover doc calls this out as
#     a hard prerequisite, and this script refuses to run rather than producing
#     a confusing failure deep in a configure log.
#   - ~2.5GB free disk for the NDK, plus ~1GB for the FFmpeg build tree.
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Locate the project root (the directory containing app/).
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---------------------------------------------------------------------------
# Preflight: the path must be ASCII and free of spaces. This is checked first
# because NDK r16b fails obscurely later, and a clear message here saves an hour.
# ---------------------------------------------------------------------------
case "$ROOT" in
    *[!\ -~]*)
        echo "ERROR: the project path contains non-ASCII characters:" >&2
        echo "  $ROOT" >&2
        echo "NDK r16b cannot build from such a path. Move the project to a" >&2
        echo "pure-ASCII directory (no spaces, no CJK, no parentheses) first." >&2
        exit 2
        ;;
esac
case "$ROOT" in
    *" "*)
        echo "ERROR: the project path contains a space:" >&2
        echo "  $ROOT" >&2
        echo "NDK r16b cannot build from such a path." >&2
        exit 2
        ;;
esac

# ---------------------------------------------------------------------------
# Locate the NDK.
# ---------------------------------------------------------------------------
NDK="${ANDROID_NDK:-${ANDROID_NDK_HOME:-}}"
if [ -z "$NDK" ]; then
    for candidate in \
        "$HOME/android-ndk-r16b" \
        "/opt/android-ndk-r16b" \
        "$ROOT/../android-ndk-r16b" \
        "${ANDROID_SDK_ROOT:-/opt/android-sdk}/ndk-bundle"
    do
        if [ -d "$candidate" ]; then NDK="$candidate"; break; fi
    done
fi
if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then
    echo "ERROR: no NDK found." >&2
    echo "Set ANDROID_NDK=/path/to/android-ndk-r16b and re-run." >&2
    echo "r16b is at https://dl.google.com/android/repository/android-ndk-r16b-linux-x86_64.zip" >&2
    exit 2
fi
echo "NDK:     $NDK"

# ---------------------------------------------------------------------------
# FFmpeg source. Not vendored: it is ~80MB of source that would dwarf this repo.
# ---------------------------------------------------------------------------
FFMPEG_VERSION="${FFMPEG_VERSION:-3.4.13}"
FFMPEG_TARBALL="ffmpeg-${FFMPEG_VERSION}.tar.xz"
FFMPEG_URL="https://ffmpeg.org/releases/${FFMPEG_TARBALL}"
WORK="${WORK:-$ROOT/.ffmpeg-build}"
SRC="$WORK/ffmpeg-${FFMPEG_VERSION}"

mkdir -p "$WORK"
if [ ! -f "$WORK/$FFMPEG_TARBALL" ]; then
    echo "Fetching FFmpeg $FFMPEG_VERSION ..."
    curl -fL --retry 3 -o "$WORK/$FFMPEG_TARBALL" "$FFMPEG_URL"
fi
if [ ! -d "$SRC" ]; then
    echo "Extracting ..."
    tar -xf "$WORK/$FFMPEG_TARBALL" -C "$WORK"
fi

# ---------------------------------------------------------------------------
# Which ABIs. The handover doc names armeabi + armeabi-v7a. Note that armeabi
# (the old soft-float ABI) was removed in NDK r17; r16b still has it, which is
# one more reason the toolchain is pinned.
# ---------------------------------------------------------------------------
ABIS="${ABIS:-armeabi armeabi-v7a}"

# ---------------------------------------------------------------------------
# Decoders/parsers to enable. Kept to the M3 list plus the containers that carry
# them, so the library stays small enough to ship in an APK that has to install
# on a device with 290MB of RAM.
# ---------------------------------------------------------------------------
# These MUST stay in sync with NativeDecoders.NATIVE_EXTENSIONS in the app:
#   .flac .opus .alac .wv .ape .oga .mka .tak .tta .wma
# If a format is listed there but not enabled here, the app offers a file the
# decoder cannot play - the precise failure NativeDecoders exists to prevent,
# arriving from the other direction. tools/run-tests.sh decoder asserts the
# Java list; this comment is the other half of that contract.
DECODERS="flac,alac,ape,wavpack,vorbis,opus,mp3,aac,pcm_s16le,tak,tta,wmav1,wmav2"
DEMUXERS="flac,ogg,matroska,ape,wv,mov,mp3,wav,tak,tta,asf"
PARSERS="flac,opus,vorbis,aac,mpegaudio"

# Overridable so this script can run outside the app tree - which is exactly
# what the GitHub Actions workflow does, building only the .so from a small
# repo that does not contain the app sources.
SHIM="${SHIM:-$ROOT/app/src/main/jni/bandbridge_ffmpeg.c}"
OUT_ROOT="${OUT_ROOT:-$ROOT/app/src/main/jniLibs}"

build_abi() {
    local abi="$1"
    local host
    case "$abi" in
        armeabi)      host="arm-linux-androideabi" ;;
        armeabi-v7a)  host="arm-linux-androideabi" ;;
        arm64-v8a)    host="aarch64-linux-android" ;;
        x86)          host="i686-linux-android" ;;
        x86_64)       host="x86_64-linux-android" ;;
        *) echo "unknown ABI: $abi" >&2; return 2 ;;
    esac

    # r16b ships BOTH a Clang tree (toolchains/llvm/...) and the legacy GCC
    # tree. Only the GCC tree has "$host-gcc" and the gnueabi sysroot layout
    # this script drives. Choosing by directory existence - as an earlier
    # version did, llvm/ first - picks a tree with no such compiler and dies
    # with "arm-linux-androideabi-gcc: not found". So the toolchain is found by
    # looking for the compiler itself.
    local toolchain=""
    local candidate
    for candidate in "$NDK"/toolchains/*/prebuilt/linux-x86_64; do
        if [ -x "$candidate/bin/$host-gcc" ]; then
            toolchain="$candidate"
            break
        fi
    done
    if [ -z "$toolchain" ]; then
        echo "ERROR: no toolchain under $NDK/toolchains contains bin/$host-gcc" >&2
        echo "r16b keeps its GCC tree in toolchains/<target>-4.9/prebuilt/." >&2
        echo "Available toolchains:" >&2
        ls "$NDK/toolchains" >&2 || true
        exit 2
    fi
    echo "toolchain: $toolchain"

    local api=14   # minSdkVersion

    # r14 unified the sysroot and removed the per-toolchain copy. Detect which
    # layout this NDK uses rather than assuming: r16b (our pin) is unified, but
    # the script also runs against an older NDK if someone points it there.
    local sysroot="$NDK/sysroot"
    if [ ! -d "$sysroot" ]; then
        sysroot="$toolchain/sysroot"
    fi
    if [ ! -d "$sysroot" ]; then
        echo "ERROR: no sysroot found at $NDK/sysroot or $toolchain/sysroot" >&2
        exit 2
    fi
    local cpu
    case "$abi" in
        armeabi)     cpu="armv5te" ;;
        armeabi-v7a) cpu="armv7-a" ;;
        *)           cpu="generic" ;;
    esac
    local extra=""
    local extra_ld=""
    if [ "$abi" = "armeabi-v7a" ]; then
        extra="--enable-neon --enable-thumb"
        # Only valid for ARMv7: the linker errors out on armv5te.
        extra_ld="-Wl,--fix-cortex-a8"
    fi

    local prefix="$WORK/out-$abi"
    local build="$WORK/build-$abi"
    rm -rf "$prefix" "$build"
    mkdir -p "$prefix" "$build"

    # Prove the compiler runs before asking FFmpeg to use it. A dead or
    # mis-symlinked cross-gcc makes configure fail with a message that points
    # at the C library rather than the compiler.
    if ! "$toolchain/bin/$host-gcc" --version >/dev/null 2>&1; then
        echo "ERROR: $toolchain/bin/$host-gcc is not runnable" >&2
        exit 2
    fi
    echo "compiler: $("$toolchain/bin/$host-gcc" --version 2>&1 | head -1)"
    echo "sysroot:  $sysroot"

    echo "=== configuring $abi ==="
    (
        cd "$build"
        "$SRC/configure" \
            --prefix="$prefix" \
            --target-os=android \
            --arch=arm \
            --cpu="$cpu" \
            --enable-cross-compile \
            --cross-prefix="$toolchain/bin/$host-" \
            --sysroot="$sysroot" \
            --enable-static \
            --disable-shared \
            --disable-programs \
            --disable-doc \
            --disable-avdevice \
            --disable-postproc \
            --disable-swresample \
            --disable-avfilter \
            --disable-network \
            --disable-everything \
            --enable-decoder="$DECODERS" \
            --enable-demuxer="$DEMUXERS" \
            --enable-parser="$PARSERS" \
            --enable-protocol=file \
            --extra-cflags="-fPIC -O2 -DANDROID -D__ANDROID_API__=$api -isystem $sysroot/usr/include/$host $extra" \
            --extra-ldflags="$extra_ld" \
            > "$WORK/configure-$abi.log" 2>&1
    ) || {
        echo "configure failed for $abi." >&2
        echo "---- configure output (tail) ----" >&2
        tail -25 "$WORK/configure-$abi.log" >&2
        # The reason "C compiler test failed" is NOT in configure's stdout: it
        # is the compiler's own stderr, written to ffbuild/config.log. Without
        # this the message names the symptom and nothing else, which is what
        # sent the first CI run chasing flags that were fine.
        if [ -f "$build/ffbuild/config.log" ]; then
            echo "---- config.log (tail, the actual compiler error) ----" >&2
            tail -40 "$build/ffbuild/config.log" >&2
        fi
        echo "---- reproduce the compiler test by hand ----" >&2
        cat > "$WORK/t.c" <<'EOF_T'
#include <stdio.h>
int main(void){return 0;}
EOF_T
        echo "$ $toolchain/bin/$host-gcc --sysroot=$sysroot -isystem $sysroot/usr/include/$host -c $WORK/t.c -o $WORK/t.o" >&2
        "$toolchain/bin/$host-gcc" --sysroot="$sysroot" \
            -isystem "$sysroot/usr/include/$host" -c "$WORK/t.c" -o "$WORK/t.o" 2>&1 \
            | head -20 >&2 || true
        echo "---- host headers present? ----" >&2
        ls "$sysroot/usr/include/$host" 2>&1 | head -5 >&2
        ls "$sysroot/usr/include/stdio.h" 2>&1 >&2
        echo "full log: $WORK/configure-$abi.log" >&2
        return 1
    }

    echo "=== building $abi ==="
    make -C "$build" -j"${JOBS:-2}" > "$WORK/make-$abi.log" 2>&1 \
        || {
            echo "make failed for $abi. Last 40 lines:" >&2
            echo "---------------- ---------------- ----------------" >&2
            tail -40 "$WORK/make-$abi.log" >&2
            echo "full log: $WORK/make-$abi.log" >&2
            return 1
        }
    make -C "$build" install >> "$WORK/make-$abi.log" 2>&1

    # -----------------------------------------------------------------------
    # Package the static libraries into ONE .so exposing the JNI surface.
    #
    # The JNI shim (src/main/jni/bandbridge_ffmpeg.c) is compiled here rather
    # than by Gradle, because r16b cannot be driven by AGP 8's NDK integration.
    # If the shim is absent the script still succeeds and simply links the
    # codecs into a library with no entry points - which NativeDecoders will
    # load successfully and then fail to use, so the shim is required for a
    # working build. It is checked for explicitly.
    # -----------------------------------------------------------------------
    local shim="$SHIM"
    if [ ! -f "$shim" ]; then
        echo "ERROR: JNI shim missing: $shim" >&2
        echo "The .so needs an entry point; without it the library links but" >&2
        echo "does nothing. Add the shim, then re-run." >&2
        return 2
    fi

    local outdir="$OUT_ROOT/$abi"
    mkdir -p "$outdir"
    "$toolchain/bin/$host-gcc" -shared -o "$outdir/libbandbridge_ffmpeg.so" \
        --sysroot="$sysroot" \
        -DBANDBRIDGE_HAVE_FFMPEG \
        -I"$prefix/include" \
        -I"$SRC" \
        "$shim" \
        -Wl,--whole-archive \
        "$prefix/lib/libavcodec.a" "$prefix/lib/libavformat.a" \
        "$prefix/lib/libavutil.a" \
        -Wl,--no-whole-archive \
        -lz -llog -lm

    echo "=== $abi -> $outdir/libbandbridge_ffmpeg.so ==="
    ls -la "$outdir"
}

for abi in $ABIS; do
    build_abi "$abi"
done

echo
echo "Done. The .so files are in $OUT_ROOT/<abi>/."
echo "Rebuild the APK with: ./gradlew :app:assembleDebug"
echo
echo "If you would rather keep them out of the tree, point gradle.properties'"
echo "bandbridge.ffmpegPrebuiltDir at this directory instead."
