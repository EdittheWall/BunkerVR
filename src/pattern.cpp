#include "pattern.h"

static int HexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void* FindPattern(HMODULE module, const char* pattern, int* matchCount)
{
    if (matchCount) *matchCount = 0;
    if (!module) return nullptr;

    // Turn the text pattern into bytes plus a "does this byte matter" mask.
    unsigned char bytes[256];
    bool matters[256];
    int length = 0;
    for (const char* p = pattern; *p != '\0' && length < 256;)
    {
        if (*p == ' ') { ++p; continue; }
        if (p[0] == '?')
        {
            bytes[length] = 0;
            matters[length] = false;
            ++length;
            p += (p[1] == '?') ? 2 : 1;
            continue;
        }
        int high = HexValue(p[0]);
        int low = (high >= 0) ? HexValue(p[1]) : -1;
        if (high < 0 || low < 0) return nullptr; // badly written pattern
        bytes[length] = (unsigned char)(high * 16 + low);
        matters[length] = true;
        ++length;
        p += 2;
    }
    if (length == 0 || !matters[0]) return nullptr;

    // Search only the parts of the exe that hold code (executable sections such as ".text").
    BYTE* base = (BYTE*)module;
    IMAGE_DOS_HEADER* dosHeader = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* ntHeaders = (IMAGE_NT_HEADERS*)(base + dosHeader->e_lfanew);
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(ntHeaders);

    void* firstMatch = nullptr;
    int matches = 0;

    for (int s = 0; s < ntHeaders->FileHeader.NumberOfSections; ++s, ++section)
    {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;

        BYTE* start = base + section->VirtualAddress;
        SIZE_T size = section->Misc.VirtualSize;
        if (size < (SIZE_T)length) continue;

        BYTE* end = start + size - length;
        for (BYTE* at = start; at <= end; ++at)
        {
            if (*at != bytes[0]) continue;

            bool equal = true;
            for (int i = 1; i < length; ++i)
            {
                if (matters[i] && at[i] != bytes[i]) { equal = false; break; }
            }
            if (!equal) continue;

            if (matches == 0) firstMatch = at;
            ++matches;
        }
    }

    if (matchCount) *matchCount = matches;
    return firstMatch;
}
