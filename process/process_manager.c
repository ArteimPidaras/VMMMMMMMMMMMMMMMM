#include "process_manager.h"
#include <intrin.h>

#define EPROCESS_DIRECTORY_TABLE_BASE 0x28

// PHASE 1.7: PE Header Signature
#define PE_SIGNATURE 0x00004550  // "PE\0\0"

static VOID ProcessFillInfo(PEPROCESS Process, HANDLE Pid, PPROCESS_DETAILS Details)
{
    Details->ProcessId = Pid;
    Details->ImageBase = (UINT64)PsGetProcessSectionBaseAddress(Process);

    
    Details->DirectoryTableBase = *(UINT64*)((PUCHAR)Process + EPROCESS_DIRECTORY_TABLE_BASE);
}

NTSTATUS ProcessQueryByPid(HANDLE Pid, PPROCESS_DETAILS Details)
{
    if (!Details)
        return STATUS_INVALID_PARAMETER;

    PEPROCESS process = NULL;
    NTSTATUS status = PsLookupProcessByProcessId(Pid, &process);
    if (!NT_SUCCESS(status))
        return status;

    ProcessFillInfo(process, Pid, Details);
    ObDereferenceObject(process);
    return STATUS_SUCCESS;
}

NTSTATUS ProcessQueryCurrent(PPROCESS_DETAILS Details)
{
    if (!Details)
        return STATUS_INVALID_PARAMETER;

    PEPROCESS process = PsGetCurrentProcess();
    ProcessFillInfo(process, PsGetCurrentProcessId(), Details);
    return STATUS_SUCCESS;
}

// PHASE 1.7: Module Base Finder - Scan physical memory for PE headers
UINT64 ProcessScanForPeHeader(UINT64 StartAddress, UINT64 EndAddress, UINT64 Cr3)
{
    UNREFERENCED_PARAMETER(Cr3);

    // Align to page boundaries
    StartAddress = (StartAddress + 0xFFF) & ~0xFFFULL;
    EndAddress = EndAddress & ~0xFFFULL;

    for (UINT64 addr = StartAddress; addr < EndAddress; addr += 0x1000)
    {
        PHYSICAL_ADDRESS pa = { .QuadPart = addr };
        PVOID mapped = MmMapIoSpace(pa, 0x1000, MmNonCached);
        if (!mapped)
            continue;

        __try
        {
            // Check for MZ signature
            UINT16 mz = *(UINT16*)mapped;
            if (mz == 0x5A4D)  // "MZ"
            {
                // Check PE signature
                UINT32 peOffset = *(UINT32*)((PUCHAR)mapped + 0x3C);
                if (peOffset < 0x1000 - sizeof(UINT32))
                {
                    UINT32 peSig = *(UINT32*)((PUCHAR)mapped + peOffset);
                    if (peSig == PE_SIGNATURE)
                    {
                        MmUnmapIoSpace(mapped, 0x1000);
                        DbgPrint("[PROC] Found PE header at: 0x%llX\n", addr);
                        return addr;
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Ignore access violations
        }

        MmUnmapIoSpace(mapped, 0x1000);
    }

    return 0;
}

// PHASE 1.7: Find module base by name (simplified - scans common ranges)
NTSTATUS ProcessFindModuleBase(UINT64 Cr3, const CHAR* ModuleName, UINT64* OutBase)
{
    UNREFERENCED_PARAMETER(ModuleName);

    if (!OutBase)
        return STATUS_INVALID_PARAMETER;

    *OutBase = 0;

    // Scan user-mode address space (0x10000 to 0x7FFFFFFFFFFF)
    // Focus on common executable ranges
    UINT64 ranges[][2] = {
        { 0x00400000, 0x01000000 },      // Classic 32-bit range
        { 0x140000000, 0x150000000 },    // Common 64-bit range
        { 0x7FF000000000, 0x7FF100000000 } // High user range
    };

    for (ULONG i = 0; i < ARRAYSIZE(ranges); i++)
    {
        UINT64 base = ProcessScanForPeHeader(ranges[i][0], ranges[i][1], Cr3);
        if (base != 0)
        {
            *OutBase = base;
            DbgPrint("[PROC] Module base found: 0x%llX\n", base);
            return STATUS_SUCCESS;
        }
    }

    DbgPrint("[PROC] Module not found in scanned ranges\n");
    return STATUS_NOT_FOUND;
}
