#include <ntifs.h>
#include "hooks.h"
#include "vcpu.h"
#include "vmcb.h"
#include "guest_mem.h"
#include "npt.h"
#include "stealth.h"
#include "msr.h"
#include "translator.h"
#include "process_manager.h"
#include "communication.h"



static UINT64 g_OriginalLstar = 0;        
static UINT64 g_OriginalStar = 0;
static UINT64 g_OriginalSfMask = 0;

static UINT64 g_HvSyscallHandler = 0;     
static BOOLEAN g_SyscallHookEnabled = FALSE;

static BOOLEAN g_Cr3EncryptionEnabled = FALSE;
static UINT64 g_Cr3XorKey = 0xCAFEBABE1337ULL;

static BOOLEAN HookIsCr3PagePresent(VCPU* V, UINT64 cr3)
{
    UINT64 pml4 = cr3 & ~0xFFFULL;
    UINT64 entry = 0;

   
	if (!GuestReadGpa(V, pml4, &entry, sizeof(entry))) // <-- ReadGuestPhysical(IVAN_KUST TECHNOLOGIES (INSOMIA.SOLUTIONS!!!! PRESENTS$))
        return FALSE;

    return (entry & 1ULL) != 0;
}



VOID HookCpuidEmulate(UINT32 leaf, UINT32 subleaf,
    UINT32* eax, UINT32* ebx, UINT32* ecx, UINT32* edx)
{
    UNREFERENCED_PARAMETER(subleaf);
    UNREFERENCED_PARAMETER(eax);
    
    //
    // :  vendor-string
    //
    if (leaf == 0)
    {
        *ebx = 'V', 'M', 'S', 'V';  // vmsv
        *ecx = 'H', 'V', 'A', 'M';  // hvam
        *edx = 'S', 'T', 'E', 'L';  // stel
    }


    *ecx &= ~(1 << 31);  
}



UINT64 HookHandleMsrRead(VCPU* V, UINT64 msr)
{
    UNREFERENCED_PARAMETER(V);
    
    switch (msr)
    {
    case MSR_LSTAR:
        if (g_SyscallHookEnabled)
            return g_HvSyscallHandler;
        return g_OriginalLstar;

    case MSR_STAR:
        return g_OriginalStar;

    case MSR_SFMASK:
        return g_OriginalSfMask;

    default:
        return StealthMaskMsrRead((UINT32)msr, __readmsr(msr));
    }
}

VOID HookInstallSyscall(VCPU* V)
{
    UNREFERENCED_PARAMETER(V);
    
    if (g_SyscallHookEnabled) return;

    g_OriginalLstar = __readmsr(MSR_LSTAR);
    g_OriginalStar = __readmsr(MSR_STAR);
    g_OriginalSfMask = __readmsr(MSR_SFMASK);

    //
    //  syscall entry    
    //
    if (g_HvSyscallHandler != 0)
    {
        __writemsr(MSR_LSTAR, g_HvSyscallHandler);
        g_SyscallHookEnabled = TRUE;
    }
}

VOID HookRemoveSyscall()
{
    if (!g_SyscallHookEnabled) return;

    __writemsr(MSR_LSTAR, g_OriginalLstar);
    __writemsr(MSR_STAR, g_OriginalStar);
    __writemsr(MSR_SFMASK, g_OriginalSfMask);

    g_SyscallHookEnabled = FALSE;
}

VOID HookHandleMsrWrite(VCPU* V, UINT64 msr, UINT64 value)
{
    UNREFERENCED_PARAMETER(V);
    
    switch (msr)
    {
    case MSR_LSTAR:
    {
        g_OriginalLstar = value;
        return;
    }
    case MSR_STAR:
    {
        g_OriginalStar = value;
        return;
    }
    case MSR_SFMASK:
    {
        g_OriginalSfMask = value;
        return;
    }
    default:
        __writemsr(msr, value);
        return;
    }
}



UINT64 HookEncryptCr3(UINT64 cr3)
{
    if (!g_Cr3EncryptionEnabled)
        return cr3;

    return cr3 ^ g_Cr3XorKey;
}

UINT64 HookDecryptCr3(VCPU* V, UINT64 cr3_enc)
{
    if (!g_Cr3EncryptionEnabled)
        return cr3_enc;

    UINT64 candidate = cr3_enc ^ g_Cr3XorKey;

    
    if (V && HookIsCr3PagePresent(V, candidate))
        return candidate;

   
    return candidate;
}

VOID HookEnableCr3Encryption()
{
    g_Cr3EncryptionEnabled = TRUE;
}

VOID HookDisableCr3Encryption()
{
    g_Cr3EncryptionEnabled = FALSE;
}



BOOLEAN HookNptHandleFault(VCPU* V, UINT64 faultingGpa)
{
    UINT64 page = faultingGpa & ~0xFFFULL;

    if (V->Npt.ShadowHook.Active && page == V->Npt.ShadowHook.TargetGpaPage)
    {
        NptHookPage(&V->Npt, page, V->Npt.ShadowHook.NewHpaPage);
        return TRUE;
    }

    return FALSE;
}



UINT64 HookVmmcallDispatch(VCPU* V, UINT64 code, UINT64 a1, UINT64 a2, UINT64 a3, UINT64 secretKey)
{
    // PHASE 3.6: VMMCALL Secret Key Verification
    if (code >= 0x400 && secretKey != V->HypercallSecretKey)
    {
        DbgPrint("[HV] Hypercall rejected: invalid secret key (got 0x%llX)\n", secretKey);
        return 0xDEADC0DE;
    }

    switch (code)
    {
    // ========================================================================
    // PHASE 1.1: CR3 Tracking and Process Management
    // ========================================================================
    case 0x400:   // Set target process CR3 (a1 = CR3, a2 = ASID)
    {
        V->ProcessContext.TargetCr3 = a1 & ~0xFFFULL;
        V->ProcessContext.TargetAsid = (UINT16)(a2 ? a2 : 0x2);
        V->ProcessContext.MonitoringActive = TRUE;

        // Update NPT state
        NptSetTargetProcess(&V->Npt, V->ProcessContext.TargetCr3, V->ProcessContext.TargetAsid);

        DbgPrint("[HV] Target process set: CR3=0x%llX ASID=%u\n",
            V->ProcessContext.TargetCr3, V->ProcessContext.TargetAsid);
        return 1;
    }

    case 0x401:   // Install shadow page (a1 = GPA, a2 = Original HPA, a3 = Injected HPA)
    {
        UINT64 targetCr3 = V->ProcessContext.TargetCr3;
        UINT16 asid = V->ProcessContext.TargetAsid;

        NTSTATUS status = NptShadowPageInstall(&V->Npt, a1, a2, a3, targetCr3, asid);
        return NT_SUCCESS(status) ? 1 : 0;
    }

    case 0x402:   // Remove shadow page (a1 = GPA)
    {
        NptShadowPageRemove(&V->Npt, a1);
        return 1;
    }

    case 0x403:   // Set hypercall secret key (a1 = new key)
    {
        V->HypercallSecretKey = a1;
        DbgPrint("[HV] Hypercall secret key updated to 0x%llX\n", a1);
        return 1;
    }

    case 0x404:   // Query shadow page count
    {
        return V->Npt.ShadowPageCount;
    }

    case 0x405:   // Allocate hidden buffer (a1 = size)
    {
        if (V->Npt.HiddenBuffer)
        {
            DbgPrint("[HV] Hidden buffer already allocated\n");
            return 0;
        }

        SIZE_T size = (SIZE_T)a1;
        if (size == 0 || size > 0x100000)  // Max 1MB
        {
            DbgPrint("[HV] Invalid hidden buffer size: 0x%llX\n", (UINT64)size);
            return 0;
        }

        PHYSICAL_ADDRESS low = { 0 };
        PHYSICAL_ADDRESS high = { .QuadPart = ~0ULL };
        PHYSICAL_ADDRESS skip = { 0 };

        V->Npt.HiddenBuffer = MmAllocateContiguousMemorySpecifyCache(
            size, low, high, skip, MmCached);

        if (!V->Npt.HiddenBuffer)
        {
            DbgPrint("[HV] Failed to allocate hidden buffer\n");
            return 0;
        }

        RtlZeroMemory(V->Npt.HiddenBuffer, size);
        V->Npt.HiddenBufferPa = MmGetPhysicalAddress(V->Npt.HiddenBuffer);
        V->Npt.HiddenBufferSize = size;

        DbgPrint("[HV] Hidden buffer allocated: VA=0x%p PA=0x%llX Size=0x%llX\n",
            V->Npt.HiddenBuffer, V->Npt.HiddenBufferPa.QuadPart, (UINT64)size);

        return V->Npt.HiddenBufferPa.QuadPart;
    }

    case 0x406:   // Free hidden buffer
    {
        if (V->Npt.HiddenBuffer)
        {
            MmFreeContiguousMemory(V->Npt.HiddenBuffer);
            V->Npt.HiddenBuffer = NULL;
            V->Npt.HiddenBufferPa.QuadPart = 0;
            V->Npt.HiddenBufferSize = 0;
            DbgPrint("[HV] Hidden buffer freed\n");
            return 1;
        }
        return 0;
    }

    case 0x407:   // Enable/Disable debug register masking (a1 = 1/0)
    {
        V->DebugRegs.Masked = (a1 != 0);
        DbgPrint("[HV] Debug register masking: %s\n", V->DebugRegs.Masked ? "ENABLED" : "DISABLED");
        return 1;
    }

    case 0x408:   // Enable/Disable TSC intercept (a1 = 1/0)
    {
        V->TscStealth.InterceptActive = (a1 != 0);
        DbgPrint("[HV] TSC intercept: %s\n", V->TscStealth.InterceptActive ? "ENABLED" : "DISABLED");
        return 1;
    }

    case 0x409:   // Query process context info
    {
        // Return target CR3 in RAX, ASID in high 16 bits
        return V->ProcessContext.TargetCr3 | ((UINT64)V->ProcessContext.TargetAsid << 48);
    }

    // ========================================================================
    // Legacy hypercalls
    // ========================================================================
    case 0x100:   // read guest virtual mem
    {
        UINT8 buf[8];
        if (GuestReadGva(V, a1, buf, sizeof(buf)))
            return *(UINT64*)buf;
        return 0;
    }

    case 0x101:   // write guest memory
    {
        UINT64 value = a2;
        GuestWriteGva(V, a1, &value, sizeof(value));
        return TRUE;
    }

    case 0x102:   // enable CR3 XOR hook
        HookEnableCr3Encryption();
        return TRUE;

    case 0x103:   // disable CR3 XOR hook
        HookDisableCr3Encryption();
        return TRUE;

    case 0x110:   // install shadow EPT hook (a1 = target GVA, a2 = new HPA/GPA)
    {
        PHYSICAL_ADDRESS gpa = GuestTranslateGvaToGpa(V, a1);
        if (!gpa.QuadPart)
            return FALSE;

        return NptInstallShadowHook(&V->Npt, gpa.QuadPart, a2);
    }

    case 0x111:   // clear shadow hook
        NptClearShadowHook(&V->Npt);
        return TRUE;

    case 0x200:  // stealth mode enable
        StealthEnable();
        return TRUE;

    case 0x201:
        StealthDisable();
        return TRUE;

    case 0x210: // fetch last mailbox payload
    {
        HV_COMM_MESSAGE message = { 0 };
        if (CommReceive(V, &message))
            return message.Code;
        return 0;
    }

    case 0x211: // send mailbox payload (a1..a3)
    {
        HV_COMM_MESSAGE message = { 0 };
        message.Code = a1;
        message.Arg0 = a2;
        message.Arg1 = a3;
        return CommSend(V, &message);
    }

    case 0x220: // translate guest virtual to guest physical
    {
        VA_TRANSLATION_RESULT tx = TranslatorTranslate(V, a1);
        return tx.Valid ? tx.GuestPhysical.QuadPart : 0;
    }

    case 0x221: // translate guest virtual to host physical
    {
        VA_TRANSLATION_RESULT tx = TranslatorTranslate(V, a1);
        return tx.Valid ? tx.HostPhysical.QuadPart : 0;
    }

    case 0x222: // translate guest physical to host physical
    {
        PHYSICAL_ADDRESS hpa = TranslatorGpaToHpa(V, a1);
        return hpa.QuadPart;
    }

    case 0x320: // query current process base
    {
        PROCESS_DETAILS details = { 0 };
        if (NT_SUCCESS(ProcessQueryCurrent(&details)))
            return details.ImageBase;
        return 0;
    }

    case 0x321: // query process base by pid
    {
        PROCESS_DETAILS details = { 0 };
        if (NT_SUCCESS(ProcessQueryByPid((HANDLE)a1, &details)))
            return details.ImageBase;
        return 0;
    }

    case 0x322: // query process dirbase by pid
    {
        PROCESS_DETAILS details = { 0 };
        if (NT_SUCCESS(ProcessQueryByPid((HANDLE)a1, &details)))
            return details.DirectoryTableBase;
        return 0;
    }

    case 0x323: // PHASE 1.7: Find module base (a1 = CR3, a2 = unused, a3 = unused)
    {
        UINT64 moduleBase = 0;
        NTSTATUS status = ProcessFindModuleBase(a1, "stalcraft.exe", &moduleBase);
        if (NT_SUCCESS(status))
        {
            DbgPrint("[HV] Module base found: 0x%llX\n", moduleBase);
            return moduleBase;
        }
        return 0;
    }

    case 0x300: // enable syscall hook
        HookInstallSyscall(V);
        return TRUE;

    case 0x301:
        HookRemoveSyscall();
        return TRUE;

    default:
        return 0xDEADBEEF;
    }
}



VOID HookIoIntercept(VCPU* V)
{
    UNREFERENCED_PARAMETER(V);
    //  todo:    IO 
}

