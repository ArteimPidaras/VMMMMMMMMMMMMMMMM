#include <ntifs.h>
#include "../../include/vcpu.h"
#include "../../include/process_manager.h"
#include "../../include/dll_injection.h"

#define VMMCALL_ATTACH_PROCESS 0x500
#define VMMCALL_INSTALL_NPT_HOOK 0x501
#define VMMCALL_REMOVE_NPT_HOOK 0x502
#define VMMCALL_INJECT_DLL 0x503
#define VMMCALL_REMOVE_DLL 0x504

extern NTSTATUS ProcessAttachTarget(HANDLE Pid, UINT64 ImageBase);
extern NTSTATUS ProcessManualCaptureCr3(HANDLE Pid);
extern NTSTATUS NptInstallInlineHook(NPT_STATE* State, UINT64 TargetGva, UINT64 TargetCr3, 
                                      const UINT8* HookBytes, SIZE_T HookSize);
extern VOID NptRemoveInlineHook(NPT_STATE* State);
extern UINT64 ProcessGetCapturedCr3(void);
extern VOID NptRemoveInlineHook(NPT_STATE* State);
extern UINT64 ProcessGetCapturedCr3(void);

// VMMCALL Handler: Attach to Target Process
UINT64 HypercallAttachProcess(VCPU* Vcpu, UINT64 Pid, UINT64 ImageBase) {
    DbgPrint("[VMM] ===========================================\n");
    DbgPrint("[VMM] VMMCALL_ATTACH_PROCESS CALLED\n");
    DbgPrint("[VMM] PID=%llu, ImageBase=0x%llX\n", Pid, ImageBase);
    DbgPrint("[VMM] ===========================================\n");

    NTSTATUS status = ProcessAttachTarget((HANDLE)Pid, ImageBase);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] ProcessAttachTarget failed: 0x%08X\n", status);
        DbgPrint("[VMM] Possible causes:\n");
        DbgPrint("[VMM] - Invalid PID or process not found\n");
        DbgPrint("[VMM] - Process already attached\n");
        DbgPrint("[VMM] - Memory allocation failure\n");
        return 0;
    }

    DbgPrint("[VMM] ProcessAttachTarget succeeded\n");

    // Optionally, manually capture CR3 immediately instead of waiting for MOV CR3
    status = ProcessManualCaptureCr3((HANDLE)Pid);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] ProcessManualCaptureCr3 failed: 0x%08X\n", status);
        DbgPrint("[VMM] Will wait for CR3 intercept instead\n");
        // Not critical - we can still wait for MOV CR3 intercept
    } else {
        DbgPrint("[VMM] ProcessManualCaptureCr3 succeeded\n");
    }

    DbgPrint("[VMM] VMMCALL_ATTACH_PROCESS returning SUCCESS (1)\n");
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

// VMMCALL Handler: Inject DLL into Target Process
UINT64 HypercallInjectDll(VCPU* Vcpu) {
    DbgPrint("[VMM] ===========================================\n");
    DbgPrint("[VMM] VMMCALL_INJECT_DLL CALLED\n");
    DbgPrint("[VMM] ===========================================\n");

    UINT64 targetCr3 = ProcessGetCapturedCr3();
    DbgPrint("[VMM] Retrieved target CR3: 0x%llX\n", targetCr3);
    
    if (targetCr3 == 0) {
        DbgPrint("[VMM] ERROR: Target CR3 not captured yet\n");
        DbgPrint("[VMM] Process attachment may have failed\n");
        DbgPrint("[VMM] Or CR3 interception not working\n");
        return 0;
    }

    DbgPrint("[VMM] Starting DLL injection process...\n");
    NTSTATUS status = InjectDllFromHypervisor(Vcpu, targetCr3);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] DLL injection failed: 0x%08X\n", status);
        DbgPrint("[VMM] Check embedded DLL integrity\n");
        DbgPrint("[VMM] Check target process modules\n");
        return 0;
    }

    DbgPrint("[VMM] DLL injection completed successfully!\n");
    DbgPrint("[VMM] VMMCALL_INJECT_DLL returning SUCCESS (1)\n");
    return 1; // Success
}

// VMMCALL Handler: Remove DLL Injection
UINT64 HypercallRemoveDll(VCPU* Vcpu) {
    DbgPrint("[VMM] VMMCALL_REMOVE_DLL\n");
    RemoveDllInjection(Vcpu);
    return 1;
}

// Main Hypercall Dispatcher (extends existing handler)
UINT64 HandleHypercallExtended(VCPU* Vcpu, UINT64 Code, UINT64 Arg1, UINT64 Arg2, UINT64 Arg3) {
    DbgPrint("[VMM] HandleHypercallExtended called\n");
    DbgPrint("[VMM] Code: 0x%llX, Args: 0x%llX, 0x%llX, 0x%llX\n", Code, Arg1, Arg2, Arg3);
    
    switch (Code) {
        case 0x500: // VMMCALL_ATTACH_PROCESS
            DbgPrint("[VMM] Dispatching to HypercallAttachProcess\n");
            return HypercallAttachProcess(Vcpu, Arg1, Arg2);

        case 0x501: // VMMCALL_INSTALL_NPT_HOOK
            DbgPrint("[VMM] Dispatching to HypercallInstallNptHook\n");
            return HypercallInstallNptHook(Vcpu, Arg1, Arg2, Arg3);

        case 0x502: // VMMCALL_REMOVE_NPT_HOOK
            DbgPrint("[VMM] Dispatching to HypercallRemoveNptHook\n");
            return HypercallRemoveNptHook(Vcpu);

        case 0x503: // VMMCALL_INJECT_DLL
            DbgPrint("[VMM] Dispatching to HypercallInjectDll\n");
            return HypercallInjectDll(Vcpu);

        case 0x504: // VMMCALL_REMOVE_DLL
            DbgPrint("[VMM] Dispatching to HypercallRemoveDll\n");
            return HypercallRemoveDll(Vcpu);

        default:
            DbgPrint("[VMM] Unknown hypercall in extended handler: 0x%llX\n", Code);
            return 0;
    }
}
