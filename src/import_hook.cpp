#include "import_hook.h"

bool HookImport(HMODULE module, const char* dllName, const char* functionName,
                void* replacement, void** original)
{
    if (!module) return false;

    BYTE* base = (BYTE*)module;
    IMAGE_DOS_HEADER* dosHeader = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* ntHeaders = (IMAGE_NT_HEADERS*)(base + dosHeader->e_lfanew);

    IMAGE_DATA_DIRECTORY& importDir =
        ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress == 0) return false;

    // One descriptor per DLL the module imports from.
    IMAGE_IMPORT_DESCRIPTOR* dll = (IMAGE_IMPORT_DESCRIPTOR*)(base + importDir.VirtualAddress);
    for (; dll->Name != 0; ++dll)
    {
        if (lstrcmpiA((const char*)(base + dll->Name), dllName) != 0) continue;
        if (dll->OriginalFirstThunk == 0) return false;

        // Two parallel lists: the function names, and the addresses they resolved to.
        IMAGE_THUNK_DATA* name = (IMAGE_THUNK_DATA*)(base + dll->OriginalFirstThunk);
        IMAGE_THUNK_DATA* address = (IMAGE_THUNK_DATA*)(base + dll->FirstThunk);

        for (; name->u1.AddressOfData != 0; ++name, ++address)
        {
            if (IMAGE_SNAP_BY_ORDINAL(name->u1.Ordinal)) continue; // imported by number, no name

            IMAGE_IMPORT_BY_NAME* byName = (IMAGE_IMPORT_BY_NAME*)(base + name->u1.AddressOfData);
            if (lstrcmpA((const char*)byName->Name, functionName) != 0) continue;

            // The table is normally read-only, so unlock it for the write.
            DWORD oldProtection = 0;
            if (!VirtualProtect(&address->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtection))
                return false;

            *original = (void*)address->u1.Function;
            address->u1.Function = (ULONG_PTR)replacement;

            VirtualProtect(&address->u1.Function, sizeof(void*), oldProtection, &oldProtection);
            return true;
        }
    }
    return false;
}
