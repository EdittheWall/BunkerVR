#pragma once

// The payload: the mod's files packed into BunkerVR Setup. pack.exe writes it;
// setup.exe reads it from its own resources.
//
//   PayloadHeader
//   then for each file: PayloadEntry, its path (UTF-8, '/' between folders,
//   relative to the game's folder), then its data (packed or stored).
//
// An entry is either one of the mod's own files or a patch to one of the
// game's files; a patch's path is its target's with ".bvrpatch" added.
// Each entry's SHA-256 is of its unpacked contents, checked by setup.exe after
// unpacking. A whole file is checked again after writing; a patch's result is
// checked against the hash the patch itself names. Packed with Windows' own
// LZMS (the Compression API, Cabinet.dll); a file that LZMS would not make
// smaller is stored as it is.

#include <stdint.h>

static const char kPayloadMagic[8] = { 'B', 'V', 'R', 'P', 'A', 'C', 'K', '1' };
static const int kPayloadResource = 101; // RCDATA in setup.rc

enum PayloadMethod : uint8_t
{
    kPayloadStored = 0,
    kPayloadLzms = 1,
};

#pragma pack(push, 1)
struct PayloadHeader
{
    char magic[8];
    uint32_t fileCount;
    uint64_t rawTotal;            // the entries' unpacked sizes added up (a patch counts as its own size)
    char modVersion[16];          // for example "2.01"
    uint8_t gameExeSha256[32];    // AmnesiaTheBunker.exe that the mod was made for (all 0: not known)
};

struct PayloadEntry
{
    uint16_t pathBytes;
    uint8_t method;               // PayloadMethod
    uint64_t rawSize;
    uint64_t packedSize;          // as stored here (rawSize when stored)
    uint8_t sha256[32];
};
#pragma pack(pop)
