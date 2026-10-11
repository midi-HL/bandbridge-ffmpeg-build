# bandbridge-ffmpeg

Cloud build for the optional FFmpeg decoder used by **AstroBox / BandBridge**
(an Android 2.3.6 / Gingerbread band manager).

## Why this repo exists

The app targets `armeabi` + `armeabi-v7a` and pins **NDK r16b**, the last
toolchain that produces libraries loadable on the target device. Two things make
the native build impossible on the development container:

- **No NDK installed**, and r16b is an 852MB download that unpacks to ~2.6GB.
- **Only ~2GB of disk free**, so extraction fails with ENOSPC.

GitHub's runners have both, so the build lives here. The output is one
`libbandbridge_ffmpeg.so` per ABI, uploaded as a workflow artifact and dropped
into the app at `app/src/main/jniLibs/<abi>/`.

## What is in here

| Path | What it is |
| --- | --- |
| `app/src/main/jni/bandbridge_ffmpeg.c` | The JNI surface. Three functions: version, probe, decode-to-PCM. |
| `scripts/build-ffmpeg.sh` | Downloads FFmpeg 3.4.13, cross-compiles it against r16b, links the shim. |
| `.github/workflows/build-ffmpeg.yml` | Runs the above and uploads the `.so` files. |

This is a **build helper**, not a fork: the app itself lives elsewhere.

## License

AGPL-3.0-only, matching the parent project. FFmpeg is built here under its own
license (LGPL-2.1+ for the decoders enabled).
