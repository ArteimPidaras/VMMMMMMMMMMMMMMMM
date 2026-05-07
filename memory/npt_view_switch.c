#include <ntifs.h>
#include "../../include/npt.h"
#include "../../include/vcpu.h"

// PART 3: NPT VIEW-SWITCHING INJECTION

#define PAGE_SIZE 4096
#define EXITINFO1_FETCH_BIT (1ULL << 2)

extern VOID __invlpga(UINT64 VirtualAddress, UINT32 Asid);

typedef struct _HOOK_PAGE_STATE {
    UINT64 TargetGva;
    UINT64 TargetGpa;
    UINT64 OriginalHpa;
    UINT64 InjectedHpa;
    PVOID OriginalPageVa;
    PVOID InjectedPageVa;
    BOOLEAN IsActive;
    UINT8 OriginalBytes[16];
    UINT8 PatchedBytes[16];
    SIZE_T PatchSize;
} HOOK_PAGE_STATE;

static HOOK_PAGE_STATE g_HookState = { 0 };

// PART 3.1: Physical Allocation
NTSTATUS NptAllocateHiddenPages(NPT_STATE* State) {
    // Allocate Original Page (for Read View)
    PHYSICAL_ADDRESS lowAddr = { 0 };
    PHYSICAL_ADDRESS highAddr;
    highAddr.QuadPart = -1;
    PHYSICAL_ADDRESS boundaryAddr = { 0 };

    State->HiddenBufferSize = PAGE_SIZE * 2; // Original + Injected
    PVOID hiddenBufferVa = MmAllocateContiguousMemory(State->HiddenBufferSize, highAddr);
    
    if (!hiddenBufferVa) {
        DbgPrint("[VMM] Failed to allocate hidden pages\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    State->HiddenBufferPa = MmGetPhysicalAddress(hiddenBufferVa);
    State->HiddenBuffer = MmMapIoSpace(State->HiddenBufferPa, State->HiddenBufferSize, MmNonCached);
    if (!State->HiddenBuffer) {
        MmFreeContiguousMemory(hiddenBufferVa);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(State->HiddenBuffer, State->HiddenBufferSize);

    DbgPrint("[VMM] Hidden pages allocated: HPA=0x%llX, Size=0x%llX\n", 
             State->HiddenBufferPa.QuadPart, State->HiddenBufferSize);

    return STATUS_SUCCESS;
}

// PART 3.2: Backup Original Bytes
NTSTATUS NptBackupOriginalPage(UINT64 TargetGva, UINT64 Cr3, PVOID OutBuffer, SIZE_T Size) {
    // Map the target page in guest context
    // This requires walking the guest page tables using the captured CR3
    
    // For simplicity, assume we can read guest memory directly
    // In production, you'd walk guest page tables to translate GVA->GPA->HPA
    
    PHYSICAL_ADDRESS targetPa;
    targetPa.QuadPart = TargetGva; // Simplified - should be GPA after translation
    
    PVOID mappedPage = MmMapIoSpace(targetPa, Size, MmNonCached);
    if (!mappedPage) {
        return STATUS_UNSUCCESSFUL;
    }

    RtlCopyMemory(OutBuffer, mappedPage, Size);
    MmUnmapIoSpace(mappedPage, Size);

    DbgPrint("[VMM] Backed up original page: GVA=0x%llX, Size=0x%llX\n", TargetGva, Size);

    return STATUS_SUCCESS;
}

// PART 3.3: Create Patched Page with Inline Hook
NTSTATUS NptCreatePatchedPage(PVOID OriginalPage, PVOID PatchedPage, UINT64 HookOffset, 
                               const UINT8* HookBytes, SIZE_T HookSize) {
    // Copy original page
    RtlCopyMemory(PatchedPage, OriginalPage, PAGE_SIZE);

    // Apply inline hook at offset
    if (HookOffset + HookSize > PAGE_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlCopyMemory((UINT8*)PatchedPage + HookOffset, HookBytes, HookSize);

    DbgPrint("[VMM] Created patched page: Offset=0x%llX, HookSize=0x%llX\n", HookOffset, HookSize);

    return STATUS_SUCCESS;
}

// PART 3.4: NPT Configuration - Dual View Setup
NTSTATUS NptConfigureDualView(NPT_STATE* State, UINT64 TargetGpa, 
                               UINT64 OriginalHpa, UINT64 InjectedHpa) {
    // Get NPT entry for the target GPA
    NPT_ENTRY* pEntry = NULL;
    
    // Walk NPT to find the entry (simplified - assumes 4KB pages)
    UINT64 pml4Index = (TargetGpa >> 39) & 0x1FF;
    UINT64 pdptIndex = (TargetGpa >> 30) & 0x1FF;
    UINT64 pdIndex = (TargetGpa >> 21) & 0x1FF;
    UINT64 ptIndex = (TargetGpa >> 12) & 0x1FF;

    if (!State->Pml4[pml4Index].Present) {
        DbgPrint("[VMM] PML4 entry not present\n");
        return STATUS_UNSUCCESSFUL;
    }

    NPT_ENTRY* pdpt = (NPT_ENTRY*)((State->Pml4[pml4Index].PageFrame << 12));
    if (!pdpt[pdptIndex].Present) {
        DbgPrint("[VMM] PDPT entry not present\n");
        return STATUS_UNSUCCESSFUL;
    }

    NPT_ENTRY* pd = (NPT_ENTRY*)((pdpt[pdptIndex].PageFrame << 12));
    if (!pd[pdIndex].Present) {
        DbgPrint("[VMM] PD entry not present\n");
        return STATUS_UNSUCCESSFUL;
    }

    NPT_ENTRY* pt = (NPT_ENTRY*)((pd[pdIndex].PageFrame << 12));
    pEntry = &pt[ptIndex];

    // Store original page frame
    g_HookState.OriginalHpa = pEntry->PageFrame << 12;

    // Initially set to Execute View (Injected Page)
    pEntry->PageFrame = InjectedHpa >> 12;
    pEntry->Present = 1;
    pEntry->Write = 0;  // Read-only for execute view
    pEntry->Nx = 0;     // Executable

    DbgPrint("[VMM] NPT configured for dual view: GPA=0x%llX, Execute HPA=0x%llX\n", 
             TargetGpa, InjectedHpa);

    return STATUS_SUCCESS;
}

// PART 3.5: NPF Dispatcher - View Switching
BOOLEAN NptHandleViewSwitch(NPT_STATE* State, UINT64 FaultGpa, UINT64 ErrorCode, 
                             UINT64 GuestRip, UINT16 Asid) {
    if (!g_HookState.IsActive) {
        return FALSE;
    }

    UINT64 faultPage = FaultGpa & ~0xFFFULL;
    UINT64 hookPage = g_HookState.TargetGpa & ~0xFFFULL;

    if (faultPage != hookPage) {
        return FALSE; // Not our hooked page
    }

    // Get NPT entry
    UINT64 ptIndex = (FaultGpa >> 12) & 0x1FF;
    UINT64 pdIndex = (FaultGpa >> 21) & 0x1FF;
    UINT64 pdptIndex = (FaultGpa >> 30) & 0x1FF;
    UINT64 pml4Index = (FaultGpa >> 39) & 0x1FF;

    NPT_ENTRY* pdpt = (NPT_ENTRY*)((State->Pml4[pml4Index].PageFrame << 12));
    NPT_ENTRY* pd = (NPT_ENTRY*)((pdpt[pdptIndex].PageFrame << 12));
    NPT_ENTRY* pt = (NPT_ENTRY*)((pd[pdIndex].PageFrame << 12));
    NPT_ENTRY* pEntry = &pt[ptIndex];

    // Check if this is a fetch (instruction execution) or data access
    BOOLEAN isFetch = (ErrorCode & EXITINFO1_FETCH_BIT) != 0;

    if (isFetch) {
        // Switch to Execute View (Injected Page)
        pEntry->PageFrame = g_HookState.InjectedHpa >> 12;
        pEntry->Write = 0;
        pEntry->Nx = 0;
        DbgPrint("[VMM] NPF: Switched to Execute View (Injected)\n");
    }
    else {
        // Switch to Read View (Original Page)
        pEntry->PageFrame = g_HookState.OriginalHpa >> 12;
        pEntry->Write = 1;
        pEntry->Nx = 1;
        DbgPrint("[VMM] NPF: Switched to Read View (Original)\n");
    }

    // TLB Flush - Critical for view switching
    __invlpga(FaultGpa, Asid);

    return TRUE; // Handled
}

// PART 3.6: Install NPT-Based Inline Hook
NTSTATUS NptInstallInlineHook(NPT_STATE* State, UINT64 TargetGva, UINT64 TargetCr3, 
                               const UINT8* HookBytes, SIZE_T HookSize) {
    NTSTATUS status;

    // Allocate hidden pages if not already done
    if (!State->HiddenBuffer) {
        status = NptAllocateHiddenPages(State);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    // Translate GVA to GPA (simplified - assumes identity mapping for now)
    g_HookState.TargetGva = TargetGva;
    g_HookState.TargetGpa = TargetGva; // Should use guest page table walk

    // Backup original page
    PVOID originalPageVa = State->HiddenBuffer;
    status = NptBackupOriginalPage(TargetGva & ~0xFFFULL, TargetCr3, originalPageVa, PAGE_SIZE);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Create patched page
    PVOID injectedPageVa = (UINT8*)State->HiddenBuffer + PAGE_SIZE;
    UINT64 hookOffset = TargetGva & 0xFFF;
    status = NptCreatePatchedPage(originalPageVa, injectedPageVa, hookOffset, HookBytes, HookSize);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Get physical addresses
    g_HookState.OriginalHpa = State->HiddenBufferPa.QuadPart;
    g_HookState.InjectedHpa = State->HiddenBufferPa.QuadPart + PAGE_SIZE;
    g_HookState.OriginalPageVa = originalPageVa;
    g_HookState.InjectedPageVa = injectedPageVa;
    g_HookState.PatchSize = HookSize;

    // Configure NPT dual view
    status = NptConfigureDualView(State, g_HookState.TargetGpa, 
                                   g_HookState.OriginalHpa, g_HookState.InjectedHpa);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    g_HookState.IsActive = TRUE;

    DbgPrint("[VMM] NPT Inline Hook installed: GVA=0x%llX, GPA=0x%llX\n", 
             TargetGva, g_HookState.TargetGpa);

    return STATUS_SUCCESS;
}

// PART 3.7: Remove Hook
VOID NptRemoveInlineHook(NPT_STATE* State) {
    if (!g_HookState.IsActive) {
        return;
    }

    // Restore original NPT entry
    UINT64 faultGpa = g_HookState.TargetGpa;
    UINT64 ptIndex = (faultGpa >> 12) & 0x1FF;
    UINT64 pdIndex = (faultGpa >> 21) & 0x1FF;
    UINT64 pdptIndex = (faultGpa >> 30) & 0x1FF;
    UINT64 pml4Index = (faultGpa >> 39) & 0x1FF;

    NPT_ENTRY* pdpt = (NPT_ENTRY*)((State->Pml4[pml4Index].PageFrame << 12));
    NPT_ENTRY* pd = (NPT_ENTRY*)((pdpt[pdptIndex].PageFrame << 12));
    NPT_ENTRY* pt = (NPT_ENTRY*)((pd[pdIndex].PageFrame << 12));
    NPT_ENTRY* pEntry = &pt[ptIndex];

    pEntry->PageFrame = g_HookState.OriginalHpa >> 12;
    pEntry->Write = 1;
    pEntry->Nx = 0;

    __invlpga(faultGpa, State->TargetAsid);

    g_HookState.IsActive = FALSE;

    DbgPrint("[VMM] NPT Inline Hook removed\n");
}
