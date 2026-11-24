// hwopus: the Switch's "hardware" Opus decoder (IHardwareOpusDecoderManager / IHardwareOpusDecoder), on top of libopus.
// CRIWARE (SMRPG) uses it for its audio; with the logging stub the work buffer size was 0 and no decoder was ever
// opened.
//
// Commands (as in yuzu / Ryujinx):
//   manager (switchbrew): 0 OpenHardwareOpusDecoder, 1 GetWorkBufferSize, 2 OpenOpusDecoderForMultiStream,
//           3 GetWorkBufferSizeForMultiStream, 4 OpenHardwareOpusDecoderEx, 5 GetWorkBufferSizeEx,
//           6 OpenHardwareOpusDecoderForMultiStreamEx, 7 GetWorkBufferSizeForMultiStreamEx, 8 GetWorkBufferSizeExEx,
//           9 GetWorkBufferSizeForMultiStreamExEx (the multistream parameters go in an X buffer)
//   decoder: 0 DecodeInterleavedOld, 1 SetContext, 2 DecodeInterleavedForMultiStreamOld,
//           3 SetContextForMultiStream, 4/5 ...WithPerfOld, 6/7 ...WithPerfAndResetOld, 8 DecodeInterleaved,
//           9 DecodeInterleavedForMultiStream
// Each input packet carries Nintendo's header: size and "final range" in big-endian (8 bytes), and then the Opus
// packet. The output is interleaved 16-bit PCM; it returns what was consumed (header + packet), the samples per channel
// and, in the "Perf" forms, the time.
// LARECOMP_HWOPUS_LOG=1: every command to the log.
#include <opus/opus.h>
#include <opus/opus_multistream.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

typedef struct {
    OpusDecoder *dec;
    OpusMSDecoder *ms;
    uint32_t rate, channels;
    int nerr;
} HwOpus;

static int hw_log = -1;
static bool hwlog(void) {
    if (hw_log < 0) hw_log = getenv("LARECOMP_HWOPUS_LOG") && atoi(getenv("LARECOMP_HWOPUS_LOG")) != 0;
    return hw_log;
}

static bool valid_rate(uint32_t r) { return r == 8000 || r == 12000 || r == 16000 || r == 24000 || r == 48000; }

// The work buffer size (like yuzu: the decoder's state + one frame + some margin). The game allocates that memory and
// passes it as transfer memory; here it isn't used (the state lives on the host).
static uint32_t work_size(uint32_t rate, uint32_t channels, uint32_t streams, uint32_t stereo, bool large) {
    int st = streams ? opus_multistream_decoder_get_size((int)streams, (int)stereo) : opus_decoder_get_size((int)channels);
    if (st < 0) st = 0x8000;
    uint32_t frame = large ? 5760 : 1920;
    uint32_t div = rate ? 48000 / rate : 1;
    if (!div) div = 1;
    uint32_t pcm = (frame * channels / div * 2 + 63) & ~63u;
    return (uint32_t)st + pcm + 0x600;
}

static void hwopus_close(Service *s) {
    HwOpus *h = s->state;
    if (!h) return;
    if (h->dec) opus_decoder_destroy(h->dec);
    if (h->ms) opus_multistream_decoder_destroy(h->ms);
    free(h);
    s->state = NULL;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// Decodes one packet. Returns the command's result (0 = OK).
static uint32_t hw_decode(HwOpus *h, IpcReq *req, IpcResp *resp, bool perf, bool reset) {
    IpcBuf *in = in_buf(req, 0), *out = out_buf(req, 0);
    const uint8_t *src = in ? (const uint8_t *)(uintptr_t)in->addr : NULL;
    int16_t *pcm = out ? (int16_t *)(uintptr_t)out->addr : NULL;
    uint64_t in_size = in ? in->size : 0, out_size = out ? out->size : 0;
    if (reset) {
        if (h->dec) opus_decoder_ctl(h->dec, OPUS_RESET_STATE);
        if (h->ms) opus_multistream_decoder_ctl(h->ms, OPUS_RESET_STATE);
    }
    if (!src || in_size < 8) {
        if (h->nerr++ < 8) LOG("[hwopus] input too short (%llu bytes)\n", (unsigned long long)in_size);
        return 0x14e6f;  // (ResultInvalidOpusDataSize: module 111, description 167)
    }
    uint32_t size = be32(src);
    if ((uint64_t)size + 8 > in_size) {
        if (h->nerr++ < 8) LOG("[hwopus] %u-byte packet in a %llu-byte input\n", size, (unsigned long long)in_size);
        return 0x14e6f;
    }
    int max = (int)(out_size / (2 * h->channels));
    if (max > 5760 * (int)h->channels) max = 5760 * (int)h->channels;
    int n = h->ms ? opus_multistream_decode(h->ms, src + 8, (opus_int32)size, pcm, max, 0)
                  : opus_decode(h->dec, src + 8, (opus_int32)size, pcm, max, 0);
    if (n < 0) {  // a packet libopus doesn't accept: 20 ms of silence (like nxnative)
        if (h->nerr++ < 8) LOG("[hwopus] libopus rejects a %u-byte packet: %s\n", size, opus_strerror(n));
        n = (int)(h->rate / 50);
        if (n > max) n = max;
        if (pcm && n > 0) memset(pcm, 0, (size_t)n * h->channels * 2);
    }
    OUT(uint32_t, 0) = size + 8;     // bytes consumidos
    OUT(uint32_t, 4) = (uint32_t)n;  // samples per channel
    if (perf) { OUT(uint64_t, 8) = 0; resp_size(resp, 16); }
    else resp_size(resp, 8);
    if (hwlog()) LOG("[hwopus] decode %u bytes -> %d samples (%u ch, output %llu bytes%s)\n", size, n, h->channels,
                     (unsigned long long)out_size, reset ? ", reset" : "");
    return 0;
}

static void hwopus_decoder(Service *s, IpcReq *req, IpcResp *resp) {
    HwOpus *h = s->state;
    switch (req->cmd) {
    case 0: case 2: resp->result = hw_decode(h, req, resp, false, false); break;  // DecodeInterleaved(ForMultiStream)Old
    case 1: case 3: break;                                                         // SetContext(ForMultiStream)
    case 4: case 5: resp->result = hw_decode(h, req, resp, true, false); break;   // ...WithPerfOld
    case 6: case 7: case 8: case 9:                                                // ...WithPerfAndResetOld, DecodeInterleaved
        resp->result = hw_decode(h, req, resp, true, req->data_size >= 1 && IN(uint8_t, 0) != 0);
        break;
    default: LOG("[hwopus] decoder: unknown command %u (ok)\n", req->cmd);
    }
}

static Service *open_single(uint32_t rate, uint32_t channels) {
    int err = 0;
    OpusDecoder *d = opus_decoder_create((opus_int32)rate, (int)channels, &err);
    if (!d) { LOG("[hwopus] opus_decoder_create(%u Hz, %u ch): %s\n", rate, channels, opus_strerror(err)); return NULL; }
    HwOpus *h = calloc(1, sizeof *h);
    h->dec = d; h->rate = rate; h->channels = channels;
    Service *svc = service_new("IHardwareOpusDecoder", hwopus_decoder, h);
    svc->on_close = hwopus_close;
    LOG("[hwopus] decoder opened: %u Hz, %u channels\n", rate, channels);
    return svc;
}

// Multistream parameters (0x110 or, in the Ex form, 0x118 bytes): rate, channels, streams, stereo streams, [Ex: large
// frame + 7 of padding], channel table (256 bytes).
static Service *open_multi(const uint8_t *p, uint64_t n, bool ex) {
    if (!p || n < (ex ? 0x18u : 0x10u)) { LOG("[hwopus] multistream parameters missing\n"); return NULL; }
    uint32_t rate = *(const uint32_t *)p, ch = *(const uint32_t *)(p + 4), streams = *(const uint32_t *)(p + 8),
             stereo = *(const uint32_t *)(p + 12);
    const uint8_t *map = p + (ex ? 0x18 : 0x10);
    int err = 0;
    OpusMSDecoder *d = opus_multistream_decoder_create((opus_int32)rate, (int)ch, (int)streams, (int)stereo, map, &err);
    if (!d) {
        LOG("[hwopus] opus_multistream_decoder_create(%u Hz, %u ch, %u streams, %u stereo): %s\n", rate, ch, streams, stereo,
            opus_strerror(err));
        return NULL;
    }
    HwOpus *h = calloc(1, sizeof *h);
    h->ms = d; h->rate = rate; h->channels = ch;
    Service *svc = service_new("IHardwareOpusDecoder", hwopus_decoder, h);
    svc->on_close = hwopus_close;
    LOG("[hwopus] multistream decoder opened: %u Hz, %u channels, %u streams (%u stereo)\n", rate, ch, streams, stereo);
    return svc;
}

SERVICE("hwopus", hwopus_manager) {
    (void)s;
    IpcBuf *x = in_buf(req, 0);
    const uint8_t *xp = x && x->size ? (const uint8_t *)(uintptr_t)x->addr : NULL;
    uint64_t xn = x ? x->size : 0;
    if (hwlog())
        LOG("[hwopus] manager: command %u (%u bytes: %08x %08x %08x %08x; x %llu bytes)\n", req->cmd, req->data_size,
            req->data_size >= 4 ? IN(uint32_t, 0) : 0, req->data_size >= 8 ? IN(uint32_t, 4) : 0,
            req->data_size >= 12 ? IN(uint32_t, 8) : 0, req->data_size >= 16 ? IN(uint32_t, 12) : 0, (unsigned long long)xn);
    switch (req->cmd) {
    case 0: case 4: {  // OpenHardwareOpusDecoder(Ex)(parameters, transfer memory size, its handle)
        uint32_t rate = IN(uint32_t, 0), ch = IN(uint32_t, 4);
        if (!valid_rate(rate) || ch < 1 || ch > 2) { resp->result = 0x14c6f; LOG("[hwopus] invalid parameters: %u Hz, "
                                                                                 "%u ch\n", rate, ch); break; }
        Service *svc = open_single(rate, ch);
        if (!svc) { resp->result = 0x14c6f; break; }
        ipc_return_object(resp, svc);
        break;
    }
    case 1: case 5: case 8: {  // GetWorkBufferSize / Ex / ExEx
        uint32_t rate = IN(uint32_t, 0), ch = IN(uint32_t, 4);
        bool large = req->cmd != 1 && req->data_size >= 9 && IN(uint8_t, 8) != 0;
        if (!valid_rate(rate) || ch < 1 || ch > 2) { resp->result = 0x14c6f; LOG("[hwopus] size: invalid parameters: "
                                                                                 "%u Hz, %u ch\n", rate, ch); break; }
        OUT(uint32_t, 0) = work_size(rate, ch, 0, 0, large);
        resp_size(resp, 4);
        break;
    }
    case 2: case 6: {  // OpenHardwareOpusDecoderForMultiStream(Ex)(transfer memory size; X: parameters)
        Service *svc = open_multi(xp, xn, req->cmd == 6);
        if (!svc) { resp->result = 0x14c6f; break; }
        ipc_return_object(resp, svc);
        break;
    }
    case 3: case 7: case 9: {  // GetWorkBufferSizeForMultiStream / Ex / ExEx (X: parameters)
        if (!xp || xn < 0x10) { resp->result = 0x14c6f; LOG("[hwopus] multistream size without parameters\n"); break; }
        uint32_t rate = *(const uint32_t *)xp, ch = *(const uint32_t *)(xp + 4), streams = *(const uint32_t *)(xp + 8),
                 stereo = *(const uint32_t *)(xp + 12);
        bool large = req->cmd != 3 && xn >= 0x11 && xp[0x10] != 0;
        OUT(uint32_t, 0) = work_size(rate, ch, streams, stereo, large);
        if (hwlog())
            LOG("[hwopus] multistream size: %u Hz, %u ch, %u streams (%u stereo)%s, table %02x %02x %02x %02x -> %u "
                "bytes\n",
                rate, ch, streams, stereo, large ? ", large frames" : "", xn > 0x1b ? xp[0x18] : 0, xn > 0x1b ? xp[0x19] : 0,
                xn > 0x1b ? xp[0x1a] : 0, xn > 0x1b ? xp[0x1b] : 0, OUT(uint32_t, 0));
        resp_size(resp, 4);
        break;
    }
    default: LOG("[hwopus] manager: unknown command %u (ok)\n", req->cmd);
    }
}
