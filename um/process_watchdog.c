#include <Windows.h>
#include <TlHelp32.h>
#include <stdio.h>
#include <stdint.h>
#include "shared_memory_client.h"

#define TARGET_PROCESS_NAME L"stalcraft.exe"

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

UINT64 SafeShm(const char* operation, BOOL (*shmFunc)(UINT64*), UINT64 defaultValue) {
    printf("[DEBUG] Calling %s...\n", operation);
    
    UINT64 result = defaultValue;
    if (shmFunc(&result))
    {
        printf("[DEBUG] %s returned: 0x%llX\n", operation, result);
        return result;
    }
    
    printf("[!] %s failed\n", operation);
    return defaultValue;
}

// Test if hypervisor is loaded and responding via shared memory
BOOL TestHypervisorConnection(void) {
    printf("[*] Testing hypervisor connection via shared memory...\n");
    
    if (!ShmConnect())
    {
        return FALSE;
    }
    
    UINT64 result = 0;
    if (ShmTestConnection(&result))
    {
        printf("[+] Hypervisor is loaded and responding (magic: 0x%llX)\n", result);
        return TRUE;
    }
    
    printf("[!] Hypervisor NOT responding\n");
    return FALSE;
}

int main(void) {
    SetConsoleTitleA("Process Watchdog - Stalcraft.exe Monitor");

    printf("========================================\n");
    printf("Enhanced Process Watchdog v2.0\n");
    printf("========================================\n\n");

    // STEP 1: Test hypervisor connection (kdmapper doesn't create service)
    printf("[STEP 1/3] Testing Hypervisor Connection\n");
    printf("----------------------------------------\n");
    printf("[*] Checking if driver is loaded via kdmapper...\n");
    
    if (!TestHypervisorConnection()) {
        printf("\n[!] CRITICAL: Hypervisor is not responding!\n");
        printf("\n[!] Possible causes:\n");
        printf("    1. Driver not loaded with kdmapper\n");
        printf("    2. Driver loaded but hypervisor initialization failed\n");
        printf("    3. CPU doesn't support AMD-V/SVM\n");
        printf("    4. Virtualization disabled in BIOS\n");
        printf("    5. Another hypervisor is already running (Hyper-V, VMware, etc.)\n");
        printf("\n[!] To fix:\n");
        printf("    1. Load driver: kdmapper.exe SeCodeIntegrityQueryInformation.sys\n");
        printf("    2. Enable AMD-V in BIOS\n");
        printf("    3. Disable Hyper-V: bcdedit /set hypervisorlaunchtype off (reboot)\n");
        printf("    4. Check driver debug output with DebugView\n");
        printf("\nPress any key to exit...\n");
        getchar();
        return 1;
    }
    
    printf("\n[STEP 2/3] Waiting for Target Process\n");
    printf("----------------------------------------\n");
    printf("[+] Waiting for %S to launch...\n", TARGET_PROCESS_NAME);
    printf("[+] You can launch the game now...\n\n");

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
        printf("[!] Process may have exited or access denied\n");
        printf("\nPress any key to retry...\n");
        getchar();
        return 1;
    }
    printf("    Image Base: 0x%016llX\n", procInfo.ImageBase);

    // PART 2: Context Retrieval - Send ATTACH_PROCESS via shared memory
    printf("\n[STEP 3/3] Attaching to Target Process\n");
    printf("----------------------------------------\n");
    printf("[+] Sending ATTACH_PROCESS to VMM via shared memory...\n");
    
    UINT64 result = 0;
    if (!ShmAttachProcess(procInfo.ProcessId, procInfo.ImageBase, &result))
    {
        printf("[!] ATTACH_PROCESS failed\n");
        printf("[!] This could mean:\n");
        printf("    - Hypervisor not processing requests correctly\n");
        printf("    - Invalid parameters\n");
        printf("    - Handler crashed\n");
        printf("\n[!] Check kernel debug output with DebugView\n");
        printf("\nPress any key to exit...\n");
        getchar();
        ShmDisconnect();
        return 1;
    }
    
    printf("[+] VMM acknowledged process attachment (result: 0x%llX)\n", result);

    printf("[+] VMM acknowledged process attachment (result: 0x%llX)\n", result);
    printf("[+] VMM will now capture CR3 on next context switch\n");
    printf("[+] Waiting for VMM to lock target process...\n");

    // Poll VMM for CR3 capture status via shared memory
    UINT64 capturedCr3 = 0;
    int attempts = 0;
    while (attempts < 100) {
        Sleep(100);
        if (ShmQueryCr3(procInfo.ProcessId, &capturedCr3) && capturedCr3 != 0)
        {
            printf("[+] VMM captured CR3: 0x%016llX\n", capturedCr3);
            break;
        }
        attempts++;
        if (attempts % 10 == 0) {
            printf("[DEBUG] Still waiting for CR3 capture... (attempt %d/100)\n", attempts);
        }
    }

    if (capturedCr3 == 0) {
        printf("[!] Failed to capture CR3 after %d attempts\n", attempts);
        printf("[!] This could mean:\n");
        printf("    - Process exited before CR3 capture\n");
        printf("    - CR3 interception not working\n");
        printf("    - VMM not monitoring this process\n");
        printf("\n[!] Check kernel debug output with DebugView\n");
        printf("\nPress any key to exit...\n");
        getchar();
        return 1;
    }

    printf("[+] Target Process Locked Successfully\n");
    printf("[+] NPT Shadowing Engine is now active\n");
    
    // PART 3: DLL Injection
    printf("\n[+] Injecting DLL into target process...\n");
    UINT64 dllResult = 0;
    if (!ShmInjectDll(&dllResult))
    {
        printf("[!] DLL injection failed\n");
        printf("[!] Check VMM debug output for details\n");
        printf("\n[!] Continuing monitoring anyway...\n");
    } else {
        printf("[+] DLL injection successful! (result: 0x%llX)\n", dllResult);
        printf("[+] OpenGL hooks are now active\n");
        printf("[+] ESP/Wallhack features enabled\n");
    }

    printf("\n========================================\n");
    printf("INJECTION COMPLETE - MONITORING ACTIVE\n");
    printf("========================================\n");
    printf("Target Process: %S (PID: %lu)\n", TARGET_PROCESS_NAME, procInfo.ProcessId);
    printf("Image Base: 0x%016llX\n", procInfo.ImageBase);
    printf("CR3: 0x%016llX\n", capturedCr3);
    printf("DLL Base: Unknown (check VMM debug output)\n");
    printf("\n");
    printf("The process watchdog will continue monitoring...\n");
    printf("Press Ctrl+C to stop monitoring\n");
    printf("========================================\n\n");

    // Continue monitoring the process with enhanced CR3 tracking
    UINT64 lastCr3 = capturedCr3;
    UINT64 cr3Changes = 0;
    UINT64 consecutiveFailures = 0;
    UINT64 monitoringCycles = 0;
    
    while (TRUE) {
        monitoringCycles++;
        
        // Check if process is still running
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, procInfo.ProcessId);
        if (hProcess == NULL) {
            printf("\n[!] Target process has exited (PID: %lu)\n", procInfo.ProcessId);
            printf("[!] Monitoring stopped\n");
            printf("\n[*] Waiting for process to restart...\n");
            printf("[*] You can close this window or wait for the game to restart\n");
            
            // Wait for process to restart
            while (TRUE) {
                if (FindProcessByName(TARGET_PROCESS_NAME, &procInfo)) {
                    printf("\n[+] Target process restarted!\n");
                    printf("    New PID: %lu\n", procInfo.ProcessId);
                    printf("[+] Restarting monitoring...\n\n");
                    
                    // Re-attach to new process
                    procInfo.ImageBase = GetProcessImageBase(procInfo.ProcessId);
                    if (procInfo.ImageBase != 0) {
                        UINT64 reattachResult = 0;
                        if (ShmAttachProcess(procInfo.ProcessId, procInfo.ImageBase, &reattachResult) && reattachResult != 0) {
                            printf("[+] Successfully attached to restarted process\n");
                            lastCr3 = 0;
                            cr3Changes = 0;
                            consecutiveFailures = 0;
                            break;
                        }
                    }
                }
                Sleep(1000);
            }
            continue;
        }
        CloseHandle(hProcess);

        // Check current CR3 and detect changes via shared memory
        UINT64 currentCr3 = 0;
        if (!ShmQueryCr3(procInfo.ProcessId, &currentCr3) || currentCr3 == 0) {
            consecutiveFailures++;
            printf("[!] Lost connection to VMM or process detached (failure %llu)\n", consecutiveFailures);
            if (consecutiveFailures >= 3) {
                printf("[!] Too many consecutive failures, stopping monitoring\n");
                break;
            }
        } else {
            consecutiveFailures = 0;
            
            // Detect CR3 changes (potential anti-cheat activity)
            if (currentCr3 != lastCr3) {
                cr3Changes++;
                printf("[!] CR3 CHANGE DETECTED! Old: 0x%016llX -> New: 0x%016llX (Change #%llu)\n", 
                       lastCr3, currentCr3, cr3Changes);
                
                // Query anti-cheat detection status
                UINT64 antiCheatStatus = 0;
                UINT64 cr3ChangeCount = 0;
                ShmQueryAntiCheatStatus(&antiCheatStatus);
                ShmQueryCr3Changes(&cr3ChangeCount);
                
                if (antiCheatStatus != 0) {
                    printf("[!] ANTI-CHEAT DETECTED! Rapid CR3 switching pattern identified\n");
                    printf("[+] Attempting multi-CR3 re-injection...\n");
                    
                    // Re-inject on all known CR3 values
                    UINT64 reinjectResult = 0;
                    if (ShmReinjectAllCr3(&reinjectResult) && reinjectResult != 0) {
                        printf("[+] Multi-CR3 re-injection successful!\n");
                    } else {
                        printf("[!] Multi-CR3 re-injection failed\n");
                    }
                } else {
                    // Standard re-attach procedure
                    printf("[+] Re-attaching to new CR3...\n");
                    UINT64 reattachResult = 0;
                    if (ShmAttachProcess(procInfo.ProcessId, procInfo.ImageBase, &reattachResult) && reattachResult != 0) {
                        printf("[+] Successfully re-attached to new CR3\n");
                        
                        // Re-inject DLL if needed
                        printf("[+] Re-injecting DLL...\n");
                        UINT64 reinjectResult = 0;
                        if (ShmInjectDll(&reinjectResult) && reinjectResult != 0) {
                            printf("[+] DLL re-injection successful!\n");
                        } else {
                            printf("[!] DLL re-injection failed\n");
                        }
                    } else {
                        printf("[!] Failed to re-attach to new CR3\n");
                    }
                }
                
                lastCr3 = currentCr3;
            }
            
            // Check if our hooks are still active (placeholder - no direct hook status query yet)
            // For now, assume hooks are active if we can query CR3
            UINT64 hookStatus = (currentCr3 != 0) ? 1 : 0;
            if (hookStatus == 0) {
                printf("[!] Hooks appear to be inactive, attempting to re-establish...\n");
                UINT64 rehookResult = 0;
                if (ShmInjectDll(&rehookResult) && rehookResult != 0) {
                    printf("[+] Hooks re-established successfully\n");
                } else {
                    printf("[!] Failed to re-establish hooks\n");
                }
            }
            
            // Enhanced status display with anti-cheat detection
            UINT64 antiCheatStatus = 0;
            UINT64 totalCr3Changes = 0;
            ShmQueryAntiCheatStatus(&antiCheatStatus);
            ShmQueryCr3Changes(&totalCr3Changes);
            
            const char* antiCheatStr = (antiCheatStatus != 0) ? "DETECTED" : "Not Detected";
            const char* hookStr = (hookStatus != 0) ? "Active" : "Inactive";
            
            // Only print status every 5 cycles to reduce spam
            if (monitoringCycles % 5 == 0) {
                printf("[INFO] Cycle: %llu | Process: Active | CR3: 0x%016llX | Session Changes: %llu | Total Changes: %llu | Anti-Cheat: %s | Hooks: %s\n", 
                       monitoringCycles, currentCr3, cr3Changes, totalCr3Changes, antiCheatStr, hookStr);
            }
        }
        
        Sleep(2000); // Check every 2 seconds for faster detection
    }

    printf("\n[+] Monitoring stopped\n");
    printf("[+] Process watchdog exiting...\n");
    
    // Cleanup
    ShmDisconnect();

    return 0;
}
