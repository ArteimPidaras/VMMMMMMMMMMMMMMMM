#pragma once
#include <ntifs.h>
#include "vcpu.h"

// DLL Injection from Hypervisor
// Loads the embedded oglwh.dll into target process via NPT view-switching

// Main injection functions
NTSTATUS InjectDllFromHypervisor(VCPU* Vcpu, UINT64 TargetCr3);
VOID RemoveDllInjection(VCPU* Vcpu);

// Status queries
BOOLEAN IsDllInjected(void);
UINT64 GetDllBaseAddress(void);

// Helper functions for guest memory access
NTSTATUS ProcessReadGuestMemory(UINT64 Cr3, UINT64 GuestVa, PVOID Buffer, SIZE_T Size);
NTSTATUS ProcessWriteGuestMemory(UINT64 Cr3, UINT64 GuestVa, const VOID* Buffer, SIZE_T Size);
NTSTATUS ProcessFindModuleBase(UINT64 Cr3, const CHAR* ModuleName, UINT64* OutBase);
NTSTATUS ProcessFindExportAddress(UINT64 ModuleBase, UINT64 Cr3, const CHAR* ExportName, UINT64* OutAddress);

// Embedded DLL data (populated by build script)
extern const UINT8 g_EmbeddedDll[];
extern const SIZE_T g_EmbeddedDllSize;