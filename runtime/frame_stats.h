// Frame-time histograms for frame-pacing work (logged every 10 s as "[frames]" lines).
// 30 fps means a 33.3 ms budget per game frame; anything over ~34 ms is a visible drop.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

struct FrameHist
{
    // Bucket upper edges in ms; the last bucket is everything above.
    static constexpr double kEdges[] = { 17.5, 34.5, 50.5, 67.5, 100.5, 250.5 };
    static constexpr int kBuckets = int(sizeof(kEdges) / sizeof(kEdges[0])) + 1;

    std::mutex lock;
    uint64_t counts[kBuckets] = {};
    uint64_t n = 0;
    double maxMs = 0, sumMs = 0, lateMs = 0;   // lateMs: time past 34.5 ms, summed over late frames

    void Add(double ms)
    {
        std::lock_guard<std::mutex> g(lock);
        int b = 0;
        while (b < kBuckets - 1 && ms > kEdges[b]) b++;
        counts[b]++;
        n++;
        sumMs += ms;
        if (ms > kEdges[1]) lateMs += ms - kEdges[1];
        maxMs = std::max(maxMs, ms);
    }

    // One line ("name: n frames, avg, max, <=17 a | <=34 b | ..."), then reset.
    std::string TakeLine(const char* name)
    {
        std::lock_guard<std::mutex> g(lock);
        char buf[512];
        int len = snprintf(buf, sizeof(buf), "%s: %llu, avg %.1f ms, max %.1f ms, late by %.0f ms in total, over 34 ms: %llu |", name,
                           (unsigned long long)n, n ? sumMs / n : 0.0, maxMs, lateMs,
                           (unsigned long long)(n - counts[0] - counts[1]));
        static const char* labels[] = { "<=17", "<=34", "<=50", "<=67", "<=100", "<=250", ">250" };
        for (int b = 0; b < kBuckets && len < int(sizeof(buf)) - 32; b++)
            len += snprintf(buf + len, sizeof(buf) - len, " %s %llu", labels[b], (unsigned long long)counts[b]);
        *this = {};
        return buf;
    }

    FrameHist() = default;
    FrameHist& operator=(const FrameHist&) { for (auto& c : counts) c = 0; n = 0; maxMs = sumMs = lateMs = 0; return *this; }
};

extern FrameHist g_swapHist;      // guest VdSwap to VdSwap (the game's own frame time)
