#include <Windows.h>
#include <TlHelp32.h>
#include <stdio.h>
#include <stdint.h>
#include "hypercall.h"

#define TARGET_PROCESS_NAME L"stalcraft.exe"
#define VMMCALL_ATTACH_PROCESS 0x400

typedef struct _PROCESS_INFO {
    DWORD ProcessId;
    UINT64 ImageBase;
    UINT64 DirectoryTableBase;
} PROCESS_INFO;

BOOL FindProcessByName(const WCHAR* processName, PROCESS_INFO* outInfo) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (!Process32FirstW(hSnapshot, &pe32)) {
        CloseHandle(hSnapshot);
        return FALSE;
    }

    do {
        if (_wcsicmp(pe32.szExeFile, processName) == 0) {
            outInfo->ProcessId = pe32.th32ProcessID;
            CloseHandle(hSnapshot);
            return TRUE;
        }
    } while (Process32NextW(hSnapshot, &pe32));

    CloseHandle(hSnapshot);
    return FALSE;
}

UINT64 GetProcessImageBase(DWORD pid) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    MODULEENTRY32W me32;
    me32.dwSize = sizeof(MODULEENTRY32W);

    if (Module32FirstW(hSnapshot, &me32)) {
        UINT64 baseAddr = (UINT64)me32.modBaseAddr;
        CloseHandle(hSnapshot);
        return baseAddr;
    }

    CloseHandle(hSnapshot);
    return 0;
}

UINT64 SafeVmcall(UINT64 code, UINT64 arg1, UINT64 arg2, UINT64 arg3) {
    __try {
        return hv_vmcall(code, arg1, arg2, arg3);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        printf("[!] VMMCALL 0x%llx faulted with 0x%08X\n", code, GetExceptionCode());
        return 0;
    }
}

int main(void) {
    SetConsoleTitleA("Process Watchdog - Stalcraft.exe Monitor");

    printf("[+] Process Watchdog Started\n");
    printf("[+] Waiting for %S to launch...\n", TARGET_PROCESS_NAME);

    PROCESS_INFO procInfo = { 0 };

    // PART 1: Wait for Process
    while (TRUE) {
        if (FindProcessByName(TARGET_PROCESS_NAME, &procInfo)) {
            printf("[+] Target process found!\n");
            printf("    PID: %lu\n", procInfo.ProcessId);
            break;
        }
        Sleep(500);
    }

    // Get Image Base
    procInfo.ImageBase = GetProcessImageBase(procInfo.ProcessId);
    if (procInfo.ImageBase == 0) {
        printf("[!] Failed to get image base\n");
        return 1;
    }
    printf("    Image Base: 0x%016llX\n", procInfo.ImageBase);

    // PART 2: Context Retrieval - Send VMMCALL_ATTACH_PROCESS
    printf("[+] Sending VMMCALL_ATTACH_PROCESS to VMM...\n");
    UINT64 result = SafeVmcall(VMMCALL_ATTACH_PROCESS, procInfo.ProcessId, procInfo.ImageBase, 0);
    
    if (result == 0) {
        printf("[!] VMMCALL_ATTACH_PROCESS failed\n");
        return 1;
    }

    printf("[+] VMM acknowledged process attachment\n");
    printf("[+] VMM will now capture CR3 on next context switch\n");
    printf("[+] Waiting for VMM to lock target process...\n");

    // Poll VMM for CR3 capture status
    UINT64 capturedCr3 = 0;
    int attempts = 0;
    while (attempts < 100) {
        Sleep(100);
        capturedCr3 = SafeVmcall(hv_vmcall_query_process_dirbase, procInfo.ProcessId, 0, 0);
        if (capturedCr3 != 0) {
            printf("[+] VMM captured CR3: 0x%016llX\n", capturedCr3);
            break;
        }
        attempts++;
    }

    if (capturedCr3 == 0) {
        printf("[!] Failed to capture CR3 after %d attempts\n", attempts);
        return 1;
    }

    printf("[+] Target Process Locked Successfully\n");
    printf("[+] NPT Shadowing Engine is now active\n");
    printf("\n[+] Process watchdog complete. Press Enter to exit...\n");
    getchar();

    return 0;
}
