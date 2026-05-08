#include <ntifs.h>
#include "svm.h"
#include "vcpu.h"
#include "vmcb.h"
#include "guest_mem.h"
#include "npt.h"
#include "hooks.h"
#include "stealth.h"
#include "shadow_idt.h"
#include "layers.h"



static VOID HvAdvanceRIP(VCPU* V, UINT8 len)
{
    VMCB_CONTROL_AREA* c = VmcbControl(V->Vmcb);
    VMCB_STATE_SAVE_AREA* s = VmcbState(V->Vmcb);

    if (c->NextRip)
        s->Rip = c->NextRip;
    else
        s->Rip += len;
}

// PHASE 1.2: Process Observer - Check if current CR3 matches target
static BOOLEAN HvIsTargetProcess(VCPU* V)
{
    if (!V->ProcessContext.MonitoringActive)
        return FALSE;

    VMCB_STATE_SAVE_AREA* s = VmcbState(V->Vmcb);
    UINT64 currentCr3 = s->Cr3 & ~0xFFFULL;

    return (currentCr3 == V->ProcessContext.TargetCr3);
}

// PHASE 1.4: KVA Shadow Awareness - Check PCIDE bit in CR4
static BOOLEAN HvIsPcidEnabled(VCPU* V)
{
    VMCB_STATE_SAVE_AREA* s = VmcbState(V->Vmcb);
    return (s->Cr4 & (1ULL << 17)) != 0;  // PCIDE is bit 17
}

// PHASE 3.5: Instruction Length Decoder (Minimal)
static UINT8 HvDecodeInstructionType(UINT8 opcode)
{
    switch (opcode)
    {
    case 0xE8:  // CALL rel32
    case 0xFF:  // CALL/JMP r/m
        return 1;  // CALL
    case 0xE9:  // JMP rel32
    case 0xEB:  // JMP rel8
        return 2;  // JMP
    case 0xC3:  // RET near
    case 0xCB:  // RET far
    case 0xC2:  // RET imm16
        return 3;  // RET
    default:
        return 0;  // Other
    }
}


static VOID HvHandleCpuid(VCPU* V)
{
    VMCB_STATE_SAVE_AREA* s = VmcbState(V->Vmcb);

    UINT32 leaf = (UINT32)s->Rax;
    UINT32 sub = (UINT32)V->GuestRegs.Rcx;

    int cpuInfo[4] = { 0 };
    __cpuidex(cpuInfo, (int)leaf, (int)sub);
    
    UINT32 eax = (UINT32)cpuInfo[0];
    UINT32 ebx = (UINT32)cpuInfo[1];
    UINT32 ecx = (UINT32)cpuInfo[2];
    UINT32 edx = (UINT32)cpuInfo[3];

    // PART 3.4: CPUID Spoofing - Hide hypervisor presence
    if (leaf == 1)
        ecx &= ~(1 << 31);  // Clear hypervisor bit

    if (leaf == 0x80000001)
        edx &= ~(1 << 2);   // Hide SVM feature

    // PART 3.4: Hide AMD-V/SVM features
    if (leaf == 0x8000000A)
    {
        eax = 0;  // No SVM revision
        ebx = 0;  // No NASID
        ecx = 0;  // No features
        edx = 0;  // No features
    }
    
    StealthMaskCpuid(leaf, &ecx, &edx);
    HookCpuidEmulate(leaf, sub, &eax, &ebx, &ecx, &edx);

    s->Rax = eax;
    V->GuestRegs.Rbx = ebx;
    V->GuestRegs.Rcx = ecx;
    V->GuestRegs.Rdx = edx;

    HvAdvanceRIP(V, 2);
}


static VOID HvHandleMsr(VCPU* V)
{
    VMCB_STATE_SAVE_AREA* s = VmcbState(V->Vmcb);

    UINT64 rcx = V->GuestRegs.Rcx;
    BOOLEAN write = (rcx >> 63) & 1;
    UINT64 msr = rcx & ~0x8000000000000000ULL;

    if (write)
    {
        UINT64 value = s->Rax;
        HookHandleMsrWrite(V, msr, value);
    }
    else
    {
        UINT64 value = HookHandleMsrRead(V, msr);
        s->Rax = value;
    }

    HvAdvanceRIP(V, 2);
}


static VOID HvHandleVmmcall(VCPU* V)
{
    VMCB_STATE_SAVE_AREA* s = VmcbState(V->Vmcb);

    UINT64 code = s->Rax;
    UINT64 arg1 = V->GuestRegs.Rbx;
    UINT64 arg2 = V->GuestRegs.Rcx;
    UINT64 arg3 = V->GuestRegs.Rdx;

    // PHASE 3.6: VMMCALL Secret Key - R10 contains the handshake key
    UINT64 secretKey = V->GuestRegs.R10;

    DbgPrint("[HV] VMMCALL received: Code=0x%llX, Args=[0x%llX, 0x%llX, 0x%llX], Key=0x%llX\n", 
             code, arg1, arg2, arg3, secretKey);

    UINT64 result = HookVmmcallDispatch(V, code, arg1, arg2, arg3, secretKey);

    DbgPrint("[HV] VMMCALL 0x%llX completed with result: 0x%llX\n", code, result);

    s->Rax = result;

    HvAdvanceRIP(V, 3);
}

// PHASE 3.2: RDTSC Intercept Handler
static VOID HvHandleRdtsc(VCPU* V)
{
    VMCB_STATE_SAVE_AREA* s = VmcbState(V->Vmcb);

    // Read actual TSC
    UINT64 tscStart = __rdtsc();

    // Apply offset to hide VMM overhead
    UINT64 adjustedTsc = tscStart - V->TscStealth.AccumulatedOverhead;

    // Store for next calculation
    V->TscStealth.LastGuestTsc = adjustedTsc;

    // Return to guest
    s->Rax = (UINT32)(adjustedTsc & 0xFFFFFFFF);
    V->GuestRegs.Rdx = (UINT32)(adjustedTsc >> 32);

    HvAdvanceRIP(V, 2);

    // Measure overhead of this handler
    UINT64 tscEnd = __rdtsc();
    V->TscStealth.AccumulatedOverhead += (tscEnd - tscStart);
}

// PHASE 3.3: Debug Register Read Handler
static VOID HvHandleDrRead(VCPU* V)
{
    VMCB_CONTROL_AREA* c = VmcbControl(V->Vmcb);
    VMCB_STATE_SAVE_AREA* s = VmcbState(V->Vmcb);

    UINT64 drIndex = c->ExitInfo1 & 0xF;

    if (V->DebugRegs.Masked)
    {
        // Return spoofed values
        switch (drIndex)
        {
        case 0: s->Rax = V->DebugRegs.Dr0; break;
        case 1: s->Rax = V->DebugRegs.Dr1; break;
        case 2: s->Rax = V->DebugRegs.Dr2; break;
        case 3: s->Rax = V->DebugRegs.Dr3; break;
        case 6: s->Rax = V->DebugRegs.Dr6; break;
        case 7: s->Rax = V->DebugRegs.Dr7; break;
        default: s->Rax = 0; break;
        }

        DbgPrint("[HV] DR%llu read intercepted (masked)\n", drIndex);
    }
    else
    {
        // Pass through to real DR
        switch (drIndex)
        {
        case 0: s->Rax = __readdr(0); break;
        case 1: s->Rax = __readdr(1); break;
        case 2: s->Rax = __readdr(2); break;
        case 3: s->Rax = __readdr(3); break;
        case 6: s->Rax = __readdr(6); break;
        case 7: s->Rax = __readdr(7); break;
        default: s->Rax = 0; break;
        }
    }

    HvAdvanceRIP(V, 2);
}

// PHASE 3.3: Debug Register Write Handler
static VOID HvHandleDrWrite(VCPU* V)
{
    VMCB_CONTROL_AREA* c = VmcbControl(V->Vmcb);
    VMCB_STATE_SAVE_AREA* s = VmcbState(V->Vmcb);

    UINT64 drIndex = c->ExitInfo1 & 0xF;
    UINT64 value = s->Rax;

    if (V->DebugRegs.Masked)
    {
        // Store in shadow registers, don't write to real DR
        switch (drIndex)
        {
        case 0: V->DebugRegs.Dr0 = value; break;
        case 1: V->DebugRegs.Dr1 = value; break;
        case 2: V->DebugRegs.Dr2 = value; break;
        case 3: V->DebugRegs.Dr3 = value; break;
        case 6: V->DebugRegs.Dr6 = value; break;
        case 7: V->DebugRegs.Dr7 = value; break;
        }

        DbgPrint("[HV] DR%llu write intercepted: 0x%llX (masked)\n", drIndex, value);
    }
    else
    {
        // Pass through to real DR
        switch (drIndex)
        {
        case 0: __writedr(0, value); break;
        case 1: __writedr(1, value); break;
        case 2: __writedr(2, value); break;
        case 3: __writedr(3, value); break;
        case 6: __writedr(6, value); break;
        case 7: __writedr(7, value); break;
        }
    }

    HvAdvanceRIP(V, 2);
}

// PHASE 3.4: Exception Injection Helper
static VOID HvInjectPageFault(VCPU* V, UINT64 faultAddress, UINT64 errorCode)
{
    VMCB_CONTROL_AREA* c = VmcbControl(V->Vmcb);
    VMCB_STATE_SAVE_AREA* s = VmcbState(V->Vmcb);

    // Set CR2 to fault address
    s->Cr2 = faultAddress;

    // Inject #PF (vector 14)
    c->EventInjection = (1ULL << 31) |  // Valid
        (3ULL << 8) |   // Exception type
        (1ULL << 11) |  // Error code valid
        14;             // Vector #PF

    c->EventInjectionError = (UINT32)errorCode;

    DbgPrint("[HV] Injected #PF: addr=0x%llX error=0x%llX\n", faultAddress, errorCode);
}


static VOID HvHandleNpf(VCPU* V)
{
    VMCB_CONTROL_AREA* c = VmcbControl(V->Vmcb);
    VMCB_STATE_SAVE_AREA* s = VmcbState(V->Vmcb);

    UINT64 fault_gpa = c->ExitInfo2;
    UINT64 error_code = c->ExitInfo1;
    UINT64 guest_rip = s->Rip;

    // PHASE 1.2: Process Observer - Enhanced monitoring for target process
    BOOLEAN isTargetProcess = HvIsTargetProcess(V);
    if (isTargetProcess)
    {
        // Enhanced monitoring path
        DbgPrint("[NPF-TARGET] GPA=0x%llX RIP=0x%llX\n", fault_gpa, guest_rip);
    }

    // PHASE 3.5: Decode instruction at fault
    if (c->InstructionLength > 0 && c->InstructionLength <= 15)
    {
        UINT8 opcode = c->InstructionBytes[0];
        UINT8 instrType = HvDecodeInstructionType(opcode);

        if (instrType > 0)
        {
            DbgPrint("[NPF] Instruction type: %s at RIP=0x%llX\n",
                instrType == 1 ? "CALL" : (instrType == 2 ? "JMP" : "RET"),
                guest_rip);
        }
    }

    // PART 2.2: The Execution Trap - Handle shadow paging first
    if (NptHandleNpfViolation(&V->Npt, fault_gpa, error_code, guest_rip))
    {
        // PART 2: NextRIP fix to prevent freezes
        if (c->NextRip)
            s->Rip = c->NextRip;
        return;
    }

    // Legacy layered NPF handling
    if (HvHandleLayeredNpf(V, fault_gpa))
    {
        if (c->NextRip)
            s->Rip = c->NextRip;
        return;
    }

    // PHASE 3.4: If unhandled and not our shadow page, inject #PF to guest
    if (!isTargetProcess)
    {
        HvInjectPageFault(V, fault_gpa, error_code);
        return;
    }

    // PART 4.6: Error recovery - log and resume
    DbgPrint("[NPT] Unhandled NPF: GPA=0x%llX ErrorCode=0x%llX RIP=0x%llX\n", 
             fault_gpa, error_code, guest_rip);

    HookNptHandleFault(V, fault_gpa);
    
    if (c->NextRip)
        s->Rip = c->NextRip;
}


static VOID HvHandleHlt(VCPU* V)
{
    HvAdvanceRIP(V, 1);
}


static VOID HvHandleIo(VCPU* V)
{
    HookIoIntercept(V);
    HvAdvanceRIP(V, 2);
}


NTSTATUS HypervisorHandleExit(VCPU* V)
{
    VMCB_CONTROL_AREA* c = VmcbControl(V->Vmcb);
    VMCB_STATE_SAVE_AREA* s = VmcbState(V->Vmcb);
    UINT64 exitCode = c->ExitCode;

    V->Exec.LastExitCode = exitCode;

    // PHASE 1.2: Update current guest CR3 for process tracking
    V->ProcessContext.CurrentGuestCr3 = s->Cr3 & ~0xFFFULL;

    // PHASE 1.2: Process Observer - Switch ASID for target process
    if (V->ProcessContext.MonitoringActive)
    {
        if (V->ProcessContext.CurrentGuestCr3 == V->ProcessContext.TargetCr3)
        {
            // Target process - use dedicated ASID
            c->GuestAsid = V->ProcessContext.TargetAsid;
        }
        else
        {
            // Other processes - use default ASID
            c->GuestAsid = 1;
        }
    }

    switch (exitCode)
    {
    case SVM_EXIT_CPUID:
        HvHandleCpuid(V);
        return STATUS_SUCCESS;

    case SVM_EXIT_MSR:
        HvHandleMsr(V);
        return STATUS_SUCCESS;

    case SVM_EXIT_VMMCALL:
        HvHandleVmmcall(V);
        return STATUS_SUCCESS;

    case SVM_EXIT_NPF:
        if (HvHandleLayeredNpf(V, c->ExitInfo1))
            return STATUS_SUCCESS;

        HvHandleNpf(V);
        return STATUS_SUCCESS;

    case SVM_EXIT_HLT:
        HvHandleHlt(V);
        return STATUS_SUCCESS;

    case SVM_EXIT_RDTSC:
        HvHandleRdtsc(V);
        return STATUS_SUCCESS;

    case SVM_EXIT_DR_READ:
        HvHandleDrRead(V);
        return STATUS_SUCCESS;

    case SVM_EXIT_DR_WRITE:
        HvHandleDrWrite(V);
        return STATUS_SUCCESS;

    default:
        return STATUS_SUCCESS;
    }
}


