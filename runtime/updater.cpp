// Updater: compares this build's version (PEGGLE2_VERSION, from git at configure time) with the
// latest GitHub release of the port, and on request installs it: downloads the release's
// Windows zip, unpacks it with Windows' own tar.exe into a staging folder, moves the running
// executable aside (Windows allows renaming a running .exe), copies the new port files over the
// old ones and restarts. game\, userdata\, crashes\ and the settings file are never touched.
// [extras] update_mode: ask (default), auto, off.
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include "runtime.h"
#include "settings.h"

#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#pragma comment(lib, "winhttp.lib")

#ifndef PEGGLE2_VERSION
#define PEGGLE2_VERSION "dev"
#endif

namespace
{
    constexpr wchar_t kHost[] = L"api.github.com";
    constexpr wchar_t kLatest[] = L"/repos/Tacitts/Peggle2Recomp/releases/latest";

    // GET over HTTPS; follows redirects (release assets redirect to a CDN). Empty on failure.
    bool HttpGet(const std::wstring& host, const std::wstring& path, std::string& body, DWORD* status = nullptr)
    {
        body.clear();
        HINTERNET s = WinHttpOpen(L"Peggle2Recomp-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!s) return false;
        WinHttpSetTimeouts(s, 5000, 5000, 10000, 30000);
        bool ok = false;
        if (HINTERNET c = WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0))
        {
            if (HINTERNET r = WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE))
            {
                const wchar_t* headers = L"Accept: application/vnd.github+json\r\n";
                if (WinHttpSendRequest(r, headers, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr))
                {
                    DWORD code = 0, len = sizeof(code);
                    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
                    if (status) *status = code;
                    DWORD avail = 0;
                    while (WinHttpQueryDataAvailable(r, &avail) && avail)
                    {
                        size_t at = body.size();
                        body.resize(at + avail);
                        DWORD got = 0;
                        if (!WinHttpReadData(r, body.data() + at, avail, &got)) break;
                        body.resize(at + got);
                    }
                    ok = code == 200;
                }
                WinHttpCloseHandle(r);
            }
            WinHttpCloseHandle(c);
        }
        WinHttpCloseHandle(s);
        return ok;
    }

    // Minimal JSON lookups for the few release fields needed (the API's layout is stable).
    std::string JsonString(const std::string& json, const std::string& key, size_t from = 0)
    {
        size_t k = json.find("\"" + key + "\"", from);
        if (k == std::string::npos) return {};
        size_t q = json.find('"', json.find(':', k) + 1);
        if (q == std::string::npos) return {};
        std::string out;
        for (size_t i = q + 1; i < json.size() && json[i] != '"'; i++)
        {
            if (json[i] == '\\' && i + 1 < json.size()) i++;
            out += json[i];
        }
        return out;
    }

    struct Release { std::string tag, name, zipUrl; };

    bool LatestRelease(Release& rel, std::string& error)
    {
        std::string body;
        DWORD status = 0;
        if (!HttpGet(kHost, kLatest, body, &status))
        {
            error = status == 404 ? "No releases have been published yet." : "Could not reach GitHub (status " + std::to_string(status) + ").";
            return false;
        }
        rel.tag = JsonString(body, "tag_name");
        rel.name = JsonString(body, "name");
        // First .zip asset whose name mentions windows (or any .zip).
        for (size_t at = body.find("\"browser_download_url\""); at != std::string::npos; at = body.find("\"browser_download_url\"", at + 1))
        {
            std::string url = JsonString(body, "browser_download_url", at);
            if (url.size() > 4 && url.compare(url.size() - 4, 4, ".zip") == 0)
            {
                if (rel.zipUrl.empty() || url.find("indows") != std::string::npos) rel.zipUrl = url;
            }
        }
        if (rel.tag.empty()) { error = "The release information was not understood."; return false; }
        return true;
    }

    bool SplitUrl(const std::string& url, std::wstring& host, std::wstring& path)
    {
        const std::string pre = "https://";
        if (url.compare(0, pre.size(), pre) != 0) return false;
        size_t slash = url.find('/', pre.size());
        if (slash == std::string::npos) return false;
        host.assign(url.begin() + long(pre.size()), url.begin() + long(slash));
        path.assign(url.begin() + long(slash), url.end());
        return true;
    }

    bool Download(std::string url, const std::filesystem::path& to, std::string& error)
    {
        // WinHTTP follows the asset redirect itself.
        std::wstring host, path;
        if (!SplitUrl(url, host, path)) { error = "Unexpected download address."; return false; }
        std::string body;
        DWORD status = 0;
        if (!HttpGet(host, path, body, &status) || body.size() < 1024) { error = "Download failed (status " + std::to_string(status) + ")."; return false; }
        FILE* f = _wfopen(to.wstring().c_str(), L"wb");
        if (!f) { error = "Cannot write the download."; return false; }
        fwrite(body.data(), 1, body.size(), f);
        fclose(f);
        return true;
    }

    bool RunAndWait(std::wstring cmd)
    {
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return code == 0;
    }

    // Unpack, swap the port's files in, restart. Returns only on failure.
    bool Install(const Release& rel, std::string& error)
    {
        namespace fs = std::filesystem;
        fs::path dir = PortDir(), stage = dir / "update_staging", zip = dir / "update.zip";
        std::error_code ec;
        fs::remove_all(stage, ec);
        fs::create_directories(stage, ec);
        Log("[update] downloading %s", rel.zipUrl.c_str());
        if (!Download(rel.zipUrl, zip, error)) return false;
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + zip.wstring() + L"\" -C \"" + stage.wstring() + L"\"";
        if (!RunAndWait(cmd)) { error = "Could not unpack the update."; return false; }
        // The zip may hold the files directly or in one top folder.
        fs::path src = stage;
        if (!fs::exists(src / "Peggle2.exe"))
            for (auto& e : fs::directory_iterator(stage, ec))
                if (e.is_directory() && fs::exists(e.path() / "Peggle2.exe")) src = e.path();
        if (!fs::exists(src / "Peggle2.exe")) { error = "The update does not contain Peggle2.exe."; return false; }
        // The running executable can be renamed, not overwritten.
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        fs::path old = fs::path(self).replace_extension(".old.exe");
        fs::remove(old, ec);
        fs::rename(self, old, ec);
        if (ec) { error = "Could not move the running program aside."; return false; }
        for (auto& e : fs::recursive_directory_iterator(src, ec))
        {
            fs::path rel_ = fs::relative(e.path(), src, ec), top = *rel_.begin();
            if (top == "game" || top == "userdata" || top == "crashes" || rel_ == "peggle2_settings.ini") continue;
            if (e.is_directory()) { fs::create_directories(dir / rel_, ec); continue; }
            fs::copy_file(e.path(), dir / rel_, fs::copy_options::overwrite_existing, ec);
            if (ec) { fs::rename(old, self, ec); error = "Could not replace " + rel_.string() + "."; return false; }
        }
        fs::remove_all(stage, ec);
        fs::remove(zip, ec);
        GetSettings().lastUpdateCheck = rel.tag;
        SavePortSettings();
        Log("[update] installed %s: restarting", rel.tag.c_str());
        ShellExecuteW(nullptr, L"open", self, nullptr, dir.wstring().c_str(), SW_SHOWNORMAL);
        ExitProcess(0);
    }
}

const char* PortVersion() { return PEGGLE2_VERSION; }

// Check for a newer release. interactive: report "up to date"/errors in message boxes (Check
// now); otherwise only offer an update that exists (startup check, update_mode ask/auto).
void CheckForUpdates(HWND owner, bool interactive)
{
    Settings& s = GetSettings();
    if (!interactive && s.updateMode == "off") return;
    Release rel;
    std::string error;
    if (!LatestRelease(rel, error))
    {
        Log("[update] check: %s", error.c_str());
        if (interactive) MessageBoxA(owner, error.c_str(), "Peggle 2 - updates", MB_ICONINFORMATION);
        return;
    }
    std::string current = PEGGLE2_VERSION;
    bool newer = rel.tag != current && current != "dev";
    Log("[update] latest release %s, this build %s", rel.tag.c_str(), current.c_str());
    if (!newer)
    {
        if (interactive)
        {
            std::string msg = current == "dev" ? "This is a development build (" + current + "); the latest release is " + rel.tag + "."
                                               : "Peggle 2 recompiled is up to date (" + current + ").";
            MessageBoxA(owner, msg.c_str(), "Peggle 2 - updates", MB_ICONINFORMATION);
        }
        return;
    }
    if (rel.zipUrl.empty())
    {
        if (interactive) MessageBoxA(owner, ("Release " + rel.tag + " has no Windows download.").c_str(), "Peggle 2 - updates", MB_ICONINFORMATION);
        return;
    }
    if (!interactive && s.updateMode == "ask" && s.lastUpdateCheck == rel.tag) return;   // already declined this one
    bool go = s.updateMode == "auto" && !interactive;
    if (!go)
    {
        std::string msg = "Version " + rel.tag + " is available (you have " + current + ").\n\nUpdate now? Your game files, saves and settings are kept.";
        go = MessageBoxA(owner, msg.c_str(), "Peggle 2 - update available", MB_YESNO | MB_ICONQUESTION) == IDYES;
    }
    s.lastUpdateCheck = rel.tag;
    SavePortSettings();
    if (!go) return;
    if (!Install(rel, error))
    {
        Log("[update] failed: %s", error.c_str());
        MessageBoxA(owner, ("The update failed: " + error).c_str(), "Peggle 2 - updates", MB_ICONERROR);
    }
}
