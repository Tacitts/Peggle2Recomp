// Launcher: a settings window shown before the game starts (settings.h). One executable: it opens
// every time, and Play starts the game in the same process. Returns whether to start the game.
//
// Look: dark window with the game's title screen across the top (captured from the player's own
// copy by a hidden run of the game: PEGGLE2_CAPTURE_TITLE, saved to userdata\launcher_title.png;
// until then the package's banner logo), a page sidebar with a large Play button, and owner-drawn
// buttons and checkboxes so the whole window can be dark. Win32 + common controls v6 + WIC only.
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
#include <dwmapi.h>
#include <uxtheme.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

const char* PortVersion();   // updater.cpp

namespace
{
    // Layout (96-dpi units): header art across the top, page sidebar on the left, pages on the right.
    constexpr int kW = 980, kH = 730, kHeaderH = 250, kSideW = 190;
    constexpr int kPageX = kSideW + 24, kPageY = kHeaderH + 20, kPageW = kW - kPageX - 20, kPageH = kH - kPageY - 16;
    enum : int { kIdPlay = 1, kIdQuit = 2, kIdNav = 10, kIdFirst = 100 };

    // Colours: dark greys, with the orange and blue of the game's logo as accents.
    const COLORREF kBg = RGB(24, 24, 30), kSide = RGB(17, 17, 22), kPanel = RGB(24, 24, 30), kText = RGB(236, 236, 242),
                   kMuted = RGB(150, 150, 165), kAccent = RGB(242, 107, 29), kAccentHot = RGB(255, 136, 64),
                   kBlue = RGB(44, 143, 214), kControl = RGB(44, 44, 54), kControlHot = RGB(60, 60, 74), kEdge = RGB(70, 70, 86);
    HBRUSH g_bgBrush = nullptr, g_sideBrush = nullptr, g_controlBrush = nullptr;

    HWND g_wnd = nullptr;
    HFONT g_font = nullptr, g_bold = nullptr, g_big = nullptr, g_nav = nullptr;
    std::vector<HWND> g_nav_buttons;
    int g_page = 0;
    HWND g_hot = nullptr;   // owner-drawn control under the mouse
    std::vector<HWND> g_pages;
    int g_dpi = 96;
    bool g_play = false;
    LauncherInfo g_info;
    void StartTitleCapture(bool force);   // below
    std::vector<HWND> g_notes;            // secondary text (muted colour)

    int S(int v) { return MulDiv(v, g_dpi, 96); }   // scale 96-dpi layout units

    // Controls bound to settings: refresh() puts the setting into the control, apply() reads it back.
    struct Binding { HWND h; std::function<void()> refresh, apply; };
    std::vector<Binding> g_bindings;
    std::vector<HWND> g_checks;   // owner-drawn checkboxes
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
        HWND n = Make(page, L"STATIC", text, SS_LEFT, x, y, w, h);
        g_notes.push_back(n);
        return n;
    }

    // Owner-drawn checkbox: its state lives in the window's GWLP_USERDATA (1 = checked).
    void Check(HWND page, int x, int y, int w, const wchar_t* text, bool* value)
    {
        int id = NewId(nullptr);
        HWND h = Make(page, L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, x, y, w, 24, id);
        g_commands[size_t(id - kIdFirst)] = [h](int code) {
            if (code != BN_CLICKED) return;
            SetWindowLongPtrW(h, GWLP_USERDATA, !GetWindowLongPtrW(h, GWLP_USERDATA));
            InvalidateRect(h, nullptr, TRUE);
        };
        g_checks.push_back(h);
        g_bindings.push_back({ h, [h, value] { SetWindowLongPtrW(h, GWLP_USERDATA, *value); InvalidateRect(h, nullptr, TRUE); },
                               [h, value] { *value = GetWindowLongPtrW(h, GWLP_USERDATA) != 0; } });
    }

    template <class T>
    void Combo(HWND page, int x, int y, int labelW, int comboW, const wchar_t* label, T* value,
               std::vector<std::pair<std::wstring, T>> options)
    {
        Label(page, x, y + 3, labelW, label);
        HWND h = Make(page, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x + labelW, y, comboW, 300, NewId(nullptr));
        SetWindowTheme(h, L"DarkMode_CFD", nullptr);
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
        return Make(page, L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, x, y, w, 28, NewId([fn](int code) { if (code == BN_CLICKED) fn(); }));
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
        HWND lv = Make(page, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, x, y, w, h, NewId(nullptr));
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        SetWindowTheme(lv, L"DarkMode_Explorer", nullptr);
        SetWindowTheme(ListView_GetHeader(lv), L"DarkMode_ItemsView", nullptr);
        ListView_SetBkColor(lv, kControl);
        ListView_SetTextBkColor(lv, kControl);
        ListView_SetTextColor(lv, kText);
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
        SetWindowTheme(g_padCombo, L"DarkMode_CFD", nullptr);
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
        Note(p, 0, 24, 700, 20, L"You need your own copy of the Xbox Live Arcade game. Saves and settings stay on this PC.");
        Note(p, 0, 46, 700, 20, (L"Version " + W(PortVersion())).c_str());
        Combo<std::string>(p, 0, 92, 100, 220, L"Updates:", &s.updateMode,
                           { { L"Ask before updating", "ask" }, { L"Update automatically", "auto" }, { L"Off", "off" } });
        if (g_info.checkForUpdates) Button(p, 340, 90, 140, L"Check now", [] { ApplyAll(); g_info.checkForUpdates(g_wnd, true); });
        Button(p, 0, 150, 160, L"Open save folder", [] { OpenFolder(PortDir() / "userdata"); });
        Button(p, 170, 150, 160, L"Open game folder", [] { OpenFolder(g_info.gameFound ? g_info.gameDir : PortDir()); });
        Button(p, 340, 150, 160, L"Open logs folder", [] { OpenFolder(PortDir()); });
        Button(p, 510, 150, 160, L"Open crash reports", [] { OpenFolder(PortDir() / "crashes"); });
        Button(p, 0, 186, 160, L"Open settings file", [] { ApplyAll(); SavePortSettings(); ShellExecuteW(nullptr, L"open", L"notepad.exe", SettingsPath().wstring().c_str(), nullptr, SW_SHOWNORMAL); });
        Button(p, 340, 186, 160, L"Recapture title art", [] { StartTitleCapture(true); });
        Button(p, 170, 186, 160, L"Reset all settings", [] {
            if (MessageBoxW(g_wnd, L"Reset every setting to its default?", L"Peggle 2", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
            ResetPortSettings();
            RefreshAll();
            FillKeyList();
            FillPadList();
        });
    }

    // ---- Art from the player's game ----------------------------------------------------------
    HBITMAP g_art = nullptr;          // 32-bit top-down DIB
    int g_artW = 0, g_artH = 0;
    bool g_artIsBanner = false;       // the package's banner logo (no title capture yet)
    HANDLE g_capture = nullptr;       // hidden capture run in progress
    constexpr UINT_PTR kTimerCapture = 1, kTimerHover = 2;

    std::filesystem::path TitleArtPath() { return PortDir() / "userdata" / "launcher_title.png"; }

    HBITMAP LoadImageFile(const std::filesystem::path& path, int& w, int& h)
    {
        IWICImagingFactory* f = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return nullptr;
        HBITMAP result = nullptr;
        IWICBitmapDecoder* dec = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* conv = nullptr;
        UINT uw = 0, uh = 0;
        if (SUCCEEDED(f->CreateDecoderFromFilename(path.wstring().c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&uw, &uh)) && SUCCEEDED(f->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        {
            BITMAPINFO bi{};
            bi.bmiHeader = { sizeof(BITMAPINFOHEADER), LONG(uw), -LONG(uh), 1, 32, BI_RGB };
            void* bits = nullptr;
            result = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (result && FAILED(conv->CopyPixels(nullptr, uw * 4, uw * uh * 4, (BYTE*)bits))) { DeleteObject(result); result = nullptr; }
            w = int(uw);
            h = int(uh);
        }
        for (IUnknown* u : { (IUnknown*)conv, (IUnknown*)frame, (IUnknown*)dec, (IUnknown*)f })
            if (u) u->Release();
        return result;
    }

    void LoadArt()
    {
        if (g_art) DeleteObject(g_art);
        g_art = nullptr;
        g_artIsBanner = false;
        std::error_code ec;
        if (std::filesystem::exists(TitleArtPath(), ec)) g_art = LoadImageFile(TitleArtPath(), g_artW, g_artH);
        if (!g_art && g_info.gameFound)
        {
            g_art = LoadImageFile(g_info.gameDir / "BannerLogo.png", g_artW, g_artH);
            g_artIsBanner = g_art != nullptr;
        }
    }

    // Run the game hidden and muted to its title screen and save that frame (render.cpp).
    void StartTitleCapture(bool force)
    {
        if (!g_info.gameFound || g_capture) return;
        std::error_code ec;
        if (!force && std::filesystem::exists(TitleArtPath(), ec)) return;
        auto work = PortDir() / "userdata" / "capture";   // its own log and shader cache
        std::filesystem::create_directories(work, ec);
        if (force) std::filesystem::remove(TitleArtPath(), ec);
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(self) + L"\" \"" + g_info.gameDir.wstring() + L"\"";
        SetEnvironmentVariableW(L"PEGGLE2_CAPTURE_TITLE", TitleArtPath().wstring().c_str());
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, work.wstring().c_str(), &si, &pi))
        {
            CloseHandle(pi.hThread);
            g_capture = pi.hProcess;
            SetTimer(g_wnd, kTimerCapture, 500, nullptr);
            Log("[launcher] capturing the title screen for the launcher art");
        }
        SetEnvironmentVariableW(L"PEGGLE2_CAPTURE_TITLE", nullptr);
        InvalidateRect(g_wnd, nullptr, FALSE);
    }

    // ---- Painting --------------------------------------------------------------------------------
    void FillSolid(HDC dc, RECT r, COLORREF c)
    {
        SetBkColor(dc, c);
        ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &r, nullptr, 0, nullptr);
    }

    void RoundBox(HDC dc, RECT r, COLORREF fill, COLORREF edge, int radius)
    {
        HBRUSH b = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, edge);
        HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(b);
        DeleteObject(pen);
    }

    // Vertical fade from transparent to `c` over r (per-pixel alpha, premultiplied).
    void Fade(HDC dc, RECT r, COLORREF c, bool down)
    {
        int w = r.right - r.left, h = r.bottom - r.top;
        if (w <= 0 || h <= 0) return;
        BITMAPINFO bi{};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), 1, -h, 1, 32, BI_RGB };
        uint32_t* px = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
        if (!bmp) return;
        for (int y = 0; y < h; y++)
        {
            uint32_t a = uint32_t(255 * (down ? y : h - 1 - y) / std::max(1, h - 1));
            uint32_t rr = GetRValue(c) * a / 255, gg = GetGValue(c) * a / 255, bb = GetBValue(c) * a / 255;
            px[y] = a << 24 | rr << 16 | gg << 8 | bb;
        }
        HDC mdc = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mdc, bmp);
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(dc, r.left, r.top, w, h, mdc, 0, 0, 1, h, bf);
        SelectObject(mdc, old);
        DeleteDC(mdc);
        DeleteObject(bmp);
    }

    void PaintHeader(HDC dc, RECT hr)
    {
        int w = hr.right - hr.left, h = hr.bottom - hr.top;
        FillSolid(dc, hr, kBg);
        if (g_art)
        {
            HDC mdc = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mdc, g_art);
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            if (!g_artIsBanner)
            {
                // Title screen: fill the width, show the band around the logo.
                double scale = double(w) / g_artW;
                int srcH = std::min(g_artH, int(h / scale));
                int srcY = std::clamp(int(g_artH * 0.47) - srcH / 2, 0, g_artH - srcH);
                StretchBlt(dc, hr.left, hr.top, w, h, mdc, 0, srcY, g_artW, srcH, SRCCOPY);
            }
            else
            {
                // Banner logo on the window colour, centred, as large as the header allows.
                double scale = std::min(double(w) * 0.6 / g_artW, double(h) * 0.7 / g_artH);
                int dw = int(g_artW * scale), dh = int(g_artH * scale);
                BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
                AlphaBlend(dc, hr.left + (w - dw) / 2, hr.top + (h - dh) / 2, dw, dh, mdc, 0, 0, g_artW, g_artH, bf);
            }
            SelectObject(mdc, old);
            DeleteDC(mdc);
        }
        Fade(dc, RECT{ hr.left, hr.bottom - S(70), hr.right, hr.bottom }, kBg, true);
        if (g_capture || !g_info.gameFound)
        {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, kMuted);
            HGDIOBJ of = SelectObject(dc, g_font);
            const wchar_t* t = g_capture ? L"Preparing the title art from your game..." : L"Install the game to see its art here.";
            RECT tr{ hr.left, hr.bottom - S(28), hr.right - S(16), hr.bottom - S(6) };
            DrawTextW(dc, t, -1, &tr, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            SelectObject(dc, of);
        }
    }

    bool IsCheck(HWND h) { return std::find(g_checks.begin(), g_checks.end(), h) != g_checks.end(); }

    void DrawOwner(const DRAWITEMSTRUCT* d)
    {
        HDC dc = d->hDC;
        RECT r = d->rcItem;
        wchar_t text[160] = L"";
        GetWindowTextW(d->hwndItem, text, 160);
        bool hot = d->hwndItem == g_hot, pressed = (d->itemState & ODS_SELECTED) != 0, focus = (d->itemState & ODS_FOCUS) != 0;
        int id = int(d->CtlID);
        SetBkMode(dc, TRANSPARENT);
        if (IsCheck(d->hwndItem))
        {
            FillSolid(dc, r, kBg);
            bool on = GetWindowLongPtrW(d->hwndItem, GWLP_USERDATA) != 0;
            int box = S(18), top = r.top + (r.bottom - r.top - box) / 2;
            RECT b{ r.left + 1, top, r.left + 1 + box, top + box };
            RoundBox(dc, b, on ? kAccent : (hot ? kControlHot : kControl), on ? kAccent : kEdge, S(6));
            if (on)
            {
                HPEN pen = CreatePen(PS_SOLID, std::max(2, S(2)), RGB(255, 255, 255));
                HGDIOBJ op = SelectObject(dc, pen);
                MoveToEx(dc, b.left + box * 22 / 100, b.top + box * 52 / 100, nullptr);
                LineTo(dc, b.left + box * 42 / 100, b.top + box * 72 / 100);
                LineTo(dc, b.left + box * 78 / 100, b.top + box * 30 / 100);
                SelectObject(dc, op);
                DeleteObject(pen);
            }
            SetTextColor(dc, kText);
            HGDIOBJ of = SelectObject(dc, g_font);
            RECT tr{ b.right + S(10), r.top, r.right, r.bottom };
            DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            if (focus) { SetTextColor(dc, kAccent); }
            SelectObject(dc, of);
            return;
        }
        if (id >= kIdNav && id < kIdNav + 16)   // sidebar page button
        {
            bool sel = id - kIdNav == g_page;
            FillSolid(dc, r, sel ? kControl : (hot ? RGB(30, 30, 38) : kSide));
            if (sel) FillSolid(dc, RECT{ r.left, r.top + S(6), r.left + S(4), r.bottom - S(6) }, kAccent);
            SetTextColor(dc, sel ? kText : (hot ? kText : kMuted));
            HGDIOBJ of = SelectObject(dc, g_nav);
            RECT tr{ r.left + S(22), r.top, r.right, r.bottom };
            DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            SelectObject(dc, of);
            return;
        }
        HWND parent = GetParent(d->hwndItem);
        FillSolid(dc, r, parent == g_wnd ? kSide : kBg);
        RECT b = r;
        InflateRect(&b, -1, -1);
        if (id == kIdPlay)
        {
            RoundBox(dc, b, pressed ? kAccent : (hot ? kAccentHot : kAccent), pressed ? kAccent : kAccentHot, S(14));
            SetTextColor(dc, RGB(255, 255, 255));
            HGDIOBJ of = SelectObject(dc, g_big);
            DrawTextW(dc, L"PLAY", -1, &b, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            SelectObject(dc, of);
            return;
        }
        RoundBox(dc, b, pressed ? kControl : (hot ? kControlHot : kControl), focus ? kBlue : kEdge, S(10));
        SetTextColor(dc, id == kIdQuit ? kMuted : kText);
        HGDIOBJ of = SelectObject(dc, g_font);
        DrawTextW(dc, text, -1, &b, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, of);
    }

    // Track the owner-drawn control under the mouse for hover highlights.
    void UpdateHover()
    {
        POINT pt;
        GetCursorPos(&pt);
        HWND h = WindowFromPoint(pt);
        wchar_t cls[16] = L"";
        if (h) GetClassNameW(h, cls, 16);
        bool owner = h && _wcsicmp(cls, L"Button") == 0 && (GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW &&
                     (GetAncestor(h, GA_ROOT) == g_wnd);
        HWND now = owner ? h : nullptr;
        if (now == g_hot) return;
        HWND was = g_hot;
        g_hot = now;
        if (was) InvalidateRect(was, nullptr, FALSE);
        if (now) InvalidateRect(now, nullptr, FALSE);
    }

    void ShowPage(int i)
    {
        g_page = i;
        for (size_t k = 0; k < g_pages.size(); k++) ShowWindow(g_pages[k], int(k) == i ? SW_SHOW : SW_HIDE);
        for (HWND b : g_nav_buttons) InvalidateRect(b, nullptr, FALSE);
    }

    LRESULT ColorStatic(HWND ctl, HDC dc)
    {
        bool note = std::find(g_notes.begin(), g_notes.end(), ctl) != g_notes.end();
        SetTextColor(dc, note ? kMuted : kText);
        SetBkColor(dc, kBg);
        return (LRESULT)g_bgBrush;
    }

    LRESULT CALLBACK PageProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_COMMAND:
        {
            int id = LOWORD(wp);
            if (id >= kIdFirst && id - kIdFirst < int(g_commands.size()) && g_commands[size_t(id - kIdFirst)]) g_commands[size_t(id - kIdFirst)](HIWORD(wp));
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
        case WM_DRAWITEM:
            DrawOwner((const DRAWITEMSTRUCT*)lp);
            return TRUE;
        case WM_CTLCOLORSTATIC:
            return ColorStatic((HWND)lp, (HDC)wp);
        case WM_CTLCOLORBTN:
            return (LRESULT)g_bgBrush;
        case WM_CTLCOLORLISTBOX:
            SetTextColor((HDC)wp, kText);
            SetBkColor((HDC)wp, kControl);
            return (LRESULT)g_controlBrush;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_COMMAND:
        {
            int id = LOWORD(wp);
            if (id == kIdPlay) { ApplyAll(); g_play = true; DestroyWindow(h); return 0; }
            if (id == kIdQuit) { ApplyAll(); g_play = false; DestroyWindow(h); return 0; }
            if (id >= kIdNav && id < kIdNav + int(g_pages.size())) { ShowPage(id - kIdNav); return 0; }
            break;
        }
        case WM_DRAWITEM:
            DrawOwner((const DRAWITEMSTRUCT*)lp);
            return TRUE;
        case WM_CTLCOLORSTATIC:
            return ColorStatic((HWND)lp, (HDC)wp);
        case WM_CTLCOLORBTN:
            return (LRESULT)g_sideBrush;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT cr;
            GetClientRect(h, &cr);
            // Double-buffered: header art, sidebar, body.
            HDC mdc = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
            HGDIOBJ old = SelectObject(mdc, bmp);
            FillSolid(mdc, cr, kBg);
            PaintHeader(mdc, RECT{ 0, 0, cr.right, S(kHeaderH) });
            FillSolid(mdc, RECT{ 0, S(kHeaderH), S(kSideW), cr.bottom }, kSide);
            BitBlt(dc, 0, 0, cr.right, cr.bottom, mdc, 0, 0, SRCCOPY);
            SelectObject(mdc, old);
            DeleteObject(bmp);
            DeleteDC(mdc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wp == kTimerHover) UpdateHover();
            if (wp == kTimerCapture && g_capture && WaitForSingleObject(g_capture, 0) == WAIT_OBJECT_0)
            {
                CloseHandle(g_capture);
                g_capture = nullptr;
                KillTimer(h, kTimerCapture);
                LoadArt();
                InvalidateRect(h, nullptr, FALSE);
                Log("[launcher] title art %s", g_artIsBanner || !g_art ? "not captured" : "ready");
            }
            return 0;
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
    if (gameFound && (getenv("PEGGLE2_NO_LAUNCHER") || getenv("PEGGLE2_AUTOPRESS") || getenv("PEGGLE2_CAPTURE_TITLE")) &&
        !getenv("PEGGLE2_LAUNCHER"))
        return false;
    return true;
}

bool RunLauncher(const LauncherInfo& info)
{
    g_info = info;
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HINSTANCE inst = GetModuleHandleW(nullptr);

    // Per-monitor DPI for this window only (the game window keeps the process default).
    auto prev = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_dpi = int(GetDpiForSystem());

    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, UINT(g_dpi));
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -MulDiv(10, g_dpi, 72);
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    g_font = CreateFontIndirectW(&lf);
    LOGFONTW bold = lf;
    bold.lfWeight = FW_SEMIBOLD;
    bold.lfHeight = -MulDiv(11, g_dpi, 72);
    g_bold = CreateFontIndirectW(&bold);
    LOGFONTW nav = lf;
    nav.lfHeight = -MulDiv(12, g_dpi, 72);
    nav.lfWeight = FW_SEMIBOLD;
    g_nav = CreateFontIndirectW(&nav);
    LOGFONTW big = lf;
    big.lfHeight = -MulDiv(20, g_dpi, 72);
    big.lfWeight = FW_BOLD;
    g_big = CreateFontIndirectW(&big);
    g_bgBrush = CreateSolidBrush(kBg);
    g_sideBrush = CreateSolidBrush(kSide);
    g_controlBrush = CreateSolidBrush(kControl);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"Peggle2Launcher";
    RegisterClassW(&wc);
    WNDCLASSW pc = wc;
    pc.lpfnWndProc = PageProc;
    pc.hbrBackground = g_bgBrush;
    pc.lpszClassName = L"Peggle2LauncherPage";
    RegisterClassW(&pc);

    RECT r{ 0, 0, S(kW), S(kH) };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, UINT(g_dpi));
    g_wnd = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"Peggle 2", style,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    LoadArt();

    // Sidebar: pages, then Play and Quit at the bottom.
    std::vector<const wchar_t*> names = { L"Play", L"Display", L"Graphics", L"Gameplay", L"Controls", L"Achievements", L"About" };
    for (size_t i = 0; i < names.size(); i++)
    {
        HWND b = CreateWindowExW(0, L"BUTTON", names[i], WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, S(kHeaderH + 14 + int(i) * 42), S(kSideW), S(40),
                                 g_wnd, (HMENU)(INT_PTR)(kIdNav + int(i)), inst, nullptr);
        g_nav_buttons.push_back(b);
    }
    HWND play = CreateWindowExW(0, L"BUTTON", L"Play", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                S(16), S(kH - 16 - 34 - 10 - 58), S(kSideW - 32), S(58), g_wnd, (HMENU)kIdPlay, inst, nullptr);
    HWND quit = CreateWindowExW(0, L"BUTTON", L"Quit", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                S(16), S(kH - 16 - 34), S(kSideW - 32), S(34), g_wnd, (HMENU)kIdQuit, inst, nullptr);

    BuildPlay();
    BuildDisplay();
    BuildGraphics();
    BuildGameplay();
    BuildControls();
    BuildAchievements();
    BuildAbout();
    RefreshAll();
    ShowPage(0);

    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
    SetFocus(play);
    (void)quit;
    SetTimer(g_wnd, kTimerHover, 50, nullptr);
    const char* shot = getenv("PEGGLE2_LAUNCHER_SHOT");
    if (!shot)
    {
        StartTitleCapture(false);   // first start after installing: fetch the art in the background
        if (g_info.checkForUpdates) g_info.checkForUpdates(g_wnd, false);
    }
    // PEGGLE2_LAUNCHER_SHOT=<prefix>: save a PNG of every page and close (for checking the layout).
    if (shot)
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
    if (g_capture)   // don't leave a capture run behind
    {
        TerminateProcess(g_capture, 1);
        CloseHandle(g_capture);
        g_capture = nullptr;
    }
    SetThreadDpiAwarenessContext(prev);
    for (HGDIOBJ o : { (HGDIOBJ)g_font, (HGDIOBJ)g_bold, (HGDIOBJ)g_nav, (HGDIOBJ)g_big, (HGDIOBJ)g_bgBrush, (HGDIOBJ)g_sideBrush,
                       (HGDIOBJ)g_controlBrush, (HGDIOBJ)g_art })
        if (o) DeleteObject(o);
    g_art = nullptr;
    g_pages.clear();
    g_bindings.clear();
    g_commands.clear();
    g_checks.clear();
    g_notes.clear();
    g_nav_buttons.clear();
    SavePortSettings();
    if (g_play && !g_info.gameFound)
    {
        MessageBoxW(nullptr, L"The game files are missing: install them from your package first.", L"Peggle 2", MB_ICONWARNING);
        return false;
    }
    return g_play;
}
