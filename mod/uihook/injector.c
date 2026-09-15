#include "injector.h"

#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

DWORD injector_find_pid(const char* exe)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe = { sizeof pe };
    DWORD pid = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, exe) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

DWORD injector_wait_for_pid(const char* exe, DWORD timeout_ms)
{
    DWORD start = GetTickCount();
    for (;;) {
        DWORD pid = injector_find_pid(exe);
        if (pid) return pid;
        if (GetTickCount() - start >= timeout_ms) return 0;
        Sleep(200);
    }
}

typedef struct { DWORD pid; BOOL found; } WindowSearch;

static BOOL CALLBACK on_window(HWND wnd, LPARAM param)
{
    WindowSearch* search = (WindowSearch*)param;
    DWORD owner = 0;
    GetWindowThreadProcessId(wnd, &owner);
    if (owner != search->pid) return TRUE;
    if (!IsWindowVisible(wnd)) return TRUE;
    // The splash comes up titled, but so does the game window; what separates
    // them from the engine's hidden helper windows is having a title at all.
    if (GetWindowTextLengthA(wnd) == 0) return TRUE;
    search->found = TRUE;
    return FALSE;
}

BOOL injector_has_window(DWORD pid)
{
    WindowSearch search = { pid, FALSE };
    EnumWindows(on_window, (LPARAM)&search);
    return search.found;
}

// The game is 32-bit, and so are we, so a process we can inject into must sit
// on the same side of WOW64 as this one.  Checking up front turns a silent
// no-op into a sentence naming the real problem.
static BOOL same_architecture(HANDLE proc)
{
    BOOL target_wow64 = FALSE, self_wow64 = FALSE;
    if (!IsWow64Process(proc, &target_wow64)) return TRUE;   // can't tell; proceed
    if (!IsWow64Process(GetCurrentProcess(), &self_wow64)) return TRUE;
    return target_wow64 == self_wow64;
}

static BOOL dll_is_x86(const char* dll)
{
    FILE* f = NULL;
    if (fopen_s(&f, dll, "rb") != 0 || !f) return FALSE;

    IMAGE_DOS_HEADER dos;
    DWORD signature;
    IMAGE_FILE_HEADER file_header;
    BOOL ok = FALSE;

    if (fread(&dos, sizeof dos, 1, f) == 1 && dos.e_magic == IMAGE_DOS_SIGNATURE &&
        fseek(f, dos.e_lfanew, SEEK_SET) == 0 &&
        fread(&signature, sizeof signature, 1, f) == 1 && signature == IMAGE_NT_SIGNATURE &&
        fread(&file_header, sizeof file_header, 1, f) == 1)
        ok = file_header.Machine == IMAGE_FILE_MACHINE_I386;

    fclose(f);
    return ok;
}

BOOL injector_inject(DWORD pid, const char* dll, char* err, size_t err_len)
{
    char full[MAX_PATH];
    if (!GetFullPathNameA(dll, MAX_PATH, full, NULL)) {
        sprintf_s(err, err_len, "Cannot resolve the path to %s.", dll);
        return FALSE;
    }
    if (GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) {
        sprintf_s(err, err_len, "The mod DLL is missing:\n%s\n\n"
                                "Build it with build.bat first.", full);
        return FALSE;
    }
    if (!dll_is_x86(full)) {
        sprintf_s(err, err_len, "%s is not a 32-bit DLL. XCOM is 32-bit, so the "
                                "mod must be built with vcvars32.", full);
        return FALSE;
    }

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) {
        sprintf_s(err, err_len, "Cannot open the game process (error %lu). "
                                "Try running the launcher as administrator.",
                  GetLastError());
        return FALSE;
    }
    if (!same_architecture(proc)) {
        sprintf_s(err, err_len, "The game process is 64-bit but the mod is 32-bit. "
                                "This is not the executable the mod expects.");
        CloseHandle(proc);
        return FALSE;
    }

    size_t len = strlen(full) + 1;
    void* remote = VirtualAllocEx(proc, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(proc, remote, full, len, NULL)) {
        sprintf_s(err, err_len, "Cannot write the DLL path into the game (error %lu).",
                  GetLastError());
        if (remote) VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return FALSE;
    }

    // kernel32 is at the same base in every process on a given boot, so the
    // local address of LoadLibraryA is valid in the target too.
    LPTHREAD_START_ROUTINE loader =
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE thread = CreateRemoteThread(proc, NULL, 0, loader, remote, 0, NULL);
    if (!thread) {
        sprintf_s(err, err_len, "Cannot start the loader thread in the game (error %lu).",
                  GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return FALSE;
    }

    WaitForSingleObject(thread, 10000);
    DWORD module = 0;
    GetExitCodeThread(thread, &module);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(thread);
    CloseHandle(proc);

    if (!module) {
        sprintf_s(err, err_len, "The game loaded no module. The DLL is present but "
                                "LoadLibrary rejected it -- a missing dependency, "
                                "most likely.");
        return FALSE;
    }
    sprintf_s(err, err_len, "%p", (void*)(UINT_PTR)module);
    return TRUE;
}
