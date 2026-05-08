#pragma once
#include <ntifs.h>

typedef struct _PROCESS_DETAILS
{
    HANDLE ProcessId;
    UINT64 ImageBase;
    UINT64 DirectoryTableBase;
} PROCESS_DETAILS, *PPROCESS_DETAILS;

NTSTATUS ProcessQueryByPid(HANDLE Pid, PPROCESS_DETAILS Details);
NTSTATUS ProcessQueryCurrent(PPROCESS_DETAILS Details);
EXTERN_C PVOID PsGetProcessSectionBaseAddress(PEPROCESS Process);

// PHASE 1.7: Module Base Finder
NTSTATUS ProcessFindModuleBase(UINT64 Cr3, const CHAR* ModuleName, UINT64* OutBase);
UINT64 ProcessScanForPeHeader(UINT64 StartAddress, UINT64 EndAddress, UINT64 Cr3);
