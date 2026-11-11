// BunkerVR Setup: installs BunkerVR into the player's own copy of Amnesia: The Bunker, updates or uninstalls it, and
// gathers the logs that help find a fault. The mod's own files are installed whole. The game's own files that the
// mod changes are installed as patches (.bvrpatch, format in tools/BunkerVRPatch.cs) applied to the player's copy,
// and the game's originals are kept in BunkerVR\install\backup.
//
// One program, three jobs:
//   (no argument)    install, update or uninstall BunkerVR in the game's folder, found through Steam
//   /uninstall       the same window, opening on the uninstall question (Windows' Apps list starts it this way)
//   /logs            gathers everything that helps find a fault into one zip on the desktop
//   /game <folder>   uses that folder instead of the one Steam reports (for tests); /go install|uninstall starts
//                    the job at once (used by the copy started with Windows' permission, when the folder needs it)
//
// Installing: each payload entry (packed into this program, payload.h) is unpacked and checked (SHA-256). A patch is
// applied to the game's original and its result checked. A game file about to be replaced is first copied to
// BunkerVR\install\backup (the game's own version, kept once). The new file is written beside the old one, swapped
// in, then read back and checked again. If anything fails, everything this run changed is put back from
// BunkerVR\install\undo. BunkerVR\install\manifest.txt lists what was installed. Updating is installing over an
// existing install: files the new version doesn't have are put back or removed. Uninstalling puts back each game
// file the mod replaced (unless Steam has changed it since), removes the mod's own files, logs and folders, the Apps
// list entry and the desktop's "BunkerVR Logs" shortcut. BunkerVR's development folder (a game folder that is a git
// repository) is never changed.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <bcrypt.h>
#include <compressapi.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <dxgi.h>
#include <intrin.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <wincodec.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "payload.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "cabinet.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "windowscodecs.lib")

static const wchar_t* const kAppName = L"BunkerVR";
static const wchar_t* const kGameExe = L"AmnesiaTheBunker.exe";
static const wchar_t* const kGameExeNoSteam = L"AmnesiaTheBunker_NoSteam.exe";
static const wchar_t* const kSteamAppId = L"1944430";
static const wchar_t* const kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\BunkerVR";
static const wchar_t* const kPublisher = L"Mina Studio";

// ---------------------------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------------------------

static std::wstring Widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring Join(const std::wstring& a, const std::wstring& b)
{
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

static std::wstring ParentOf(const std::wstring& path)
{
    size_t at = path.find_last_of(L"\\/");
    return at == std::wstring::npos ? std::wstring() : path.substr(0, at);
}

static bool FileExists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool DirExists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring Trim(const std::wstring& s)
{
    size_t a = s.find_first_not_of(L" \t\r\n\"");
    size_t b = s.find_last_not_of(L" \t\r\n\"");
    return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
}

static std::wstring ErrorText(DWORD code)
{
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   (wchar_t*)&text, 0, nullptr);
    std::wstring out = text ? Trim(text) : L"";
    if (text) LocalFree(text);
    while (!out.empty() && (out.back() == L'.' || out.back() == L' ')) out.pop_back();
    return out.empty() ? L"Windows error " + std::to_wstring(code) : out;
}

// What went wrong with one file, short enough for the window. Windows' own wording goes in Setup's log.
static std::wstring FileProblem(const std::string& path, const wchar_t* what, DWORD code)
{
    std::wstring name = Widen(path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1));
    switch (code)
    {
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_USER_MAPPED_FILE:
        return name + L" is open in another program. Close it and try again.";
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return L"The drive is full. Free some space and try again.";
    case ERROR_ACCESS_DENIED:
        return L"Windows blocked changes to " + name + L". Your antivirus may be blocking it.";
    case ERROR_WRITE_PROTECT:
        return L"The game's drive can't be written to.";
    default:
        return name + L" " + what + L" (" + ErrorText(code) + L").";
    }
}

static bool ReadWhole(const std::wstring& path, std::vector<unsigned char>& data)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    bool good = GetFileSizeEx(file, &size) && size.QuadPart < 0x7fffffff;
    if (good)
    {
        data.resize((size_t)size.QuadPart);
        DWORD got = 0;
        good = size.QuadPart == 0 || (ReadFile(file, data.data(), (DWORD)size.QuadPart, &got, nullptr) && got == (DWORD)size.QuadPart);
    }
    CloseHandle(file);
    return good;
}

static bool Sha256(const unsigned char* data, size_t size, uint8_t out[32])
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool good = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
                BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
                BCryptHashData(hash, (PUCHAR)data, (ULONG)size, 0) == 0 && BCryptFinishHash(hash, out, 32, 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return good;
}

static std::string Hex(const uint8_t* bytes, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += digits[bytes[i] >> 4]; s += digits[bytes[i] & 15]; }
    return s;
}

static std::string FileSha256(const std::wstring& path)
{
    std::vector<unsigned char> data;
    uint8_t hash[32];
    if (!ReadWhole(path, data) || !Sha256(data.data(), data.size(), hash)) return std::string();
    return Hex(hash, 32);
}

static bool GameRunning()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry = { sizeof(entry) };
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &entry); ok && !found; ok = Process32NextW(snap, &entry))
        found = _wcsicmp(entry.szExeFile, kGameExe) == 0 || _wcsicmp(entry.szExeFile, kGameExeNoSteam) == 0;
    CloseHandle(snap);
    return found;
}

#ifdef BUNKERVR_COPY_LOCK
static bool ProcessRunning(const wchar_t* exe)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry = { sizeof(entry) };
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &entry); ok && !found; ok = Process32NextW(snap, &entry)) found = _wcsicmp(entry.szExeFile, exe) == 0;
    CloseHandle(snap);
    return found;
}

// Private test builds only: whose copy this is, from resource 102 ("name=" and "account=" lines): a name and the
// Steam accounts (SteamID64) the copy is for. The mod's DLL in the payload carries the same list, signed, and checks
// it in the game against Steam itself; Setup's own check only tells the player before anything is installed.
struct TesterInfo
{
    std::wstring name;
    std::vector<unsigned long long> accounts;
};

static TesterInfo LoadTesterInfo()
{
    TesterInfo info;
    HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(102), RT_RCDATA);
    HGLOBAL loaded = found ? LoadResource(nullptr, found) : nullptr;
    const char* bytes = loaded ? (const char*)LockResource(loaded) : nullptr;
    DWORD size = found ? SizeofResource(nullptr, found) : 0;
    if (!bytes) return info;
    std::string text(bytes, size), line;
    for (size_t at = 0; at <= text.size(); ++at)
    {
        if (at < text.size() && text[at] != '\n') { line += text[at]; continue; }
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.compare(0, 5, "name=") == 0) info.name = Widen(line.substr(5));
        else if (line.compare(0, 8, "account=") == 0) info.accounts.push_back(_strtoui64(line.c_str() + 8, nullptr, 10));
        line.clear();
    }
    return info;
}

// The Steam account signed in now, as a SteamID64, built from Steam's ActiveUser registry value (the account number).
// Read only while steam.exe runs, because the value stays after Steam is closed. 0: not known.
// BUNKERVR_SETUP_TEST_STEAM_ACCOUNT overrides it for Setup's own tests. That is harmless: Setup only informs the
// player, and the mod checks with Steam itself.
static unsigned long long SteamAccountNow()
{
    wchar_t test[32];
    if (GetEnvironmentVariableW(L"BUNKERVR_SETUP_TEST_STEAM_ACCOUNT", test, 32) > 0) return _wcstoui64(test, nullptr, 10);
    if (!ProcessRunning(L"steam.exe")) return 0;
    DWORD account = 0, size = sizeof(account);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"ActiveUser", RRF_RT_REG_DWORD, nullptr, &account, &size) != ERROR_SUCCESS ||
        account == 0)
        return 0;
    return 76561197960265728ull + account;
}

static bool TesterAllows(const TesterInfo& info, unsigned long long account)
{
    for (unsigned long long a : info.accounts)
        if (a == account) return true;
    return false;
}
#endif

static std::wstring KnownFolder(REFKNOWNFOLDERID id)
{
    wchar_t* path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path)) && path) out = path;
    if (path) CoTaskMemFree(path);
    return out;
}

static std::wstring OwnPath()
{
    wchar_t path[MAX_PATH * 2] = L"";
    GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    return path;
}

static bool IsElevated()
{
    HANDLE token = nullptr;
    TOKEN_ELEVATION elevation = {};
    DWORD size = 0;
    bool elevated = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
                    GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
    if (token) CloseHandle(token);
    return elevated;
}

// Can files be made in this folder (and its script folder) without Windows' permission?
static bool FolderWritable(const std::wstring& folder)
{
    for (const std::wstring& where : { folder, Join(folder, L"script") })
    {
        if (!DirExists(where)) continue;
        std::wstring probe = Join(where, L"BunkerVR_setup_probe.tmp");
        HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                                  nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        CloseHandle(file);
    }
    return true;
}

static std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* name, DWORD flags = 0)
{
    wchar_t value[2048] = L"";
    DWORD size = sizeof(value);
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | flags, nullptr, value, &size) != ERROR_SUCCESS) return std::wstring();
    return value;
}

// ---------------------------------------------------------------------------------------------------------------
// The payload (this program's own resource)
// ---------------------------------------------------------------------------------------------------------------

struct PayloadFile
{
    std::string path;     // '/' between folders
    uint8_t method = 0;
    uint64_t rawSize = 0;
    const unsigned char* data = nullptr;
    uint64_t packedSize = 0;
    std::string sha256;   // hex
};

struct Payload
{
    bool good = false;
    std::wstring version;
    std::string gameExeSha256; // hex, "" if not known
    uint64_t rawTotal = 0;
    std::vector<PayloadFile> files;
};

static Payload LoadPayload()
{
    Payload p;
    HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(kPayloadResource), RT_RCDATA);
    HGLOBAL loaded = found ? LoadResource(nullptr, found) : nullptr;
    const unsigned char* bytes = loaded ? (const unsigned char*)LockResource(loaded) : nullptr;
    DWORD size = found ? SizeofResource(nullptr, found) : 0;
    if (!bytes || size < sizeof(PayloadHeader)) return p;
    PayloadHeader header;
    memcpy(&header, bytes, sizeof(header));
    if (memcmp(header.magic, kPayloadMagic, 8) != 0) return p;
    header.modVersion[sizeof(header.modVersion) - 1] = 0;
    p.version = Widen(header.modVersion);
    bool known = false;
    for (uint8_t b : header.gameExeSha256) known = known || b != 0;
    if (known) p.gameExeSha256 = Hex(header.gameExeSha256, 32);
    p.rawTotal = header.rawTotal;
    size_t at = sizeof(header);
    for (uint32_t i = 0; i < header.fileCount; ++i)
    {
        if (at + sizeof(PayloadEntry) > size) return p;
        PayloadEntry entry;
        memcpy(&entry, bytes + at, sizeof(entry));
        at += sizeof(entry);
        if (at + entry.pathBytes + entry.packedSize > size) return p;
        PayloadFile f;
        f.path.assign((const char*)bytes + at, entry.pathBytes);
        at += entry.pathBytes;
        f.method = entry.method;
        f.rawSize = entry.rawSize;
        f.packedSize = entry.packedSize;
        f.data = bytes + at;
        f.sha256 = Hex(entry.sha256, 32);
        at += (size_t)entry.packedSize;
        if (f.path.empty() || f.path.find("..") != std::string::npos || f.path.find(':') != std::string::npos) return p;
        p.files.push_back(f);
    }
    p.good = !p.files.empty();
    return p;
}

static bool Unpack(const PayloadFile& f, std::vector<unsigned char>& out)
{
    out.resize((size_t)f.rawSize);
    if (f.method == kPayloadStored)
    {
        if (f.packedSize != f.rawSize) return false;
        if (f.rawSize) memcpy(out.data(), f.data, (size_t)f.rawSize);
    }
    else if (f.method == kPayloadLzms)
    {
        DECOMPRESSOR_HANDLE d = nullptr;
        if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &d)) return false;
        SIZE_T got = 0;
        bool good = Decompress(d, f.data, (SIZE_T)f.packedSize, out.data(), out.size(), &got) && got == f.rawSize;
        CloseDecompressor(d);
        if (!good) return false;
    }
    else return false;
    uint8_t hash[32];
    return Sha256(out.data(), out.size(), hash) && Hex(hash, 32) == f.sha256;
}

// ---------------------------------------------------------------------------------------------------------------
// Patches: the mod's changes to the game's own files, applied to the player's copy (format: tools/BunkerVRPatch.cs)
// ---------------------------------------------------------------------------------------------------------------

static const char kPatchSuffix[] = ".bvrpatch";
static const size_t kPatchSuffixLength = sizeof(kPatchSuffix) - 1;

static bool IsPatch(const std::string& path)
{
    return path.size() > kPatchSuffixLength && _stricmp(path.c_str() + path.size() - kPatchSuffixLength, kPatchSuffix) == 0;
}

// The game file a payload entry installs: a patch's target, or the entry's own path.
static std::string TargetPath(const PayloadFile& f)
{
    return IsPatch(f.path) ? f.path.substr(0, f.path.size() - kPatchSuffixLength) : f.path;
}

struct PatchInfo
{
    bool good = false;
    std::string file;         // the game file it applies to, '/' between folders
    std::string fromSha256;   // the game's own file
    uint64_t fromSize = 0;
    std::string toSha256;     // the mod's version of it
    uint64_t toSize = 0;
    size_t body = 0;          // where the operations start
};

// Reads one '\n'-ended line of a patch's text; false at the end of the data.
static bool PatchLine(const std::vector<unsigned char>& patch, size_t& at, std::string& line)
{
    if (at >= patch.size()) return false;
    const unsigned char* end = (const unsigned char*)memchr(patch.data() + at, '\n', patch.size() - at);
    if (!end) return false;
    size_t to = (size_t)(end - patch.data());
    line.assign((const char*)patch.data() + at, to - at);
    at = to + 1;
    return true;
}

static bool ParseHashAndSize(const std::string& text, std::string& hash, uint64_t& size)
{
    size_t space = text.find(' ');
    if (space != 64 || text.size() < 66 || text.size() > 66 + 19) return false;
    hash = text.substr(0, 64);
    for (char c : hash) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    size = 0;
    for (size_t i = 65; i < text.size(); ++i)
    {
        if (text[i] < '0' || text[i] > '9') return false;
        size = size * 10 + (uint64_t)(text[i] - '0');
    }
    return true;
}

static PatchInfo ReadPatchInfo(const std::vector<unsigned char>& patch)
{
    PatchInfo info;
    size_t at = 0;
    std::string magic, file, from, to;
    if (!PatchLine(patch, at, magic) || magic != "BunkerVR patch 1") return info;
    if (!PatchLine(patch, at, file) || !PatchLine(patch, at, from) || !PatchLine(patch, at, to)) return info;
    if (file.compare(0, 5, "file ") != 0 || from.compare(0, 5, "from ") != 0 || to.compare(0, 3, "to ") != 0) return info;
    info.file = file.substr(5);
    if (info.file.empty() || info.file.find("..") != std::string::npos || info.file.find(':') != std::string::npos || info.file[0] == '/')
        return info;
    if (!ParseHashAndSize(from.substr(5), info.fromSha256, info.fromSize) || !ParseHashAndSize(to.substr(3), info.toSha256, info.toSize)) return info;
    info.body = at;
    info.good = true;
    return info;
}

// Applies a patch to the game's file. False if the file is not the one the patch was made for, if the patch is
// damaged, or if the result is not exactly the mod's file.
static bool ApplyPatch(const std::vector<unsigned char>& original, const std::vector<unsigned char>& patch, std::vector<unsigned char>& out)
{
    PatchInfo info = ReadPatchInfo(patch);
    uint8_t hash[32];
    if (!info.good || original.size() != info.fromSize || !Sha256(original.data(), original.size(), hash) || Hex(hash, 32) != info.fromSha256)
        return false;

    // Where each of the original's lines starts; a line keeps its '\n', the last one may have none.
    std::vector<size_t> starts;
    for (size_t i = 0; i < original.size(); ++i)
        if (i == 0 || original[i - 1] == '\n') starts.push_back(i);
    const size_t lines = starts.size();
    auto lineEnd = [&](size_t k) { return k + 1 < lines ? starts[k + 1] : original.size(); };

    out.clear();
    out.reserve(original.size() + patch.size());
    size_t cursor = 0, at = info.body;
    size_t lastInsertEnd = (size_t)-1; // where the last inserted line ended in `out`, for a "\" line right after it
    std::string op;
    for (;;)
    {
        if (at >= patch.size()) return false;
        if (patch[at] == '+')
        {
            const unsigned char* end = (const unsigned char*)memchr(patch.data() + at, '\n', patch.size() - at);
            if (!end) return false;
            size_t to = (size_t)(end - patch.data());
            out.insert(out.end(), patch.begin() + at + 1, patch.begin() + to + 1);
            lastInsertEnd = out.size();
            at = to + 1;
            continue;
        }
        if (!PatchLine(patch, at, op)) return false;
        if (op == "end") break;
        if (op == "\\")
        {
            if (lastInsertEnd != out.size() || out.empty()) return false;
            out.pop_back(); // the '\n' that only ended the patch's own line
            lastInsertEnd = (size_t)-1;
            continue;
        }
        if (op.size() < 2 || op.size() > 10 || (op[0] != '=' && op[0] != '-')) return false;
        size_t n = 0;
        for (size_t i = 1; i < op.size(); ++i)
        {
            if (op[i] < '0' || op[i] > '9') return false;
            n = n * 10 + (size_t)(op[i] - '0');
        }
        if (n == 0 || cursor + n > lines) return false;
        if (op[0] == '=') out.insert(out.end(), original.begin() + starts[cursor], original.begin() + lineEnd(cursor + n - 1));
        cursor += n;
    }
    if (cursor < lines) out.insert(out.end(), original.begin() + starts[cursor], original.end());
    return out.size() == info.toSize && Sha256(out.data(), out.size(), hash) && Hex(hash, 32) == info.toSha256;
}

// ---------------------------------------------------------------------------------------------------------------
// Finding the game (Steam)
// ---------------------------------------------------------------------------------------------------------------

static bool IsGameFolder(const std::wstring& folder)
{
    return !folder.empty() && FileExists(Join(folder, kGameExe)) && DirExists(Join(folder, L"script"));
}

// Every "path" in Steam's libraryfolders.vdf (its own library first).
static std::vector<std::wstring> SteamLibraries()
{
    std::vector<std::wstring> libraries;
    std::wstring steam = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty()) steam = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (steam.empty()) steam = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
    for (wchar_t& c : steam) if (c == L'/') c = L'\\';
    if (steam.empty()) return libraries;
    libraries.push_back(steam);
    std::vector<unsigned char> vdf;
    if (!ReadWhole(Join(steam, L"steamapps\\libraryfolders.vdf"), vdf)) return libraries;
    std::string text(vdf.begin(), vdf.end());
    size_t at = 0;
    while ((at = text.find("\"path\"", at)) != std::string::npos)
    {
        size_t a = text.find('"', at + 6);
        if (a == std::string::npos) break;
        size_t b = a + 1;
        std::string value;
        while (b < text.size() && text[b] != '"')
        {
            if (text[b] == '\\' && b + 1 < text.size()) { value += text[b + 1]; b += 2; }
            else value += text[b++];
        }
        std::wstring library = Widen(value);
        for (wchar_t& c : library) if (c == L'/') c = L'\\';
        bool have = false;
        for (const std::wstring& l : libraries) have = have || _wcsicmp(l.c_str(), library.c_str()) == 0;
        if (!have && !library.empty()) libraries.push_back(library);
        at = b + 1;
    }
    return libraries;
}

static std::wstring FindGame()
{
    for (const std::wstring& library : SteamLibraries())
    {
        std::vector<unsigned char> acf;
        std::wstring installDir = L"Amnesia The Bunker";
        if (ReadWhole(Join(library, std::wstring(L"steamapps\\appmanifest_") + kSteamAppId + L".acf"), acf))
        {
            std::string text(acf.begin(), acf.end());
            size_t at = text.find("\"installdir\"");
            if (at != std::string::npos)
            {
                size_t a = text.find('"', at + 12), b = a == std::string::npos ? a : text.find('"', a + 1);
                if (a != std::string::npos && b != std::string::npos) installDir = Widen(text.substr(a + 1, b - a - 1));
            }
        }
        std::wstring folder = Join(Join(library, L"steamapps\\common"), installDir);
        if (IsGameFolder(folder)) return folder;
    }
    std::wstring uninstall = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App ") + kSteamAppId;
    std::wstring location = RegString(HKEY_LOCAL_MACHINE, uninstall.c_str(), L"InstallLocation", RRF_SUBKEY_WOW6464KEY);
    if (IsGameFolder(location)) return location;
    location = RegString(HKEY_LOCAL_MACHINE, uninstall.c_str(), L"InstallLocation", RRF_SUBKEY_WOW6432KEY);
    if (IsGameFolder(location)) return location;
    return std::wstring();
}

// ---------------------------------------------------------------------------------------------------------------
// What is installed (BunkerVR\install\manifest.txt)
// ---------------------------------------------------------------------------------------------------------------

struct ManifestFile
{
    bool replaced = false; // a game file was there (kept in backup\); otherwise added by the mod
    std::string sha256;    // as installed
    std::string path;      // '/' between folders
};

struct Manifest
{
    bool present = false;
    std::wstring version;
    std::vector<ManifestFile> files;
    std::vector<std::string> dirs; // folders the mod made (deepest last)

    const ManifestFile* Find(const std::string& path) const
    {
        for (const ManifestFile& f : files) if (_stricmp(f.path.c_str(), path.c_str()) == 0) return &f;
        return nullptr;
    }
};

static std::wstring InstallDir(const std::wstring& game) { return Join(game, L"BunkerVR\\install"); }
static std::wstring ManifestPath(const std::wstring& game) { return Join(InstallDir(game), L"manifest.txt"); }
static std::wstring BackupPath(const std::wstring& game, const std::string& path)
{
    std::wstring rel = Widen(path);
    for (wchar_t& c : rel) if (c == L'/') c = L'\\';
    return Join(Join(InstallDir(game), L"backup"), rel);
}
static std::wstring GamePath(const std::wstring& game, const std::string& path)
{
    std::wstring rel = Widen(path);
    for (wchar_t& c : rel) if (c == L'/') c = L'\\';
    return Join(game, rel);
}

static Manifest ReadManifest(const std::wstring& game)
{
    Manifest m;
    std::vector<unsigned char> data;
    if (!ReadWhole(ManifestPath(game), data)) return m;
    std::string text(data.begin(), data.end());
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        start = end + 1;
        if (line.rfind("version=", 0) == 0) m.version = Widen(line.substr(8));
        else if (line.rfind("dir=", 0) == 0) m.dirs.push_back(line.substr(4));
        else if (line.rfind("file=", 0) == 0)
        {
            size_t a = line.find('|', 5), b = a == std::string::npos ? a : line.find('|', a + 1);
            if (b == std::string::npos) continue;
            ManifestFile f;
            f.replaced = line.compare(5, a - 5, "replaced") == 0;
            f.sha256 = line.substr(a + 1, b - a - 1);
            f.path = line.substr(b + 1);
            if (!f.path.empty() && f.path.find("..") == std::string::npos) m.files.push_back(f);
        }
    }
    m.present = !m.files.empty();
    return m;
}

static bool WriteTextAtomic(const std::wstring& path, const std::string& text)
{
    std::wstring temp = path + L".new";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool good = WriteFile(file, text.data(), (DWORD)text.size(), &written, nullptr) && written == text.size() && FlushFileBuffers(file);
    CloseHandle(file);
    return good && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

static bool WriteManifest(const std::wstring& game, const Manifest& m)
{
    std::string text = "BunkerVR install manifest (BunkerVR Setup; do not edit)\r\nversion=" + Narrow(m.version) + "\r\n";
    for (const std::string& d : m.dirs) text += "dir=" + d + "\r\n";
    for (const ManifestFile& f : m.files) text += "file=" + std::string(f.replaced ? "replaced" : "added") + "|" + f.sha256 + "|" + f.path + "\r\n";
    return WriteTextAtomic(ManifestPath(game), text);
}

// Makes the folders up to `folder`, noting each one made (relative to the game's folder, '/').
static bool MakeFolders(const std::wstring& game, const std::wstring& folder, std::vector<std::string>* made)
{
    if (DirExists(folder)) return true;
    std::wstring parent = ParentOf(folder);
    if (!parent.empty() && !DirExists(parent) && !MakeFolders(game, parent, made)) return false;
    if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    if (made && folder.size() > game.size() + 1 && _wcsnicmp(folder.c_str(), game.c_str(), game.size()) == 0)
    {
        std::string rel = Narrow(folder.substr(game.size() + 1));
        for (char& c : rel) if (c == '\\') c = '/';
        made->push_back(rel);
    }
    return true;
}

static bool WriteFileAtomic(const std::wstring& path, const std::vector<unsigned char>& data, DWORD& error)
{
    std::wstring temp = path + L".bvrnew";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    DWORD written = 0;
    bool good = (data.empty() || (WriteFile(file, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size())) &&
                FlushFileBuffers(file);
    if (!good) error = GetLastError();
    CloseHandle(file);
    if (!good) { DeleteFileW(temp.c_str()); return false; }
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        error = GetLastError();
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

static bool CopyInto(const std::wstring& from, const std::wstring& to, const std::wstring& game)
{
    if (!MakeFolders(game, ParentOf(to), nullptr)) return false;
    return CopyFileW(from.c_str(), to.c_str(), FALSE) != 0;
}

static void DeleteTree(const std::wstring& folder)
{
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW(Join(folder, L"*").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) continue;
            std::wstring path = Join(folder, found.cFileName);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(path);
            else
            {
                SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(path.c_str());
            }
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    RemoveDirectoryW(folder.c_str());
}

// ---------------------------------------------------------------------------------------------------------------
// Installing, updating, uninstalling (on a worker thread)
// ---------------------------------------------------------------------------------------------------------------

struct Progress
{
    std::atomic<int> done{ 0 };
    std::atomic<int> total{ 0 };
};

struct Outcome
{
    bool good = false;
    bool rolledBack = true; // (when not good: the game was left as it was)
    std::wstring message;
};

static std::wstring SetupCopyFolder() { return Join(KnownFolder(FOLDERID_LocalAppData), L"BunkerVR"); }
static std::wstring SetupCopyPath() { return Join(SetupCopyFolder(), L"BunkerVR Setup.exe"); }
static std::wstring LogsShortcutPath() { return Join(KnownFolder(FOLDERID_Desktop), L"BunkerVR Logs.lnk"); }

// Setup's own log: what it found and did, and Windows' own words for anything that failed. Kept beside the copy
// Windows' Apps list starts (%LOCALAPPDATA%\BunkerVR\setup_log.txt) and put in the logs zip. Lines wait in memory and
// are saved once that folder is there (installing makes it; uninstalling removes it with the log).
static std::mutex g_setupLogLock;
static std::string g_setupLog;
static std::wstring SetupLogPath() { return Join(SetupCopyFolder(), L"setup_log.txt"); }

static void SetupLog(const std::wstring& line)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::lock_guard<std::mutex> hold(g_setupLogLock);
    g_setupLog += stamp + Narrow(line) + "\r\n";
}

static void SetupLogSave(bool makeFolder)
{
    std::lock_guard<std::mutex> hold(g_setupLogLock);
    if (g_setupLog.empty()) return;
    if (!DirExists(SetupCopyFolder()) && (!makeFolder || !CreateDirectoryW(SetupCopyFolder().c_str(), nullptr))) return;
    std::wstring path = SetupLogPath();
    // Past 256 KB the older half of the file is dropped, cut at the start of a line.
    std::vector<unsigned char> old;
    if (ReadWhole(path, old) && old.size() > 256 * 1024)
    {
        size_t from = old.size() / 2;
        while (from < old.size() && old[from - 1] != '\n') ++from;
        std::vector<unsigned char> kept(old.begin() + from, old.end());
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            if (!kept.empty()) WriteFile(file, kept.data(), (DWORD)kept.size(), &written, nullptr);
            CloseHandle(file);
        }
    }
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, g_setupLog.data(), (DWORD)g_setupLog.size(), &written, nullptr);
    CloseHandle(file);
    g_setupLog.clear();
}

// The Apps list entry, the copy of this program it starts, and the desktop's "BunkerVR Logs".
static void Register(const std::wstring& game, const Payload& payload)
{
    std::wstring copy = SetupCopyPath();
    CreateDirectoryW(SetupCopyFolder().c_str(), nullptr);
    if (_wcsicmp(OwnPath().c_str(), copy.c_str()) != 0) CopyFileW(OwnPath().c_str(), copy.c_str(), FALSE);
    // Remove the download's mark (the Zone.Identifier stream, which CopyFile carries over): Windows' Apps list and the
    // desktop's "BunkerVR Logs" start this copy, and with the mark Windows would warn about it at every start.
    DeleteFileW((copy + L":Zone.Identifier").c_str());

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS)
    {
        auto setString = [&](const wchar_t* name, const std::wstring& value) {
            RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
        };
        auto setNumber = [&](const wchar_t* name, DWORD value) { RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value)); };
        setString(L"DisplayName", L"BunkerVR");
        setString(L"DisplayVersion", payload.version);
        setString(L"Publisher", kPublisher);
        setString(L"DisplayIcon", copy + L",0");
        setString(L"InstallLocation", game);
        setString(L"UninstallString", L"\"" + copy + L"\" /uninstall");
        setNumber(L"EstimatedSize", (DWORD)(payload.rawTotal / 1024));
        setNumber(L"NoModify", 1);
        setNumber(L"NoRepair", 1);
        RegCloseKey(key);
    }

    IShellLinkW* link = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
    {
        link->SetPath(copy.c_str());
        link->SetArguments(L"/logs");
        link->SetDescription(L"Collects BunkerVR's logs into one file for a bug report");
        link->SetIconLocation(copy.c_str(), 0);
        link->SetWorkingDirectory(SetupCopyFolder().c_str());
        IPersistFile* file = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))))
        {
            file->Save(LogsShortcutPath().c_str(), TRUE);
            file->Release();
        }
        link->Release();
    }
}

static void Unregister()
{
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    DeleteFileW(LogsShortcutPath().c_str());
    // The copy Windows' Apps list starts is removed now, unless it is this one: that one is removed after it closes
    // (RemoveOwnCopyLater).
    if (_wcsicmp(OwnPath().c_str(), SetupCopyPath().c_str()) != 0) DeleteTree(SetupCopyFolder());
}

static Outcome Install(const std::wstring& game, const Payload& payload, Progress& progress)
{
    Outcome out;
    if (DirExists(Join(game, L".git"))) { out.message = L"This is BunkerVR's development folder: Setup leaves it as it is."; return out; }
    if (GameRunning()) { out.message = L"Amnesia: The Bunker is running. Close it and try again."; return out; }
    Manifest old = ReadManifest(game);
    Manifest now;
    now.version = payload.version;
    now.dirs = old.dirs;

    std::wstring undo = Join(InstallDir(game), L"undo");
    DeleteTree(undo);
    std::vector<std::string> madeDirs;
    if (!MakeFolders(game, Join(InstallDir(game), L"backup"), &madeDirs) || !MakeFolders(game, undo, nullptr))
    {
        DWORD e = GetLastError();
        SetupLog(L"install: BunkerVR\\install could not be made: " + ErrorText(e));
        out.message = e == ERROR_ACCESS_DENIED ? L"Windows didn't allow the game's folder to be changed." : FileProblem("BunkerVR/install", L"could not be made", e);
        return out;
    }

    struct Change { std::string path; bool existed; bool backedUp; };
    std::vector<Change> changes;
    auto rollBack = [&]() {
        for (auto it = changes.rbegin(); it != changes.rend(); ++it)
        {
            std::wstring target = GamePath(game, it->path);
            std::wstring saved = Join(undo, Widen(it->path));
            for (wchar_t& c : saved) if (c == L'/') c = L'\\';
            if (it->existed) MoveFileExW(saved.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            else DeleteFileW(target.c_str());
            if (it->backedUp) DeleteFileW(BackupPath(game, it->path).c_str());
        }
        for (auto it = madeDirs.rbegin(); it != madeDirs.rend(); ++it)
        {
            std::wstring dir = GamePath(game, *it);
            if (_wcsnicmp(dir.c_str(), InstallDir(game).c_str(), InstallDir(game).size()) != 0) RemoveDirectoryW(dir.c_str());
        }
        DeleteTree(undo);
    };

    // Files the installed version has and this one doesn't (an update): put back or removed, as uninstalling does.
    std::vector<const ManifestFile*> gone;
    for (const ManifestFile& f : old.files)
    {
        bool still = false;
        for (const PayloadFile& p : payload.files) still = still || _stricmp(TargetPath(p).c_str(), f.path.c_str()) == 0;
        if (!still) gone.push_back(&f);
    }
    progress.total = (int)(payload.files.size() + gone.size());
    progress.done = 0;
    SetupLog(L"install: BunkerVR " + payload.version + (old.present ? L" over " + old.version : std::wstring()) + L" in " + game);
    int written = 0, same = 0, keptAside = 0, steamChanged = 0, putBack = 0, removed = 0;

    std::vector<unsigned char> data, patched, original;
    for (const PayloadFile& file : payload.files)
    {
        const std::string path = TargetPath(file);
        std::wstring target = GamePath(game, path);
        if (!Unpack(file, data))
        {
            rollBack();
            out.message = L"This installer is damaged. Please download it again.";
            return out;
        }
        const ManifestFile* before = old.Find(path);
        bool exists = FileExists(target);
        std::string currentHash = exists ? FileSha256(target) : std::string();
        std::string wanted = file.sha256; // the file as it should end up

        // A patch is applied to the game's own file: the one in place if it is untouched, or, when the one in place
        // is an earlier BunkerVR's, the game's own kept aside when that was installed. Any other version (the game
        // was updated, or something else changed the file) stops the install with nothing changed.
        if (IsPatch(file.path))
        {
            PatchInfo info = ReadPatchInfo(data);
            if (!info.good || _stricmp(info.file.c_str(), path.c_str()) != 0)
            {
                rollBack();
                out.message = L"This installer is damaged. Please download it again.";
                return out;
            }
            wanted = info.toSha256;
            if (!(exists && currentHash == wanted))
            {
                std::wstring backup = BackupPath(game, path);
                bool fromGame = exists && currentHash == info.fromSha256;
                bool fromBackup = !fromGame && before && (!exists || currentHash == before->sha256) && FileExists(backup) &&
                                  FileSha256(backup) == info.fromSha256;
                if (!fromGame && !fromBackup)
                {
                    rollBack();
                    SetupLog(L"install: the game's " + Widen(path) + L" is not the version BunkerVR " + payload.version + L" was made for (" +
                             Widen(exists ? currentHash : std::string("missing")) + L"); nothing changed");
                    out.message = L"Your game's " + Widen(path.substr(path.find_last_of('/') + 1)) +
                                  L" isn't the version this BunkerVR was made for. If the game was updated, BunkerVR needs an update too.";
                    return out;
                }
                if (!ReadWhole(fromGame ? target : backup, original) || !ApplyPatch(original, data, patched))
                {
                    rollBack();
                    SetupLog(L"install: the patch for " + Widen(path) + L" could not be applied; nothing changed");
                    out.message = L"BunkerVR's changes to " + Widen(path.substr(path.find_last_of('/') + 1)) +
                                  L" couldn't be applied. Verify the game's files in Steam, then try again.";
                    return out;
                }
                data.swap(patched);
            }
        }

        ManifestFile entry;
        entry.path = path;
        entry.sha256 = wanted;
        bool backedUp = false;
        // Whose is the file there now? Not installed before: the game's own (it is kept, then replaced), unless it
        // is already the mod's (copied by hand: the game's own cannot be known, so none is kept). Installed before
        // as a replacement but changed since (Steam updated the game): the game's new one, kept instead of the old.
        // Installed before as the mod's own: ours, whatever it holds now.
        bool gamesOwnHere = false;
        if (!before) { entry.replaced = exists; gamesOwnHere = exists && currentHash != wanted; }
        else { entry.replaced = before->replaced; gamesOwnHere = before->replaced && exists && currentHash != before->sha256; }
        if (gamesOwnHere)
        {
            if (!CopyInto(target, BackupPath(game, path), game))
            {
                DWORD e = GetLastError();
                rollBack();
                SetupLog(L"install: the game's own " + Widen(path) + L" could not be kept aside: " + ErrorText(e) + L"; put back as it was");
                out.message = FileProblem(path, L"could not be kept aside", e);
                return out;
            }
            backedUp = !before;
            ++keptAside;
            if (before)
            {
                ++steamChanged;
                SetupLog(L"install: the game's " + Widen(path) + L" changed since BunkerVR was installed (Steam): its new one kept aside");
            }
        }
        if (exists && currentHash == wanted)
        {
            now.files.push_back(entry); // (already as it should be)
            ++same;
            ++progress.done;
            continue;
        }
        // Kept for undoing this run, then written beside it and swapped in.
        Change change = { path, exists, backedUp };
        if (exists)
        {
            std::wstring saved = Join(undo, Widen(path));
            for (wchar_t& c : saved) if (c == L'/') c = L'\\';
            if (!CopyInto(target, saved, game))
            {
                DWORD e = GetLastError();
                rollBack();
                SetupLog(L"install: " + Widen(path) + L" could not be kept for undoing: " + ErrorText(e) + L"; put back as it was");
                out.message = FileProblem(path, L"could not be kept for undoing", e);
                return out;
            }
        }
        DWORD error = 0;
        if (!MakeFolders(game, ParentOf(target), &madeDirs) || !WriteFileAtomic(target, data, error))
        {
            if (!error) error = GetLastError();
            if (exists) changes.push_back(change); // (it may have been swapped already: put back)
            rollBack();
            SetupLog(L"install: " + Widen(path) + L" could not be written: " + ErrorText(error) + L"; put back as it was");
            out.message = FileProblem(path, L"could not be written", error);
            return out;
        }
        changes.push_back(change);
        if (FileSha256(target) != wanted)
        {
            rollBack();
            SetupLog(L"install: " + Widen(path) + L" did not read back as written; put back as it was");
            out.message = Widen(path.substr(path.find_last_of('/') + 1)) + L" did not read back as written. The drive may be failing or full.";
            return out;
        }
        now.files.push_back(entry);
        ++written;
        ++progress.done;
    }

    // Files of the previous version that this one doesn't have.
    for (const ManifestFile* f : gone)
    {
        std::wstring target = GamePath(game, f->path);
        if (f->replaced)
        {
            std::wstring backup = BackupPath(game, f->path);
            if (FileExists(backup) && (!FileExists(target) || FileSha256(target) == f->sha256) && CopyFileW(backup.c_str(), target.c_str(), FALSE)) ++putBack;
            DeleteFileW(backup.c_str());
        }
        else if (FileExists(target) && DeleteFileW(target.c_str())) ++removed;
        ++progress.done;
    }

    for (const std::string& d : madeDirs)
    {
        bool have = false;
        for (const std::string& e : now.dirs) have = have || _stricmp(e.c_str(), d.c_str()) == 0;
        if (!have) now.dirs.push_back(d);
    }
    if (!WriteManifest(game, now))
    {
        rollBack();
        out.message = L"BunkerVR's list of installed files could not be written.";
        return out;
    }
    DeleteTree(undo);
    SetupLog(L"install: done: " + std::to_wstring(written) + L" written, " + std::to_wstring(same) + L" already right, " + std::to_wstring(keptAside) +
             L" of the game's own kept aside (" + std::to_wstring(steamChanged) + L" changed by Steam), " + std::to_wstring(gone.size()) +
             L" no longer in the mod (" + std::to_wstring(putBack) + L" put back, " + std::to_wstring(removed) + L" removed)");
    Register(game, payload);
    out.good = true;
    return out;
}

static Outcome Uninstall(const std::wstring& game, Progress& progress)
{
    Outcome out;
    out.rolledBack = false;
    if (DirExists(Join(game, L".git")))
    {
        out.message = L"This is BunkerVR's development folder: Setup leaves it as it is.";
        out.rolledBack = true;
        return out;
    }
    if (GameRunning()) { out.message = L"Amnesia: The Bunker is running. Close it and try again."; out.rolledBack = true; return out; }
    Manifest m = ReadManifest(game);
    progress.total = (int)m.files.size() + 1;
    progress.done = 0;
    int kept = 0;
    for (const ManifestFile& f : m.files)
    {
        std::wstring target = GamePath(game, f.path);
        if (f.replaced)
        {
            std::wstring backup = BackupPath(game, f.path);
            bool ours = !FileExists(target) || FileSha256(target) == f.sha256;
            if (FileExists(backup) && ours)
            {
                std::vector<unsigned char> data;
                DWORD error = 0;
                if (!ReadWhole(backup, data) || !WriteFileAtomic(target, data, error)) ++kept;
            }
            else if (ours) ++kept; // (the game's own was not there to put back)
        }
        else if (FileExists(target))
        {
            SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(target.c_str())) ++kept;
        }
        ++progress.done;
    }
    // The mod's own files made while playing (logs, settings, hints, pictures), and its folder.
    const wchar_t* patterns[] = { L"BunkerVR.log", L"BunkerVR_before*.log", L"BunkerVR_watch.log", L"BunkerVR_settings.txt", L"BunkerVR_settings.txt.*",
                                  L"BunkerVR_hints.txt", L"BunkerVR_grips.txt", L"BunkerVR_shot*.bmp", L"BunkerVR_start_*.txt", L"version.dll.vr" };
    for (const wchar_t* pattern : patterns)
    {
        WIN32_FIND_DATAW found;
        HANDLE find = FindFirstFileW(Join(game, pattern).c_str(), &found);
        if (find == INVALID_HANDLE_VALUE) continue;
        do
        {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW(Join(game, found.cFileName).c_str());
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    DeleteTree(Join(game, L"BunkerVR"));
    for (auto it = m.dirs.rbegin(); it != m.dirs.rend(); ++it) RemoveDirectoryW(GamePath(game, *it).c_str()); // (only if empty)
    Unregister();
    ++progress.done;
    SetupLog(L"uninstall: BunkerVR " + m.version + L" from " + game + L": " + std::to_wstring(m.files.size()) + L" files, " + std::to_wstring(kept) +
             L" of the game's own not put back");
    out.good = kept == 0;
    if (!out.good)
        out.message = std::to_wstring(kept) + L" of the game's files couldn't be restored. In Steam, right-click Amnesia: The Bunker and choose Properties, Installed Files, "
                                              L"\"Verify integrity of game files\" to restore them.";
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Logs: everything that helps find a fault, in one zip on the desktop
// ---------------------------------------------------------------------------------------------------------------

static void CopyMatching(const std::wstring& folder, const wchar_t* pattern, const std::wstring& to, int newestDays, int most = 1000)
{
    struct Found { std::wstring name; FILETIME written; };
    std::vector<Found> list;
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW(Join(folder, pattern).c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return;
    FILETIME nowFile;
    GetSystemTimeAsFileTime(&nowFile);
    ULARGE_INTEGER now = { { nowFile.dwLowDateTime, nowFile.dwHighDateTime } };
    do
    {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULARGE_INTEGER t = { { found.ftLastWriteTime.dwLowDateTime, found.ftLastWriteTime.dwHighDateTime } };
        if (newestDays > 0 && now.QuadPart > t.QuadPart && (now.QuadPart - t.QuadPart) / 10000000ULL > (ULONGLONG)newestDays * 86400ULL) continue;
        list.push_back({ found.cFileName, found.ftLastWriteTime });
    } while (FindNextFileW(find, &found));
    FindClose(find);
    std::sort(list.begin(), list.end(), [](const Found& a, const Found& b) { return CompareFileTime(&a.written, &b.written) > 0; });
    if ((int)list.size() > most) list.resize(most);
    if (!list.empty()) CreateDirectoryW(to.c_str(), nullptr);
    for (const Found& f : list) CopyFileW(Join(folder, f.name).c_str(), Join(to, f.name).c_str(), FALSE);
}

// A picture the mod saved (.bmp, 8 MB) as a JPEG (a few hundred KB), by Windows' own imaging.
static bool BmpToJpeg(const std::wstring& from, const std::wstring& to)
{
    IWICImagingFactory* wic = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* out = nullptr;
    IPropertyBag2* options = nullptr;
    UINT w = 0, h = 0;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    PROPBAG2 quality = {};
    quality.pstrName = (LPOLESTR)L"ImageQuality";
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_R4;
    value.fltVal = 0.85f;
    bool good = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
                SUCCEEDED(wic->CreateDecoderFromFilename(from.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
                SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
                SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
                SUCCEEDED(converter->GetSize(&w, &h)) && SUCCEEDED(wic->CreateStream(&stream)) &&
                SUCCEEDED(stream->InitializeFromFilename(to.c_str(), GENERIC_WRITE)) &&
                SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) &&
                SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&out, &options)) &&
                SUCCEEDED(options->Write(1, &quality, &value)) && SUCCEEDED(out->Initialize(options)) && SUCCEEDED(out->SetSize(w, h)) &&
                SUCCEEDED(out->SetPixelFormat(&format)) && IsEqualGUID(format, GUID_WICPixelFormat24bppBGR) &&
                SUCCEEDED(out->WriteSource(converter, nullptr)) && SUCCEEDED(out->Commit()) && SUCCEEDED(encoder->Commit());
    if (options) options->Release();
    if (out) out->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (wic) wic->Release();
    if (!good) DeleteFileW(to.c_str());
    return good;
}

// The marks the player made (the mod's BunkerVR\recordings: "<date>_<time>_mark<n>_" then motion.bin and the
// pictures), the newest `most` of the last `days` days, the pictures as JPEGs (a mark's three are 25 MB as saved).
static int CopyMarks(const std::wstring& folder, const std::wstring& to, int days, int most)
{
    struct Found { std::wstring name, mark; };
    std::vector<Found> files;
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW(Join(folder, L"*_mark*_*").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return 0;
    FILETIME nowFile;
    GetSystemTimeAsFileTime(&nowFile);
    ULARGE_INTEGER now = { { nowFile.dwLowDateTime, nowFile.dwHighDateTime } };
    do
    {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULARGE_INTEGER t = { { found.ftLastWriteTime.dwLowDateTime, found.ftLastWriteTime.dwHighDateTime } };
        if (now.QuadPart > t.QuadPart && (now.QuadPart - t.QuadPart) / 10000000ULL > (ULONGLONG)days * 86400ULL) continue;
        std::wstring name = found.cFileName;
        size_t at = name.find(L"_mark");
        size_t end = at == std::wstring::npos ? std::wstring::npos : name.find(L'_', at + 1);
        if (end == std::wstring::npos) continue;
        files.push_back({ name, name.substr(0, end) });
    } while (FindNextFileW(find, &found));
    FindClose(find);
    // (The names start with the date and time: newest last when sorted.)
    std::vector<std::wstring> marks;
    for (const Found& f : files) if (std::find(marks.begin(), marks.end(), f.mark) == marks.end()) marks.push_back(f.mark);
    std::sort(marks.begin(), marks.end());
    if ((int)marks.size() > most) marks.erase(marks.begin(), marks.end() - most);
    if (marks.empty()) return 0;
    CreateDirectoryW(to.c_str(), nullptr);
    for (const Found& f : files)
    {
        if (std::find(marks.begin(), marks.end(), f.mark) == marks.end()) continue;
        std::wstring from = Join(folder, f.name);
        size_t dot = f.name.find_last_of(L'.');
        if (dot != std::wstring::npos && _wcsicmp(f.name.c_str() + dot, L".bmp") == 0 && BmpToJpeg(from, Join(to, f.name.substr(0, dot) + L".jpg"))) continue;
        CopyFileW(from.c_str(), Join(to, f.name).c_str(), FALSE);
    }
    return (int)marks.size();
}

// The system report's details on what paces a game's frames or draws over it. A game held below the headset's
// refresh rate (60 frames a second on a 90 Hz headset, say) leaves no trace in the mod's or the game's own logs, so
// the likely causes are reported below.

static DWORD RegDword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD missing)
{
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(root, key, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) return missing;
    return value;
}

static std::string LowerAscii(std::string s)
{
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

// The PC's screens, each one's size and refresh rate (a graphics driver's forced vertical sync holds a game to its
// screen's rate, the headset's own notwithstanding).
static void AddScreens(std::string& r)
{
    int shown = 0;
    for (DWORD i = 0; i < 32; ++i)
    {
        DISPLAY_DEVICEW device = {};
        device.cb = sizeof(device);
        if (!EnumDisplayDevicesW(nullptr, i, &device, 0)) break;
        if (!(device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        DEVMODEW mode = {};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW(device.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) continue;
        DISPLAY_DEVICEW monitor = {};
        monitor.cb = sizeof(monitor);
        std::wstring name = EnumDisplayDevicesW(device.DeviceName, 0, &monitor, 0) ? std::wstring(monitor.DeviceString) : std::wstring();
        r += "PC screen: " + std::to_string(mode.dmPelsWidth) + " x " + std::to_string(mode.dmPelsHeight) + " at " +
             std::to_string(mode.dmDisplayFrequency) + " Hz" + ((device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? ", the main one" : "") +
             " (" + Narrow(name.empty() ? std::wstring(L"unnamed") : name) + ", on " + Narrow(device.DeviceString) + ")\r\n";
        ++shown;
    }
    if (shown == 0) r += "PC screen: none found\r\n";
}

// Windows' own settings that change how frames are paced.
static void AddWindowsGraphics(std::string& r)
{
    DWORD hags = RegDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode", 0);
    r += std::string("Hardware-accelerated GPU scheduling: ") + (hags == 2 ? "on" : (hags == 1 ? "off" : "not set")) + "\r\n";
    DWORD gameMode = RegDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\GameBar", L"AutoGameModeEnabled", 1);
    r += std::string("Game Mode: ") + (gameMode ? "on" : "off") + "\r\n";
    std::wstring scheme = RegString(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes", L"ActivePowerScheme");
    std::string plan = Narrow(scheme);
    std::string lower = LowerAscii(plan);
    if (lower == "381b4222-f694-41f0-9685-ff5bb260df2e") plan = "Balanced";
    else if (lower == "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c") plan = "High performance";
    else if (lower == "a1841308-3541-4fab-bc81-f71556f20b4a") plan = "Power saver";
    else if (lower == "e9a42b02-d5df-448d-aa00-03f14749eb61") plan = "Ultimate performance";
    r += "Power plan: " + (plan.empty() ? std::string("not known") : plan) + "\r\n";
}

// OpenXR's implicit layers: programs (OpenXR Toolkit, overlays, other mods) that put themselves between every OpenXR
// game and the VR software. DWORD 0 means the layer is on.
static void AddOpenXrLayers(std::string& r)
{
    const HKEY roots[2] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    const char* const whose[2] = { "for the PC", "for this user" };
    int found = 0;
    for (int k = 0; k < 2; ++k)
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(roots[k], L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit", 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        for (DWORD i = 0; i < 256; ++i)
        {
            wchar_t name[1024];
            DWORD nameLength = 1024, type = 0;
            BYTE data[64] = {};
            DWORD size = sizeof(data);
            LONG result = RegEnumValueW(key, i, name, &nameLength, nullptr, &type, data, &size);
            if (result == ERROR_NO_MORE_ITEMS) break;
            if (result != ERROR_SUCCESS) continue;
            DWORD value = 1;
            if (type == REG_DWORD && size == sizeof(DWORD)) memcpy(&value, data, sizeof(DWORD));
            r += std::string("OpenXR layer (") + whose[k] + "): " + Narrow(name) + (value == 0 ? " (on)" : " (off)") + "\r\n";
            ++found;
        }
        RegCloseKey(key);
    }
    if (found == 0) r += "OpenXR layers: none\r\n";
}

// Programs running now that limit frame rates, draw over games or are VR software (only those: nothing else that
// runs on the player's PC is reported).
static void AddProgramsRunning(std::string& r)
{
    static const char* const kWatched[] = { "rtss", "afterburner", "obs64", "obs32", "discord", "overlay", "gamebar", "ovrserver",
                                            "oculus", "vrserver", "vrcompositor", "vrmonitor", "vrdashboard", "virtualdesktop", "alvr",
                                            "playstation", "psvr", "fpsvr", "reshade", "specialk", "lossless", "radeonsoftware",
                                            "nvidia app", "nvidia share", "openkneeboard", "ovr toolkit", "xsoverlay" };
    std::vector<std::string> seen;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32W entry = { sizeof(entry) };
        for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry))
        {
            std::string name = Narrow(entry.szExeFile);
            std::string lower = LowerAscii(name);
            bool watched = false;
            for (const char* w : kWatched) if (lower.find(w) != std::string::npos) watched = true;
            if (watched && std::find(seen.begin(), seen.end(), name) == seen.end()) seen.push_back(name);
        }
        CloseHandle(snap);
    }
    std::sort(seen.begin(), seen.end());
    std::string list;
    for (const std::string& s : seen) list += (list.empty() ? "" : ", ") + s;
    r += "Running now (frame limiters, overlays, VR software): " + (list.empty() ? std::string("none of those") : list) + "\r\n";
}

// Every DLL in the game's folder: the game's own, BunkerVR's version.dll, and anything else put there (a ReShade, an
// opengl32.dll or another injector changes how the game draws).
static void AddGameFolderDlls(std::string& r, const std::wstring& game)
{
    std::vector<std::string> names;
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW(Join(game, L"*.dll").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(Narrow(found.cFileName));
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    std::sort(names.begin(), names.end());
    std::string list;
    for (const std::string& s : names) list += (list.empty() ? "" : ", ") + s;
    r += "DLLs in the game's folder: " + (list.empty() ? std::string("none") : list) + "\r\n";
}

// Windows' own reports of programs that crashed or stopped answering (the Application log's "Application Error",
// "Application Hang" and "Windows Error Reporting"), the last 14 days, only those that name the game, BunkerVR or
// SteamVR: written to events.txt.
static void WriteCrashEvents(const std::wstring& to, const std::wstring& work)
{
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    std::wstring raw = Join(work, L"events_raw.txt");
    SECURITY_ATTRIBUTES inherit = { sizeof(inherit), nullptr, TRUE };
    HANDLE file = CreateFileW(raw.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    std::wstring command = L"\"" + Join(system, L"wevtutil.exe") +
                           L"\" qe Application /q:\"*[System[(Provider[@Name='Application Error'] or Provider[@Name='Application Hang'] or "
                           L"Provider[@Name='Windows Error Reporting']) and TimeCreated[timediff(@SystemTime) <= 1209600000]]]\" /f:text /rd:true /c:400";
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = file;
    si.hStdError = file;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> line(command.begin(), command.end());
    line.push_back(0);
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        if (WaitForSingleObject(pi.hProcess, 60 * 1000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(file);
    std::vector<unsigned char> data;
    bool read = ReadWhole(raw, data);
    DeleteFileW(raw.c_str());
    std::string text;
    if (read && data.size() >= 2 && ((data[0] == 0xFF && data[1] == 0xFE) || data[1] == 0))
    {
        size_t start = (data[0] == 0xFF && data[1] == 0xFE) ? 2 : 0;
        std::wstring wide((const wchar_t*)(data.data() + start), (data.size() - start) / sizeof(wchar_t));
        text = Narrow(wide);
    }
    else if (read)
    {
        text.assign(data.begin(), data.end());
    }
    static const char* const kNames[] = { "amnesiathebunker", "bunkervr", "vrserver", "vrcompositor", "vrmonitor", "vrdashboard",
                                          "vrstartup", "vrclient", "openxr" };
    std::string kept;
    int count = 0;
    size_t at = text.find("Event[");
    while (at != std::string::npos)
    {
        size_t next = text.find("Event[", at + 6);
        std::string block = text.substr(at, next == std::string::npos ? std::string::npos : next - at);
        std::string lower = LowerAscii(block);
        for (const char* n : kNames)
        {
            if (lower.find(n) != std::string::npos)
            {
                kept += block;
                ++count;
                break;
            }
        }
        at = next;
    }
    std::string out = "Windows' reports of crashes and hangs of the game, BunkerVR and SteamVR, the last 14 days: " +
                      std::to_string(count) + (read ? "" : " (Windows' event log could not be read)") + "\r\n\r\n" + kept;
    WriteTextAtomic(to, out);
}

static std::string SystemReport(const std::wstring& game, const Payload& payload)
{
    std::string r;
    auto line = [&](const std::string& s) { r += s + "\r\n"; };
    SYSTEMTIME t;
    GetLocalTime(&t);
    char when[64];
    sprintf_s(when, "%04d-%02d-%02d %02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    line("BunkerVR logs, gathered " + std::string(when) + " by BunkerVR Setup " + Narrow(payload.version));
    line("");

    // Windows.
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    OSVERSIONINFOW v = { sizeof(v) };
    RtlGetVersionFn getVersion = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (getVersion) getVersion(&v);
    line("Windows: " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + " build " + std::to_string(v.dwBuildNumber) +
         " (" + Narrow(RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion")) + ")");

    // Processor and memory.
    int info[4] = {};
    char brand[49] = "";
    __cpuid(info, 0x80000000);
    if ((unsigned)info[0] >= 0x80000004)
        for (int i = 0; i < 3; ++i) { __cpuid(info, 0x80000002 + i); memcpy(brand + i * 16, info, 16); }
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    MEMORYSTATUSEX mem = { sizeof(mem) };
    GlobalMemoryStatusEx(&mem);
    line("Processor: " + Narrow(Trim(Widen(brand))) + " (" + std::to_string(si.dwNumberOfProcessors) + " threads)");
    line("Memory: " + std::to_string((unsigned long long)(mem.ullTotalPhys / (1024ull * 1024 * 1024))) + " GB");

    // Graphics cards.
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(adapter->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            {
                // The driver's version. NVIDIA's own version number is its last five digits (32.0.15.6094 -> 560.94).
                std::string driver;
                LARGE_INTEGER umd = {};
                if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
                {
                    DWORD high = (DWORD)umd.HighPart, low = (DWORD)umd.LowPart;
                    driver = ", driver " + std::to_string(HIWORD(high)) + "." + std::to_string(LOWORD(high)) + "." + std::to_string(HIWORD(low)) + "." +
                             std::to_string(LOWORD(low));
                    if (d.VendorId == 0x10DE)
                    {
                        unsigned n = (HIWORD(low) % 10) * 10000 + LOWORD(low);
                        char nvidia[32];
                        sprintf_s(nvidia, " (NVIDIA %u.%02u)", n / 100, n % 100);
                        driver += nvidia;
                    }
                }
                line("Graphics: " + Narrow(d.Description) + " (" + std::to_string((unsigned long long)(d.DedicatedVideoMemory / (1024 * 1024))) + " MB" + driver + ")");
            }
            adapter->Release();
        }
        factory->Release();
    }

    AddScreens(r);
    AddWindowsGraphics(r);

    // VR software.
    line("OpenXR runtime: " + Narrow(RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime")));
    AddOpenXrLayers(r);
    AddProgramsRunning(r);
    line("");

    // The game and the mod.
#ifdef BUNKERVR_COPY_LOCK
    // Private test builds only: whose copy this is, and the Steam account signed in now.
    {
        TesterInfo tester = LoadTesterInfo();
        std::string accounts;
        for (unsigned long long a : tester.accounts) accounts += (accounts.empty() ? "" : ", ") + std::to_string(a);
        unsigned long long now = SteamAccountNow();
        line("This copy is for: " + Narrow(tester.name) + " (Steam " + accounts + ")");
        line("Steam account signed in now: " + (now ? std::to_string(now) + (TesterAllows(tester, now) ? " (one this copy is for)" : " (NOT one this copy is for)") : std::string("none (Steam not running)")));
    }
#endif
    line("Game folder: " + Narrow(game));
    if (!game.empty()) AddGameFolderDlls(r, game);
    std::string exeHash = game.empty() ? std::string() : FileSha256(Join(game, kGameExe));
    line("Game exe SHA-256: " + exeHash + (payload.gameExeSha256.empty() ? "" : (exeHash == payload.gameExeSha256 ? " (the version BunkerVR was made for)"
                                                                                                                  : " (NOT the version BunkerVR was made for)")));
    Manifest m = game.empty() ? Manifest() : ReadManifest(game);
    if (!m.present) line("BunkerVR: not installed by BunkerVR Setup");
    else
    {
        int changed = 0, missing = 0;
        std::string list;
        for (const ManifestFile& f : m.files)
        {
            std::wstring target = GamePath(game, f.path);
            if (!FileExists(target)) { ++missing; list += "  missing: " + f.path + "\r\n"; }
            else if (FileSha256(target) != f.sha256 && f.path != "BunkerVR_grips.txt") { ++changed; list += "  changed: " + f.path + "\r\n"; }
        }
        line("BunkerVR: version " + Narrow(m.version) + ", " + std::to_string(m.files.size()) + " files, " + std::to_string(missing) + " missing, " +
             std::to_string(changed) + " changed since installed");
        r += list;
    }
    return r;
}

struct LogsOutcome
{
    bool good = false;
    std::wstring zip;
    std::wstring message;
};

static LogsOutcome GatherLogs(const std::wstring& game, const Payload& payload)
{
    LogsOutcome out;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[64];
    swprintf_s(stamp, L"%04d-%02d-%02d %02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    wchar_t tempRoot[MAX_PATH];
    GetTempPathW(MAX_PATH, tempRoot);
    std::wstring work = Join(tempRoot, std::wstring(L"BunkerVR logs ") + stamp);
    DeleteTree(work);
    std::wstring inside = Join(work, std::wstring(L"BunkerVR logs ") + stamp);
    if (!MakeFolders(work, inside, nullptr)) { out.message = L"A folder for the logs could not be made."; return out; }

    std::string report = SystemReport(game, payload);
    WriteTextAtomic(Join(inside, L"system.txt"), report);

    if (!game.empty())
    {
        std::wstring mod = Join(inside, L"mod");
        CopyMatching(game, L"BunkerVR*.log", mod, 0);
        CopyMatching(game, L"BunkerVR_settings.txt", mod, 0);
        CopyMatching(game, L"BunkerVR_hints.txt", mod, 0);
        CopyMatching(Join(game, L"BunkerVR\\install"), L"manifest.txt", mod, 0);
        CopyMarks(Join(game, L"BunkerVR\\recordings"), Join(inside, L"marks"), 14, 10);
    }
    CopyMatching(SetupCopyFolder(), L"setup_log.txt", Join(inside, L"mod"), 0);
    std::wstring myGames = Join(KnownFolder(FOLDERID_Documents), L"My Games\\Amnesia The Bunker");
    std::wstring gameLogs = Join(inside, L"game");
    CopyMatching(Join(myGames, L"Main"), L"hpl*.log", gameLogs, 0);
    CopyMatching(Join(myGames, L"Main"), L"*_user_settings.cfg", gameLogs, 0);
    CopyMatching(Join(myGames, L"Main\\LogHistory"), L"*.log", Join(gameLogs, L"LogHistory"), 14, 40);
    // (Crash dumps are 10 to 60 MB each: the last week's, two at most.)
    CopyMatching(Join(KnownFolder(FOLDERID_LocalAppData), L"CrashDumps"), L"AmnesiaTheBunker*.dmp", Join(inside, L"crashes"), 7, 2);
    std::wstring steam = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    for (wchar_t& c : steam) if (c == L'/') c = L'\\';
    if (!steam.empty())
    {
        CopyMatching(Join(steam, L"logs"), L"xrclient_AmnesiaTheBunker*.txt", Join(inside, L"steamvr"), 0);
        CopyMatching(Join(steam, L"logs"), L"vrclient_AmnesiaTheBunker*.txt", Join(inside, L"steamvr"), 0);
        // SteamVR's own logs from the last two weeks (the headset's refresh rate, reprojection and frame throttling,
        // dropped frames, the headset's driver: PSVR2, Index, Quest...), and SteamVR's settings, each game's included.
        CopyMatching(Join(steam, L"logs"), L"vrserver*.txt", Join(inside, L"steamvr"), 14);
        CopyMatching(Join(steam, L"logs"), L"vrcompositor*.txt", Join(inside, L"steamvr"), 14);
        CopyMatching(Join(steam, L"logs"), L"vrmonitor*.txt", Join(inside, L"steamvr"), 14);
        CopyMatching(Join(steam, L"config"), L"steamvr.vrsettings", Join(inside, L"steamvr"), 0);
    }
    // Windows' own reports of the game's and SteamVR's crashes and hangs.
    WriteCrashEvents(Join(inside, L"events.txt"), work);

    // Packed by Windows' own tar (Windows 10 1803 and later), else PowerShell; made beside the logs, then moved to
    // the desktop whole (it never shows there half written).
    out.zip = Join(KnownFolder(FOLDERID_Desktop), std::wstring(L"BunkerVR logs ") + stamp + L".zip");
    std::wstring making = Join(work, L"logs.zip");
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    std::wstring tar = Join(system, L"tar.exe");
    std::wstring command;
    if (FileExists(tar))
        command = L"\"" + tar + L"\" -a -c -f \"" + making + L"\" -C \"" + work + L"\" \"BunkerVR logs " + stamp + L"\"";
    else
        command = L"powershell.exe -NoProfile -NonInteractive -Command \"Compress-Archive -Path '" + inside + L"' -DestinationPath '" + making + L"' -Force\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> line(command.begin(), command.end());
    line.push_back(0);
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, work.c_str(), &si, &pi))
    {
        WaitForSingleObject(pi.hProcess, 5 * 60 * 1000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    WIN32_FILE_ATTRIBUTE_DATA made = {};
    if (GetFileAttributesExW(making.c_str(), GetFileExInfoStandard, &made) && (made.nFileSizeLow > 0 || made.nFileSizeHigh > 0))
        MoveFileExW(making.c_str(), out.zip.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH);
    DeleteTree(work);
    WIN32_FILE_ATTRIBUTE_DATA zipInfo = {};
    out.good = GetFileAttributesExW(out.zip.c_str(), GetFileExInfoStandard, &zipInfo) && (zipInfo.nFileSizeLow > 0 || zipInfo.nFileSizeHigh > 0);
    if (!out.good) out.message = L"The zip could not be made.";
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------------------------------------------

enum class Page { Ready, NotFound, DevFolder, Working, Done, ConfirmUninstall, Removed, Failed, Logs, LogsDone };
enum class Job { None, Install, Uninstall, Logs };

struct Button
{
    int id = 0;          // 1 primary, 2 secondary, 3 the folder's "Change" link
    D2D1_RECT_F rect = {};
    bool enabled = true;
};

struct App
{
    HWND window = nullptr;
    ID2D1Factory* d2d = nullptr;
    ID2D1DCRenderTarget* target = nullptr; // (drawn into the window's paint: an Hwnd target skips presenting when Windows thinks the window can't be seen)
    IDWriteFactory* dwrite = nullptr;
    IDWriteTextFormat* title = nullptr;
    IDWriteTextFormat* body = nullptr;
    IDWriteTextFormat* bodyStrong = nullptr;
    IDWriteTextFormat* fine = nullptr;
    IDWriteTextFormat* label = nullptr;
    IDWriteTextFormat* button = nullptr;
    IDWriteTextFormat* path = nullptr;
    UINT dpi = 96;

    Payload payload;
    std::wstring game;
    bool gameGiven = false;
    bool devFolder = false;
    bool writable = true;
    bool running = false;
    bool versionKnownDifferent = false;
#ifdef BUNKERVR_COPY_LOCK
    TesterInfo tester;                   // whose copy this is (private test builds)
#endif
    unsigned long long steamAccount = 0; // the Steam account signed in now (0: Steam not running)
    bool steamOther = false;             // ...and it is not one this copy was made for
    Manifest installed;
    std::wstring choiceProblem;

    Page page = Page::Ready;
    Job job = Job::None;
    Progress progress;
    Outcome outcome;
    LogsOutcome logs;
    std::thread worker;
    double pageShownMs = 0;

    std::vector<Button> buttons;
    int hover = 0, pressed = 0;
};

static App g;
static const UINT kMessageDone = WM_APP + 1;
static const float kWidth = 600.0f, kHeight = 356.0f; // the client area, in 96-dpi pixels

static double NowMs() { return (double)GetTickCount64(); }
static void Trace(const char* format, ...); // (below)

static D2D1_COLOR_F Colour(UINT32 rgb, float a = 1.0f)
{
    return D2D1::ColorF(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a);
}

static const UINT32 kBackground = 0x121214, kCard = 0x1A1A1E, kCardLine = 0x2A2A30, kText = 0xECECEE, kTextDim = 0x9A9AA3,
                    kTextFaint = 0x6E6E76, kGood = 0x6CC48A, kWarn = 0xE0B04F, kBad = 0xE56A5C, kLink = 0xCDB68C;

static IDWriteTextFormat* MakeFormat(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, bool ellipsis, bool wrap)
{
    IDWriteTextFormat* format = nullptr;
    // (Segoe UI Variable on Windows 11; Segoe UI on Windows 10.)
    IDWriteFontCollection* fonts = nullptr;
    const wchar_t* use = family;
    if (SUCCEEDED(g.dwrite->GetSystemFontCollection(&fonts, FALSE)))
    {
        UINT32 index = 0;
        BOOL exists = FALSE;
        fonts->FindFamilyName(family, &index, &exists);
        if (!exists) use = L"Segoe UI";
        fonts->Release();
    }
    g.dwrite->CreateTextFormat(use, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &format);
    if (!format) return nullptr;
    format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (ellipsis)
    {
        IDWriteInlineObject* sign = nullptr;
        g.dwrite->CreateEllipsisTrimmingSign(format, &sign);
        DWRITE_TRIMMING trimming = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        format->SetTrimming(&trimming, sign);
        if (sign) sign->Release();
    }
    return format;
}

static float TextWidth(IDWriteTextFormat* format, const std::wstring& text)
{
    IDWriteTextLayout* layout = nullptr;
    float width = 0;
    if (SUCCEEDED(g.dwrite->CreateTextLayout(text.c_str(), (UINT32)text.size(), format, 2000.0f, 200.0f, &layout)))
    {
        DWRITE_TEXT_METRICS m;
        if (SUCCEEDED(layout->GetMetrics(&m))) width = m.widthIncludingTrailingWhitespace;
        layout->Release();
    }
    return width;
}

static void Text(IDWriteTextFormat* format, const std::wstring& text, float x, float y, float w, float h, UINT32 rgb, float alpha,
                 DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING)
{
    ID2D1SolidColorBrush* brush = nullptr;
    g.target->CreateSolidColorBrush(Colour(rgb, alpha), &brush);
    if (!brush) return;
    format->SetTextAlignment(align);
    g.target->DrawTextW(text.c_str(), (UINT32)text.size(), format, D2D1::RectF(x, y, x + w, y + h), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    brush->Release();
}

static void Fill(const D2D1_RECT_F& r, float radius, UINT32 rgb, float alpha)
{
    ID2D1SolidColorBrush* brush = nullptr;
    g.target->CreateSolidColorBrush(Colour(rgb, alpha), &brush);
    if (!brush) return;
    g.target->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush);
    brush->Release();
}

static void Outline(const D2D1_RECT_F& r, float radius, UINT32 rgb, float alpha, float width = 1.0f)
{
    ID2D1SolidColorBrush* brush = nullptr;
    g.target->CreateSolidColorBrush(Colour(rgb, alpha), &brush);
    if (!brush) return;
    g.target->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush, width);
    brush->Release();
}

static void Dot(float x, float y, float radius, UINT32 rgb, float alpha)
{
    ID2D1SolidColorBrush* brush = nullptr;
    g.target->CreateSolidColorBrush(Colour(rgb, alpha), &brush);
    if (!brush) return;
    g.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius), brush);
    brush->Release();
}

// A round mark: a tick (good) or a cross.
static void Mark(float cx, float cy, bool good, float alpha)
{
    UINT32 rgb = good ? kGood : kBad;
    ID2D1SolidColorBrush* brush = nullptr;
    g.target->CreateSolidColorBrush(Colour(rgb, alpha), &brush);
    if (!brush) return;
    g.target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 15.0f, 15.0f), brush, 1.6f);
    if (good)
    {
        g.target->DrawLine(D2D1::Point2F(cx - 6.5f, cy + 0.5f), D2D1::Point2F(cx - 2.0f, cy + 5.0f), brush, 2.0f);
        g.target->DrawLine(D2D1::Point2F(cx - 2.0f, cy + 5.0f), D2D1::Point2F(cx + 7.0f, cy - 5.5f), brush, 2.0f);
    }
    else
    {
        g.target->DrawLine(D2D1::Point2F(cx - 5.5f, cy - 5.5f), D2D1::Point2F(cx + 5.5f, cy + 5.5f), brush, 2.0f);
        g.target->DrawLine(D2D1::Point2F(cx + 5.5f, cy - 5.5f), D2D1::Point2F(cx - 5.5f, cy + 5.5f), brush, 2.0f);
    }
    brush->Release();
}

static void AddButton(int id, const std::wstring& label, bool primary, float right, bool enabled, float alpha, float& leftOut)
{
    float w = std::max(112.0f, TextWidth(g.button, label) + 44.0f);
    D2D1_RECT_F r = D2D1::RectF(right - w, kHeight - 36.0f - 38.0f, right, kHeight - 36.0f);
    leftOut = r.left;
    g.buttons.push_back({ id, r, enabled });
    bool hot = enabled && g.hover == id, down = hot && g.pressed == id;
    if (primary)
    {
        UINT32 fill = !enabled ? 0x34343A : (down ? 0xD6D6DA : (hot ? 0xFFFFFF : 0xEDEDEF));
        Fill(r, 6.0f, fill, alpha);
        Text(g.button, label, r.left, r.top + 9.5f, w, 22.0f, enabled ? 0x121214 : 0x75757D, alpha, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    else
    {
        if (hot) Fill(r, 6.0f, down ? 0x2C2C32 : 0x232328, alpha);
        Outline(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 6.0f, 0x3A3A42, alpha);
        Text(g.button, label, r.left, r.top + 9.5f, w, 22.0f, enabled ? kText : kTextFaint, alpha, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

static int CompareVersions(const std::wstring& a, const std::wstring& b)
{
    double x = _wtof(a.c_str()), y = _wtof(b.c_str());
    return x < y ? -1 : (x > y ? 1 : 0);
}

// Reads the folder's state again; called whenever it may have changed.
static void Refresh()
{
    g.devFolder = !g.game.empty() && DirExists(Join(g.game, L".git"));
    g.writable = g.game.empty() || FolderWritable(g.game);
    g.running = GameRunning();
    g.installed = g.game.empty() ? Manifest() : ReadManifest(g.game);
    g.versionKnownDifferent = false;
#ifdef BUNKERVR_COPY_LOCK
    g.steamAccount = SteamAccountNow();
    g.steamOther = g.steamAccount != 0 && !TesterAllows(g.tester, g.steamAccount);
#endif
    if (!g.game.empty() && !g.payload.gameExeSha256.empty()) g.versionKnownDifferent = FileSha256(Join(g.game, kGameExe)) != g.payload.gameExeSha256;
}

static void ShowPage(Page page)
{
    g.page = page;
    g.pageShownMs = NowMs();
    g.hover = g.pressed = 0;
    SetTimer(g.window, 1, 15, nullptr); // (the page fading in)
    InvalidateRect(g.window, nullptr, FALSE);
}

static void Render()
{
    if (!g.target) return;
    g.target->BeginDraw();
    g.target->Clear(Colour(kBackground));
    g.buttons.clear();
    float t = (float)std::min(1.0, (NowMs() - g.pageShownMs) / 220.0);
    float alpha = t * t * (3.0f - 2.0f * t);
    const float m = 36.0f;
    const std::wstring version = g.payload.version.empty() ? L"" : L"Version " + g.payload.version;

    // The header, always drawn and never faded: the name, with the version on the same line at the right.
    Text(g.title, L"BunkerVR", m, 28.0f, 300.0f, 40.0f, kText, 1.0f);
    Text(g.fine, version, kWidth - m - 320.0f, 42.0f, 320.0f, 18.0f, kTextFaint, 1.0f, DWRITE_TEXT_ALIGNMENT_TRAILING);

    D2D1_RECT_F card = D2D1::RectF(m, 92.0f, kWidth - m, 190.0f);
    Fill(card, 10.0f, kCard, alpha);
    Outline(D2D1::RectF(card.left + 0.5f, card.top + 0.5f, card.right - 0.5f, card.bottom - 0.5f), 10.0f, kCardLine, alpha);
    float inner = card.left + 22.0f, innerWidth = card.right - card.left - 44.0f;
    float right = kWidth - m, left = right;

    switch (g.page)
    {
    case Page::Ready:
    case Page::NotFound:
    case Page::DevFolder:
    {
        Text(g.label, L"GAME FOLDER", inner, card.top + 18.0f, 200.0f, 16.0f, kTextFaint, alpha);
        std::wstring shown = g.game.empty() ? L"Not found" : g.game;
        float linkWidth = TextWidth(g.body, L"Change") + 4.0f;
        Text(g.path, shown, inner, card.top + 38.0f, innerWidth - linkWidth - 24.0f, 22.0f, g.game.empty() ? kTextDim : kText, alpha);
        D2D1_RECT_F link = D2D1::RectF(card.right - 22.0f - linkWidth, card.top + 36.0f, card.right - 18.0f, card.top + 60.0f);
        g.buttons.push_back({ 3, link, true });
        Text(g.body, L"Change", link.left, card.top + 38.0f, linkWidth + 4.0f, 22.0f, g.hover == 3 ? 0xEADCBF : kLink, alpha);

        UINT32 dot = kGood;
        std::wstring status;
        if (!g.choiceProblem.empty()) { dot = kBad; status = g.choiceProblem; }
        else if (g.page == Page::NotFound) { dot = kBad; status = L"Amnesia: The Bunker wasn't found. Choose its installation folder."; }
        else if (g.devFolder) { dot = kWarn; status = L"This is BunkerVR's development folder: Setup leaves it as it is."; }
        else if (g.running) { dot = kWarn; status = L"Close Amnesia: The Bunker to continue."; }
#ifdef BUNKERVR_COPY_LOCK
        else if (g.steamOther) { dot = kBad; status = L"This copy is registered to a different Steam account."; }
#endif
        else if (g.installed.present) status = L"BunkerVR " + g.installed.version + L" is installed.";
        else if (g.versionKnownDifferent) { dot = kWarn; status = L"This version of the game hasn't been tested with BunkerVR."; }
        else status = L"Amnesia: The Bunker found. Ready to install.";
        Dot(inner + 4.0f, card.top + 79.0f, 3.5f, dot, alpha);
        Text(g.fine, status, inner + 16.0f, card.top + 70.0f, innerWidth - 16.0f, 18.0f, kTextDim, alpha);

        std::wstring note;
        if (g.page == Page::Ready && !g.devFolder)
        {
#ifdef BUNKERVR_COPY_LOCK
            if (g.steamOther && !g.running)
                note = L"Sign in to Steam with the account you registered for testing, or contact the developer.";
            else
#endif
            if (!g.writable) note = L"Windows will ask for permission to change the game's folder.";
            else if (g.installed.present) note = L"Your saves and settings will be kept.";
            else note = L"Your saves won't be affected, and uninstalling restores the original game files.";
        }
        Text(g.fine, note, m + 2.0f, card.bottom + 18.0f, kWidth - 2 * m, 36.0f, kTextFaint, alpha);

        bool canChange = g.page == Page::Ready && !g.devFolder && !g.running && !g.game.empty() && g.payload.good;
        bool can = canChange && !g.steamOther; // (installing; uninstalling stays open to any account)
        if (g.page == Page::NotFound || g.game.empty()) AddButton(1, L"Choose folder", true, right, true, alpha, left);
        else if (g.installed.present)
        {
            int c = CompareVersions(g.payload.version, g.installed.version);
            AddButton(1, c > 0 ? L"Update to " + g.payload.version : (c == 0 ? L"Reinstall" : L"Install " + g.payload.version), true, right, can, alpha, left);
            AddButton(2, L"Uninstall", false, left - 12.0f, canChange, alpha, left);
        }
        else AddButton(1, L"Install", true, right, can, alpha, left);
        break;
    }
    case Page::Working:
    case Page::Logs:
    {
        std::wstring what = g.page == Page::Logs ? L"Collecting logs" : (g.job == Job::Uninstall ? L"Uninstalling BunkerVR" : L"Installing BunkerVR " + g.payload.version);
        Text(g.bodyStrong, what, inner, card.top + 22.0f, innerWidth, 22.0f, kText, alpha);
        D2D1_RECT_F track = D2D1::RectF(inner, card.top + 58.0f, inner + innerWidth, card.top + 62.0f);
        Fill(track, 2.0f, 0x2A2A30, alpha);
        int total = g.progress.total.load(), done = g.progress.done.load();
        if (g.page == Page::Logs || total <= 0)
        {
            // (Not counted: a short bar going round.)
            float phase = (float)fmod(NowMs() / 1400.0, 1.0);
            float a = track.left + (track.right - track.left) * std::max(0.0f, phase * 1.3f - 0.3f);
            float b = track.left + (track.right - track.left) * std::min(1.0f, phase * 1.3f);
            Fill(D2D1::RectF(a, track.top, b, track.bottom), 2.0f, kText, alpha);
        }
        else
        {
            float share = std::min(1.0f, (float)done / (float)total);
            Fill(D2D1::RectF(track.left, track.top, track.left + (track.right - track.left) * share, track.bottom), 2.0f, kText, alpha);
            Text(g.fine, std::to_wstring(done) + L" of " + std::to_wstring(total) + L" files", inner, card.top + 70.0f, innerWidth, 18.0f,
                 kTextFaint, alpha);
        }
        break;
    }
    case Page::Done:
    case Page::Removed:
    case Page::Failed:
    case Page::LogsDone:
    {
        bool good = g.page != Page::Failed;
        Mark(inner + 15.0f, card.top + 49.0f, good, alpha);
        std::wstring head, line;
        if (g.page == Page::Done)
        {
            head = L"BunkerVR " + g.payload.version + L" is installed";
            line = L"Put on your headset and start Amnesia: The Bunker from Steam.";
        }
        else if (g.page == Page::Removed)
        {
            head = L"BunkerVR has been uninstalled";
            line = g.outcome.good ? L"The game's original files have been restored." : g.outcome.message;
        }
        else if (g.page == Page::LogsDone)
        {
            head = L"Logs saved to your desktop";
            line = L"Attach \u201C" + g.logs.zip.substr(g.logs.zip.find_last_of(L'\\') + 1) + L"\u201D to your bug report.";
        }
        else
        {
            head = g.job == Job::Logs ? L"The logs couldn't be collected" : (g.job == Job::Uninstall ? L"Uninstall couldn't be completed" : L"Installation couldn't be completed");
            line = g.job == Job::Logs ? g.logs.message : g.outcome.message;
        }
        Text(g.bodyStrong, head, inner + 46.0f, card.top + 26.0f, innerWidth - 46.0f, 22.0f, kText, alpha);
        Text(g.fine, line, inner + 46.0f, card.top + 50.0f, innerWidth - 46.0f, 40.0f, kTextDim, alpha);
        std::wstring note;
        if (g.page == Page::Done) note = L"To report a problem, hold the left stick in and click the right stick to mark the moment, then open "
                                         L"\u201CBunkerVR Logs\u201D on your desktop and attach the file it makes to your bug report.";
        else if (g.page == Page::Failed && g.job == Job::Install && g.outcome.rolledBack) note = L"No changes were made to the game.";
        Text(g.fine, note, m + 2.0f, card.bottom + 18.0f, kWidth - 2 * m, 40.0f, kTextFaint, alpha);
        if (g.page == Page::LogsDone)
        {
            AddButton(1, L"Show file", true, right, true, alpha, left);
            AddButton(2, L"Close", false, left - 12.0f, true, alpha, left);
        }
        else AddButton(1, g.page == Page::Failed ? L"Close" : L"Done", true, right, true, alpha, left);
        break;
    }
    case Page::ConfirmUninstall:
    {
        Text(g.bodyStrong, L"Uninstall BunkerVR?", inner, card.top + 24.0f, innerWidth, 22.0f, kText, alpha);
        Text(g.fine, L"BunkerVR's files, settings and logs will be removed, and the game's original files restored. Your saves won't be affected.",
             inner, card.top + 50.0f, innerWidth, 40.0f, kTextDim, alpha);
        bool can = !g.running && g.installed.present;
        if (g.running) Text(g.fine, L"Close Amnesia: The Bunker to continue.", m + 2.0f, card.bottom + 18.0f, kWidth - 2 * m, 20.0f, kWarn, alpha);
        else if (!g.installed.present) Text(g.fine, L"BunkerVR isn't installed in this folder.", m + 2.0f, card.bottom + 18.0f, kWidth - 2 * m, 20.0f, kTextFaint, alpha);
        AddButton(1, L"Uninstall", true, right, can, alpha, left);
        AddButton(2, L"Cancel", false, left - 12.0f, true, alpha, left);
        break;
    }
    }

    HRESULT result = g.target->EndDraw();
    static int traced = 0;
    if (traced < 5 || FAILED(result)) { ++traced; Trace("drawn, page %d: 0x%08lx", (int)g.page, (unsigned long)result); }
    if (result == D2DERR_RECREATE_TARGET)
    {
        g.target->Release();
        g.target = nullptr;
    }
}

// (A trace for finding faults: only when BUNKERVR_SETUP_TRACE is set, to %TEMP%\BunkerVR Setup trace.txt.)
static void Trace(const char* format, ...)
{
    static int on = -1;
    if (on < 0) on = GetEnvironmentVariableW(L"BUNKERVR_SETUP_TRACE", nullptr, 0) > 0 ? 1 : 0;
    if (!on) return;
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    FILE* file = nullptr;
    if (_wfopen_s(&file, Join(temp, L"BunkerVR Setup trace.txt").c_str(), L"ab") == 0 && file)
    {
        fprintf(file, "%s\r\n", line);
        fclose(file);
    }
}

static void MakeTarget()
{
    if (g.target) return;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), (float)g.dpi, (float)g.dpi);
    HRESULT made = g.d2d->CreateDCRenderTarget(&props, &g.target);
    Trace("render target at %u dpi: 0x%08lx", g.dpi, (unsigned long)made);
    if (g.target) g.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
}

static void StartJob(Job job)
{
    if (g.worker.joinable()) return;
    g.job = job;
    g.progress.done = 0;
    g.progress.total = 0;
    ShowPage(job == Job::Logs ? Page::Logs : Page::Working);
    SetTimer(g.window, 2, 33, nullptr); // (the bar)
    std::wstring game = g.game;
    g.worker = std::thread([job, game]() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (job == Job::Install) g.outcome = Install(game, g.payload, g.progress);
        else if (job == Job::Uninstall) g.outcome = Uninstall(game, g.progress);
        else g.logs = GatherLogs(game, g.payload);
        CoUninitialize();
        PostMessageW(g.window, kMessageDone, 0, 0);
    });
}

// The same program again with Windows' permission, to do `job` in `game`; this one then closes.
static bool StartElevated(const wchar_t* job)
{
    std::wstring arguments = L"/game \"" + g.game + L"\" /go " + job;
    SHELLEXECUTEINFOW info = { sizeof(info) };
    info.fMask = SEE_MASK_NOASYNC;
    info.hwnd = g.window;
    info.lpVerb = L"runas";
    std::wstring self = OwnPath();
    info.lpFile = self.c_str();
    info.lpParameters = arguments.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info))
    {
        g.choiceProblem = L"Windows didn't allow changes to the game's folder.";
        InvalidateRect(g.window, nullptr, FALSE);
        return false;
    }
    DestroyWindow(g.window);
    return true;
}

static void ChooseFolder()
{
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Select the Amnesia: The Bunker installation folder");
    if (SUCCEEDED(dialog->Show(g.window)))
    {
        IShellItem* item = nullptr;
        wchar_t* path = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
        {
            std::wstring chosen = path;
            CoTaskMemFree(path);
            if (IsGameFolder(chosen))
            {
                g.game = chosen;
                g.choiceProblem.clear();
                Refresh();
                ShowPage(Page::Ready);
            }
            else
            {
                g.choiceProblem = L"Amnesia: The Bunker wasn't found in that folder.";
                InvalidateRect(g.window, nullptr, FALSE);
            }
        }
        if (item) item->Release();
    }
    dialog->Release();
}

static void Activate(int id)
{
    switch (g.page)
    {
    case Page::Ready:
    case Page::NotFound:
    case Page::DevFolder:
        if (id == 3 || (id == 1 && (g.page == Page::NotFound || g.game.empty()))) { ChooseFolder(); return; }
        Refresh();
        if (g.devFolder || g.running || g.game.empty()) { InvalidateRect(g.window, nullptr, FALSE); return; }
        if (id == 1)
        {
            if (g.steamOther) { InvalidateRect(g.window, nullptr, FALSE); return; }
            if (!g.writable && !IsElevated()) { StartElevated(L"install"); return; }
            StartJob(Job::Install);
        }
        else if (id == 2) ShowPage(Page::ConfirmUninstall);
        break;
    case Page::ConfirmUninstall:
        if (id == 2) { Refresh(); ShowPage(g.game.empty() ? Page::NotFound : Page::Ready); return; }
        Refresh();
        if (g.running || !g.installed.present) { InvalidateRect(g.window, nullptr, FALSE); return; }
        if (!g.writable && !IsElevated()) { StartElevated(L"uninstall"); return; }
        StartJob(Job::Uninstall);
        break;
    case Page::LogsDone:
        if (id == 1)
        {
            PIDLIST_ABSOLUTE item = ILCreateFromPathW(g.logs.zip.c_str());
            if (item)
            {
                SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
                ILFree(item);
            }
        }
        DestroyWindow(g.window);
        break;
    case Page::Done:
    case Page::Removed:
    case Page::Failed:
        DestroyWindow(g.window);
        break;
    default:
        break;
    }
}

static int HitTest(float x, float y)
{
    for (const Button& b : g.buttons)
        if (b.enabled && x >= b.rect.left && x <= b.rect.right && y >= b.rect.top && y <= b.rect.bottom) return b.id;
    return 0;
}

static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    switch (message)
    {
    case WM_CREATE:
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window, &ps);
        MakeTarget();
        RECT client;
        GetClientRect(window, &client);
        if (g.target && SUCCEEDED(g.target->BindDC(dc, &client))) Render();
        EndPaint(window, &ps);
        return 0;
    }
    case WM_DPICHANGED:
    {
        g.dpi = HIWORD(w);
        RECT* r = (RECT*)l;
        SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g.target) g.target->SetDpi((float)g.dpi, (float)g.dpi);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_TIMER:
        if (w == 1 && NowMs() - g.pageShownMs > 260) KillTimer(window, 1);
        if (w == 3) // (every second: the game started or closed, the folder's state)
        {
            if (g.page == Page::Ready || g.page == Page::ConfirmUninstall)
            {
                bool wasRunning = g.running;
                g.running = GameRunning();
#ifdef BUNKERVR_COPY_LOCK
                if (wasRunning != g.running || g.steamAccount != SteamAccountNow()) Refresh(); // (or Steam signed in with another account)
#else
                if (wasRunning != g.running) Refresh();
#endif
            }
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
    {
        float x = GET_X_LPARAM(l) * 96.0f / g.dpi, y = GET_Y_LPARAM(l) * 96.0f / g.dpi;
        int id = HitTest(x, y);
        if (id != g.hover) { g.hover = id; InvalidateRect(window, nullptr, FALSE); }
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, window, 0 };
        TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE:
        g.hover = g.pressed = 0;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && g.hover) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        break;
    case WM_LBUTTONDOWN:
        g.pressed = g.hover;
        SetCapture(window);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
    {
        ReleaseCapture();
        float x = GET_X_LPARAM(l) * 96.0f / g.dpi, y = GET_Y_LPARAM(l) * 96.0f / g.dpi;
        int id = HitTest(x, y);
        int was = g.pressed;
        g.pressed = 0;
        InvalidateRect(window, nullptr, FALSE);
        if (id && id == was) Activate(id);
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_RETURN)
        {
            for (const Button& b : g.buttons) if (b.id == 1 && b.enabled) { Activate(1); break; }
            return 0;
        }
        if (w == VK_ESCAPE)
        {
            if (g.page == Page::Working || g.page == Page::Logs) return 0;
            if (g.page == Page::ConfirmUninstall) { Activate(2); return 0; }
            DestroyWindow(window);
            return 0;
        }
        break;
    case kMessageDone:
        if (g.worker.joinable()) g.worker.join();
        KillTimer(window, 2);
        if (g.job == Job::Logs)
        {
            SetupLog(g.logs.good ? L"logs: " + g.logs.zip : L"logs: failed: " + g.logs.message);
            ShowPage(g.logs.good ? Page::LogsDone : Page::Failed);
        }
        else if (g.job == Job::Install)
        {
            SetupLog(g.outcome.good ? L"install: finished" : L"install: failed: " + g.outcome.message);
            Refresh();
            ShowPage(g.outcome.good ? Page::Done : Page::Failed);
        }
        else
        {
            SetupLog(g.outcome.good ? L"uninstall: finished" : L"uninstall: " + g.outcome.message);
            ShowPage(g.outcome.good || !g.outcome.rolledBack ? Page::Removed : Page::Failed);
        }
        SetupLogSave(g.job == Job::Install); // (an install's log is kept even when it failed; uninstalling took the folder)
        return 0;
    case WM_CLOSE:
        if (g.page == Page::Working || g.page == Page::Logs) return 0; // (not while files are being changed)
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}

// After uninstalling from the copy Windows' Apps list starts: that copy and its folder removed once it has closed.
static void RemoveOwnCopyLater()
{
    if (_wcsicmp(OwnPath().c_str(), SetupCopyPath().c_str()) != 0) return;
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    std::wstring command = L"\"" + Join(system, L"cmd.exe") + L"\" /c ping 127.0.0.1 -n 3 >nul & rmdir /s /q \"" + SetupCopyFolder() + L"\"";
    std::vector<wchar_t> line(command.begin(), command.end());
    line.push_back(0);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, temp, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g.payload = LoadPayload();
    std::wstring whose;
#ifdef BUNKERVR_COPY_LOCK
    // Private test builds only: whose copy this is. A build without a name or any accounts is treated as damaged,
    // because its DLL would not run for anyone.
    g.tester = LoadTesterInfo();
    if (g.tester.name.empty() || g.tester.accounts.empty()) g.payload.good = false;
    whose = L" (" + g.tester.name + L"'s copy)";
#endif

    // The command line.
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring go;
    bool wantUninstall = false, wantLogs = false;
    for (int i = 1; argv && i < argc; ++i)
    {
        if (_wcsicmp(argv[i], L"/game") == 0 && i + 1 < argc) { g.game = Trim(argv[++i]); g.gameGiven = true; }
        else if (_wcsicmp(argv[i], L"/go") == 0 && i + 1 < argc) go = argv[++i];
        else if (_wcsicmp(argv[i], L"/uninstall") == 0) wantUninstall = true;
        else if (_wcsicmp(argv[i], L"/logs") == 0) wantLogs = true;
    }
    if (argv) LocalFree(argv);
    while (!g.game.empty() && (g.game.back() == L'\\' || g.game.back() == L'/')) g.game.pop_back();
    if (!g.gameGiven)
    {
        // (Uninstalling from Windows' Apps list: the folder it was installed in.)
        std::wstring registered = RegString(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation");
        if ((wantUninstall || wantLogs) && IsGameFolder(registered)) g.game = registered;
        else g.game = FindGame();
    }
    if (!g.game.empty() && !IsGameFolder(g.game)) g.game.clear();
    Refresh();
    SetupLog(L"BunkerVR Setup " + g.payload.version + whose + L" started" + (wantLogs ? L" for the logs" : (wantUninstall ? L" to uninstall" : L"")) +
             (go.empty() ? L"" : L" (go " + go + L")") + (IsElevated() ? L" as administrator" : L"") + L"; the game: " +
             (g.game.empty() ? std::wstring(L"not found") : g.game + (g.gameGiven ? L" (given)" : L"")) +
             (g.installed.present ? L"; BunkerVR " + g.installed.version + L" installed there" : std::wstring()) +
             (g.devFolder ? L"; the development folder" : L"") + (g.running ? L"; the game is running" : L"") +
             (g.versionKnownDifferent ? L"; NOT the game's version BunkerVR was made for" : L"") + (g.payload.good ? L"" : L"; the installer is damaged"));

    // Direct2D and DirectWrite.
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g.d2d)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g.dwrite)))
    {
        MessageBoxW(nullptr, L"This PC cannot draw BunkerVR Setup's window (Direct2D).", L"BunkerVR Setup", MB_ICONERROR);
        return 1;
    }
    g.title = MakeFormat(L"Segoe UI Variable Display", 30.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, false);
    g.body = MakeFormat(L"Segoe UI Variable Text", 14.0f, DWRITE_FONT_WEIGHT_NORMAL, false, false);
    g.bodyStrong = MakeFormat(L"Segoe UI Variable Text", 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true, false);
    g.fine = MakeFormat(L"Segoe UI Variable Text", 12.5f, DWRITE_FONT_WEIGHT_NORMAL, false, true);
    g.label = MakeFormat(L"Segoe UI Variable Text", 11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, false);
    g.button = MakeFormat(L"Segoe UI Variable Text", 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, false);
    g.path = MakeFormat(L"Segoe UI Variable Text", 14.0f, DWRITE_FONT_WEIGHT_NORMAL, true, false);
    if (!g.title || !g.body || !g.bodyStrong || !g.fine || !g.label || !g.button || !g.path)
    {
        MessageBoxW(nullptr, L"This PC cannot draw BunkerVR Setup's text (DirectWrite).", L"BunkerVR Setup", MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hbrBackground = CreateSolidBrush(RGB(0x12, 0x12, 0x14));
    wc.lpszClassName = L"BunkerVRSetup";
    RegisterClassExW(&wc);

    // The window's size for the screen it opens on, in the middle of it.
    POINT origin = { 0, 0 };
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    UINT dpiX = 96, dpiY = 96;
    typedef HRESULT(WINAPI * GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    GetDpiForMonitorFn getDpi = shcore ? (GetDpiForMonitorFn)GetProcAddress(shcore, "GetDpiForMonitor") : nullptr;
    if (getDpi) getDpi(monitor, 0, &dpiX, &dpiY);
    g.dpi = dpiX;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = { 0, 0, (LONG)(kWidth * g.dpi / 96.0f), (LONG)(kHeight * g.dpi / 96.0f) };
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, g.dpi);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(monitor, &mi);
    int width = r.right - r.left, height = r.bottom - r.top;
    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - width) / 2;
    int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - height) / 2;
    g.window = CreateWindowExW(0, wc.lpszClassName, L"BunkerVR Setup", style, x, y, width, height, nullptr, nullptr, instance, nullptr);
    if (!g.window) return 1;
    g.dpi = GetDpiForWindow(g.window);

    // A dark title bar in the window's own color (Windows 11), like the rest of the window.
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g.window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF caption = RGB(0x12, 0x12, 0x14);
    DwmSetWindowAttribute(g.window, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));

    if (!g.payload.good)
    {
        g.outcome.message = L"This installer is damaged or incomplete. Please download it again.";
        g.outcome.rolledBack = false;
        g.job = Job::Install;
        g.page = Page::Failed;
    }
    else if (wantLogs) g.page = Page::Logs;
    else if (g.game.empty()) g.page = Page::NotFound;
    else if (g.devFolder) g.page = Page::DevFolder;
    else if (wantUninstall || _wcsicmp(go.c_str(), L"uninstall") == 0) g.page = Page::ConfirmUninstall;
    else g.page = Page::Ready;
    g.pageShownMs = NowMs();

    ShowWindow(g.window, show);
    UpdateWindow(g.window);
    SetTimer(g.window, 1, 15, nullptr);
    SetTimer(g.window, 3, 1000, nullptr);

    if (g.payload.good && wantLogs)
    {
        SetupLogSave(false); // (so this start is in the zip)
        StartJob(Job::Logs);
    }
    else if (g.page == Page::Ready && _wcsicmp(go.c_str(), L"install") == 0 && !g.running && !g.steamOther) StartJob(Job::Install);
    else if (g.page == Page::ConfirmUninstall && _wcsicmp(go.c_str(), L"uninstall") == 0 && !g.running && g.installed.present) StartJob(Job::Uninstall);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g.worker.joinable()) g.worker.join();
    SetupLogSave(false);
    if (g.job == Job::Uninstall && g.page == Page::Removed) RemoveOwnCopyLater();
    CoUninitialize();
    return 0;
}
