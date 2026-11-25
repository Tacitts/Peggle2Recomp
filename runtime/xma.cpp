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
#include <algorithm>
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
    // ---- XMA2 frame map --------------------------------------------------------------------
    // A buffer is a run of 2 KB packets. Packet header (32 bits, big-endian): frame count:6 @26
    // (frames starting in this packet), first frame offset:15 @11 (bits after the header),
    // metadata:3 @8, packet skip:8. Frames are packed back to back across packets (headers
    // skipped); each starts with a 15-bit length in bits (0x7FFF = padding to the next packet)
    // and decodes to 512 samples per channel. Positions are buffer bit offsets including the
    // headers - the coordinates of the context's read offset and loop points.
    constexpr uint32_t kSamplesPerFrame = 512;
    struct FrameMap
    {
        uint32_t ptr = 0, count = 0;
        std::vector<std::pair<uint32_t, uint32_t>> frames;   // [start, end)
        std::vector<uint32_t> firstStart;                     // per packet, ~0u = no frame starts
        std::vector<uint16_t> endingIn;                       // per packet: frames ending in it
        uint32_t resyncs = 0;
    };

    uint32_t Advance(uint32_t b, uint32_t n)
    {
        while (n)
        {
            uint32_t end = (b / kPacketBits + 1) * kPacketBits;
            if (b + n < end) return b + n;
            n -= end - b;
            b = end + 32;   // skip the next packet's header
        }
        return b;
    }

    void BuildFrameMap(FrameMap& m, uint32_t ptr, uint32_t count)
    {
        m = FrameMap{};
        m.ptr = ptr;
        m.count = count;
        const uint8_t* buf = GuestPtr(0xA0000000u | ptr);
        uint32_t total = count * kPacketBits;
        m.firstStart.assign(count, ~0u);
        m.endingIn.assign(count, 0);
        for (uint32_t p = 0; p < count; p++)
        {
            uint32_t h = (uint32_t(buf[p * kPacketBytes]) << 24) | (buf[p * kPacketBytes + 1] << 16) | (buf[p * kPacketBytes + 2] << 8) | buf[p * kPacketBytes + 3];
            if (h >> 26) m.firstStart[p] = p * kPacketBits + 32 + ((h >> 11) & 0x7FFF);
        }
        auto bit = [&](uint32_t b) { return (buf[b >> 3] >> (7 - (b & 7))) & 1; };
        auto nextFirst = [&](uint32_t fromPacket) {
            for (uint32_t p = fromPacket; p < count; p++) if (m.firstStart[p] != ~0u) return m.firstStart[p];
            return total;
        };
        uint32_t cur = nextFirst(0);
        while (cur + 15 < total)
        {
            uint32_t len = 0, b = cur;
            for (int i = 0; i < 15 && b < total; i++) { len = (len << 1) | bit(b); b = Advance(b, 1); }
            if (len == 0x7FFF || len < 15) { cur = nextFirst(cur / kPacketBits + 1); continue; }   // padding
            uint32_t end = Advance(cur, len);
            if (end > total) break;
            m.frames.push_back({ cur, end });
            uint32_t ep = (end - 1) / kPacketBits;
            if (ep < count) m.endingIn[ep]++;
            // A frame ending in a later packet: the next frame must be where that packet's header says.
            uint32_t sp = cur / kPacketBits;
            cur = end;
            if (ep > sp && ep < count && m.firstStart[ep] != ~0u && cur != m.firstStart[ep]) { m.resyncs++; cur = m.firstStart[ep]; }
        }
    }

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
        // Frame-accurate position since the decoder (re)started at startBit: samples to drop
        // (frames ahead of startBit in its packet), samples decoded since.
        FrameMap map;
        bool needBegin = true;
        uint32_t startBit = 0;
        uint32_t feedBit = 0;      // next packet to feed the decoder (internal)
        uint64_t skip = 0, sinceStart = 0;
        uint32_t checkMismatches = 0, checkedPackets = 0;
        bool loopLogged = false;
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
    void DecodePacket(HostContext& h, const uint8_t* data, uint32_t packetIndex)
    {
        uint64_t decoded = 0;
        if (data)
        {
            h.packetData.assign(data, data + kPacketBytes);
            h.packetData.resize(kPacketBytes + AV_INPUT_BUFFER_PADDING_SIZE, 0);
            h.packet->data = h.packetData.data();
            h.packet->size = int(kPacketBytes);
            if (avcodec_send_packet(h.codec, h.packet) < 0) return;
        }
        else if (avcodec_send_packet(h.codec, nullptr) < 0) return;   // drain
        while (avcodec_receive_frame(h.codec, h.frame) == 0)
        {
            int n = h.frame->nb_samples, ch = std::min<int>(h.frame->ch_layout.nb_channels, int(h.channels));
            bool planar = av_sample_fmt_is_planar(AVSampleFormat(h.frame->format));
            decoded += uint64_t(n);
            for (int i = 0; i < n; i++)
            {
                if (h.skip) { h.skip--; continue; }   // frames before the start position
                h.sinceStart++;
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
        }
        // Self-check: the frame map and the decoder must agree on the frames ending here
        // (frame-accurate loops depend on it). Packets right after a (re)start are skipped.
        if (!data) return;
        if (packetIndex < h.map.endingIn.size() && !h.needBegin)
        {
            h.checkedPackets++;
            if (decoded != uint64_t(h.map.endingIn[packetIndex]) * kSamplesPerFrame)
            {
                h.checkMismatches++;
                static int logged = 0;
                if (logged++ < 10)
                    Log("[xma] frame map check: packet %u decoded %llu samples, map says %u frames", packetIndex,
                        (unsigned long long)decoded, h.map.endingIn[packetIndex]);
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
        h.needBegin = true;
    }

    // (Re)start position R: the decoder resumes at the first frame starting in R's packet, so
    // drop the frames between that and R.
    void BeginAt(HostContext& h, uint32_t r)
    {
        h.startBit = r;
        h.feedBit = r;
        h.sinceStart = 0;
        h.skip = 0;
        h.needBegin = false;
        uint32_t p = r / kPacketBits;
        if (p >= h.map.firstStart.size()) return;
        uint32_t first = h.map.firstStart[p];
        for (auto& [fs, fe] : h.map.frames)
            if (fs >= first && fs < r) h.skip += kSamplesPerFrame;
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
            h.needBegin = true;
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
            uint32_t inPtr = current ? d[6] : d[5];
            if (h.map.ptr != inPtr || h.map.count != count) BuildFrameMap(h.map, inPtr, count);
            if (h.needBegin) BeginAt(h, readOffset);
            uint32_t packet = h.feedBit / kPacketBits;
            if (packet >= count)
            {
                // Buffer finished: hand it back and continue with the other one (same stream:
                // the decoder carries on, positions restart at the new buffer's start).
                d[0] = SetBits(d[0], validBit, 1, 0);
                current ^= 1;
                d[4] = SetBits(d[4], 31, 1, current);
                d[2] = SetBits(d[2], 0, 26, 32);
                h.expectedReadOffset = 32;
                h.switches++;
                h.startBit = 0;
                h.feedBit = 32;
                h.sinceStart = 0;
                changed = true;
                continue;
            }
            DecodePacket(h, GuestPtr(0xA0000000u | (inPtr + packet * kPacketBytes)), packet);
            h.feedBit = (packet + 1) * kPacketBits + 32;
            // Read offset as the hardware reports it: the next frame to be decoded, i.e. the first
            // frame starting in the next packet to feed (frame-accurate, ahead of the output, like
            // Xenia). Reporting the output's position instead (behind the input) crashed the
            // sound engine at the main menu.
            uint32_t nextPacket = h.feedBit / kPacketBits;
            uint32_t next = nextPacket < count && h.map.firstStart[nextPacket] != ~0u ? h.map.firstStart[nextPacket] : h.feedBit;
            // Hardware loop: loop count (d0 bits 12-19, 255 = forever), loop start/end (d3/d4).
            // Frame-accurate: once the frames from startBit up to the loop end have been decoded,
            // cut the output exactly there and continue at the loop start. (Playing past the loop
            // end desynchronised the sound engine and crashed it at the main menu.)
            uint32_t loops = Bits(d[0], 12, 8), loopStart = Bits(d[3], 0, 26), loopEnd = Bits(d[4], 0, 26);
            if (loops && loopEnd > loopStart && h.startBit < loopEnd)
            {
                uint64_t target = 0;
                uint32_t lastPacket = 0;
                for (auto& [fs, fe] : h.map.frames)
                    if (fs >= h.startBit && fe <= loopEnd) { target += kSamplesPerFrame; lastPacket = std::max(lastPacket, (fe - 1) / kPacketBits); }
                // The decoder holds back the last samples it decoded: once the packet with the
                // loop's last frame is in, drain it so the whole loop has come out.
                if (target && h.sinceStart < target && packet >= lastPacket)
                {
                    DecodePacket(h, nullptr, packet);
                    if (h.sinceStart < target)
                    {
                        static int logged = 0;
                        if (logged++ < 5) Log("[xma] loop short: %llu of %llu samples after draining", (unsigned long long)h.sinceStart, (unsigned long long)target);
                        h.sinceStart = target;   // jump anyway
                    }
                }
                if (!h.loopLogged)   // debugging: one line per looping sound
                {
                    h.loopLogged = true;
                    Log("[xma] loop: %u packets, loop %u..%u, start %u, %zu frames mapped, target %llu samples, feeding packet %u, output %llu",
                        count, loopStart, loopEnd, h.startBit, h.map.frames.size(), (unsigned long long)target, packet, (unsigned long long)h.sinceStart);
                }
                if (target && h.sinceStart >= target)
                {
                    uint64_t excess = h.sinceStart - target;
                    size_t bytes = size_t(excess) * h.channels * 2;
                    if (bytes <= h.pending.size())
                        h.pending.resize(h.pending.size() - bytes);
                    else
                    {
                        static int logged = 0;
                        if (logged++ < 5) Log("[xma] loop end overshoot already written (%llu samples)", (unsigned long long)excess);
                        h.pending.clear();
                    }
                    if (loops != 255) d[0] = SetBits(d[0], 12, 8, loops - 1);
                    avcodec_flush_buffers(h.codec);   // bitstream discontinuity; queued output is kept
                    BeginAt(h, loopStart);
                    next = loopStart;
                    static int loggedJumps = 0;
                    if (loggedJumps++ < 20)
                        Log("[xma] loop taken: %llu samples per pass (%u frames), %llu excess trimmed, %u loops left",
                            (unsigned long long)target, uint32_t(target / kSamplesPerFrame), (unsigned long long)excess, loops == 255 ? 255u : loops - 1);
                    h.loopsTaken++;
                }
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
                Log("[xma] ctx %08X: %u Hz %u ch, %llu packets, peak %.3f, %u switches, %u seeks, %u restarts, %u reopens, %u loops taken, map check %u/%u bad, loop count %u start %u end %u",
                    c, h->rate, h->channels, (unsigned long long)h->packets, h->peak, h->switches, h->seeks, h->restarts, h->reopens, h->loopsTaken, h->checkMismatches, h->checkedPackets,
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
