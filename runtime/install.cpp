// Install the game from the player's own Xbox Live Arcade package (STFS "LIVE"/"CON"/"PIRS"):
// checks the title ID, extracts every file into game\ next to the executable (the same block
// layout as tools/stfs_extract.py: interleaved level 0/1/2 hash tables, one or two table copies),
// then checks that Default.xex arrived. Used by the launcher's Install button.
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include "runtime.h"
#include "settings.h"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

namespace
{
    constexpr uint32_t kTitleId = 0x58411436, kContentArcade = 0x000D0000;
    constexpr uint32_t kBlock = 0x1000, kL0 = 0xAA, kL1 = 0xAA * 0xAA;

    uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
    uint32_t U24LE(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16; }

    struct Entry { std::string name; bool dir, consecutive; uint32_t blocks, start, size; int parent; };

    class Stfs
    {
    public:
        bool Open(const std::filesystem::path& path, std::string& error)
        {
            f.open(path, std::ios::binary);
            uint8_t hdr[0x971A] = {};
            if (!f.read((char*)hdr, sizeof(hdr))) { error = "The file is too small to be an Xbox 360 package."; return false; }
            if (memcmp(hdr, "LIVE", 4) && memcmp(hdr, "CON ", 4) && memcmp(hdr, "PIRS", 4))
            { error = "This is not an Xbox 360 package (LIVE/CON/PIRS)."; return false; }
            headerSize = BE32(hdr + 0x340);
            contentType = BE32(hdr + 0x344);
            titleId = BE32(hdr + 0x360);
            const uint8_t* vd = hdr + 0x379;
            blockSep = vd[2];
            ftBlockCount = uint32_t(vd[3]) | uint32_t(vd[4]) << 8;
            ftBlock = U24LE(vd + 5);
            totalBlocks = BE32(vd + 0x1C);
            sex = (~blockSep) & 1;
            step0 = sex == 0 ? 0xAB : 0xAC;
            step1 = sex == 0 ? 0x718F : 0x723A;
            first = (headerSize + 0xFFF) & 0xFFFFF000u;
            top = totalBlocks <= kL0 ? 0 : totalBlocks <= kL1 ? 1 : 2;
            return true;
        }

        uint32_t titleId = 0, contentType = 0;

        bool Entries(std::vector<Entry>& out)
        {
            std::vector<uint8_t> raw;
            uint32_t b = ftBlock;
            for (uint32_t i = 0; i < ftBlockCount; i++)
            {
                std::vector<uint8_t> blk(kBlock);
                if (!ReadBlock(b, blk.data())) return false;
                raw.insert(raw.end(), blk.begin(), blk.end());
                b = Next(b);
            }
            for (size_t i = 0; i + 0x40 <= raw.size(); i += 0x40)
            {
                const uint8_t* e = raw.data() + i;
                uint8_t flags = e[0x28];
                uint32_t nlen = flags & 0x3F;
                if (!nlen) break;
                Entry en;
                en.name.assign((const char*)e, nlen);
                en.dir = flags & 0x80;
                en.consecutive = flags & 0x40;
                en.blocks = U24LE(e + 0x29);
                en.start = U24LE(e + 0x2F);
                en.parent = int16_t(uint16_t(e[0x32] << 8 | e[0x33]));
                en.size = BE32(e + 0x34);
                out.push_back(en);
            }
            return true;
        }

        bool ReadBlock(uint32_t b, uint8_t* dst)
        {
            f.clear();
            f.seekg(std::streamoff(BlockAddr(b)));
            return bool(f.read((char*)dst, kBlock));
        }

        uint32_t Next(uint32_t b)
        {
            uint32_t active = (blockSep >> 1) & 1;
            for (int level = top; level > 0; level--)
            {
                const uint8_t* t = Table(b, level, active);
                uint32_t div = level == 1 ? kL0 : kL1;
                uint32_t idx = (b / div) % kL0;
                active = (t[idx * 0x18 + 0x14] >> 6) & 1;
            }
            const uint8_t* t = Table(b, 0, active);
            const uint8_t* e = t + (b % kL0) * 0x18;
            return uint32_t(e[0x15]) << 16 | uint32_t(e[0x16]) << 8 | e[0x17];
        }

    private:
        std::ifstream f;
        uint32_t headerSize = 0, ftBlockCount = 0, ftBlock = 0, totalBlocks = 0, first = 0, step0 = 0, step1 = 0;
        uint8_t blockSep = 0;
        int sex = 0, top = 0;
        std::map<uint64_t, std::vector<uint8_t>> cache;

        uint64_t DataBacking(uint64_t b) const
        {
            uint64_t r = (((b + kL0) / kL0) << sex) + b;
            if (b < kL0) return r;
            if (b < kL1) return r + (((b + kL1) / kL1) << sex);
            return (uint64_t(1) << sex) + r + (((b + kL1) / kL1) << sex);
        }
        uint64_t BlockAddr(uint64_t b) const { return (DataBacking(b) << 12) + first; }
        uint64_t HashBacking(uint64_t b, int level) const
        {
            if (level == 0)
            {
                uint64_t n = (b / kL0) * step0;
                if (b >= kL0)
                {
                    n += ((b / kL1) + 1) << sex;
                    if (b >= kL1) n += uint64_t(1) << sex;
                }
                return n;
            }
            if (level == 1) return b < kL1 ? step0 : (b / kL1) * step1 + (uint64_t(1) << sex);
            return step1;
        }
        const uint8_t* Table(uint64_t b, int level, uint32_t active)
        {
            uint64_t addr = (HashBacking(b, level) << 12) + first;
            if (sex == 1 && active) addr += kBlock;
            auto it = cache.find(addr);
            if (it != cache.end()) return it->second.data();
            if (cache.size() > 64) cache.clear();
            std::vector<uint8_t> t(kBlock);
            f.clear();
            f.seekg(std::streamoff(addr));
            f.read((char*)t.data(), kBlock);
            return cache.emplace(addr, std::move(t)).first->second.data();
        }
    };

    std::atomic<uint64_t> g_done{ 0 }, g_total{ 1 };
    std::atomic<bool> g_finished{ false };
    std::string g_error;

    void Extract(std::filesystem::path package, std::filesystem::path out)
    {
        Stfs s;
        std::string err;
        std::vector<Entry> ents;
        if (!s.Open(package, err)) { g_error = err; g_finished = true; return; }
        if (s.titleId != kTitleId || s.contentType != kContentArcade)
        {
            char buf[200];
            snprintf(buf, sizeof(buf), "This package is not Peggle 2 for Xbox Live Arcade (title %08X, content type %08X; expected %08X, %08X).",
                     s.titleId, s.contentType, kTitleId, kContentArcade);
            g_error = buf;
            g_finished = true;
            return;
        }
        if (!s.Entries(ents)) { g_error = "Could not read the package's file table."; g_finished = true; return; }
        uint64_t total = 0;
        for (auto& e : ents) if (!e.dir) total += e.size;
        g_total = total ? total : 1;
        std::vector<uint8_t> blk(kBlock);
        for (auto& e : ents)
        {
            std::filesystem::path rel;
            std::vector<std::string> parts{ e.name };
            for (int p = e.parent; p >= 0 && p < int(ents.size()); p = ents[size_t(p)].parent) parts.push_back(ents[size_t(p)].name);
            for (auto it = parts.rbegin(); it != parts.rend(); ++it) rel /= *it;
            std::filesystem::path dst = out / rel;
            std::error_code ec;
            if (e.dir) { std::filesystem::create_directories(dst, ec); continue; }
            std::filesystem::create_directories(dst.parent_path(), ec);
            std::ofstream o(dst, std::ios::binary | std::ios::trunc);
            if (!o) { g_error = "Cannot write " + dst.string(); g_finished = true; return; }
            uint32_t remaining = e.size, b = e.start;
            for (uint32_t i = 0; i < e.blocks && remaining; i++)
            {
                if (!s.ReadBlock(b, blk.data())) { g_error = "The package is truncated or damaged (" + rel.string() + ")."; g_finished = true; return; }
                uint32_t n = std::min(remaining, kBlock);
                o.write((const char*)blk.data(), n);
                remaining -= n;
                g_done += n;
                if (remaining) b = e.consecutive ? b + 1 : s.Next(b);
            }
        }
        if (!std::filesystem::exists(out / "Default.xex")) g_error = "The package did not contain Default.xex.";
        g_finished = true;
    }
}

// Ask for the package, extract it with a progress window. Returns true when installed.
bool InstallGamePackageFrom(HWND owner, const std::filesystem::path& file);

bool InstallGamePackage(HWND owner)
{
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = owner;
    ofn.lpstrTitle = L"Choose your Peggle 2 package (Xbox 360 content folder 58411436\\000D0000)";
    ofn.lpstrFilter = L"Xbox 360 package (all files)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    return InstallGamePackageFrom(owner, file);
}

// Extract a package file (chosen above, or dropped onto the launcher).
bool InstallGamePackageFrom(HWND owner, const std::filesystem::path& file)
{
    std::filesystem::path out = PortDir() / "game";
    g_done = 0;
    g_total = 1;
    g_finished = false;
    g_error.clear();
    std::thread worker(Extract, std::filesystem::path(file), out);

    // Progress window (a pumped modal loop).
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"STATIC", L"Installing Peggle 2...", WS_POPUP | WS_CAPTION | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT, 420, 110, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    RECT pr;
    GetWindowRect(owner, &pr);
    SetWindowPos(dlg, HWND_TOP, (pr.left + pr.right) / 2 - 210, (pr.top + pr.bottom) / 2 - 55, 420, 110, 0);
    HWND bar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 20, 25, 360, 22, dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(bar, PBM_SETRANGE32, 0, 1000);
    EnableWindow(owner, FALSE);
    while (!g_finished)
    {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        SendMessageW(bar, PBM_SETPOS, WPARAM(g_done.load() * 1000 / std::max<uint64_t>(g_total.load(), 1)), 0);
        Sleep(30);
    }
    worker.join();
    EnableWindow(owner, TRUE);
    DestroyWindow(dlg);
    SetForegroundWindow(owner);
    if (!g_error.empty())
    {
        Log("[install] failed: %s", g_error.c_str());
        std::wstring w(g_error.begin(), g_error.end());
        MessageBoxW(owner, w.c_str(), L"Peggle 2 - install", MB_ICONERROR);
        return false;
    }
    Log("[install] installed %.1f MB into %s", g_done.load() / 1048576.0, out.string().c_str());
    return true;
}
