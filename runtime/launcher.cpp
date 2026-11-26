// Launcher: a settings window shown before the game starts (settings.h). Plain Win32 controls
// (common controls v6), no extra dependencies. One executable: it opens every time, and Play starts
// the game in the same process. Returns whether to start the game.
#ifndef UNICODE
#define UNICODE   // the ListView_/TabCtrl_ macros pick the wide-character messages
#define _UNICODE
#endif
#include "launcher.h"
#include "runtime.h"
#include "settings.h"
#include "achievements.h"

#include <functional>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <wincodec.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

const char* PortVersion();   // updater.cpp

namespace
{
    constexpr int kW = 720, kH = 560;           // client size at 96 dpi
    constexpr int kPageX = 20, kPageY = 44, kPageW = kW - 40, kPageH = kH - 110;
    enum : int { kIdPlay = 1, kIdQuit = 2, kIdTabs = 3, kIdFirst = 100 };

    HWND g_wnd = nullptr, g_tabs = nullptr;
    HFONT g_font = nullptr, g_bold = nullptr;
    std::vector<HWND> g_pages;
    int g_dpi = 96;
    bool g_play = false;
    LauncherInfo g_info;

    int S(int v) { return MulDiv(v, g_dpi, 96); }   // scale 96-dpi layout units

    // Controls bound to settings: refresh() puts the setting into the control, apply() reads it back.
    struct Binding { HWND h; std::function<void()> refresh, apply; };
    std::vector<Binding> g_bindings;
    std::vector<std::function<void(int)>> g_commands;   // id - kIdFirst -> handler(notification)

    int NewId(std::function<void(int)> fn)
    {
        g_commands.push_back(std::move(fn));
        return kIdFirst + int(g_commands.size()) - 1;
    }

    HWND Make(HWND page, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0, DWORD ex = 0)
    {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), page,
                                 (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
        return c;
    }
    std::wstring W(const std::string& s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(size_t(n - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    }

    HWND Label(HWND page, int x, int y, int w, const wchar_t* text, bool bold = false)
    {
        HWND h = Make(page, L"STATIC", text, SS_LEFT, x, y, w, 20);
        if (bold) SendMessageW(h, WM_SETFONT, (WPARAM)g_bold, TRUE);
        return h;
    }
    HWND Note(HWND page, int x, int y, int w, int h, const wchar_t* text)
    {
        return Make(page, L"STATIC", text, SS_LEFT, x, y, w, h);
    }

    void Check(HWND page, int x, int y, int w, const wchar_t* text, bool* value)
    {
        HWND h = Make(page, L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 22, NewId(nullptr));
        g_bindings.push_back({ h, [h, value] { SendMessageW(h, BM_SETCHECK, *value ? BST_CHECKED : BST_UNCHECKED, 0); },
                               [h, value] { *value = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; } });
    }

    template <class T>
    void Combo(HWND page, int x, int y, int labelW, int comboW, const wchar_t* label, T* value,
               std::vector<std::pair<std::wstring, T>> options)
    {
        Label(page, x, y + 3, labelW, label);
        HWND h = Make(page, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x + labelW, y, comboW, 300, NewId(nullptr));
        for (auto& [text, v] : options) SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)text.c_str());
        g_bindings.push_back({ h,
            [h, value, options] {
                int sel = 0;
                for (size_t i = 0; i < options.size(); i++) if (options[i].second == *value) sel = int(i);
                SendMessageW(h, CB_SETCURSEL, sel, 0);
            },
            [h, value, options] {
                int sel = int(SendMessageW(h, CB_GETCURSEL, 0, 0));
                if (sel >= 0 && sel < int(options.size())) *value = options[sel].second;
            } });
    }

    HWND Button(HWND page, int x, int y, int w, const wchar_t* text, std::function<void()> fn)
    {
        return Make(page, L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, x, y, w, 26, NewId([fn](int code) { if (code == BN_CLICKED) fn(); }));
    }

    void RefreshAll() { for (auto& b : g_bindings) if (b.refresh) b.refresh(); }
    void ApplyAll() { for (auto& b : g_bindings) if (b.apply) b.apply(); }

    void OpenFolder(const std::filesystem::path& p)
    {
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // ---- Key names / capture -------------------------------------------------------------------
    std::wstring KeyName(int vk)
    {
        if (!vk) return L"-";
        switch (vk)
        {
        case VK_LEFT: return L"Left"; case VK_RIGHT: return L"Right"; case VK_UP: return L"Up"; case VK_DOWN: return L"Down";
        case VK_SHIFT: return L"Shift"; case VK_CONTROL: return L"Ctrl"; case VK_MENU: return L"Alt";
        case VK_RETURN: return L"Enter"; case VK_SPACE: return L"Space"; case VK_ESCAPE: return L"Esc";
        case VK_TAB: return L"Tab"; case VK_BACK: return L"Backspace";
        }
        UINT sc = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC);
        wchar_t buf[64] = L"";
        if (sc && GetKeyNameTextW(LONG(sc << 16), buf, 64) > 0) return buf;
        wchar_t b2[16];
        swprintf(b2, 16, L"Key %d", vk);
        return b2;
    }

    HWND g_keyList = nullptr, g_keyHint = nullptr;
    int g_captureRow = -1, g_captureSlot = 0;

    void FillKeyList()
    {
        const Settings& s = GetSettings();
        ListView_DeleteAllItems(g_keyList);
        for (int i = 0; i < kKeyControlCount; i++)
        {
            std::wstring name = W(kKeyControls[i].name), k1 = KeyName(s.keys[i][0]), k2 = KeyName(s.keys[i][1]);
            LVITEMW it{};
            it.mask = LVIF_TEXT;
            it.iItem = i;
            it.pszText = name.data();
            ListView_InsertItem(g_keyList, &it);
            ListView_SetItemText(g_keyList, i, 1, k1.data());
            ListView_SetItemText(g_keyList, i, 2, k2.data());
        }
    }

    // ---- Controller remap ------------------------------------------------------------------------
    HWND g_padList = nullptr, g_padCombo = nullptr;
    const int kPadButtons[] = { 12, 13, 14, 15, 4, 5, 8, 9, 0, 1, 2, 3, 6, 7 };   // shown order (A B X Y Start Back LB RB ...)
    std::wstring HostButtonName(uint16_t mask)
    {
        for (int b = 0; b < 16; b++) if (mask == (1u << b)) return W(kButtonNames[b]);
        return mask ? L"(several)" : L"(none)";
    }
    void FillPadList()
    {
        const Settings& s = GetSettings();
        ListView_DeleteAllItems(g_padList);
        for (int r = 0; r < int(std::size(kPadButtons)); r++)
        {
            int b = kPadButtons[r];
            std::wstring name = W(kButtonNames[b]), host = HostButtonName(s.buttonMap[b]);
            LVITEMW it{};
            it.mask = LVIF_TEXT;
            it.iItem = r;
            it.pszText = name.data();
            ListView_InsertItem(g_padList, &it);
            ListView_SetItemText(g_padList, r, 1, host.data());
        }
    }

    HWND MakeList(HWND page, int x, int y, int w, int h, std::vector<std::pair<const wchar_t*, int>> cols)
    {
        HWND lv = Make(page, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, x, y, w, h, NewId(nullptr));
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        for (size_t i = 0; i < cols.size(); i++)
        {
            LVCOLUMNW c{};
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = (LPWSTR)cols[i].first;
            c.cx = S(cols[i].second);
            ListView_InsertColumn(lv, int(i), &c);
        }
        return lv;
    }

    // ---- Pages -----------------------------------------------------------------------------------
    HWND NewPage()
    {
        HWND p = CreateWindowExW(WS_EX_CONTROLPARENT, L"Peggle2LauncherPage", L"", WS_CHILD, S(kPageX), S(kPageY), S(kPageW), S(kPageH),
                                 g_wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        g_pages.push_back(p);
        return p;
    }

    void BuildPlay()
    {
        HWND p = NewPage();
        Settings& s = GetSettings();
        Label(p, 0, 0, 600, L"Peggle 2 (recompiled)", true);
        std::wstring status = g_info.gameFound
            ? L"Game files: " + g_info.gameDir.wstring()
            : L"Game files not found. Install them from your own Xbox Live Arcade package (Install from package), "
              L"or place the extracted files in a folder named \"game\" next to the program.";
        Note(p, 0, 28, 660, 40, status.c_str());
        if (g_info.installPackage)
            Button(p, 0, 74, 200, L"Install from package...", [] {
                if (g_info.installPackage(g_wnd)) { g_info.gameFound = true; MessageBoxW(g_wnd, L"Installed. Press Play to start.", L"Peggle 2", MB_ICONINFORMATION); }
            });
        Combo<bool>(p, 0, 120, 120, 200, L"Game mode:", &s.trial, { { L"Full game", false }, { L"Trial", true } });
        Note(p, 0, 160, 660, 120,
             L"Keys while playing: F11 or Alt+Enter fullscreen, F2 frame counter, F3 performance overlay, F12 screenshot.\n\n"
             L"Keyboard: arrows or WASD aim, Space fires, Enter pauses, Esc goes back (rebind on the Controls page).");
    }

    void BuildDisplay()
    {
        HWND p = NewPage();
        Settings& s = GetSettings();
        Check(p, 0, 0, 600, L"Fullscreen (F11 or Alt+Enter switches while playing)", &s.fullscreen);
        std::vector<std::pair<std::wstring, int>> monitors;
        int n = 0;
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* list = (std::vector<std::pair<std::wstring, int>>*)lp;
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            GetMonitorInfoW(m, &mi);
            wchar_t text[128];
            swprintf(text, 128, L"%s%ls  (%ldx%ld)", (mi.dwFlags & MONITORINFOF_PRIMARY) ? L"Primary: " : L"", mi.szDevice,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
            list->push_back({ text, int(list->size()) });
            return TRUE;
        }, (LPARAM)&monitors);
        (void)n;
        if (monitors.empty()) monitors.push_back({ L"Primary", 0 });
        Combo<int>(p, 0, 36, 160, 320, L"Monitor:", &s.monitor, monitors);
        Combo<int>(p, 0, 72, 160, 320, L"Window size:", &s.windowWidth,
                   { { L"Fit to the screen", 0 }, { L"1280 x 720", 1280 }, { L"1600 x 900", 1600 }, { L"1920 x 1080", 1920 }, { L"2560 x 1440", 2560 } });
        Combo<bool>(p, 0, 108, 160, 320, L"Picture:", &s.stretch, { { L"Keep 16:9 (black bars)", false }, { L"Stretch to fill", true } });
        Check(p, 0, 148, 600, L"VSync (off can tear but lowers latency)", &s.vsync);
    }

    void BuildGraphics()
    {
        HWND p = NewPage();
        Settings& s = GetSettings();
        Combo<int>(p, 0, 0, 160, 320, L"Render resolution:", &s.renderScale,
                   { { L"Window resolution (sharpest at no extra cost)", 1 }, { L"At least 2x (1440p, supersampled)", 2 },
                     { L"At least 3x (2160p, supersampled)", 3 }, { L"At least 4x (2880p, supersampled)", 4 } });
        Check(p, 0, 40, 600, L"FXAA anti-aliasing (smooths jagged edges after rendering)", &s.fxaa);
        Note(p, 0, 66, 660, 34, L"For the cleanest edges combine FXAA with a supersampled render resolution. (MSAA is not "
                                L"offered: Peggle 2 draws sprites, which MSAA does not smooth.)");
        Combo<int>(p, 0, 108, 160, 320, L"Texture filtering:", &s.anisotropy,
                   { { L"Bilinear (as on the console)", 1 }, { L"Anisotropic 2x", 2 }, { L"Anisotropic 4x", 4 }, { L"Anisotropic 8x", 8 }, { L"Anisotropic 16x", 16 } });
        Note(p, 0, 156, 660, 60, L"Higher render resolutions and anti-aliasing cost GPU time; on integrated graphics 1x keeps 30 fps steady.");
    }

    void BuildGameplay()
    {
        HWND p = NewPage();
        Settings& s = GetSettings();
        Check(p, 0, 0, 640, L"60 fps (draws every logic update; game speed unchanged)", &s.fps60);
        Check(p, 0, 30, 640, L"Frame counter (F2 toggles while playing)", &s.fpsCounter);
        Check(p, 0, 60, 640, L"Performance overlay (F3 toggles while playing)", &s.perfOverlay);
        Combo<std::string>(p, 0, 100, 160, 320, L"Language:", &s.language,
                           { { L"System default", "auto" }, { L"English", "en" }, { L"French", "fr" }, { L"German", "de" },
                             { L"Spanish", "es" }, { L"Italian", "it" }, { L"Japanese", "ja" }, { L"Korean", "ko" },
                             { L"Chinese (Traditional)", "zh" }, { L"Portuguese", "pt" }, { L"Polish", "pl" }, { L"Russian", "ru" } });
        Note(p, 0, 140, 660, 40, L"The language is passed to the game the way the console's system setting is; languages the game "
                                 L"does not include fall back to English.");
    }

    void BuildControls()
    {
        HWND p = NewPage();
        Settings& s = GetSettings();
        Label(p, 0, 0, 300, L"Keyboard", true);
        g_keyList = MakeList(p, 0, 22, 330, 300, { { L"Control", 130 }, { L"Key 1", 85 }, { L"Key 2", 85 } });
        FillKeyList();
        g_keyHint = Note(p, 0, 330, 330, 20, L"Select a row, then Set key 1 / Set key 2 and press a key.");
        Button(p, 0, 354, 105, L"Set key 1", [] { g_captureRow = ListView_GetNextItem(g_keyList, -1, LVNI_SELECTED); g_captureSlot = 0;
            if (g_captureRow >= 0) SetWindowTextW(g_keyHint, L"Press a key (Esc cancels, Delete clears)..."); });
        Button(p, 112, 354, 105, L"Set key 2", [] { g_captureRow = ListView_GetNextItem(g_keyList, -1, LVNI_SELECTED); g_captureSlot = 1;
            if (g_captureRow >= 0) SetWindowTextW(g_keyHint, L"Press a key (Esc cancels, Delete clears)..."); });
        Button(p, 224, 354, 105, L"Default keys", [] { Settings d; memcpy(GetSettings().keys, d.keys, sizeof(d.keys)); FillKeyList(); });

        Label(p, 350, 0, 300, L"Controller", true);
        g_padList = MakeList(p, 350, 22, 320, 200, { { L"Game button", 140 }, { L"Your button", 150 } });
        FillPadList();
        Label(p, 350, 230, 120, L"Driven by:");
        g_padCombo = Make(p, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 440, 227, 150, 300, NewId([](int code) {
            if (code != CBN_SELCHANGE) return;
            int row = ListView_GetNextItem(g_padList, -1, LVNI_SELECTED);
            int sel = int(SendMessageW(g_padCombo, CB_GETCURSEL, 0, 0));
            if (row < 0 || sel < 0) return;
            GetSettings().buttonMap[kPadButtons[row]] = sel == 0 ? 0 : uint16_t(1u << kPadButtons[sel - 1]);
            FillPadList();
            ListView_SetItemState(g_padList, row, LVIS_SELECTED, LVIS_SELECTED);
        }));
        SendMessageW(g_padCombo, CB_ADDSTRING, 0, (LPARAM)L"(none)");
        for (int b : kPadButtons) SendMessageW(g_padCombo, CB_ADDSTRING, 0, (LPARAM)W(kButtonNames[b]).c_str());
        Button(p, 598, 226, 72, L"Default", [] { for (int i = 0; i < 16; i++) GetSettings().buttonMap[i] = uint16_t(1u << i); FillPadList(); });
        Combo<int>(p, 350, 264, 120, 200, L"Stick deadzone:", &s.deadzone,
                   { { L"None (game decides)", 0 }, { L"5%", 5 }, { L"10%", 10 }, { L"15%", 15 }, { L"20%", 20 }, { L"25%", 25 }, { L"30%", 30 } });
        Check(p, 350, 296, 160, L"Invert aim X", &s.invertLX);
        Check(p, 510, 296, 160, L"Invert aim Y", &s.invertLY);
        Check(p, 350, 320, 160, L"Vibration", &s.vibration);
        Combo<int>(p, 510, 318, 70, 90, L"Strength:", &s.vibrationStrength,
                   { { L"25%", 25 }, { L"50%", 50 }, { L"75%", 75 }, { L"100%", 100 } });
    }

    // PNG bytes -> 32-bit top-down HBITMAP of size x size (WIC).
    HBITMAP DecodeIcon(const std::vector<uint8_t>& png, int size)
    {
        IWICImagingFactory* f = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return nullptr;
        HBITMAP result = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapDecoder* dec = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICBitmapScaler* scaler = nullptr;
        IWICFormatConverter* conv = nullptr;
        if (SUCCEEDED(f->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromMemory((BYTE*)png.data(), DWORD(png.size()))) &&
            SUCCEEDED(f->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &frame)) &&
            SUCCEEDED(f->CreateBitmapScaler(&scaler)) && SUCCEEDED(scaler->Initialize(frame, UINT(size), UINT(size), WICBitmapInterpolationModeFant)) &&
            SUCCEEDED(f->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(scaler, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        {
            BITMAPINFO bi{};
            bi.bmiHeader = { sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB };
            void* bits = nullptr;
            HDC dc = GetDC(nullptr);
            result = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            ReleaseDC(nullptr, dc);
            if (result && FAILED(conv->CopyPixels(nullptr, UINT(size) * 4, UINT(size) * UINT(size) * 4, (BYTE*)bits)))
            {
                DeleteObject(result);
                result = nullptr;
            }
        }
        for (IUnknown* u : { (IUnknown*)conv, (IUnknown*)scaler, (IUnknown*)frame, (IUnknown*)dec, (IUnknown*)stream, (IUnknown*)f })
            if (u) u->Release();
        return result;
    }

    void BuildAchievements()
    {
        HWND p = NewPage();
        Settings& s = GetSettings();
        if (g_info.gameFound && Achievements().empty()) LoadAchievements(g_info.gameDir, XboxLanguageId());
        const auto& list = Achievements();
        uint32_t total = 0, got = 0, score = 0, unlocked = 0;
        for (auto& a : list) { total += a.gamerscore; if (a.unlockedAt) { unlocked++; score += a.gamerscore; } }
        wchar_t head[160];
        swprintf(head, 160, L"%u of %zu unlocked  -  %u of %u G", unlocked, list.size(), score, total);
        (void)got;
        Label(p, 0, 0, 660, list.empty() ? L"Achievements appear here once the game files are installed." : head, true);
        HWND lv = MakeList(p, 0, 24, 680, 300, { { L"Achievement", 210 }, { L"Description", 330 }, { L"G", 40 }, { L"Unlocked", 80 } });
        int icon = S(32);
        HIMAGELIST il = ImageList_Create(icon, icon, ILC_COLOR32, int(list.size()), 0);
        ListView_SetImageList(lv, il, LVSIL_SMALL);
        for (size_t i = 0; i < list.size(); i++)
        {
            const AchievementDef& a = list[i];
            int img = -1;
            if (const auto* png = AchievementImage(a.imageId))
                if (HBITMAP bmp = DecodeIcon(*png, icon)) { img = ImageList_Add(il, bmp, nullptr); DeleteObject(bmp); }
            bool hide = a.secret && !a.unlockedAt;
            std::wstring name = hide ? L"Secret achievement" : W(a.name);
            std::wstring desc = hide ? L"Keep playing to unlock it." : W(a.unlockedAt || a.lockedDescription.empty() ? a.description : a.lockedDescription);
            std::wstring g = std::to_wstring(a.gamerscore);
            wchar_t when[32] = L"-";
            if (a.unlockedAt)
            {
                time_t t = time_t(a.unlockedAt);
                tm lt{};
                localtime_s(&lt, &t);
                wcsftime(when, 32, L"%Y-%m-%d", &lt);
            }
            LVITEMW it{};
            it.mask = LVIF_TEXT | LVIF_IMAGE;
            it.iItem = int(i);
            it.pszText = name.data();
            it.iImage = img;
            ListView_InsertItem(lv, &it);
            ListView_SetItemText(lv, int(i), 1, desc.data());
            ListView_SetItemText(lv, int(i), 2, g.data());
            ListView_SetItemText(lv, int(i), 3, when);
        }
        Check(p, 0, 336, 330, L"Pop-up when an achievement unlocks", &s.achievementPopups);
        Combo<int>(p, 340, 334, 120, 120, L"Pop-up volume:", &s.popupVolume,
                   { { L"Silent", 0 }, { L"25%", 25 }, { L"50%", 50 }, { L"80%", 80 }, { L"100%", 100 } });
        static HWND soundLabel = nullptr;
        soundLabel = Note(p, 0, 372, 420, 20, s.popupSound.empty() ? L"Sound: built-in chime" : (L"Sound: " + W(s.popupSound)).c_str());
        Button(p, 430, 368, 120, L"Choose .wav...", [] {
            wchar_t file[MAX_PATH] = L"";
            OPENFILENAMEW ofn{ sizeof(ofn) };
            ofn.hwndOwner = g_wnd;
            ofn.lpstrFilter = L"WAV sound (*.wav)\0*.wav\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (!GetOpenFileNameW(&ofn)) return;
            int n = WideCharToMultiByte(CP_UTF8, 0, file, -1, nullptr, 0, nullptr, nullptr);
            std::string u(size_t(n - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, file, -1, u.data(), n, nullptr, nullptr);
            GetSettings().popupSound = u;
            SetWindowTextW(soundLabel, (std::wstring(L"Sound: ") + file).c_str());
        });
        Button(p, 560, 368, 120, L"Built-in chime", [] {
            GetSettings().popupSound.clear();
            SetWindowTextW(soundLabel, L"Sound: built-in chime");
        });
    }

    void BuildAbout()
    {
        HWND p = NewPage();
        Settings& s = GetSettings();
        Label(p, 0, 0, 600, L"Peggle 2 (recompiled): a native Windows port by static recompilation.", true);
        Note(p, 440, 74, 240, 20, (L"Version " + W(PortVersion())).c_str());
        Note(p, 0, 24, 660, 40, L"You need your own copy of the Xbox Live Arcade game. Saves and settings stay on this PC.");
        Combo<std::string>(p, 0, 74, 160, 200, L"Updates:", &s.updateMode,
                           { { L"Ask before updating", "ask" }, { L"Update automatically", "auto" }, { L"Off", "off" } });
        if (g_info.checkForUpdates) Button(p, 300, 107, 140, L"Check now", [] { ApplyAll(); g_info.checkForUpdates(g_wnd, true); });
        Button(p, 0, 150, 160, L"Open save folder", [] { OpenFolder(PortDir() / "userdata"); });
        Button(p, 170, 150, 160, L"Open game folder", [] { OpenFolder(g_info.gameFound ? g_info.gameDir : PortDir()); });
        Button(p, 340, 150, 160, L"Open logs folder", [] { OpenFolder(PortDir()); });
        Button(p, 510, 150, 160, L"Open crash reports", [] { OpenFolder(PortDir() / "crashes"); });
        Button(p, 0, 186, 160, L"Open settings file", [] { ApplyAll(); SavePortSettings(); ShellExecuteW(nullptr, L"open", L"notepad.exe", SettingsPath().wstring().c_str(), nullptr, SW_SHOWNORMAL); });
        Button(p, 170, 186, 160, L"Reset all settings", [] {
            if (MessageBoxW(g_wnd, L"Reset every setting to its default?", L"Peggle 2", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
            ResetPortSettings();
            RefreshAll();
            FillKeyList();
            FillPadList();
        });
    }

    void ShowPage(int i)
    {
        for (size_t k = 0; k < g_pages.size(); k++) ShowWindow(g_pages[k], int(k) == i ? SW_SHOW : SW_HIDE);
    }

    LRESULT CALLBACK PageProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_COMMAND:
        {
            int id = LOWORD(wp);
            if (id >= kIdFirst && id - kIdFirst < int(g_commands.size()) && g_commands[id - kIdFirst]) g_commands[id - kIdFirst](HIWORD(wp));
            return 0;
        }
        case WM_NOTIFY:
        {
            auto* nm = (NMHDR*)lp;
            if (nm->hwndFrom == g_padList && nm->code == LVN_ITEMCHANGED)
            {
                int row = ListView_GetNextItem(g_padList, -1, LVNI_SELECTED);
                if (row >= 0)
                {
                    uint16_t m = GetSettings().buttonMap[kPadButtons[row]];
                    int sel = 0;
                    for (int r = 0; r < int(std::size(kPadButtons)); r++) if (m == (1u << kPadButtons[r])) sel = r + 1;
                    SendMessageW(g_padCombo, CB_SETCURSEL, sel, 0);
                }
            }
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_COMMAND:
            if (LOWORD(wp) == kIdPlay) { ApplyAll(); g_play = true; DestroyWindow(h); return 0; }
            if (LOWORD(wp) == kIdQuit) { ApplyAll(); g_play = false; DestroyWindow(h); return 0; }
            break;
        case WM_NOTIFY:
            if (((NMHDR*)lp)->hwndFrom == g_tabs && ((NMHDR*)lp)->code == TCN_SELCHANGE) ShowPage(TabCtrl_GetCurSel(g_tabs));
            return 0;
        case WM_CTLCOLORSTATIC:
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        case WM_CLOSE:
            ApplyAll();
            g_play = false;
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    // A key press while a binding is being captured.
    bool CaptureKey(const MSG& m)
    {
        if (g_captureRow < 0 || (m.message != WM_KEYDOWN && m.message != WM_SYSKEYDOWN)) return false;
        int vk = int(m.wParam);
        if (vk == VK_ESCAPE) vk = -1;          // cancel
        else if (vk == VK_DELETE) vk = 0;      // clear
        if (vk == VK_LSHIFT || vk == VK_RSHIFT) vk = VK_SHIFT;
        if (vk == VK_LCONTROL || vk == VK_RCONTROL) vk = VK_CONTROL;
        if (vk >= 0) GetSettings().keys[g_captureRow][g_captureSlot] = vk;
        int row = g_captureRow;
        g_captureRow = -1;
        SetWindowTextW(g_keyHint, L"Select a row, then Set key 1 / Set key 2 and press a key.");
        FillKeyList();
        ListView_SetItemState(g_keyList, row, LVIS_SELECTED, LVIS_SELECTED);
        return true;
    }
}

bool LauncherWanted(bool gameFound)
{
    // The game is always started from the launcher (one executable: launcher, then game).
    // Only automated runs (soak/benchmark scripts, autopress) skip it, and only when the
    // game files are there.
    if (gameFound && (getenv("PEGGLE2_NO_LAUNCHER") || getenv("PEGGLE2_AUTOPRESS")) && !getenv("PEGGLE2_LAUNCHER")) return false;
    return true;
}

bool RunLauncher(const LauncherInfo& info)
{
    g_info = info;
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    HINSTANCE inst = GetModuleHandleW(nullptr);

    // Per-monitor DPI for this window only (the game window keeps the process default).
    auto prev = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_dpi = int(GetDpiForSystem());

    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, UINT(g_dpi));
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bold = ncm.lfMessageFont;
    bold.lfWeight = FW_SEMIBOLD;
    g_bold = CreateFontIndirectW(&bold);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"Peggle2Launcher";
    RegisterClassW(&wc);
    WNDCLASSW pc = wc;
    pc.lpfnWndProc = PageProc;
    pc.lpszClassName = L"Peggle2LauncherPage";
    RegisterClassW(&pc);

    RECT r{ 0, 0, S(kW), S(kH) };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, UINT(g_dpi));
    g_wnd = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"Peggle 2 (recompiled) - Launcher", style,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);

    g_tabs = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP, S(10), S(8), S(kW - 20), S(kH - 70),
                             g_wnd, (HMENU)kIdTabs, inst, nullptr);
    SendMessageW(g_tabs, WM_SETFONT, (WPARAM)g_font, TRUE);
    std::vector<const wchar_t*> names = { L"Play", L"Display", L"Graphics", L"Gameplay", L"Controls", L"Achievements", L"About" };
    for (size_t i = 0; i < names.size(); i++)
    {
        TCITEMW t{};
        t.mask = TCIF_TEXT;
        t.pszText = (LPWSTR)names[i];
        TabCtrl_InsertItem(g_tabs, int(i), &t);
    }
    BuildPlay();
    BuildDisplay();
    BuildGraphics();
    BuildGameplay();
    BuildControls();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    BuildAchievements();
    BuildAbout();
    for (HWND pg : g_pages) SetWindowPos(pg, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    RefreshAll();
    ShowPage(0);

    HWND play = CreateWindowExW(0, L"BUTTON", L"Play", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                S(kW - 230), S(kH - 50), S(105), S(32), g_wnd, (HMENU)kIdPlay, inst, nullptr);
    HWND quit = CreateWindowExW(0, L"BUTTON", L"Quit", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                S(kW - 115), S(kH - 50), S(105), S(32), g_wnd, (HMENU)kIdQuit, inst, nullptr);
    SendMessageW(play, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(quit, WM_SETFONT, (WPARAM)g_font, TRUE);
    EnableWindow(play, TRUE);

    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
    SetFocus(play);
    if (g_info.checkForUpdates && !getenv("PEGGLE2_LAUNCHER_SHOT")) g_info.checkForUpdates(g_wnd, false);
    // PEGGLE2_LAUNCHER_SHOT=<prefix>: save a PNG of every page and close (for checking the layout).
    if (const char* shot = getenv("PEGGLE2_LAUNCHER_SHOT"))
    {
        RECT cr;
        GetClientRect(g_wnd, &cr);
        int w = cr.right, hgt = cr.bottom;
        HDC wdc = GetDC(g_wnd), mdc = CreateCompatibleDC(wdc);
        BITMAPINFO bi{};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -hgt, 1, 32, BI_RGB };
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mdc, bmp);
        for (size_t i = 0; i < g_pages.size(); i++)
        {
            TabCtrl_SetCurSel(g_tabs, int(i));
            ShowPage(int(i));
            RedrawWindow(g_wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            PrintWindow(g_wnd, mdc, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
            GdiFlush();
            char path[512];
            snprintf(path, sizeof(path), "%s%zu.png", shot, i);
            SavePngFile(path, (const uint32_t*)bits, unsigned(w), unsigned(hgt), unsigned(w));
        }
        SelectObject(mdc, old);
        DeleteObject(bmp);
        DeleteDC(mdc);
        ReleaseDC(g_wnd, wdc);
        g_play = false;
        DestroyWindow(g_wnd);
    }
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        if (CaptureKey(m)) continue;
        if (!IsDialogMessageW(g_wnd, &m))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    SetThreadDpiAwarenessContext(prev);
    DeleteObject(g_font);
    DeleteObject(g_bold);
    g_pages.clear();
    g_bindings.clear();
    g_commands.clear();
    SavePortSettings();
    if (g_play && !g_info.gameFound)
    {
        MessageBoxW(nullptr, L"The game files are missing: install them from your package first.", L"Peggle 2", MB_ICONWARNING);
        return false;
    }
    return g_play;
}
