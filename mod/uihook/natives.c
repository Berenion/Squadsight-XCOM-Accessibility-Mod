// Runtime discovery of XCOM's UnrealScript native-function table.
//
// Both executables keep a { const char* name; void* func; } array in .data,
// one entry per native, with the name being the two C++ symbol halves
// concatenated: "UGFxMoviePlayer" "execActionScriptVoid".  Class prefix is
// U for UObject subclasses and A for Actor subclasses.
//
// Scanning for it at runtime (rather than baking in an address) is what lets
// one build of this DLL serve both EU and EW, whose addresses differ:
//
//     UGFxMoviePlayer::ActionScriptVoid   EW 0x008565d0   EU 0x00950f20
//     AUI_FxsPanel::Invoke                EW 0x009469e0   EU 0x00e644d0

#include "natives.h"
#include <string.h>

// Shared with names.c, which scans .data for the name table.
int module_section(HMODULE mod, const char* want, uint8_t** base, size_t* size)
{
    uint8_t* m = (uint8_t*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)m;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(m + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        char name[9] = { 0 };
        memcpy(name, sec[i].Name, 8);
        if (strcmp(name, want) == 0) {
            *base = m + sec[i].VirtualAddress;
            // Prefer the mapped virtual size; fall back to the raw size.
            *size = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize
                                            : sec[i].SizeOfRawData;
            return 1;
        }
    }
    return 0;
}

// A native table name: ^[UA][A-Z_][A-Za-z0-9_]*exec[A-Z][A-Za-z0-9_]*$
static int valid_name(const char* s, const char* limit)
{
    if (s + 8 >= limit) return 0;
    if (s[0] != 'U' && s[0] != 'A') return 0;
    if (!((s[1] >= 'A' && s[1] <= 'Z') || s[1] == '_')) return 0;

    const char* exec = NULL;
    const char* p = s + 1;
    for (; p < limit && *p; p++) {
        char c = *p;
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '_';
        if (!ok) return 0;
        if (!exec && p[0] == 'e' && p + 4 < limit &&
            p[1] == 'x' && p[2] == 'e' && p[3] == 'c' &&
            p[4] >= 'A' && p[4] <= 'Z')
            exec = p;
    }
    if (p >= limit || *p) return 0;             // unterminated
    return exec && p - s < 128;
}

int natives_scan(HMODULE mod, NativeEntry* out, int max)
{
    uint8_t *data, *text, *rdata;
    size_t data_sz, text_sz, rdata_sz;
    if (!module_section(mod, ".data", &data, &data_sz)) return 0;
    if (!module_section(mod, ".text", &text, &text_sz)) return 0;
    if (!module_section(mod, ".rdata", &rdata, &rdata_sz)) return 0;

    int n = 0;
    // Entries are 8 bytes but are not guaranteed 8-byte aligned, so step by 4.
    for (size_t off = 0; off + 8 <= data_sz && n < max; off += 4) {
        uint32_t name_p = *(uint32_t*)(data + off);
        uint32_t func_p = *(uint32_t*)(data + off + 4);

        if (func_p < (uint32_t)(uintptr_t)text ||
            func_p >= (uint32_t)(uintptr_t)(text + text_sz)) continue;
        if (name_p < (uint32_t)(uintptr_t)rdata ||
            name_p >= (uint32_t)(uintptr_t)(rdata + rdata_sz)) continue;

        const char* name = (const char*)(uintptr_t)name_p;
        if (!valid_name(name, (const char*)(rdata + rdata_sz))) continue;

        out[n].name = name;
        out[n].func = (void*)(uintptr_t)func_p;
        n++;
    }
    return n;
}

void* natives_find(const NativeEntry* tbl, int n, const char* name)
{
    for (int i = 0; i < n; i++)
        if (strcmp(tbl[i].name, name) == 0) return tbl[i].func;
    return NULL;
}
