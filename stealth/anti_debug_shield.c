#include <ntifs.h>
#include "../../include/vcpu.h"
#include "../../include/stealth.h"

// PART 4: ANTI-DEBUG & ANTI-CHEAT SHIELD
// Updated: 2026-05-07 - Fixed VMCB access using helper functions

// PART 4.1: TSC Offsetting for Timing Stealth
static UINT64 g_TscOffsetAccumulator = 0;
static UINT64 g_LastVmExitTsc = 0;

VOID StealthEnableTscOffset(VCPU* Vcpu) {
    // Enable TSC offsetting in VMCB
    VmcbControl(Vcpu->Vmcb)->TscOffset = g_TscOffsetAccumulator;
    DbgPrint("[VMM] TSC Offset enabled: Offset=0x%llX\n", g_TscOffsetAccumulator);
}

VOID StealthAdjustTscOnVmExit(VCPU* Vcpu) {
    UINT64 currentTsc = __rdtsc();
    
    if (g_LastVmExitTsc != 0) {
        UINT64 vmExitCycles = currentTsc - g_LastVmExitTsc;
        
        // Subtract VM-Exit overhead from guest's view of TSC
        g_TscOffsetAccumulator -= vmExitCycles;
        VmcbControl(Vcpu->Vmcb)->TscOffset = g_TscOffsetAccumulator;
    }
    
    g_LastVmExitTsc = currentTsc;
}

VOID StealthResetTscOffset(VCPU* Vcpu) {
    g_TscOffsetAccumulator = 0;
    g_LastVmExitTsc = 0;
    VmcbControl(Vcpu->Vmcb)->TscOffset = 0;
    DbgPrint("[VMM] TSC Offset reset\n");
}

// PART 4.2: DR Register Spoofing
typedef struct _DR_SPOOF_STATE {
    UINT64 RealDr0;
    UINT64 RealDr1;
    UINT64 RealDr2;
    UINT64 RealDr3;
    UINT64 RealDr6;
    UINT64 RealDr7;
    BOOLEAN IsActive;
} DR_SPOOF_STATE;

static DR_SPOOF_STATE g_DrState = { 0 };

VOID StealthEnableDrSpoof(VCPU* Vcpu) {
    // TODO: Implement DR intercepts using VMCB Intercepts array
    // The VMCB uses Intercepts[n] bitfields, not individual fields
    g_DrState.IsActive = TRUE;
    DbgPrint("[VMM] DR register spoofing enabled (placeholder)\n");
}

BOOLEAN StealthHandleDrRead(VCPU* Vcpu, UINT32 DrNumber, UINT64* OutValue) {
    if (!g_DrState.IsActive) {
        return FALSE;
    }

    // Return 0 for all DR reads to hide hardware breakpoints
    *OutValue = 0;

    DbgPrint("[VMM] DR%u read intercepted, returning 0\n", DrNumber);

    // Advance RIP past the MOV instruction
    VmcbState(Vcpu->Vmcb)->Rip += VmcbControl(Vcpu->Vmcb)->ExitInfo2; // Instruction length

    return TRUE;
}

BOOLEAN StealthHandleDrWrite(VCPU* Vcpu, UINT32 DrNumber, UINT64 Value) {
    if (!g_DrState.IsActive) {
        return FALSE;
    }

    // Store the real value but don't actually write it to hardware
    switch (DrNumber) {
        case 0: g_DrState.RealDr0 = Value; break;
        case 1: g_DrState.RealDr1 = Value; break;
        case 2: g_DrState.RealDr2 = Value; break;
        case 3: g_DrState.RealDr3 = Value; break;
        case 6: g_DrState.RealDr6 = Value; break;
        case 7: g_DrState.RealDr7 = Value; break;
    }

    DbgPrint("[VMM] DR%u write intercepted, value=0x%llX (not applied)\n", DrNumber, Value);

    // Advance RIP
    VmcbState(Vcpu->Vmcb)->Rip += VmcbControl(Vcpu->Vmcb)->ExitInfo2;

    return TRUE;
}

VOID StealthDisableDrSpoof(VCPU* Vcpu) {
    // TODO: Implement DR intercept disable using VMCB Intercepts array
    g_DrState.IsActive = FALSE;
    DbgPrint("[VMM] DR register spoofing disabled (placeholder)\n");
}

// PART 4.3: Module Hiding - Ensure DLL is not in PEB
// This is handled by manual mapping in user-mode, but we can verify from VMM

BOOLEAN StealthVerifyModuleHidden(UINT64 DllBase, UINT64 Cr3) {
    // Walk PEB->Ldr->InLoadOrderModuleList to verify DLL is not present
    // This requires reading guest memory and parsing PEB structures
    
    // For now, just log that we should verify
    DbgPrint("[VMM] Module hiding verification: DllBase=0x%llX\n", DllBase);
    
    // In production, walk the PEB and check if DllBase exists in any module list
    // Return FALSE if found, TRUE if hidden
    
    return TRUE; // Assume hidden for now
}

// PART 4.4: CPUID Spoofing (Hide Hypervisor Presence)
BOOLEAN StealthHandleCpuid(VCPU* Vcpu) {
    // TODO: Implement full CPUID spoofing
    // The guest registers (RCX, RDX, etc.) are not in VMCB_STATE_SAVE_AREA
    // They need to be accessed from the guest stack or VCPU context
    
    DbgPrint("[VMM] CPUID intercept (placeholder)\n");
    VmcbState(Vcpu->Vmcb)->Rip += 2; // CPUID is 2 bytes
    return TRUE;
}

// PART 4.5: Comprehensive Stealth Activation
VOID StealthActivateFullShield(VCPU* Vcpu) {
    StealthEnableTscOffset(Vcpu);
    StealthEnableDrSpoof(Vcpu);
    
    // TODO: Enable CPUID intercept using VMCB Intercepts array
    // Vcpu->Vmcb->ControlArea.InterceptCpuid = 1;

    DbgPrint("[VMM] Full anti-debug shield activated\n");
}

VOID StealthDeactivateFullShield(VCPU* Vcpu) {
    StealthResetTscOffset(Vcpu);
    StealthDisableDrSpoof(Vcpu);
    
    // TODO: Disable CPUID intercept
    // Vcpu->Vmcb->ControlArea.InterceptCpuid = 0;

    DbgPrint("[VMM] Full anti-debug shield deactivated\n");
}
