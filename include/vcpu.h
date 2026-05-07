#pragma once
#include <ntifs.h>
#include "vmcb.h"
#include "npt.h"

typedef struct _VCPU
{
    //
    // VMCB region
    //
    VMCB* Vmcb;
    PHYSICAL_ADDRESS VmcbPa;

    //
    // Host save area for vmrun
    //
    PVOID HostSave;
    PHYSICAL_ADDRESS HostSavePa;

    //
    // Guest stack
    //
    PVOID GuestStack;
    SIZE_T GuestStackSize;

    //
    // Nested Page Tables
    //
    NPT_STATE Npt;

    //
    // -------------------------
    // NEW FIELDS (REQUIRED BY svm.c)
    // -------------------------
    //

    // MSR Permission Map (3 pages = 0x6000)
    PVOID Msrpm;
    PHYSICAL_ADDRESS MsrpmPa;

    // I/O Permission Map (0x2000)
    PVOID Iopm;
    PHYSICAL_ADDRESS IopmPa;

    //
    // Runtime statistics
    //
    struct
    {
        UINT64 ExitCount;
        UINT64 LastExitCode;
        UINT64 ExitBudget;
    } Exec;

    //
    // IPC / Mailbox subsystem (optional)
    //
    struct
    {
        UINT64 MailboxGpa;
        UINT64 LastMessage;
        BOOLEAN Active;
    } Ipc;

    //
    // Extra metadata
    //
    UINT64 CloakedTscOffset;

    // Instruction Emulator State (PART 3.1)
    struct
    {
        UINT8 InstructionBuffer[16];
        UINT8 InstructionLength;
        BOOLEAN EmulationActive;
    } Emulator;

    // Secret Key for Hypercall Verification (PART 3.10)
    UINT64 HypercallSecretKey;

    // PHASE 1.5: Thread Context Safety - Guest Register Backup
    struct
    {
        UINT64 Rbx;
        UINT64 Rcx;
        UINT64 Rdx;
        UINT64 Rsi;
        UINT64 Rdi;
        UINT64 Rbp;
        UINT64 R8;
        UINT64 R9;
        UINT64 R10;
        UINT64 R11;
        UINT64 R12;
        UINT64 R13;
        UINT64 R14;
        UINT64 R15;
    } GuestRegs;

    // PHASE 1.1: Process Context Awareness
    struct
    {
        UINT64 TargetCr3;
        UINT16 TargetAsid;
        BOOLEAN MonitoringActive;
        UINT64 CurrentGuestCr3;
    } ProcessContext;

    // PHASE 3.3: Debug Register Masking
    struct
    {
        UINT64 Dr0;
        UINT64 Dr1;
        UINT64 Dr2;
        UINT64 Dr3;
        UINT64 Dr6;
        UINT64 Dr7;
        BOOLEAN Masked;
    } DebugRegs;

    // PHASE 3.2: TSC Timing Stealth
    struct
    {
        UINT64 BaseOffset;
        UINT64 LastGuestTsc;
        UINT64 AccumulatedOverhead;
        BOOLEAN InterceptActive;
    } TscStealth;

    // PHASE 3.6: Kernel/User Mode Tracking
    struct
    {
        BOOLEAN InKernelMode;
        UINT64 LastSyscallRip;
        UINT64 SyscallCount;
    } ModeTracking;

    BOOLEAN Active;

} VCPU;
