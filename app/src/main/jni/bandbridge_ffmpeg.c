/*
 * JNI entry points for the optional FFmpeg decoder (milestone M3).
 *
 * WHAT THIS IS
 * ------------
 * A deliberately small surface. Java asks three things:
 *
 *   1. Is the library alive, and what version?      -> nativeVersion()
 *   2. What is in this file, without decoding it?   -> nativeProbe()
 *   3. Decode this file to 16-bit PCM?              -> nativeDecodeToPcm()
 *
 * Everything else - which files to offer, when to decode, what to do with the
 * PCM - stays in Java, where it is testable on a desktop JVM. The native side
 * exists only because FFmpeg is C.
 *
 * WHY THE SURFACE IS SO SMALL
 * ---------------------------
 * Every JNI function is a place where a mistake is a hard crash with no stack
 * trace, on a device with no debugger attached. This boundary is therefore kept
 * thin: no callbacks into Java, no long-lived native handles, no cached global
 * state except the version string. The decoder returns a byte[] that Java owns.
 *
 * FAILURES ARE RETURN VALUES, NOT EXCEPTIONS
 * ------------------------------------------
 * Every function reports failure in-band (NULL, or a status element) rather
 * than throwing. A thrown Java exception from native code has to be caught at
 * every call site, and the one that gets forgotten is the one that crashes the
 * app on a user's corrupt file. A NULL is impossible to ignore.
 *
 * VERSION COMPATIBILITY
 * ---------------------
 * This file must compile against two very different FFmpeg versions:
 *   - FFmpeg 3.4.x, the version scripts/build-ffmpeg.sh pins for NDK r16b
 *   - FFmpeg 5.1+, where AVCodecContext.channels / AVFrame.channels /
 *     AVCodecParameters.channels were removed in favour of ch_layout
 * So channel counts go through the BB_CHANNELS macro below. Getting this wrong
 * is a build break, not a runtime bug, which is why it is worth the macro: the
 * CI-style check in tools/run-tests.sh compiles this file against whatever
 * libav* the machine has, and that is how the 5.1+ branch is exercised at all.
 *
 * It is also written to C89 rules (declarations first, no // comments) because
 * NDK r16b's GCC 4.9 defaults to -std=gnu89.
 *
 * BUILDING
 * --------
 * Not built by Gradle: NDK r16b cannot be driven by AGP 8's NDK integration, so
 * scripts/build-ffmpeg.sh compiles this file against the static FFmpeg archives
 * and links one libbandbridge_ffmpeg.so per ABI.
 *
 * Without BANDBRIDGE_HAVE_FFMPEG the whole file becomes a stub that reports
 * "unavailable", so a build never depends on FFmpeg being present.
 */

#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef BANDBRIDGE_HAVE_FFMPEG

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/version.h>

/*
 * Channel count, across the API break.
 *
 * AVFrame.ch_layout landed in libavutil 57.28 (FFmpeg 5.1); before that the
 * field was a plain `channels` int on AVFrame, AVCodecContext and
 * AVCodecParameters alike.
 */
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
#  define BB_CHANNELS(x) ((x)->ch_layout.nb_channels)
#else
#  define BB_CHANNELS(x) ((x)->channels)
#endif

#endif /* BANDBRIDGE_HAVE_FFMPEG */

#define JNI_FN(name) Java_com_astralight_bandbridge_player_engine_NativeDecoders_##name

/* Byte offsets of the two PCM samples, written little-endian by hand. */
#define BB_LO(v) ((uint8_t)((v) & 0xFF))
#define BB_HI(v) ((uint8_t)(((v) >> 8) & 0xFF))

/*
 * Reports the FFmpeg version, or "unavailable" when built without it.
 *
 * Returning a string rather than throwing means Java has one code path for
 * "is it usable": anything other than "unavailable" is usable.
 */
JNIEXPORT jstring JNICALL
JNI_FN(nativeVersion)(JNIEnv *env, jclass clazz)
{
    (void)clazz;
#ifdef BANDBRIDGE_HAVE_FFMPEG
    return (*env)->NewStringUTF(env, av_version_info());
#else
    return (*env)->NewStringUTF(env, "unavailable");
#endif
}

#ifdef BANDBRIDGE_HAVE_FFMPEG

/*
 * Converts one decoded frame into interleaved S16, appending to *pcm.
 *
 * swresample is disabled in the build to keep the library small, so the
 * conversion is done by hand. The M3 codecs decode to S16, FLT or FLTP; any
 * other layout produces silence rather than a crash, because a wrong sample
 * format is a bug in this table and not a reason to take the app down.
 *
 * Returns the new byte length, or -1 when the buffer would exceed maxBytes or
 * realloc fails. Both are reported the same way on purpose: the caller's only
 * useful response is to stop and tell the user the file is too large.
 */
static int bb_append_frame(uint8_t **pcm, size_t *pcmLen, size_t *pcmCap,
                           const AVFrame *frame, int64_t fallbackCh,
                           size_t maxBytes)
{
    int ch;
    int samples;
    size_t need;
    int i;
    int c;

    ch = BB_CHANNELS(frame) > 0 ? BB_CHANNELS(frame) : (int)fallbackCh;
    if (ch <= 0) {
        ch = 1;
    }
    samples = frame->nb_samples;
    need = (size_t)samples * (size_t)ch * 2u;

    if (*pcmLen + need > maxBytes) {
        return -1;
    }
    if (*pcmLen + need > *pcmCap) {
        size_t cap = *pcmCap ? *pcmCap * 2u : 65536u;
        uint8_t *grown;
        while (cap < *pcmLen + need) {
            cap *= 2u;
        }
        grown = (uint8_t *)realloc(*pcm, cap);
        if (grown == NULL) {
            return -1;
        }
        *pcm = grown;
        *pcmCap = cap;
    }

    for (i = 0; i < samples; i++) {
        for (c = 0; c < ch; c++) {
            int16_t s16 = 0;
            switch (frame->format) {
                case AV_SAMPLE_FMT_FLT: {
                    float f = ((const float *)frame->data[0])[i * ch + c];
                    if (f > 1.0f) f = 1.0f;
                    if (f < -1.0f) f = -1.0f;
                    s16 = (int16_t)(f * 32767.0f);
                    break;
                }
                case AV_SAMPLE_FMT_FLTP: {
                    float f = ((const float *)frame->data[c])[i];
                    if (f > 1.0f) f = 1.0f;
                    if (f < -1.0f) f = -1.0f;
                    s16 = (int16_t)(f * 32767.0f);
                    break;
                }
                case AV_SAMPLE_FMT_S16:
                    s16 = ((const int16_t *)frame->data[0])[i * ch + c];
                    break;
                case AV_SAMPLE_FMT_S16P:
                    s16 = ((const int16_t *)frame->data[c])[i];
                    break;
                case AV_SAMPLE_FMT_U8:
                    s16 = (int16_t)((((const uint8_t *)frame->data[0])[i * ch + c]) - 128) << 8;
                    break;
                case AV_SAMPLE_FMT_S32:
                    s16 = (int16_t)(((const int32_t *)frame->data[0])[i * ch + c] >> 16);
                    break;
                case AV_SAMPLE_FMT_S32P:
                    s16 = (int16_t)(((const int32_t *)frame->data[c])[i] >> 16);
                    break;
                default:
                    s16 = 0;
                    break;
            }
            (*pcm)[*pcmLen + ((size_t)i * (size_t)ch + (size_t)c) * 2u] = BB_LO(s16);
            (*pcm)[*pcmLen + ((size_t)i * (size_t)ch + (size_t)c) * 2u + 1u] = BB_HI(s16);
        }
    }

    *pcmLen += need;
    return (int)*pcmLen;
}

/*
 * Opens an audio file and reports its properties without decoding it.
 *
 * Returns a 4-element long array: { sampleRate, channels, durationMs, ok }.
 * "ok" is 1 on success and 0 on any failure, so a corrupt file in a playlist
 * becomes a skip rather than a crash.
 */
JNIEXPORT jlongArray JNICALL
JNI_FN(nativeProbe)(JNIEnv *env, jclass clazz, jstring path)
{
    jlong out[4];
    jlongArray result;
    const char *cpath;
    AVFormatContext *fmt;
    int idx;

    (void)clazz;
    out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0;

    if (path == NULL) {
        return (*env)->NewLongArray(env, 0);
    }
    cpath = (*env)->GetStringUTFChars(env, path, NULL);
    if (cpath == NULL) {
        return (*env)->NewLongArray(env, 0);
    }

    fmt = NULL;
    if (avformat_open_input(&fmt, cpath, NULL, NULL) == 0) {
        if (avformat_find_stream_info(fmt, NULL) >= 0) {
            idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
            if (idx >= 0) {
                AVStream *st = fmt->streams[idx];
                AVCodecParameters *par = st->codecpar;
                out[0] = par->sample_rate;
                out[1] = BB_CHANNELS(par);
                if (st->duration != AV_NOPTS_VALUE) {
                    out[2] = (jlong)((st->duration * 1000) / AV_TIME_BASE);
                } else if (fmt->duration != AV_NOPTS_VALUE) {
                    out[2] = (jlong)(fmt->duration / (AV_TIME_BASE / 1000));
                }
                out[3] = 1;
            }
        }
        avformat_close_input(&fmt);
    }

    (*env)->ReleaseStringUTFChars(env, path, cpath);

    result = (*env)->NewLongArray(env, 4);
    if (result != NULL) {
        (*env)->SetLongArrayRegion(env, result, 0, 4, out);
    }
    return result;
}

/*
 * Decodes a whole file to 16-bit signed PCM.
 *
 * WHY A WHOLE-FILE API AND NOT A STREAM
 * -------------------------------------
 * A streaming API needs Java callbacks or a long-lived native handle, and both
 * bring failure modes this design has none of: a handle leaked when the Activity
 * is destroyed, a callback into a collected object from a decoder thread. M3's
 * scope is local files of a few minutes, and the result is handed to AudioTrack
 * in one go, so the simple contract is also the sufficient one.
 *
 * maxBytes is a hard budget chosen by Java, because the target device has 290MB
 * of RAM and an unbounded malloc is how you get an OOM-kill with no useful log.
 * Returns NULL on failure or when the decoded size would exceed it.
 */
JNIEXPORT jbyteArray JNICALL
JNI_FN(nativeDecodeToPcm)(JNIEnv *env, jclass clazz, jstring path, jint maxBytes)
{
    jbyteArray result = NULL;
    const char *cpath = NULL;
    AVFormatContext *fmt = NULL;
    AVCodecContext *dec = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    uint8_t *pcm = NULL;
    size_t pcmLen = 0;
    size_t pcmCap = 0;
    int idx;
    int64_t fallbackCh;
    int draining = 0;

    (void)clazz;
    if (path == NULL || maxBytes <= 0) {
        return NULL;
    }
    cpath = (*env)->GetStringUTFChars(env, path, NULL);
    if (cpath == NULL) {
        return NULL;
    }

    if (avformat_open_input(&fmt, cpath, NULL, NULL) != 0) {
        goto done;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        goto done;
    }
    idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (idx < 0) {
        goto done;
    }
    {
        AVCodecParameters *par = fmt->streams[idx]->codecpar;
        const AVCodec *codec = avcodec_find_decoder(par->codec_id);
        if (codec == NULL) {
            goto done;
        }
        dec = avcodec_alloc_context3(codec);
        if (dec == NULL) {
            goto done;
        }
        if (avcodec_parameters_to_context(dec, par) < 0) {
            goto done;
        }
        if (avcodec_open2(dec, codec, NULL) < 0) {
            goto done;
        }
    }

    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (pkt == NULL || frame == NULL) {
        goto done;
    }

    fallbackCh = BB_CHANNELS(dec);
    if (fallbackCh <= 0) {
        fallbackCh = 1;
    }

    /*
     * Two loops, because a decoder can hold frames back: the first feeds
     * packets, the second drains whatever is left once the input is exhausted.
     * Skipping the drain is the classic bug that loses the last fraction of a
     * second of every file.
     */
    while (!draining) {
        int readRc = av_read_frame(fmt, pkt);
        if (readRc < 0) {
            /* End of input: flush the decoder and stop. */
            avcodec_send_packet(dec, NULL);
            draining = 1;
        } else if (pkt->stream_index != idx) {
            av_packet_unref(pkt);
            continue;
        } else {
            int sendRc = avcodec_send_packet(dec, pkt);
            av_packet_unref(pkt);
            if (sendRc < 0) {
                /* A corrupt packet is skipped, not fatal: one bad frame in a
                 * long file should not lose the whole file. */
                continue;
            }
        }

        for (;;) {
            int recvRc = avcodec_receive_frame(dec, frame);
            if (recvRc == AVERROR(EAGAIN) || recvRc == AVERROR_EOF) {
                break;
            }
            if (recvRc < 0) {
                break;
            }
            if (bb_append_frame(&pcm, &pcmLen, &pcmCap, frame, fallbackCh,
                                (size_t)maxBytes) < 0) {
                /* Over budget or out of memory: give up cleanly. */
                goto done;
            }
            av_frame_unref(frame);
        }
    }

    if (pcmLen > 0) {
        result = (*env)->NewByteArray(env, (jsize)pcmLen);
        if (result != NULL) {
            (*env)->SetByteArrayRegion(env, result, 0, (jsize)pcmLen,
                                       (const jbyte *)pcm);
        }
    }

done:
    if (pcm != NULL) {
        free(pcm);
    }
    if (frame != NULL) {
        av_frame_free(&frame);
    }
    if (pkt != NULL) {
        av_packet_free(&pkt);
    }
    if (dec != NULL) {
        avcodec_free_context(&dec);
    }
    if (fmt != NULL) {
        avformat_close_input(&fmt);
    }
    if (cpath != NULL) {
        (*env)->ReleaseStringUTFChars(env, path, cpath);
    }
    return result;
}

#else /* !BANDBRIDGE_HAVE_FFMPEG */

/*
 * Stubs, so a build with no FFmpeg headers still links. NativeDecoders never
 * calls these when nativeVersion() reported "unavailable", but the symbols must
 * exist or the link fails.
 */
JNIEXPORT jlongArray JNICALL
JNI_FN(nativeProbe)(JNIEnv *env, jclass clazz, jstring path)
{
    (void)clazz; (void)path;
    return (*env)->NewLongArray(env, 0);
}

JNIEXPORT jbyteArray JNICALL
JNI_FN(nativeDecodeToPcm)(JNIEnv *env, jclass clazz, jstring path, jint maxBytes)
{
    (void)clazz; (void)path; (void)maxBytes;
    return NULL;
}

#endif /* BANDBRIDGE_HAVE_FFMPEG */
