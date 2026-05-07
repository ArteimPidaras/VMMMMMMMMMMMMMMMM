#include <ntifs.h>
#include "../../include/vcpu.h"
#include "../../include/npt.h"
#include "../../include/svm.h"

// External functions from other modules
extern VOID ProcessInterceptCr3Load(VCPU* Vcpu, UINT64 NewCr3);
extern BOOLEAN ProcessIsTargetAttached(void);
extern BOOLEAN ProcessIsCr3Captured(void);
extern UINT64 ProcessGetCapturedCr3(void);
extern BOOLEAN NptHandleViewSwitch(NPT_STATE* State, UINT64 FaultGpa, UINT64 ErrorCode, UINT64 GuestRip, UINT16 Asid);
extern VOID StealthAdjustTscOnVmExit(VCPU* Vcpu);
extern BOOLEAN StealthHandleDrRead(VCPU* Vcpu, UINT32 DrNumber, UINT64* OutValue);
extern BOOLEAN StealthHandleDrWrite(VCPU* Vcpu, UINT32 DrNumber, UINT64 Value);
extern BOOLEAN StealthHandleCpuid(VCPU* Vcpu);

#define VMEXIT_NPF 0x400
#define VMEXIT_CR3_READ 0x03
#define VMEXIT_CR3_WRITE 0x13
#define VMEXIT_DR0_READ 0x20
#define VMEXIT_DR0_WRITE 0x30
#define VMEXIT_CPUID 0x72

// Main NPF Handler with View-Switching
BOOLEAN HandleNestedPageFault(VCPU* Vcpu) {
    UINT64 faultGpa = VmcbControl(Vcpu->Vmcb)->ExitInfo2;
    UINT64 errorCode = VmcbControl(Vcpu->Vmcb)->ExitInfo1;
    UINT64 guestRip = VmcbState(Vcpu->Vmcb)->Rip;
    UINT16 asid = (UINT16)VmcbControl(Vcpu->Vmcb)->GuestAsid;

    // Adjust TSC to hide VM-Exit latency
    StealthAdjustTscOnVmExit(Vcpu);

    // Try NPT view-switching first
    if (NptHandleViewSwitch(&Vcpu->Npt, faultGpa, errorCode, guestRip, asid)) {
        DbgPrint("[VMM] NPF handled by view-switching: GPA=0x%llX, RIP=0x%llX\n", faultGpa, guestRip);
        return TRUE; // Resume guest
    }

    // If not our hook, handle as normal NPF
    DbgPrint("[VMM] NPF not handled by view-switching: GPA=0x%llX, ErrorCode=0x%llX\n", faultGpa, errorCode);
    
    // Could be a legitimate page fault - let guest handle it or inject #PF
    return FALSE;
}

// CR3 Load Intercept Handler
BOOLEAN HandleCr3Write(VCPU* Vcpu) {
    UINT64 newCr3 = VmcbState(Vcpu->Vmcb)->Rax; // New CR3 value is in RAX for MOV CR3, RAX

    // If we're waiting to capture CR3 for target process, intercept it
    if (ProcessIsTargetAttached() && !ProcessIsCr3Captured()) {
        ProcessInterceptCr3Load(Vcpu, newCr3);
    }

    // Let the CR3 load proceed normally
    return FALSE; // Continue with normal CR3 load
}

// DR Register Read Intercept
BOOLEAN HandleDrRead(VCPU* Vcpu, UINT32 DrNumber) {
    UINT64 spoofedValue = 0;
    
    if (StealthHandleDrRead(Vcpu, DrNumber, &spoofedValue)) {
        // Write spoofed value to destination register
        // The destination register is encoded in ExitInfo1
        UINT64 exitInfo1 = VmcbControl(Vcpu->Vmcb)->ExitInfo1;
        UINT32 destReg = (UINT32)(exitInfo1 & 0xF);
        
        // Set the destination register
        UINT64* pGuestRegs = (UINT64*)&VmcbState(Vcpu->Vmcb)->Rax;
        pGuestRegs[destReg] = spoofedValue;
        
        return TRUE; // Handled, resume guest
    }

    return FALSE; // Not handled
}

// DR Register Write Intercept
BOOLEAN HandleDrWrite(VCPU* Vcpu, UINT32 DrNumber) {
    UINT64 exitInfo1 = VmcbControl(Vcpu->Vmcb)->ExitInfo1;
    UINT32 srcReg = (UINT32)(exitInfo1 & 0xF);
    
    UINT64* pGuestRegs = (UINT64*)&VmcbState(Vcpu->Vmcb)->Rax;
    UINT64 value = pGuestRegs[srcReg];
    
    if (StealthHandleDrWrite(Vcpu, DrNumber, value)) {
        return TRUE; // Handled, resume guest
    }

    return FALSE; // Not handled
}

// CPUID Intercept
BOOLEAN HandleCpuidIntercept(VCPU* Vcpu) {
    if (StealthHandleCpuid(Vcpu)) {
        return TRUE; // Handled and spoofed
    }

    // Let CPUID execute normally
    return FALSE;
}

// Main VM-Exit Dispatcher
VOID VmExitHandler(VCPU* Vcpu) {
    UINT64 exitCode = VmcbControl(Vcpu->Vmcb)->ExitCode;
    BOOLEAN handled = FALSE;

    switch (exitCode) {
        case VMEXIT_NPF:
            handled = HandleNestedPageFault(Vcpu);
            break;

        case VMEXIT_CR3_WRITE:
            handled = HandleCr3Write(Vcpu);
            break;

        case VMEXIT_DR0_READ:
        case VMEXIT_DR0_READ + 1:
        case VMEXIT_DR0_READ + 2:
        case VMEXIT_DR0_READ + 3:
        case VMEXIT_DR0_READ + 6:
        case VMEXIT_DR0_READ + 7:
            handled = HandleDrRead(Vcpu, (UINT32)(exitCode - VMEXIT_DR0_READ));
            break;

        case VMEXIT_DR0_WRITE:
        case VMEXIT_DR0_WRITE + 1:
        case VMEXIT_DR0_WRITE + 2:
        case VMEXIT_DR0_WRITE + 3:
        case VMEXIT_DR0_WRITE + 6:
        case VMEXIT_DR0_WRITE + 7:
            handled = HandleDrWrite(Vcpu, (UINT32)(exitCode - VMEXIT_DR0_WRITE));
            break;

        case VMEXIT_CPUID:
            handled = HandleCpuidIntercept(Vcpu);
            break;

        default:
            DbgPrint("[VMM] Unhandled VM-Exit: Code=0x%llX\n", exitCode);
            break;
    }

    if (!handled) {
        // Handle other VM-Exits or inject exception
        DbgPrint("[VMM] VM-Exit not handled: Code=0x%llX, RIP=0x%llX\n", 
                 exitCode, VmcbState(Vcpu->Vmcb)->Rip);
    }
}
