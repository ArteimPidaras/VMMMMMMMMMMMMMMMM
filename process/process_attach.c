#include <ntifs.h>
#include "../../include/process_manager.h"
#include "../../include/npt.h"
#include "../../include/vcpu.h"

// PART 1: CR3 CAPTURE LOGIC

typedef struct _ATTACHED_PROCESS_STATE {
    HANDLE TargetPid;
    UINT64 TargetImageBase;
    UINT64 CapturedCr3;
    UINT16 CapturedAsid;
    BOOLEAN IsAttached;
    BOOLEAN Cr3Captured;
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
    if (!g_AttachedProcess.IsAttached || g_AttachedProcess.Cr3Captured) {
        return;
    }

    // Check if this CR3 belongs to our target process
    // We can verify by checking if the current PID matches
    HANDLE currentPid = PsGetCurrentProcessId();
    
    if (currentPid == g_AttachedProcess.TargetPid) {
        g_AttachedProcess.CapturedCr3 = NewCr3;
        g_AttachedProcess.CapturedAsid = (UINT16)(VmcbControl(Vcpu->Vmcb)->GuestAsid & 0xFFFF);
        g_AttachedProcess.Cr3Captured = TRUE;

        DbgPrint("[VMM] Target Process Locked: CR3=0x%llX, ASID=%u\n", 
                 NewCr3, g_AttachedProcess.CapturedAsid);

        // Enable NPT Shadowing for this process
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
