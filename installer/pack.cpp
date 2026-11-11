// pack.exe: packs the mod's files into payload.bin, which setup.exe carries as a resource (format: payload.h).
//
//   pack.exe <root folder> <list file> <out payload.bin> <mod version> <AmnesiaTheBunker.exe, or ->
//
// The list has one path per line, relative to the root (the stage folder laid out as the game's folder). Patches
// (.bvrpatch) are listed and packed like any other file.
// Each file is packed with Windows' LZMS (Compression API) unless that would not make it smaller.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <compressapi.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "payload.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "cabinet.lib")

static bool ReadWhole(const std::wstring& path, std::vector<unsigned char>& data)
{
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return false;
    fseek(file, 0, SEEK_END);
    long long size = _ftelli64(file);
    fseek(file, 0, SEEK_SET);
    data.resize((size_t)size);
    bool good = size == 0 || fread(data.data(), 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    return good;
}

static bool Sha256(const unsigned char* data, size_t size, uint8_t out[32])
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool good = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
                BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
                BCryptHashData(hash, (PUCHAR)data, (ULONG)size, 0) == 0 &&
                BCryptFinishHash(hash, out, 32, 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return good;
}

static std::wstring Wide(const char* text)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, text, -1, &out[0], n);
    return out;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 6)
    {
        fwprintf(stderr, L"pack.exe <root> <list> <out payload.bin> <mod version> <AmnesiaTheBunker.exe or ->\n");
        return 2;
    }
    std::wstring root = argv[1];
    if (!root.empty() && root.back() != L'\\') root += L'\\';

    // The list.
    std::vector<unsigned char> listData;
    if (!ReadWhole(argv[2], listData)) { fwprintf(stderr, L"The list %s could not be read.\n", argv[2]); return 1; }
    std::vector<std::string> paths;
    std::string line;
    for (size_t i = 0; i <= listData.size(); ++i)
    {
        char c = i < listData.size() ? (char)listData[i] : '\n';
        if (c == '\r') continue;
        if (c == '\n')
        {
            if (!line.empty())
            {
                for (char& ch : line) if (ch == '\\') ch = '/';
                if (line.find("..") != std::string::npos || line[0] == '/' || line.find(':') != std::string::npos)
                {
                    fprintf(stderr, "Not a path inside the game's folder: %s\n", line.c_str());
                    return 1;
                }
                paths.push_back(line);
            }
            line.clear();
        }
        else line += c;
    }
    if (paths.empty()) { fprintf(stderr, "The list is empty.\n"); return 1; }

    PayloadHeader header = {};
    memcpy(header.magic, kPayloadMagic, 8);
    header.fileCount = (uint32_t)paths.size();
    char version[64] = "";
    WideCharToMultiByte(CP_UTF8, 0, argv[4], -1, version, sizeof(version), nullptr, nullptr);
    strncpy_s(header.modVersion, version, _TRUNCATE);
    if (wcscmp(argv[5], L"-") != 0)
    {
        std::vector<unsigned char> exe;
        if (!ReadWhole(argv[5], exe) || !Sha256(exe.data(), exe.size(), header.gameExeSha256))
        {
            fwprintf(stderr, L"The game's exe %s could not be read.\n", argv[5]);
            return 1;
        }
    }

    COMPRESSOR_HANDLE compressor = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &compressor))
    {
        fprintf(stderr, "Windows' LZMS compressor could not be made (error %lu).\n", GetLastError());
        return 1;
    }

    std::vector<unsigned char> out;
    out.resize(sizeof(header));
    unsigned long long packedTotal = 0;
    for (const std::string& path : paths)
    {
        std::wstring full = root + Wide(path.c_str());
        for (wchar_t& ch : full) if (ch == L'/') ch = L'\\';
        std::vector<unsigned char> data;
        if (!ReadWhole(full, data)) { fwprintf(stderr, L"%s could not be read.\n", full.c_str()); CloseCompressor(compressor); return 1; }

        PayloadEntry entry = {};
        entry.pathBytes = (uint16_t)path.size();
        entry.rawSize = data.size();
        if (!Sha256(data.data(), data.size(), entry.sha256)) { fprintf(stderr, "SHA-256 failed.\n"); CloseCompressor(compressor); return 1; }
        header.rawTotal += data.size();

        std::vector<unsigned char> packed;
        SIZE_T packedSize = 0;
        if (!data.empty())
        {
            packed.resize(data.size() + data.size() / 2 + 65536);
            if (!Compress(compressor, data.data(), data.size(), packed.data(), packed.size(), &packedSize)) packedSize = 0;
        }
        const std::vector<unsigned char>& stored = (packedSize > 0 && packedSize < data.size()) ? packed : data;
        entry.method = (packedSize > 0 && packedSize < data.size()) ? kPayloadLzms : kPayloadStored;
        entry.packedSize = entry.method == kPayloadLzms ? packedSize : data.size();
        packedTotal += entry.packedSize;

        size_t at = out.size();
        out.resize(at + sizeof(entry) + path.size() + (size_t)entry.packedSize);
        memcpy(&out[at], &entry, sizeof(entry));
        memcpy(&out[at + sizeof(entry)], path.data(), path.size());
        if (entry.packedSize) memcpy(&out[at + sizeof(entry) + path.size()], stored.data(), (size_t)entry.packedSize);
    }
    CloseCompressor(compressor);
    memcpy(out.data(), &header, sizeof(header));

    FILE* file = nullptr;
    if (_wfopen_s(&file, argv[3], L"wb") != 0 || !file) { fwprintf(stderr, L"%s could not be written.\n", argv[3]); return 1; }
    bool good = fwrite(out.data(), 1, out.size(), file) == out.size();
    fclose(file);
    if (!good) { fprintf(stderr, "The payload could not be written whole.\n"); return 1; }
    printf("Packed %u files, %.2f MB as installed, %.2f MB packed (version %s).\n", header.fileCount, header.rawTotal / 1048576.0,
           packedTotal / 1048576.0, header.modVersion);
    return 0;
}
