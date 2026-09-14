// Minimal LoadLibrary injector for the UI hook.
//
//   inject.exe <XComEW.exe|XComGame.exe> <full\path\to\xcom_uihook.dll>
//
// Both the injector and the DLL must be 32-bit to match the game.

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static DWORD find_pid(const char* exe)
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

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: inject <process.exe> <full path to dll>\n");
        return 2;
    }

    char dll[MAX_PATH];
    if (!GetFullPathNameA(argv[2], MAX_PATH, dll, NULL)) {
        fprintf(stderr, "cannot resolve dll path\n");
        return 2;
    }
    if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "dll not found: %s\n", dll);
        return 2;
    }

    DWORD pid = find_pid(argv[1]);
    if (!pid) { fprintf(stderr, "process not running: %s\n", argv[1]); return 1; }

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) { fprintf(stderr, "OpenProcess failed (%lu) -- try running as admin\n",
                         GetLastError()); return 1; }

    size_t len = strlen(dll) + 1;
    void* remote = VirtualAllocEx(proc, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(proc, remote, dll, len, NULL)) {
        fprintf(stderr, "failed to stage dll path (%lu)\n", GetLastError());
        CloseHandle(proc); return 1;
    }

    // kernel32 is at the same base in every process on a given boot, so the
    // local address of LoadLibraryA is valid in the target too.
    LPTHREAD_START_ROUTINE loader =
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE th = CreateRemoteThread(proc, NULL, 0, loader, remote, 0, NULL);
    if (!th) {
        fprintf(stderr, "CreateRemoteThread failed (%lu)\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc); return 1;
    }

    WaitForSingleObject(th, 10000);
    DWORD ok = 0;
    GetExitCodeThread(th, &ok);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(th);
    CloseHandle(proc);

    if (!ok) { fprintf(stderr, "LoadLibrary returned NULL in target\n"); return 1; }
    printf("injected into pid %lu (module %p)\n", pid, (void*)(uintptr_t)ok);
    printf("log: xcom_uihook.log next to the game exe\n");
    return 0;
}
