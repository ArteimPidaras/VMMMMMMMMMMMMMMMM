#pragma once
#include <ntifs.h>
#include "npt.h"
#include "vcpu.h"

// NPT View-Switching API

NTSTATUS NptAllocateHiddenPages(NPT_STATE* State);
NTSTATUS NptBackupOriginalPage(UINT64 TargetGva, UINT64 Cr3, PVOID OutBuffer, SIZE_T Size);
NTSTATUS NptCreatePatchedPage(PVOID OriginalPage, PVOID PatchedPage, UINT64 HookOffset, 
                               const UINT8* HookBytes, SIZE_T HookSize);
NTSTATUS NptConfigureDualView(NPT_STATE* State, UINT64 TargetGpa, 
                               UINT64 OriginalHpa, UINT64 InjectedHpa);
BOOLEAN NptHandleViewSwitch(NPT_STATE* State, UINT64 FaultGpa, UINT64 ErrorCode, 
                             UINT64 GuestRip, UINT16 Asid);
NTSTATUS NptInstallInlineHook(NPT_STATE* State, UINT64 TargetGva, UINT64 TargetCr3, 
                               const UINT8* HookBytes, SIZE_T HookSize);
VOID NptRemoveInlineHook(NPT_STATE* State);
