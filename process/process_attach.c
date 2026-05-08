#include <ntifs.h>
#include "../../include/process_manager.h"
#include "../../include/npt.h"
#include "../../include/vcpu.h"

// PART 1: CR3 CAPTURE LOGIC

#define MAX_CR3_HISTORY 16

typedef struct _CR3_HISTORY_ENTRY {
    UINT64 Cr3Value;
    UINT64 Timestamp;
    UINT32 SwitchCount;
} CR3_HISTORY_ENTRY;

typedef struct _ATTACHED_PROCESS_STATE {
    HANDLE TargetPid;
    UINT64 TargetImageBase;
    UINT64 CapturedCr3;
    UINT16 CapturedAsid;
    BOOLEAN IsAttached;
    BOOLEAN Cr3Captured;
    
    // Enhanced CR3 tracking for anti-cheat detection
    CR3_HISTORY_ENTRY Cr3History[MAX_CR3_HISTORY];
    UINT32 Cr3HistoryIndex;
    UINT32 TotalCr3Changes;
    UINT64 LastCr3ChangeTime;
    BOOLEAN AntiCheatDetected;
    UINT64 PrimaryCr3;
    UINT64 SecondaryCr3;
} ATTACHED_PROCESS_STATE;

static ATTACHED_PROCESS_STATE g_AttachedProcess = { 0 };

NTSTATUS ProcessAttachTarget(HANDLE Pid, UINT64 ImageBase) {
    if (g_AttachedProcess.IsAttached) {
        DbgPrint("[VMM] Process already attached\n");
        return STATUS_ALREADY_REGISTERED;
    }

    g_AttachedProcess.TargetPid = Pid;
    g_AttachedProcess.TargetImageBase = ImageBase;
    g_AttachedProcess.IsAttached = TRUE;
    g_AttachedProcess.Cr3Captured = FALSE;
    g_AttachedProcess.CapturedCr3 = 0;
    g_AttachedProcess.CapturedAsid = 0;
    
    // Initialize enhanced tracking
    RtlZeroMemory(g_AttachedProcess.Cr3History, sizeof(g_AttachedProcess.Cr3History));
    g_AttachedProcess.Cr3HistoryIndex = 0;
    g_AttachedProcess.TotalCr3Changes = 0;
    g_AttachedProcess.LastCr3ChangeTime = 0;
    g_AttachedProcess.AntiCheatDetected = FALSE;
    g_AttachedProcess.PrimaryCr3 = 0;
    g_AttachedProcess.SecondaryCr3 = 0;

    DbgPrint("[VMM] Process attachment initiated: PID=%lu, ImageBase=0x%llX\n", 
             (ULONG)Pid, ImageBase);

    return STATUS_SUCCESS;
}

BOOLEAN ProcessIsTargetAttached(void) {
    return g_AttachedProcess.IsAttached;
}

BOOLEAN ProcessIsCr3Captured(void) {
    return g_AttachedProcess.Cr3Captured;
}

UINT64 ProcessGetCapturedCr3(void) {
    return g_AttachedProcess.CapturedCr3;
}

// PART 2: CR3 INTERCEPTION
// Called from VM-Exit handler on MOV CR3 or context switch
VOID ProcessInterceptCr3Load(VCPU* Vcpu, UINT64 NewCr3) {
    if (!g_AttachedProcess.IsAttached) {
        return;
    }

    // Check if this CR3 belongs to our target process
    HANDLE currentPid = PsGetCurrentProcessId();
    
    if (currentPid == g_AttachedProcess.TargetPid) {
        UINT64 currentTime = KeQueryPerformanceCounter(NULL).QuadPart;
        
        // Track CR3 changes for anti-cheat detection
        if (g_AttachedProcess.Cr3Captured && NewCr3 != g_AttachedProcess.CapturedCr3) {
            g_AttachedProcess.TotalCr3Changes++;
            
            // Add to history
            UINT32 index = g_AttachedProcess.Cr3HistoryIndex % MAX_CR3_HISTORY;
            g_AttachedProcess.Cr3History[index].Cr3Value = NewCr3;
            g_AttachedProcess.Cr3History[index].Timestamp = currentTime;
            g_AttachedProcess.Cr3History[index].SwitchCount = g_AttachedProcess.TotalCr3Changes;
            g_AttachedProcess.Cr3HistoryIndex++;
            
            DbgPrint("[VMM] CR3 CHANGE DETECTED: Old=0x%llX -> New=0x%llX (Change #%u)\n", 
                     g_AttachedProcess.CapturedCr3, NewCr3, g_AttachedProcess.TotalCr3Changes);
            
            // Detect anti-cheat pattern (rapid CR3 switching)
            if (g_AttachedProcess.LastCr3ChangeTime != 0) {
                UINT64 timeDelta = currentTime - g_AttachedProcess.LastCr3ChangeTime;
                if (timeDelta < 1000000) { // Less than ~100ms between switches
                    g_AttachedProcess.AntiCheatDetected = TRUE;
                    DbgPrint("[VMM] ANTI-CHEAT DETECTED: Rapid CR3 switching (delta=%llu)\n", timeDelta);
                }
            }
            
            // Track primary and secondary CR3 values
            if (g_AttachedProcess.PrimaryCr3 == 0) {
                g_AttachedProcess.PrimaryCr3 = g_AttachedProcess.CapturedCr3;
                g_AttachedProcess.SecondaryCr3 = NewCr3;
            } else if (NewCr3 != g_AttachedProcess.PrimaryCr3 && NewCr3 != g_AttachedProcess.SecondaryCr3) {
                DbgPrint("[VMM] NEW CR3 DETECTED: 0x%llX (Primary: 0x%llX, Secondary: 0x%llX)\n", 
                         NewCr3, g_AttachedProcess.PrimaryCr3, g_AttachedProcess.SecondaryCr3);
            }
            
            g_AttachedProcess.LastCr3ChangeTime = currentTime;
        }
        
        // Update current CR3
        g_AttachedProcess.CapturedCr3 = NewCr3;
        g_AttachedProcess.CapturedAsid = (UINT16)(VmcbControl(Vcpu->Vmcb)->GuestAsid & 0xFFFF);
        
        if (!g_AttachedProcess.Cr3Captured) {
            g_AttachedProcess.Cr3Captured = TRUE;
            g_AttachedProcess.PrimaryCr3 = NewCr3;
            DbgPrint("[VMM] Target Process Locked: CR3=0x%llX, ASID=%u\n", 
                     NewCr3, g_AttachedProcess.CapturedAsid);
        }

        // Enable NPT Shadowing for this process (handle multiple CR3s)
        NptSetTargetProcess(&Vcpu->Npt, NewCr3, g_AttachedProcess.CapturedAsid);
    }
}

// Alternative: Parse EPROCESS manually if MOV CR3 intercept is not available
NTSTATUS ProcessCaptureCr3FromEprocess(HANDLE Pid, UINT64* OutCr3, UINT16* OutAsid) {
    PEPROCESS Process = NULL;
    NTSTATUS status = PsLookupProcessByProcessId(Pid, &Process);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // DirectoryTableBase is at offset 0x28 in EPROCESS (Windows 10/11 x64)
    // This offset may vary by Windows version
    UINT64* pDirBase = (UINT64*)((UINT8*)Process + 0x28);
    *OutCr3 = *pDirBase;
    
    // ASID is typically derived from process ID or assigned sequentially
    // For simplicity, use lower 16 bits of PID
    *OutAsid = (UINT16)((UINT64)Pid & 0xFFFF);

    ObDereferenceObject(Process);

    DbgPrint("[VMM] Captured CR3 from EPROCESS: CR3=0x%llX, ASID=%u\n", *OutCr3, *OutAsid);

    return STATUS_SUCCESS;
}

NTSTATUS ProcessManualCaptureCr3(HANDLE Pid) {
    if (!g_AttachedProcess.IsAttached) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    UINT64 cr3 = 0;
    UINT16 asid = 0;
    NTSTATUS status = ProcessCaptureCr3FromEprocess(Pid, &cr3, &asid);
    
    if (NT_SUCCESS(status)) {
        g_AttachedProcess.CapturedCr3 = cr3;
        g_AttachedProcess.CapturedAsid = asid;
        g_AttachedProcess.Cr3Captured = TRUE;

        DbgPrint("[VMM] Target Process Locked: 0x%llX\n", cr3);
    }

    return status;
}

// PART 3: ANTI-CHEAT DETECTION AND MULTI-CR3 SUPPORT

BOOLEAN ProcessIsAntiCheatDetected(void) {
    return g_AttachedProcess.AntiCheatDetected;
}

UINT32 ProcessGetCr3ChangeCount(void) {
    return g_AttachedProcess.TotalCr3Changes;
}

UINT64 ProcessGetPrimaryCr3(void) {
    return g_AttachedProcess.PrimaryCr3;
}

UINT64 ProcessGetSecondaryCr3(void) {
    return g_AttachedProcess.SecondaryCr3;
}

VOID ProcessGetCr3History(CR3_HISTORY_ENTRY* OutHistory, UINT32 MaxEntries, UINT32* OutCount) {
    UINT32 count = min(MaxEntries, min(g_AttachedProcess.Cr3HistoryIndex, MAX_CR3_HISTORY));
    *OutCount = count;
    
    for (UINT32 i = 0; i < count; i++) {
        UINT32 index = (g_AttachedProcess.Cr3HistoryIndex - count + i) % MAX_CR3_HISTORY;
        OutHistory[i] = g_AttachedProcess.Cr3History[index];
    }
}

// Force re-injection on all known CR3 values
NTSTATUS ProcessReinjectOnAllCr3s(VCPU* Vcpu) {
    if (!g_AttachedProcess.IsAttached) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    
    DbgPrint("[VMM] Re-injecting on all known CR3 values...\n");
    
    // Re-inject on primary CR3
    if (g_AttachedProcess.PrimaryCr3 != 0) {
        DbgPrint("[VMM] Re-injecting on Primary CR3: 0x%llX\n", g_AttachedProcess.PrimaryCr3);
        NptSetTargetProcess(&Vcpu->Npt, g_AttachedProcess.PrimaryCr3, g_AttachedProcess.CapturedAsid);
    }
    
    // Re-inject on secondary CR3
    if (g_AttachedProcess.SecondaryCr3 != 0 && g_AttachedProcess.SecondaryCr3 != g_AttachedProcess.PrimaryCr3) {
        DbgPrint("[VMM] Re-injecting on Secondary CR3: 0x%llX\n", g_AttachedProcess.SecondaryCr3);
        NptSetTargetProcess(&Vcpu->Npt, g_AttachedProcess.SecondaryCr3, g_AttachedProcess.CapturedAsid);
    }
    
    // Re-inject on recent CR3 values from history
    for (UINT32 i = 0; i < min(g_AttachedProcess.Cr3HistoryIndex, MAX_CR3_HISTORY); i++) {
        UINT64 historyCr3 = g_AttachedProcess.Cr3History[i].Cr3Value;
        if (historyCr3 != g_AttachedProcess.PrimaryCr3 && historyCr3 != g_AttachedProcess.SecondaryCr3) {
            DbgPrint("[VMM] Re-injecting on History CR3[%u]: 0x%llX\n", i, historyCr3);
            NptSetTargetProcess(&Vcpu->Npt, historyCr3, g_AttachedProcess.CapturedAsid);
        }
    }
    
    return STATUS_SUCCESS;
}
