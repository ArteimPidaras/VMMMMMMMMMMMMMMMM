#include <ntifs.h>
#include <ntimage.h>
#include "../../include/npt.h"
#include "../../include/vcpu.h"
#include "../../include/dll_injection.h"

// Guest Memory Access Functions
// Provides safe access to guest virtual memory through NPT translation

// Translate Guest Virtual Address to Host Physical Address
NTSTATUS TranslateGvaToHpa(UINT64 Cr3, UINT64 Gva, PHYSICAL_ADDRESS* OutHpa) {
    // Walk guest page tables using the provided CR3
    
    // Extract page table indices
    UINT64 pml4Index = (Gva >> 39) & 0x1FF;
    UINT64 pdptIndex = (Gva >> 30) & 0x1FF;
    UINT64 pdIndex = (Gva >> 21) & 0x1FF;
    UINT64 ptIndex = (Gva >> 12) & 0x1FF;
    UINT64 offset = Gva & 0xFFF;

    // Map guest CR3 to access PML4
    PHYSICAL_ADDRESS guestCr3Pa;
    guestCr3Pa.QuadPart = Cr3 & ~0xFFF; // Clear lower 12 bits
    
    UINT64* pml4 = (UINT64*)MmMapIoSpace(guestCr3Pa, PAGE_SIZE, MmNonCached);
    if (!pml4) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    UINT64 pml4Entry = pml4[pml4Index];
    MmUnmapIoSpace(pml4, PAGE_SIZE);

    if (!(pml4Entry & PAGE_PRESENT)) {
        return STATUS_INVALID_ADDRESS;
    }

    // Map PDPT
    PHYSICAL_ADDRESS pdptPa;
    pdptPa.QuadPart = pml4Entry & ~0xFFF;
    
    UINT64* pdpt = (UINT64*)MmMapIoSpace(pdptPa, PAGE_SIZE, MmNonCached);
    if (!pdpt) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    UINT64 pdptEntry = pdpt[pdptIndex];
    MmUnmapIoSpace(pdpt, PAGE_SIZE);

    if (!(pdptEntry & PAGE_PRESENT)) {
        return STATUS_INVALID_ADDRESS;
    }

    // Check for 1GB page
    if (pdptEntry & (1ULL << 7)) { // PS bit
        OutHpa->QuadPart = (pdptEntry & ~0x3FFFFFFF) + (Gva & 0x3FFFFFFF);
        return STATUS_SUCCESS;
    }

    // Map PD
    PHYSICAL_ADDRESS pdPa;
    pdPa.QuadPart = pdptEntry & ~0xFFF;
    
    UINT64* pd = (UINT64*)MmMapIoSpace(pdPa, PAGE_SIZE, MmNonCached);
    if (!pd) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    UINT64 pdEntry = pd[pdIndex];
    MmUnmapIoSpace(pd, PAGE_SIZE);

    if (!(pdEntry & PAGE_PRESENT)) {
        return STATUS_INVALID_ADDRESS;
    }

    // Check for 2MB page
    if (pdEntry & (1ULL << 7)) { // PS bit
        OutHpa->QuadPart = (pdEntry & ~0x1FFFFF) + (Gva & 0x1FFFFF);
        return STATUS_SUCCESS;
    }

    // Map PT
    PHYSICAL_ADDRESS ptPa;
    ptPa.QuadPart = pdEntry & ~0xFFF;
    
    UINT64* pt = (UINT64*)MmMapIoSpace(ptPa, PAGE_SIZE, MmNonCached);
    if (!pt) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    UINT64 ptEntry = pt[ptIndex];
    MmUnmapIoSpace(pt, PAGE_SIZE);

    if (!(ptEntry & PAGE_PRESENT)) {
        return STATUS_INVALID_ADDRESS;
    }

    // 4KB page
    OutHpa->QuadPart = (ptEntry & ~0xFFF) + offset;
    return STATUS_SUCCESS;
}

// Read from guest virtual memory
NTSTATUS ProcessReadGuestMemory(UINT64 Cr3, UINT64 GuestVa, PVOID Buffer, SIZE_T Size) {
    NTSTATUS status;
    SIZE_T bytesRead = 0;
    UINT8* outputBuffer = (UINT8*)Buffer;

    while (bytesRead < Size) {
        UINT64 currentVa = GuestVa + bytesRead;
        SIZE_T remainingBytes = Size - bytesRead;
        
        // Calculate bytes to read from current page
        UINT64 pageOffset = currentVa & 0xFFF;
        SIZE_T bytesInPage = min(remainingBytes, PAGE_SIZE - pageOffset);

        // Translate GVA to HPA
        PHYSICAL_ADDRESS hpa;
        status = TranslateGvaToHpa(Cr3, currentVa, &hpa);
        if (!NT_SUCCESS(status)) {
            DbgPrint("[VMM] GVA translation failed: 0x%llX -> 0x%08X\n", currentVa, status);
            return status;
        }

        // Map physical page and read
        PVOID mappedPage = MmMapIoSpace(hpa, PAGE_SIZE, MmNonCached);
        if (!mappedPage) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        __try {
            RtlCopyMemory(outputBuffer + bytesRead, 
                         (UINT8*)mappedPage + pageOffset, 
                         bytesInPage);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            MmUnmapIoSpace(mappedPage, PAGE_SIZE);
            return STATUS_ACCESS_VIOLATION;
        }

        MmUnmapIoSpace(mappedPage, PAGE_SIZE);
        bytesRead += bytesInPage;
    }

    return STATUS_SUCCESS;
}

// Write to guest virtual memory
NTSTATUS ProcessWriteGuestMemory(UINT64 Cr3, UINT64 GuestVa, const VOID* Buffer, SIZE_T Size) {
    NTSTATUS status;
    SIZE_T bytesWritten = 0;
    const UINT8* inputBuffer = (const UINT8*)Buffer;

    while (bytesWritten < Size) {
        UINT64 currentVa = GuestVa + bytesWritten;
        SIZE_T remainingBytes = Size - bytesWritten;
        
        // Calculate bytes to write to current page
        UINT64 pageOffset = currentVa & 0xFFF;
        SIZE_T bytesInPage = min(remainingBytes, PAGE_SIZE - pageOffset);

        // Translate GVA to HPA
        PHYSICAL_ADDRESS hpa;
        status = TranslateGvaToHpa(Cr3, currentVa, &hpa);
        if (!NT_SUCCESS(status)) {
            DbgPrint("[VMM] GVA translation failed: 0x%llX -> 0x%08X\n", currentVa, status);
            return status;
        }

        // Map physical page and write
        PVOID mappedPage = MmMapIoSpace(hpa, PAGE_SIZE, MmNonCached);
        if (!mappedPage) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        __try {
            RtlCopyMemory((UINT8*)mappedPage + pageOffset, 
                         inputBuffer + bytesWritten, 
                         bytesInPage);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            MmUnmapIoSpace(mappedPage, PAGE_SIZE);
            return STATUS_ACCESS_VIOLATION;
        }

        MmUnmapIoSpace(mappedPage, PAGE_SIZE);
        bytesWritten += bytesInPage;
    }

    return STATUS_SUCCESS;
}

// Find export address in guest module
NTSTATUS ProcessFindExportAddress(UINT64 ModuleBase, UINT64 Cr3, const CHAR* ExportName, UINT64* OutAddress) {
    NTSTATUS status;
    
    // Read DOS header
    IMAGE_DOS_HEADER dosHeader;
    status = ProcessReadGuestMemory(Cr3, ModuleBase, &dosHeader, sizeof(dosHeader));
    if (!NT_SUCCESS(status) || dosHeader.e_magic != IMAGE_DOS_SIGNATURE) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    // Read NT headers
    IMAGE_NT_HEADERS64 ntHeaders;
    UINT64 ntHeadersAddr = ModuleBase + dosHeader.e_lfanew;
    status = ProcessReadGuestMemory(Cr3, ntHeadersAddr, &ntHeaders, sizeof(ntHeaders));
    if (!NT_SUCCESS(status) || ntHeaders.Signature != IMAGE_NT_SIGNATURE) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    // Get export directory
    IMAGE_DATA_DIRECTORY exportDir = ntHeaders.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exportDir.Size == 0) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    IMAGE_EXPORT_DIRECTORY exportTable;
    UINT64 exportTableAddr = ModuleBase + exportDir.VirtualAddress;
    status = ProcessReadGuestMemory(Cr3, exportTableAddr, &exportTable, sizeof(exportTable));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Read export arrays
    UINT64 namesAddr = ModuleBase + exportTable.AddressOfNames;
    UINT64 functionsAddr = ModuleBase + exportTable.AddressOfFunctions;
    UINT64 ordinalsAddr = ModuleBase + exportTable.AddressOfNameOrdinals;

    // Search for the export
    for (ULONG i = 0; i < exportTable.NumberOfNames; i++) {
        UINT32 nameRva;
        status = ProcessReadGuestMemory(Cr3, namesAddr + (i * sizeof(UINT32)), &nameRva, sizeof(nameRva));
        if (!NT_SUCCESS(status)) continue;

        CHAR currentExportName[64] = { 0 };
        status = ProcessReadGuestMemory(Cr3, ModuleBase + nameRva, currentExportName, sizeof(currentExportName) - 1);
        if (!NT_SUCCESS(status)) continue;

        if (strcmp(currentExportName, ExportName) == 0) {
            UINT16 ordinal;
            status = ProcessReadGuestMemory(Cr3, ordinalsAddr + (i * sizeof(UINT16)), &ordinal, sizeof(ordinal));
            if (!NT_SUCCESS(status)) continue;

            UINT32 functionRva;
            status = ProcessReadGuestMemory(Cr3, functionsAddr + (ordinal * sizeof(UINT32)), &functionRva, sizeof(functionRva));
            if (!NT_SUCCESS(status)) continue;

            *OutAddress = ModuleBase + functionRva;
            DbgPrint("[VMM] Found export %s at 0x%llX\n", ExportName, *OutAddress);
            return STATUS_SUCCESS;
        }
    }

    return STATUS_PROCEDURE_NOT_FOUND;
}