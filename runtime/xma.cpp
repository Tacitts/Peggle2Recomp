// XMA decoder emulation (the console's hardware XMA2 decoder) on FFmpeg's xma2 decoder.
//
// The title fills 64-byte XMA contexts (XMACreateContext) with input buffers of 2 KB XMA2
// packets and an output ring of 256-byte blocks, and reads 16-bit big-endian PCM from the ring.
// Each audio tick, before the render-driver clients mix, every live context with a valid output
// buffer and a valid input buffer is decoded until its output ring is full.
//
// Context layout (dwords, big-endian; bitfields LSB first), as documented by Xenia:
//   d0: in0 packet count:12, loop count:8, in0 valid:1 @20, in1 valid:1 @21,
//       output block count:5 @22, output write offset:5 @27
//   d1: in1 packet count:12, loop subframe start:2, end:3, skip:3, subframe decode count:4 @20,
//       output padding:3 @24, sample rate:2 @27 (24/32/44.1/48 kHz), stereo:1 @29, -, output valid:1 @31
//   d2: input read offset:26 (bits from the current buffer's start)
//   d3: loop start:26, d4: loop end:26, packet metadata:5 @26, current buffer:1 @31
//   d5/d6: input buffer 0/1 (physical), d7: output buffer (physical), d8: work buffer,
//   d9: output read offset:5
#include "runtime.h"

#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef PEGGLE2_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}
#endif

namespace
{
    constexpr uint32_t kPacketBytes = 2048, kPacketBits = kPacketBytes * 8, kBlockBytes = 256;
    constexpr uint32_t kSampleRates[4] = { 24000, 32000, 44100, 48000 };

    uint32_t Bits(uint32_t v, int at, int n) { return (v >> at) & ((1u << n) - 1); }
    uint32_t SetBits(uint32_t v, int at, int n, uint32_t x)
    {
        uint32_t m = ((1u << n) - 1) << at;
        return (v & ~m) | ((x << at) & m);
    }

#ifdef PEGGLE2_HAVE_FFMPEG
    struct HostContext
    {
        AVCodecContext* codec = nullptr;
        AVPacket* packet = nullptr;
        AVFrame* frame = nullptr;
        uint32_t rate = 0, channels = 0, inputPtr = 0;
        uint32_t expectedReadOffset = 0xFFFFFFFF;   // where we left off (to notice seeks)
        std::vector<uint8_t> pending;                // decoded PCM (16-bit BE, interleaved) not yet written
        std::vector<uint8_t> packetData;
        uint64_t packets = 0;
        float peak = 0;                              // decoded peak since the last log (debugging)
        uint32_t switches = 0, seeks = 0, reopens = 0, restarts = 0, loopsTaken = 0;
        uint32_t lastWrite = 0xFFFFFFFF;             // output write offset we last left
        bool ended = false;                          // input ran out (next valid input = new sound)
        uint32_t lastLoop[3] = {};                   // loop count, start, end seen (debugging)
        ~HostContext()
        {
            av_frame_free(&frame);
            av_packet_free(&packet);
            avcodec_free_context(&codec);
        }
    };
    std::mutex g_lock;
    std::unordered_map<uint32_t, std::unique_ptr<HostContext>> g_host;   // guest context -> decoder
    std::atomic<uint64_t> g_totalPackets{ 0 };

    bool OpenDecoder(HostContext& h, uint32_t rate, uint32_t channels)
    {
        avcodec_free_context(&h.codec);
        static bool logged = false;
        const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
        if (!dec)
        {
            if (!logged) { logged = true; Log("[xma] FFmpeg has no XMA2 decoder"); }
            return false;
        }
        h.codec = avcodec_alloc_context3(dec);
        h.codec->sample_rate = int(rate);
        av_channel_layout_default(&h.codec->ch_layout, int(channels));
        // What a container would give FFmpeg: the packet size, and XMA2WAVEFORMATEX (34 bytes,
        // little-endian): stream count 1, channel mask (FC for mono, FL|FR for stereo).
        h.codec->block_align = int(kPacketBytes);
        h.codec->extradata = (uint8_t*)av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE);
        h.codec->extradata_size = 34;
        h.codec->extradata[0] = 1;
        h.codec->extradata[2] = channels == 2 ? 0x03 : 0x04;
        if (int err = avcodec_open2(h.codec, dec, nullptr); err < 0)
        {
            if (!logged) { logged = true; Log("[xma] avcodec_open2(xma2, %u Hz, %u ch) failed: %d", rate, channels, err); }
            avcodec_free_context(&h.codec);
            return false;
        }
        if (!h.packet) h.packet = av_packet_alloc();
        if (!h.frame) h.frame = av_frame_alloc();
        h.rate = rate;
        h.channels = channels;
        h.pending.clear();
        return true;
    }

    // Decode one 2 KB packet into h.pending (16-bit big-endian, interleaved).
    void DecodePacket(HostContext& h, const uint8_t* data)
    {
        h.packetData.assign(data, data + kPacketBytes);
        h.packetData.resize(kPacketBytes + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        h.packet->data = h.packetData.data();
        h.packet->size = int(kPacketBytes);
        if (avcodec_send_packet(h.codec, h.packet) < 0) return;
        while (avcodec_receive_frame(h.codec, h.frame) == 0)
        {
            int n = h.frame->nb_samples, ch = std::min<int>(h.frame->ch_layout.nb_channels, int(h.channels));
            bool planar = av_sample_fmt_is_planar(AVSampleFormat(h.frame->format));
            for (int i = 0; i < n; i++)
                for (int c = 0; c < int(h.channels); c++)
                {
                    float v = 0;
                    if (c < ch)
                        v = planar ? ((const float*)h.frame->extended_data[c])[i] : ((const float*)h.frame->extended_data[0])[i * ch + c];
                    static const bool testTone = getenv("PEGGLE2_XMA_TEST") != nullptr;   // debugging: square wave
                    if (testTone) v = ((i / 64) & 1) ? 0.5f : -0.5f;
                    h.peak = std::max(h.peak, std::fabs(v));
                    int s = int(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
                    h.pending.push_back(uint8_t(uint16_t(s) >> 8));
                    h.pending.push_back(uint8_t(s));
                }
        }
        h.packets++;
        g_totalPackets++;
    }

    // Start a sound over: the title re-armed the context (reset the output ring, moved the read
    // position, or gave new input after the previous sound ran out).
    void Restart(HostContext& h)
    {
        avcodec_flush_buffers(h.codec);
        h.pending.clear();
    }

    void ProcessContext(uint32_t addr, HostContext& h)
    {
        uint32_t d[10], o[10];
        for (int i = 0; i < 10; i++) o[i] = d[i] = Load32(addr + i * 4);
        if (!Bits(d[1], 31, 1)) return;                      // output buffer not valid
        uint32_t blocks = Bits(d[0], 22, 5);
        uint32_t outPtr = d[7];
        if (!blocks || !outPtr) return;
        uint32_t rate = kSampleRates[Bits(d[1], 27, 2)], channels = Bits(d[1], 29, 1) ? 2 : 1;
        // The decoder keeps its state across input buffers (frames span packets and buffers);
        // it is only rebuilt when the format changes.
        if (!h.codec || h.rate != rate || h.channels != channels)
        {
            if (!OpenDecoder(h, rate, channels)) return;
            h.reopens++;
            h.expectedReadOffset = 0xFFFFFFFF;
            h.lastWrite = 0xFFFFFFFF;
        }
        uint32_t current = Bits(d[4], 31, 1);
        uint32_t write = Bits(d[0], 27, 5), read = Bits(d[9], 0, 5);
        if (h.lastWrite != 0xFFFFFFFF && write != h.lastWrite) { Restart(h); h.restarts++; }   // ring reset by the title
        if (h.ended && Bits(d[0], current ? 21 : 20, 1)) { Restart(h); h.ended = false; }      // new input after the end
        h.lastLoop[0] = Bits(d[0], 12, 8);
        h.lastLoop[1] = Bits(d[3], 0, 26);
        h.lastLoop[2] = Bits(d[4], 0, 26);
        bool changed = false;
        for (;;)
        {
            // Free blocks in the ring (write == read means empty).
            uint32_t used = (write + blocks - read) % blocks;
            uint32_t freeBlocks = blocks - 1 - used;
            while (freeBlocks && h.pending.size() >= kBlockBytes)
            {
                memcpy(GuestPtr(0xA0000000u | (outPtr + write * kBlockBytes)), h.pending.data(), kBlockBytes);
                h.pending.erase(h.pending.begin(), h.pending.begin() + kBlockBytes);
                write = (write + 1) % blocks;
                freeBlocks--;
                changed = true;
            }
            if (!freeBlocks) break;

            uint32_t validBit = current ? 21 : 20;
            if (!Bits(d[0], validBit, 1))
            {
                h.ended = true;   // starved or finished: the next valid input starts fresh
                break;
            }
            uint32_t count = current ? Bits(d[1], 0, 12) : Bits(d[0], 0, 12);
            uint32_t readOffset = Bits(d[2], 0, 26);
            if (h.expectedReadOffset != 0xFFFFFFFF && readOffset != h.expectedReadOffset)
            {
                h.seeks++;
                Restart(h);       // the title moved the read position
            }
            uint32_t packet = readOffset / kPacketBits;
            if (packet >= count)
            {
                // Buffer finished: hand it back and continue with the other one (same stream).
                d[0] = SetBits(d[0], validBit, 1, 0);
                current ^= 1;
                d[4] = SetBits(d[4], 31, 1, current);
                d[2] = SetBits(d[2], 0, 26, 32);
                h.expectedReadOffset = 32;
                h.switches++;
                changed = true;
                continue;
            }
            DecodePacket(h, GuestPtr(0xA0000000u | ((current ? d[6] : d[5]) + packet * kPacketBytes)));
            uint32_t next = (packet + 1) * kPacketBits + 32;   // past the next packet's header
            // Hardware loop: loop count (d0 bits 12-19, 255 = forever), loop start/end (d3/d4,
            // bit offsets in the input). After the packet holding the loop end, continue at the
            // loop start (packet granularity: the loop end is normally the end of the data).
            uint32_t loops = Bits(d[0], 12, 8), loopStart = Bits(d[3], 0, 26), loopEnd = Bits(d[4], 0, 26);
            if (loops && loopEnd > loopStart && packet >= loopEnd / kPacketBits)
            {
                next = loopStart;
                if (loops != 255) d[0] = SetBits(d[0], 12, 8, loops - 1);
                avcodec_flush_buffers(h.codec);   // bitstream discontinuity; queued output is kept
                h.loopsTaken++;
            }
            d[2] = SetBits(d[2], 0, 26, next);
            h.expectedReadOffset = next;
            changed = true;
        }
        h.lastWrite = write;
        if (!changed) return;

        // Write back only what the decoder changed, and only where the title hasn't written since
        // the context was read (it refills buffers and resets offsets from its own threads).
        uint32_t c0 = Load32(addr), c2 = Load32(addr + 8), c4 = Load32(addr + 16);
        uint32_t c5 = Load32(addr + 20), c6 = Load32(addr + 24);
        if (Bits(c0, 27, 5) == Bits(o[0], 27, 5)) c0 = SetBits(c0, 27, 5, write);
        else h.lastWrite = Bits(c0, 27, 5);
        // Clear a finished buffer's valid bit unless the title has already refilled it.
        if (Bits(o[0], 20, 1) && !Bits(d[0], 20, 1) && c5 == o[5]) c0 = SetBits(c0, 20, 1, 0);
        if (Bits(o[0], 21, 1) && !Bits(d[0], 21, 1) && c6 == o[6]) c0 = SetBits(c0, 21, 1, 0);
        if (Bits(c0, 12, 8) == Bits(o[0], 12, 8)) c0 = SetBits(c0, 12, 8, Bits(d[0], 12, 8));   // loop count
        if (Bits(c2, 0, 26) == Bits(o[2], 0, 26)) c2 = SetBits(c2, 0, 26, Bits(d[2], 0, 26));
        else h.expectedReadOffset = 0xFFFFFFFF;   // the title moved it meanwhile: follow it next tick
        if (Bits(c4, 31, 1) == Bits(o[4], 31, 1)) c4 = SetBits(c4, 31, 1, Bits(d[4], 31, 1));
        Store32(addr, c0);
        Store32(addr + 8, c2);
        Store32(addr + 16, c4);
    }
#endif
}

// Called by the audio driver thread once per tick with the live contexts.
void XmaTick(const std::vector<uint32_t>& contexts)
{
#ifdef PEGGLE2_HAVE_FFMPEG
    std::lock_guard<std::mutex> g(g_lock);
    for (uint32_t c : contexts)
    {
        auto& h = g_host[c];
        if (!h) h = std::make_unique<HostContext>();
        ProcessContext(c, *h);
    }
    static auto nextLog = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    if (std::chrono::steady_clock::now() >= nextLog)
    {
        nextLog += std::chrono::seconds(10);
        for (auto& [c, h] : g_host)
            if (h->packets)
                Log("[xma] ctx %08X: %u Hz %u ch, %llu packets, peak %.3f, %u switches, %u seeks, %u restarts, %u reopens, %u loops taken, loop count %u start %u end %u",
                    c, h->rate, h->channels, (unsigned long long)h->packets, h->peak, h->switches, h->seeks, h->restarts, h->reopens, h->loopsTaken,
                    h->lastLoop[0], h->lastLoop[1], h->lastLoop[2]), h->peak = 0;
    }
    // Forget decoders of released contexts.
    for (auto it = g_host.begin(); it != g_host.end();)
    {
        if (std::find(contexts.begin(), contexts.end(), it->first) == contexts.end()) it = g_host.erase(it);
        else ++it;
    }
#else
    (void)contexts;
#endif
}

uint64_t XmaPacketsDecoded()
{
#ifdef PEGGLE2_HAVE_FFMPEG
    return g_totalPackets.load();
#else
    return 0;
#endif
}
