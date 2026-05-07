#include <ntifs.h>
#include "../../include/vcpu.h"
#include "../../include/process_manager.h"

#define VMMCALL_ATTACH_PROCESS 0x400
#define VMMCALL_INSTALL_NPT_HOOK 0x401
#define VMMCALL_REMOVE_NPT_HOOK 0x402

extern NTSTATUS ProcessAttachTarget(HANDLE Pid, UINT64 ImageBase);
extern NTSTATUS ProcessManualCaptureCr3(HANDLE Pid);
extern NTSTATUS NptInstallInlineHook(NPT_STATE* State, UINT64 TargetGva, UINT64 TargetCr3, 
                                      const UINT8* HookBytes, SIZE_T HookSize);
extern VOID NptRemoveInlineHook(NPT_STATE* State);
extern UINT64 ProcessGetCapturedCr3(void);

// VMMCALL Handler: Attach to Target Process
UINT64 HypercallAttachProcess(VCPU* Vcpu, UINT64 Pid, UINT64 ImageBase) {
    DbgPrint("[VMM] VMMCALL_ATTACH_PROCESS: PID=%llu, ImageBase=0x%llX\n", Pid, ImageBase);

    NTSTATUS status = ProcessAttachTarget((HANDLE)Pid, ImageBase);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] ProcessAttachTarget failed: 0x%08X\n", status);
        return 0;
    }

    // Optionally, manually capture CR3 immediately instead of waiting for MOV CR3
    status = ProcessManualCaptureCr3((HANDLE)Pid);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] ProcessManualCaptureCr3 failed: 0x%08X\n", status);
        // Not critical - we can still wait for MOV CR3 intercept
    }

    return 1; // Success
}

// VMMCALL Handler: Install NPT-Based Inline Hook
UINT64 HypercallInstallNptHook(VCPU* Vcpu, UINT64 TargetGva, UINT64 HookBytesGva, UINT64 HookSize) {
    DbgPrint("[VMM] VMMCALL_INSTALL_NPT_HOOK: GVA=0x%llX, HookSize=%llu\n", TargetGva, HookSize);

    UINT64 targetCr3 = ProcessGetCapturedCr3();
    if (targetCr3 == 0) {
        DbgPrint("[VMM] Target CR3 not captured yet\n");
        return 0;
    }

    // Read hook bytes from guest memory
    UINT8 hookBytes[64] = { 0 };
    if (HookSize > sizeof(hookBytes)) {
        DbgPrint("[VMM] Hook size too large: %llu\n", HookSize);
        return 0;
    }

    // TODO: Read from guest memory at HookBytesGva
    // For now, use a simple JMP hook (E9 XX XX XX XX)
    // This should be provided by user-mode caller

    NTSTATUS status = NptInstallInlineHook(&Vcpu->Npt, TargetGva, targetCr3, hookBytes, HookSize);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] NptInstallInlineHook failed: 0x%08X\n", status);
        return 0;
    }

    return 1; // Success
}

// VMMCALL Handler: Remove NPT Hook
UINT64 HypercallRemoveNptHook(VCPU* Vcpu) {
    DbgPrint("[VMM] VMMCALL_REMOVE_NPT_HOOK\n");
    NptRemoveInlineHook(&Vcpu->Npt);
    return 1;
}

// Main Hypercall Dispatcher (extends existing handler)
UINT64 HandleHypercallExtended(VCPU* Vcpu, UINT64 Code, UINT64 Arg1, UINT64 Arg2, UINT64 Arg3) {
    switch (Code) {
        case VMMCALL_ATTACH_PROCESS:
            return HypercallAttachProcess(Vcpu, Arg1, Arg2);

        case VMMCALL_INSTALL_NPT_HOOK:
            return HypercallInstallNptHook(Vcpu, Arg1, Arg2, Arg3);

        case VMMCALL_REMOVE_NPT_HOOK:
            return HypercallRemoveNptHook(Vcpu);

        default:
            DbgPrint("[VMM] Unknown hypercall: 0x%llX\n", Code);
            return 0;
    }
}
