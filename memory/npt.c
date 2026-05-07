#include "npt.h"
#include "svm.h"
#include <ntifs.h>
#include <intrin.h>

#ifndef PAGE_ALIGN
#define PAGE_ALIGN(x) ((x) & ~0xFFFULL)
#endif

// PHASE 2.1: NPF Error Code Bits
#define NPF_ERROR_PRESENT       (1ULL << 0)
#define NPF_ERROR_WRITE         (1ULL << 1)
#define NPF_ERROR_USER          (1ULL << 2)
#define NPF_ERROR_RESERVED      (1ULL << 3)
#define NPF_ERROR_INSTRUCTION   (1ULL << 4)

// PHASE 1.6: Alignment Check
#define IS_ALIGNED_4K(x) (((x) & 0xFFF) == 0)
#define IS_ALIGNED_2M(x) (((x) & 0x1FFFFF) == 0)

// PHASE 2.8: INVLPGA intrinsic declaration
extern void __invlpga(void* VirtualAddress, UINT32 Asid);

// PHASE 2.7: Memory Type Synchronization - Copy PAT settings
static VOID NptSyncPatSettings(NPT_ENTRY* nptEntry, UINT64 guestPte)
{
    // Copy cache-related bits from guest PTE to NPT entry
    // Bits: WriteThrough (3), CacheDisable (4)
    nptEntry->WriteThrough = (guestPte >> 3) & 1;
    nptEntry->CacheDisable = (guestPte >> 4) & 1;

    // PAT bit (7) is also important for cache attributes
    if (guestPte & (1ULL << 7))
    {
        // Guest uses PAT - ensure we don't conflict
        // For now, just log it
        DbgPrint("[NPT] Guest PTE uses PAT bit\n");
    }
}

static NPT_ENTRY* NptAllocTable(PHYSICAL_ADDRESS* outPa)
{
    PHYSICAL_ADDRESS low = { 0 };
    PHYSICAL_ADDRESS high = { .QuadPart = ~0ULL };
    PHYSICAL_ADDRESS skip = { 0 };

    // PART 1.9: Use custom pool tag
    NPT_ENTRY* tbl = MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, low, high, skip, MmCached);
    if (!tbl)
        return NULL;

    // PART 1.10: Zero-init to prevent leakage
    RtlZeroMemory(tbl, PAGE_SIZE);
    *outPa = MmGetPhysicalAddress(tbl);
    
    // PART 1.6: Alignment check
    if (!IS_ALIGNED_4K(outPa->QuadPart))
    {
        DbgPrint("[NPT] ERROR: Allocated table not 4K aligned: 0x%llX\n", outPa->QuadPart);
        MmFreeContiguousMemory(tbl);
        return NULL;
    }
    
    return tbl;
}

static NPT_ENTRY* NptResolveTableFromEntry(NPT_ENTRY* entry)
{
    return (NPT_ENTRY*)MmGetVirtualForPhysical((PHYSICAL_ADDRESS){ entry->PageFrame << 12 });
}

static NPT_ENTRY* NptEnsureSubtable(NPT_ENTRY* parent, UINT64 index)
{
    if (!parent[index].Present)
    {
        PHYSICAL_ADDRESS pa;
        NPT_ENTRY* tbl = NptAllocTable(&pa);
        if (!tbl)
            return NULL;

        parent[index].Present = 1;
        parent[index].Write = 1;
        parent[index].PageFrame = pa.QuadPart >> 12;
    }

    return NptResolveTableFromEntry(&parent[index]);
}


static NPT_ENTRY* NptGetEntry(
    NPT_STATE* State,
    UINT64 gpa,
    UINT64* outLevel)
{
    UINT64 gpaPage = gpa >> 12;

    UINT64 pml4_i = (gpa >> 39) & 0x1FF;
    UINT64 pdpt_i = (gpa >> 30) & 0x1FF;
    UINT64 pd_i = (gpa >> 21) & 0x1FF;
    UINT64 pt_i = (gpa >> 12) & 0x1FF;

    NPT_ENTRY* pml4 = State->Pml4;
    if (!pml4[pml4_i].Present)
        return NULL;

    NPT_ENTRY* pdpt = (NPT_ENTRY*)MmGetVirtualForPhysical(
        (PHYSICAL_ADDRESS) {
        pml4[pml4_i].PageFrame << 12
    });

    if (!pdpt[pdpt_i].Present)
        return NULL;

    if (pdpt[pdpt_i].LargePage)
    {
        *outLevel = 1;
        return &pdpt[pdpt_i];
    }

    NPT_ENTRY* pd = (NPT_ENTRY*)MmGetVirtualForPhysical(
        (PHYSICAL_ADDRESS) {
        pdpt[pdpt_i].PageFrame << 12
    });

    if (!pd[pd_i].Present)
        return NULL;

    if (pd[pd_i].LargePage)
    {
        *outLevel = 2;
        return &pd[pd_i];
    }

    NPT_ENTRY* pt = (NPT_ENTRY*)MmGetVirtualForPhysical(
        (PHYSICAL_ADDRESS) {
        pd[pd_i].PageFrame << 12
    });

    *outLevel = 3;
    return &pt[pt_i];
}

static BOOLEAN NptReadGuestQword(NPT_STATE* State, UINT64 gpa, UINT64* outValue)
{
    PHYSICAL_ADDRESS hpa = NptTranslateGpaToHpa(State, gpa);
    if (!hpa.QuadPart)
        return FALSE;

    PVOID mapped = MmMapIoSpace(hpa, sizeof(UINT64), MmNonCached);
    if (!mapped)
        return FALSE;

    *outValue = *(volatile UINT64*)mapped;
    MmUnmapIoSpace(mapped, sizeof(UINT64));
    return TRUE;
}

static VOID NptProtectPageForTrap(NPT_STATE* State, UINT64 gpa, NPT_ENTRY* entry,
    UINT64* originalFrame,
    BOOLEAN arm)
{
    UNREFERENCED_PARAMETER(State);
    UNREFERENCED_PARAMETER(gpa);
    
    if (!entry)
        return;

    if (arm)
    {
        *originalFrame = entry->PageFrame;
        entry->Present = 0; 
    }
    else
    {
        entry->PageFrame = *originalFrame;
        entry->Present = 1;
    }
}


PHYSICAL_ADDRESS NptTranslateGpaToHpa(NPT_STATE* State, UINT64 gpa)
{
    PHYSICAL_ADDRESS pa = { 0 };
    UINT64 level;

    NPT_ENTRY* entry = NptGetEntry(State, gpa, &level);
    if (!entry)
        return pa;

    UINT64 offset = gpa & 0xFFFULL;
    pa.QuadPart = (entry->PageFrame << 12) + offset;
    return pa;
}


PHYSICAL_ADDRESS NptTranslateGvaToHpa(NPT_STATE* State, UINT64 gva)
{
    PHYSICAL_ADDRESS pa = { 0 };

    if (!State->ShadowCr3)
        return pa;

    UINT64 cr3 = State->ShadowCr3 & ~0xFFFULL;
    UINT64 index = (gva >> 39) & 0x1FF;

    UINT64 pml4e;
    if (!NptReadGuestQword(State, cr3 + index * sizeof(UINT64), &pml4e) || !(pml4e & PAGE_PRESENT))
        return pa;

    UINT64 pdpt = pml4e & ~0xFFFULL;
    index = (gva >> 30) & 0x1FF;

    UINT64 pdpte;
    if (!NptReadGuestQword(State, pdpt + index * sizeof(UINT64), &pdpte) || !(pdpte & PAGE_PRESENT))
        return pa;

    if (pdpte & (1ULL << 7))
    {
        pa.QuadPart = (pdpte & ~0x3FFFFFFFULL) + (gva & 0x3FFFFFFFULL);
        pa = NptTranslateGpaToHpa(State, pa.QuadPart);
        return pa;
    }

    UINT64 pd = pdpte & ~0xFFFULL;
    index = (gva >> 21) & 0x1FF;

    UINT64 pde;
    if (!NptReadGuestQword(State, pd + index * sizeof(UINT64), &pde) || !(pde & PAGE_PRESENT))
        return pa;

    if (pde & (1ULL << 7))
    {
        pa.QuadPart = (pde & ~0x1FFFFFULL) + (gva & 0x1FFFFFULL);
        pa = NptTranslateGpaToHpa(State, pa.QuadPart);
        return pa;
    }

    UINT64 pt = pde & ~0xFFFULL;
    index = (gva >> 12) & 0x1FF;

    UINT64 pte;
    if (!NptReadGuestQword(State, pt + index * sizeof(UINT64), &pte) || !(pte & PAGE_PRESENT))
        return pa;

    pa.QuadPart = (pte & ~0xFFFULL) + (gva & 0xFFFULL);
    pa = NptTranslateGpaToHpa(State, pa.QuadPart);
    return pa;
}


BOOLEAN NptHookPage(NPT_STATE* State, UINT64 targetGpaPage, UINT64 newHpaPage)
{
    UINT64 level;

    NPT_ENTRY* entry = NptGetEntry(State, targetGpaPage, &level);
    if (!entry)
        return FALSE;

    entry->PageFrame = (newHpaPage >> 12);
    entry->Dirty = 1;
    entry->Accessed = 1;

    return TRUE;
}

VOID NptUpdateShadowCr3(NPT_STATE* State, UINT64 GuestCr3)
{
    State->ShadowCr3 = GuestCr3;
}

static BOOLEAN NptArmTrap(NPT_STATE* State, UINT64 gpa, NPT_ENTRY* entry,
    UINT64* originalFrame,
    BOOLEAN* armed)
{
    UNREFERENCED_PARAMETER(originalFrame);
    
    if (!entry)
        return FALSE;

    NptProtectPageForTrap(State, gpa, entry, originalFrame, TRUE);
    *armed = TRUE;
    return TRUE;
}

static BOOLEAN NptPromoteTrapToFake(NPT_STATE* State, NPT_ENTRY* entry)
{
    if (!entry)
        return FALSE;

    ULONG slot = State->FakePageIndex & 1;
    PHYSICAL_ADDRESS fakePa = State->FakePagePa[slot];
    if (!fakePa.QuadPart)
        return FALSE;

    entry->PageFrame = fakePa.QuadPart >> 12;
    entry->Present = 1;
    entry->Write = 1;
    entry->Accessed = 1;
    entry->Dirty = 1;

    State->FakePageIndex ^= 1; 
    return TRUE;
}

static BOOLEAN NptHandleSingleTrigger(NPT_STATE* State,
    UINT64 gpa,
    NPT_ENTRY* entry,
    UINT64* originalFrame,
    BOOLEAN* armed,
    BOOLEAN* usingFake,
    UINT64* mailboxValue)
{
    if (!*armed || !entry)
        return FALSE;

    if ((gpa & ~0xFFFULL) != (entry->PageFrame << 12) && entry->Present)
    {
       
        return FALSE;
    }

    if (!*usingFake)
    {
        *usingFake = NptPromoteTrapToFake(State, entry);
        *armed = FALSE;
        if (mailboxValue)
            *mailboxValue = gpa;
        return *usingFake;
    }

    return FALSE;
}

BOOLEAN NptSetupHardwareTriggers(NPT_STATE* State, UINT64 apicGpa, UINT64 acpiGpa, UINT64 smmGpa, UINT64 mmioGpa)
{
    UINT64 level;

    NPT_ENTRY* apic = NptGetEntry(State, apicGpa, &level);
    NPT_ENTRY* acpi = NptGetEntry(State, acpiGpa, &level);
    NPT_ENTRY* smm = NptGetEntry(State, smmGpa, &level);
    NPT_ENTRY* mmio = NptGetEntry(State, mmioGpa, &level);

    BOOLEAN ok = TRUE;
    ok &= NptArmTrap(State, apicGpa, apic, &State->Apic.OriginalPageFrame, &State->Apic.Armed);
    ok &= NptArmTrap(State, acpiGpa, acpi, &State->Acpi.OriginalPageFrame, &State->Acpi.Armed);
    ok &= NptArmTrap(State, smmGpa, smm, &State->Smm.OriginalPageFrame, &State->Smm.Armed);
    ok &= NptArmTrap(State, mmioGpa, mmio, &State->Mmio.OriginalPageFrame, &State->Mmio.Armed);

    State->Apic.GpaPage = apicGpa & ~0xFFFULL;
    State->Acpi.GpaPage = acpiGpa & ~0xFFFULL;
    State->Smm.GpaPage = smmGpa & ~0xFFFULL;
    State->Mmio.GpaPage = mmioGpa & ~0xFFFULL;

    State->Apic.UsingFakePage = FALSE;
    State->Acpi.UsingFakePage = FALSE;
    State->Smm.UsingFakePage = FALSE;
    State->Mmio.UsingFakePage = FALSE;

    State->Mailbox.GpaPage = apicGpa & ~0xFFFULL;
    State->Mailbox.Active = TRUE;
    State->Mailbox.LastMessage = 0;

    return ok;
}

BOOLEAN NptHandleHardwareTriggers(NPT_STATE* State, UINT64 faultGpa, UINT64* mailboxValue)
{
    UINT64 level;

    NPT_ENTRY* apic = NptGetEntry(State, State->Apic.GpaPage, &level);
    NPT_ENTRY* acpi = NptGetEntry(State, State->Acpi.GpaPage, &level);
    NPT_ENTRY* smm = NptGetEntry(State, State->Smm.GpaPage, &level);
    NPT_ENTRY* mmio = NptGetEntry(State, State->Mmio.GpaPage, &level);

    if (NptHandleSingleTrigger(State, faultGpa, apic, &State->Apic.OriginalPageFrame, &State->Apic.Armed, &State->Apic.UsingFakePage, mailboxValue))
        return TRUE;
    if (NptHandleSingleTrigger(State, faultGpa, acpi, &State->Acpi.OriginalPageFrame, &State->Acpi.Armed, &State->Acpi.UsingFakePage, mailboxValue))
        return TRUE;
    if (NptHandleSingleTrigger(State, faultGpa, smm, &State->Smm.OriginalPageFrame, &State->Smm.Armed, &State->Smm.UsingFakePage, mailboxValue))
        return TRUE;
    if (NptHandleSingleTrigger(State, faultGpa, mmio, &State->Mmio.OriginalPageFrame, &State->Mmio.Armed, &State->Mmio.UsingFakePage, mailboxValue))
        return TRUE;

    return FALSE;
}

VOID NptRearmHardwareTriggers(NPT_STATE* State)
{
    UINT64 level;
    NPT_ENTRY* apic = NptGetEntry(State, State->Apic.GpaPage, &level);
    NPT_ENTRY* acpi = NptGetEntry(State, State->Acpi.GpaPage, &level);
    NPT_ENTRY* smm = NptGetEntry(State, State->Smm.GpaPage, &level);
    NPT_ENTRY* mmio = NptGetEntry(State, State->Mmio.GpaPage, &level);

    if (State->Apic.UsingFakePage)
    {
        if (apic)
        {
            apic->PageFrame = State->Apic.OriginalPageFrame;
            apic->Present = 0;
        }
        State->Apic.UsingFakePage = FALSE;
        State->Apic.Armed = TRUE;
    }

    if (State->Acpi.UsingFakePage)
    {
        if (acpi)
        {
            acpi->PageFrame = State->Acpi.OriginalPageFrame;
            acpi->Present = 0;
        }
        State->Acpi.UsingFakePage = FALSE;
        State->Acpi.Armed = TRUE;
    }

    if (State->Smm.UsingFakePage)
    {
        if (smm)
        {
            smm->PageFrame = State->Smm.OriginalPageFrame;
            smm->Present = 0;
        }
        State->Smm.UsingFakePage = FALSE;
        State->Smm.Armed = TRUE;
    }

    if (State->Mmio.UsingFakePage)
    {
        if (mmio)
        {
            mmio->PageFrame = State->Mmio.OriginalPageFrame;
            mmio->Present = 0;
        }
        State->Mmio.UsingFakePage = FALSE;
        State->Mmio.Armed = TRUE;
    }
}

BOOLEAN NptInstallShadowHook(NPT_STATE* State, UINT64 TargetGpa, UINT64 NewHpa)
{
    if (!State)
        return FALSE;

    State->ShadowHook.TargetGpaPage = TargetGpa & ~0xFFFULL;
    State->ShadowHook.NewHpaPage = NewHpa & ~0xFFFULL;
    State->ShadowHook.Active = TRUE;
    return TRUE;
}

VOID NptClearShadowHook(NPT_STATE* State)
{
    if (!State)
        return;

    State->ShadowHook.Active = FALSE;
    State->ShadowHook.TargetGpaPage = 0;
    State->ShadowHook.NewHpaPage = 0;
}

static UINT64 NptGetMaxPhysicalAddress()
{
    UINT64 maxPa = 0;

    PPHYSICAL_MEMORY_RANGE ranges = MmGetPhysicalMemoryRanges();
    if (!ranges)
        return 0;

    for (PPHYSICAL_MEMORY_RANGE r = ranges; r->BaseAddress.QuadPart || r->NumberOfBytes.QuadPart; r++)
    {
        UINT64 end = r->BaseAddress.QuadPart + r->NumberOfBytes.QuadPart;
        if (end > maxPa)
            maxPa = end;
    }

    ExFreePool(ranges);
    return maxPa;
}

// ============================================================================
// PART 1: NPT SHADOW PAGING INFRASTRUCTURE
// ============================================================================

// PHASE 2.3: Shadow PT Allocation
static NPT_ENTRY* NptAllocShadowPageTable(PHYSICAL_ADDRESS* outPa)
{
    return NptAllocTable(outPa);
}

// PHASE 1.4: Shadow Page Tracking
static SHADOW_PAGE_ENTRY* NptFindShadowPage(NPT_STATE* State, UINT64 Gpa)
{
    PLIST_ENTRY entry = State->ShadowPageList.Flink;
    
    while (entry != &State->ShadowPageList)
    {
        SHADOW_PAGE_ENTRY* shadow = CONTAINING_RECORD(entry, SHADOW_PAGE_ENTRY, ListEntry);
        if (shadow->GuestPhysicalPage == (Gpa & ~0xFFFULL))
            return shadow;
        entry = entry->Flink;
    }
    
    return NULL;
}

// PHASE 2.1: Page Splitting Logic - Split 2MB page into 512 4KB pages
BOOLEAN NptSplitLargePage(NPT_STATE* State, UINT64 Gpa)
{
    UINT64 level;
    NPT_ENTRY* entry = NptGetEntry(State, Gpa, &level);
    
    if (!entry || !entry->LargePage)
        return FALSE;
    
    DbgPrint("[NPT] Splitting 2MB page at GPA: 0x%llX\n", Gpa);
    
    // Allocate new PT for 512 4KB pages
    PHYSICAL_ADDRESS ptPa;
    NPT_ENTRY* pt = NptAllocTable(&ptPa);
    if (!pt)
        return FALSE;
    
    // Copy permissions from large page
    UINT64 baseFrame = entry->PageFrame;
    UINT64 basePerms = entry->Value & 0xFFF;
    
    // PHASE 2.7: Preserve cache attributes
    BOOLEAN writeThrough = entry->WriteThrough;
    BOOLEAN cacheDisable = entry->CacheDisable;
    
    // Create 512 4KB entries
    for (ULONG i = 0; i < 512; i++)
    {
        pt[i].Value = ((baseFrame + i) << 12) | basePerms;
        pt[i].LargePage = 0;
        pt[i].WriteThrough = writeThrough;
        pt[i].CacheDisable = cacheDisable;
    }
    
    // Replace large page entry with PT pointer
    entry->PageFrame = ptPa.QuadPart >> 12;
    entry->LargePage = 0;
    entry->Present = 1;
    entry->Write = 1;
    
    return TRUE;
}

// PHASE 1.8: CR3 Monitoring
VOID NptSetTargetProcess(NPT_STATE* State, UINT64 Cr3, UINT16 Asid)
{
    State->TargetCr3 = Cr3 & ~0xFFFULL;
    State->TargetAsid = Asid;
    State->Cr3MonitorActive = TRUE;
    
    DbgPrint("[NPT] Target process set: CR3=0x%llX ASID=%u\n", State->TargetCr3, Asid);
}

// PHASE 2.4: TLB Management - High-performance wrapper
VOID NptInvalidateTlb(UINT64 Gva, UINT16 Asid)
{
    __invlpga((void*)Gva, Asid);
}

// ============================================================================
// PART 2: VIEW-SWITCHING LOGIC
// ============================================================================

// PART 2.6: Access/Dirty Bit Management
static VOID NptSetAccessDirtyBits(NPT_ENTRY* entry)
{
    entry->Accessed = 1;
    entry->Dirty = 1;
}

// PART 2.4: Execute View Swap
static VOID NptSwapToExecuteView(NPT_STATE* State, SHADOW_PAGE_ENTRY* shadow, NPT_ENTRY* entry, UINT64 Gva)
{
    // PART 3.9: Atomic swapping
    UINT64 newEntry = (shadow->InjectedHostPage >> 12) << 12;
    newEntry |= NPT_EXEC_NO_RW;
    
    NptSetAccessDirtyBits(entry);
    InterlockedExchange64((LONG64*)&entry->Value, newEntry);
    
    shadow->IsExecuteView = TRUE;
    
    // PART 2.8: Invalidate TLB
    NptInvalidateTlb(Gva, State->TargetAsid);
    
    DbgPrint("[NPT] Swapped to EXEC view for GPA: 0x%llX\n", shadow->GuestPhysicalPage);
}

// PART 2.5: Read View Swap
static VOID NptSwapToReadView(NPT_STATE* State, SHADOW_PAGE_ENTRY* shadow, NPT_ENTRY* entry, UINT64 Gva)
{
    // PART 3.9: Atomic swapping
    UINT64 newEntry = (shadow->OriginalHostPage >> 12) << 12;
    newEntry |= NPT_RW_NO_EXEC;
    
    NptSetAccessDirtyBits(entry);
    InterlockedExchange64((LONG64*)&entry->Value, newEntry);
    
    shadow->IsExecuteView = FALSE;
    
    // PART 2.8: Invalidate TLB
    NptInvalidateTlb(Gva, State->TargetAsid);
    
    DbgPrint("[NPT] Swapped to READ view for GPA: 0x%llX\n", shadow->GuestPhysicalPage);
}

// PART 2.10: Write Protection Handler
static BOOLEAN NptHandleWriteToShadow(NPT_STATE* State, SHADOW_PAGE_ENTRY* shadow, UINT64 FaultGpa)
{
    // Mirror write to injected buffer only, keep original clean
    DbgPrint("[NPT] Write to shadowed page blocked: GPA=0x%llX\n", FaultGpa);
    
    // Set to read view temporarily to allow write to injected page
    UINT64 level;
    NPT_ENTRY* entry = NptGetEntry(State, FaultGpa, &level);
    if (entry)
    {
        entry->PageFrame = shadow->InjectedHostPage >> 12;
        entry->Present = 1;
        entry->Write = 1;
        entry->Nx = 1;
        NptSetAccessDirtyBits(entry);
        NptInvalidateTlb(FaultGpa, State->TargetAsid);
        return TRUE;
    }
    
    return FALSE;
}

// PART 2.3: NPF Analysis and View Switching
BOOLEAN NptHandleNpfViolation(NPT_STATE* State, UINT64 FaultGpa, UINT64 ErrorCode, UINT64 GuestRip)
{
    UNREFERENCED_PARAMETER(GuestRip);
    
    KIRQL oldIrql;
    KeAcquireSpinLock(&State->ShadowPageLock, &oldIrql);
    
    SHADOW_PAGE_ENTRY* shadow = NptFindShadowPage(State, FaultGpa);
    if (!shadow || !shadow->IsActive)
    {
        KeReleaseSpinLock(&State->ShadowPageLock, oldIrql);
        return FALSE;
    }
    
    UINT64 level;
    NPT_ENTRY* entry = NptGetEntry(State, FaultGpa, &level);
    if (!entry)
    {
        KeReleaseSpinLock(&State->ShadowPageLock, oldIrql);
        return FALSE;
    }
    
    // PART 2.3: Analyze error code
    BOOLEAN isInstructionFetch = (ErrorCode & NPF_ERROR_INSTRUCTION) != 0;
    BOOLEAN isWrite = (ErrorCode & NPF_ERROR_WRITE) != 0;
    BOOLEAN isPresent = (ErrorCode & NPF_ERROR_PRESENT) != 0;
    
    // PART 2.1: Initial state check
    if (!isPresent && !shadow->IsExecuteView)
    {
        // First access - set to read view
        NptSwapToReadView(State, shadow, entry, FaultGpa);
    }
    else if (isInstructionFetch)
    {
        // PART 2.4: Execute View Swap
        NptSwapToExecuteView(State, shadow, entry, FaultGpa);
    }
    else if (isWrite)
    {
        // PART 2.10: Write protection
        NptHandleWriteToShadow(State, shadow, FaultGpa);
    }
    else
    {
        // PART 2.5: Read View Swap
        NptSwapToReadView(State, shadow, entry, FaultGpa);
    }
    
    KeReleaseSpinLock(&State->ShadowPageLock, oldIrql);
    return TRUE;
}

// ============================================================================
// PART 1: SHADOW PAGE INSTALLATION
// ============================================================================

NTSTATUS NptShadowPageInstall(NPT_STATE* State, UINT64 Gpa, UINT64 OriginalHpa, UINT64 InjectedHpa, UINT64 TargetCr3, UINT16 Asid)
{
    if (!State)
        return STATUS_INVALID_PARAMETER;
    
    UINT64 gpaPage = Gpa & ~0xFFFULL;
    
    // PHASE 2.1: Check if we need to split a large page
    UINT64 level;
    NPT_ENTRY* entry = NptGetEntry(State, Gpa, &level);
    if (entry && entry->LargePage)
    {
        if (!NptSplitLargePage(State, Gpa))
        {
            DbgPrint("[NPT] Failed to split large page at GPA: 0x%llX\n", Gpa);
            return STATUS_UNSUCCESSFUL;
        }
        
        // Re-fetch entry after split
        entry = NptGetEntry(State, Gpa, &level);
    }
    
    if (!entry)
        return STATUS_UNSUCCESSFUL;
    
    // Allocate shadow page entry
    SHADOW_PAGE_ENTRY* shadow = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(SHADOW_PAGE_ENTRY), SHADOW_POOL_TAG);
    if (!shadow)
        return STATUS_INSUFFICIENT_RESOURCES;
    
    RtlZeroMemory(shadow, sizeof(SHADOW_PAGE_ENTRY));
    shadow->GuestPhysicalPage = gpaPage;
    shadow->OriginalHostPage = OriginalHpa & ~0xFFFULL;
    shadow->InjectedHostPage = InjectedHpa & ~0xFFFULL;
    shadow->TargetCr3 = TargetCr3;
    shadow->Asid = Asid;
    shadow->IsActive = TRUE;
    shadow->IsExecuteView = FALSE;
    
    // PHASE 2.7: Preserve cache attributes from original entry
    BOOLEAN origWriteThrough = entry->WriteThrough;
    BOOLEAN origCacheDisable = entry->CacheDisable;
    
    // PHASE 2.2: Set initial state to Read/Write but NX
    entry->PageFrame = shadow->OriginalHostPage >> 12;
    entry->Present = 1;
    entry->Write = 1;
    entry->Nx = 1;
    entry->WriteThrough = origWriteThrough;
    entry->CacheDisable = origCacheDisable;
    NptSetAccessDirtyBits(entry);
    
    // Add to tracking list
    KIRQL oldIrql;
    KeAcquireSpinLock(&State->ShadowPageLock, &oldIrql);
    InsertTailList(&State->ShadowPageList, &shadow->ListEntry);
    State->ShadowPageCount++;
    KeReleaseSpinLock(&State->ShadowPageLock, oldIrql);
    
    DbgPrint("[NPT] Shadow page installed: GPA=0x%llX Orig=0x%llX Inj=0x%llX\n", 
             gpaPage, shadow->OriginalHostPage, shadow->InjectedHostPage);
    
    return STATUS_SUCCESS;
}

VOID NptShadowPageRemove(NPT_STATE* State, UINT64 Gpa)
{
    if (!State)
        return;
    
    KIRQL oldIrql;
    KeAcquireSpinLock(&State->ShadowPageLock, &oldIrql);
    
    SHADOW_PAGE_ENTRY* shadow = NptFindShadowPage(State, Gpa);
    if (shadow)
    {
        RemoveEntryList(&shadow->ListEntry);
        State->ShadowPageCount--;
        KeReleaseSpinLock(&State->ShadowPageLock, oldIrql);
        
        // Restore original mapping
        UINT64 level;
        NPT_ENTRY* entry = NptGetEntry(State, Gpa, &level);
        if (entry)
        {
            entry->PageFrame = shadow->OriginalHostPage >> 12;
            entry->Present = 1;
            entry->Write = 1;
            entry->Nx = 0;
        }
        
        ExFreePoolWithTag(shadow, SHADOW_POOL_TAG);
        DbgPrint("[NPT] Shadow page removed: GPA=0x%llX\n", Gpa & ~0xFFFULL);
    }
    else
    {
        KeReleaseSpinLock(&State->ShadowPageLock, oldIrql);
    }
}

static UINT64 NptGetMaxPhysicalAddress_Old()
{
    UINT64 maxPa = 0;

    PPHYSICAL_MEMORY_RANGE ranges = MmGetPhysicalMemoryRanges();
    if (!ranges)
        return 0;

    for (PPHYSICAL_MEMORY_RANGE r = ranges; r->BaseAddress.QuadPart || r->NumberOfBytes.QuadPart; r++)
    {
        UINT64 end = r->BaseAddress.QuadPart + r->NumberOfBytes.QuadPart;
        if (end > maxPa)
            maxPa = end;
    }

    ExFreePool(ranges);
    return maxPa;
}


NTSTATUS NptInitialize(NPT_STATE* State)
{
    if (!State) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(State, sizeof(*State));

    // PHASE 1.4: Initialize shadow page tracking
    InitializeListHead(&State->ShadowPageList);
    KeInitializeSpinLock(&State->ShadowPageLock);
    State->ShadowPageCount = 0;
    State->Cr3MonitorActive = FALSE;

    // PHASE 2.6: Initialize hidden buffer (will be allocated on demand)
    State->HiddenBuffer = NULL;
    State->HiddenBufferPa.QuadPart = 0;
    State->HiddenBufferSize = 0;
    
    // PART 1.10: Zero-init fake pages
    for (ULONG i = 0; i < 2; i++)
    {
        State->FakePageVa[i] =
            MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE,
                (PHYSICAL_ADDRESS) {
            0
        },
                (PHYSICAL_ADDRESS) {
            .QuadPart = ~0ULL
        },
                (PHYSICAL_ADDRESS) {
            0
        }, MmCached);

        if (!State->FakePageVa[i])
        {
            DbgPrint("SVM-HV: NPT fake page alloc failed (slot=%lu)\n", i);
            return HV_STATUS_NPT_FAKEPAGE;
        }

        RtlZeroMemory(State->FakePageVa[i], PAGE_SIZE);
        State->FakePagePa[i] = MmGetPhysicalAddress(State->FakePageVa[i]);
    }

    // PART 1.3: Allocate primary PML4 (identity mapping)
    PHYSICAL_ADDRESS pml4Pa;
    NPT_ENTRY* pml4 = NptAllocTable(&pml4Pa);
    if (!pml4)
    {
        DbgPrint("SVM-HV: NPT PML4 alloc failed\n");
        return HV_STATUS_NPT_PML4;
    }

    State->Pml4 = pml4;
    State->Pml4Pa = pml4Pa;

    // PHASE 2.3: Allocate Shadow Page Table for Execute views (single page for now)
    PHYSICAL_ADDRESS shadowPageTablePa;
    NPT_ENTRY* shadowPageTable = NptAllocTable(&shadowPageTablePa);
    if (!shadowPageTable)
    {
        DbgPrint("SVM-HV: NPT Shadow Page Table alloc failed\n");
        return HV_STATUS_NPT_PML4;
    }

    State->ShadowPageTable = shadowPageTable;
    State->ShadowPageTablePa = shadowPageTablePa;

    UINT64 mapLimit = NptGetMaxPhysicalAddress();
    if (!mapLimit)
        return HV_STATUS_NPT_RANGES;

    if (mapLimit < (1ULL << 32))
        mapLimit = (1ULL << 32);

    mapLimit = (mapLimit + 0x1FFFFFULL) & ~0x1FFFFFULL;
    UINT64 pageCount = mapLimit / 0x200000ULL;

    DbgPrint("SVM-HV: NPT map limit=0x%llx pages=%llu\n", mapLimit, pageCount);

    // PART 1.3: Create 1:1 identity mapping in primary NPT
    for (UINT64 i = 0; i < pageCount; i++)
    {
        UINT64 phys = i * 0x200000ULL;

        UINT64 pml4_i = (phys >> 39) & 0x1FF;
        UINT64 pdpt_i = (phys >> 30) & 0x1FF;
        UINT64 pd_i = (phys >> 21) & 0x1FF;

        NPT_ENTRY* pdpt = NptEnsureSubtable(pml4, pml4_i);
        if (!pdpt)
        {
            DbgPrint("SVM-HV: NPT PDPT alloc failed (pml4=%llu)\n", pml4_i);
            return HV_STATUS_NPT_PDPT;
        }

        NPT_ENTRY* pd = NptEnsureSubtable(pdpt, pdpt_i);
        if (!pd)
        {
            DbgPrint("SVM-HV: NPT PD alloc failed (pml4=%llu pdpt=%llu)\n", pml4_i, pdpt_i);
            return HV_STATUS_NPT_PD;
        }

        NPT_ENTRY* pde = &pd[pd_i];
        if (!pde->Present)
        {
            pde->Present = 1;
            pde->Write = 1;
            pde->LargePage = 1;
            pde->PageFrame = phys >> 12;
        }
    }

    // PHASE 2.3: Initialize shadow page table (will be populated on demand)
    RtlZeroMemory(shadowPageTable, PAGE_SIZE);

    return STATUS_SUCCESS;
}




VOID NptDestroy(NPT_STATE* State)
{
    if (!State)
        return;

    // PHASE 4.7: Cleanup shadow pages
    while (!IsListEmpty(&State->ShadowPageList))
    {
        PLIST_ENTRY entry = RemoveHeadList(&State->ShadowPageList);
        SHADOW_PAGE_ENTRY* shadow = CONTAINING_RECORD(entry, SHADOW_PAGE_ENTRY, ListEntry);
        ExFreePoolWithTag(shadow, SHADOW_POOL_TAG);
    }

    // PHASE 2.6: Free hidden buffer if allocated
    if (State->HiddenBuffer)
    {
        MmFreeContiguousMemory(State->HiddenBuffer);
        State->HiddenBuffer = NULL;
    }

    for (ULONG i = 0; i < 2; i++)
    {
        if (State->FakePageVa[i])
            MmFreeContiguousMemory(State->FakePageVa[i]);
    }

    // Free Shadow Page Table if allocated
    if (State->ShadowPageTable)
    {
        MmFreeContiguousMemory(State->ShadowPageTable);
    }

    if (State->Pml4)
    {
        for (UINT64 pml4_i = 0; pml4_i < 512; pml4_i++)
        {
            if (!State->Pml4[pml4_i].Present)
                continue;

            NPT_ENTRY* pdpt = NptResolveTableFromEntry(&State->Pml4[pml4_i]);
            if (!pdpt)
                continue;

            for (UINT64 pdpt_i = 0; pdpt_i < 512; pdpt_i++)
            {
                if (!pdpt[pdpt_i].Present || pdpt[pdpt_i].LargePage)
                    continue;

                NPT_ENTRY* pd = NptResolveTableFromEntry(&pdpt[pdpt_i]);
                if (!pd)
                    continue;

                for (UINT64 pd_i = 0; pd_i < 512; pd_i++)
                {
                    if (!pd[pd_i].Present || pd[pd_i].LargePage)
                        continue;

                    NPT_ENTRY* pt = NptResolveTableFromEntry(&pd[pd_i]);
                    if (pt)
                        MmFreeContiguousMemory(pt);
                }

                MmFreeContiguousMemory(pd);
            }

            MmFreeContiguousMemory(pdpt);
        }

        MmFreeContiguousMemory(State->Pml4);
    }
}

