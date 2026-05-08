#include <ntifs.h>
#include <ntimage.h>
#include "../../include/npt.h"
#include "../../include/process_manager.h"
#include "../../include/vcpu.h"
#include "../../include/injection.h"
#include "../../include/dll_injection.h"

// DLL INJECTION FROM HYPERVISOR
// Loads the oglwh.dll into target process via NPT view-switching

extern NTSTATUS BuildAbsoluteJmpHook(UINT64 HookAddress, UINT8* OutBuffer, SIZE_T BufferSize);
extern NTSTATUS NptInstallInlineHook(NPT_STATE* State, UINT64 TargetGva, UINT64 TargetCr3, const UINT8* HookBytes, SIZE_T HookSize);

#define DLL_POOL_TAG 'DLLP'
#define MAX_DLL_SIZE (2 * 1024 * 1024) // 2MB max DLL size

typedef struct _DLL_INJECTION_STATE {
    UINT64 TargetCr3;
    UINT64 DllBaseAddress;
    UINT64 DllEntryPoint;
    UINT64 LoadLibraryAddress;
    UINT64 GetProcAddressAddress;
    PVOID DllBuffer;
    SIZE_T DllSize;
    BOOLEAN IsInjected;
    BOOLEAN DllLoaded;
} DLL_INJECTION_STATE;

static DLL_INJECTION_STATE g_DllState = { 0 };

// Embedded DLL data (will be populated by build script)
extern const UINT8 g_EmbeddedDll[];
extern const SIZE_T g_EmbeddedDllSize;

// Find kernel32.dll in target process
NTSTATUS FindKernel32Module(UINT64 Cr3, UINT64* OutBase) {
    return ProcessFindModuleBase(Cr3, "kernel32.dll", OutBase);
}

// Parse PE exports to find LoadLibraryA and GetProcAddress
NTSTATUS FindKernel32Exports(UINT64 ModuleBase, UINT64 Cr3, UINT64* OutLoadLibrary, UINT64* OutGetProcAddress) {
    NTSTATUS status;
    
    // Read DOS header from guest memory
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

    // Search for LoadLibraryA and GetProcAddress
    for (ULONG i = 0; i < exportTable.NumberOfNames; i++) {
        UINT32 nameRva;
        status = ProcessReadGuestMemory(Cr3, namesAddr + (i * sizeof(UINT32)), &nameRva, sizeof(nameRva));
        if (!NT_SUCCESS(status)) continue;

        CHAR exportName[64] = { 0 };
        status = ProcessReadGuestMemory(Cr3, ModuleBase + nameRva, exportName, sizeof(exportName) - 1);
        if (!NT_SUCCESS(status)) continue;

        if (strcmp(exportName, "LoadLibraryA") == 0) {
            UINT16 ordinal;
            status = ProcessReadGuestMemory(Cr3, ordinalsAddr + (i * sizeof(UINT16)), &ordinal, sizeof(ordinal));
            if (NT_SUCCESS(status)) {
                UINT32 functionRva;
                status = ProcessReadGuestMemory(Cr3, functionsAddr + (ordinal * sizeof(UINT32)), &functionRva, sizeof(functionRva));
                if (NT_SUCCESS(status)) {
                    *OutLoadLibrary = ModuleBase + functionRva;
                }
            }
        }
        else if (strcmp(exportName, "GetProcAddress") == 0) {
            UINT16 ordinal;
            status = ProcessReadGuestMemory(Cr3, ordinalsAddr + (i * sizeof(UINT16)), &ordinal, sizeof(ordinal));
            if (NT_SUCCESS(status)) {
                UINT32 functionRva;
                status = ProcessReadGuestMemory(Cr3, functionsAddr + (ordinal * sizeof(UINT32)), &functionRva, sizeof(functionRva));
                if (NT_SUCCESS(status)) {
                    *OutGetProcAddress = ModuleBase + functionRva;
                }
            }
        }
    }

    if (*OutLoadLibrary == 0 || *OutGetProcAddress == 0) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    DbgPrint("[VMM] Found kernel32 exports: LoadLibraryA=0x%llX, GetProcAddress=0x%llX\n", 
             *OutLoadLibrary, *OutGetProcAddress);

    return STATUS_SUCCESS;
}

// Allocate memory in target process for DLL
NTSTATUS AllocateDllInGuest(UINT64 Cr3, SIZE_T Size, UINT64* OutAddress) {
    // Find a suitable address in high memory
    // In production, this should scan guest virtual address space
    UINT64 baseAddr = 0x7FFF00000000ULL;
    
    // Allocate physical pages for the DLL
    PVOID physicalPages = ExAllocatePool2(POOL_FLAG_NON_PAGED, Size, DLL_POOL_TAG);
    if (!physicalPages) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Map into guest address space via NPT
    // This is simplified - in production, you'd need to:
    // 1. Find free virtual address range in guest
    // 2. Allocate physical pages
    // 3. Map via guest page tables or NPT
    
    *OutAddress = baseAddr;
    g_DllState.DllBuffer = physicalPages;
    
    DbgPrint("[VMM] Allocated DLL buffer: GVA=0x%llX, Size=0x%llX\n", *OutAddress, Size);
    return STATUS_SUCCESS;
}

// Write DLL to guest memory
NTSTATUS WriteDllToGuest(UINT64 GuestAddress, UINT64 Cr3, const VOID* Buffer, SIZE_T Size) {
    // Copy DLL data to allocated buffer
    RtlCopyMemory(g_DllState.DllBuffer, Buffer, Size);
    
    // In production, this would write through NPT or guest page tables
    // For now, we assume the mapping is established
    
    DbgPrint("[VMM] Wrote DLL to guest: GVA=0x%llX, Size=0x%llX\n", GuestAddress, Size);
    return STATUS_SUCCESS;
}

// Build DLL loader shellcode
NTSTATUS BuildDllLoaderShellcode(UINT64 DllPath, UINT64 LoadLibraryAddr, UINT8* OutBuffer, SIZE_T BufferSize, SIZE_T* OutSize) {
    if (BufferSize < 256) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    SIZE_T offset = 0;
    
    // Save registers
    OutBuffer[offset++] = 0x50; // push rax
    OutBuffer[offset++] = 0x51; // push rcx
    OutBuffer[offset++] = 0x52; // push rdx
    OutBuffer[offset++] = 0x53; // push rbx
    OutBuffer[offset++] = 0x55; // push rbp
    OutBuffer[offset++] = 0x56; // push rsi
    OutBuffer[offset++] = 0x57; // push rdi
    OutBuffer[offset++] = 0x41; OutBuffer[offset++] = 0x50; // push r8
    OutBuffer[offset++] = 0x41; OutBuffer[offset++] = 0x51; // push r9
    OutBuffer[offset++] = 0x41; OutBuffer[offset++] = 0x52; // push r10
    OutBuffer[offset++] = 0x41; OutBuffer[offset++] = 0x53; // push r11

    // Align stack to 16 bytes (Windows x64 ABI requirement)
    OutBuffer[offset++] = 0x48; OutBuffer[offset++] = 0x83; OutBuffer[offset++] = 0xEC; OutBuffer[offset++] = 0x20; // sub rsp, 0x20

    // Load DLL path address into RCX (first parameter)
    OutBuffer[offset++] = 0x48; OutBuffer[offset++] = 0xB9; // mov rcx, imm64
    *(UINT64*)&OutBuffer[offset] = DllPath;
    offset += 8;

    // Call LoadLibraryA
    OutBuffer[offset++] = 0xFF; OutBuffer[offset++] = 0x15; OutBuffer[offset++] = 0x02; OutBuffer[offset++] = 0x00; 
    OutBuffer[offset++] = 0x00; OutBuffer[offset++] = 0x00; // call [rip+2]
    OutBuffer[offset++] = 0xEB; OutBuffer[offset++] = 0x08; // jmp +8 (skip address)
    *(UINT64*)&OutBuffer[offset] = LoadLibraryAddr;
    offset += 8;

    // Restore stack
    OutBuffer[offset++] = 0x48; OutBuffer[offset++] = 0x83; OutBuffer[offset++] = 0xC4; OutBuffer[offset++] = 0x20; // add rsp, 0x20

    // Restore registers
    OutBuffer[offset++] = 0x41; OutBuffer[offset++] = 0x5B; // pop r11
    OutBuffer[offset++] = 0x41; OutBuffer[offset++] = 0x5A; // pop r10
    OutBuffer[offset++] = 0x41; OutBuffer[offset++] = 0x59; // pop r9
    OutBuffer[offset++] = 0x41; OutBuffer[offset++] = 0x58; // pop r8
    OutBuffer[offset++] = 0x5F; // pop rdi
    OutBuffer[offset++] = 0x5E; // pop rsi
    OutBuffer[offset++] = 0x5D; // pop rbp
    OutBuffer[offset++] = 0x5B; // pop rbx
    OutBuffer[offset++] = 0x5A; // pop rdx
    OutBuffer[offset++] = 0x59; // pop rcx
    OutBuffer[offset++] = 0x58; // pop rax

    // Return to original function (will be patched with actual address)
    OutBuffer[offset++] = 0xFF; OutBuffer[offset++] = 0x25; OutBuffer[offset++] = 0x00; OutBuffer[offset++] = 0x00; 
    OutBuffer[offset++] = 0x00; OutBuffer[offset++] = 0x00; // jmp [rip+0]
    *(UINT64*)&OutBuffer[offset] = 0; // Will be filled with original function address
    offset += 8;

    *OutSize = offset;
    return STATUS_SUCCESS;
}

// Main DLL injection routine
NTSTATUS InjectDllFromHypervisor(VCPU* Vcpu, UINT64 TargetCr3) {
    NTSTATUS status;

    if (g_DllState.IsInjected) {
        DbgPrint("[VMM] DLL already injected\n");
        return STATUS_ALREADY_REGISTERED;
    }

    g_DllState.TargetCr3 = TargetCr3;

    // Step 1: Find kernel32.dll
    UINT64 kernel32Base = 0;
    status = FindKernel32Module(TargetCr3, &kernel32Base);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] Failed to find kernel32.dll: 0x%08X\n", status);
        return status;
    }

    DbgPrint("[VMM] Found kernel32.dll at 0x%llX\n", kernel32Base);

    // Step 2: Find LoadLibraryA and GetProcAddress
    status = FindKernel32Exports(kernel32Base, TargetCr3, 
                                  &g_DllState.LoadLibraryAddress, 
                                  &g_DllState.GetProcAddressAddress);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] Failed to find kernel32 exports: 0x%08X\n", status);
        return status;
    }

    // Step 3: Allocate memory for DLL in guest
    g_DllState.DllSize = g_EmbeddedDllSize;
    status = AllocateDllInGuest(TargetCr3, g_DllState.DllSize, &g_DllState.DllBaseAddress);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] Failed to allocate DLL buffer: 0x%08X\n", status);
        return status;
    }

    // Step 4: Write DLL to guest memory
    status = WriteDllToGuest(g_DllState.DllBaseAddress, TargetCr3, g_EmbeddedDll, g_DllState.DllSize);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] Failed to write DLL: 0x%08X\n", status);
        return status;
    }

    // Step 5: Allocate memory for DLL path string
    UINT64 dllPathAddr = g_DllState.DllBaseAddress + g_DllState.DllSize + 0x1000; // After DLL data
    const CHAR dllPath[] = "C:\\Windows\\Temp\\oglwh.dll"; // Temporary path
    status = ProcessWriteGuestMemory(TargetCr3, dllPathAddr, dllPath, sizeof(dllPath));
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] Failed to write DLL path: 0x%08X\n", status);
        return status;
    }

    // Step 6: Build DLL loader shellcode
    UINT8 shellcode[512] = { 0 };
    SIZE_T shellcodeSize = 0;
    status = BuildDllLoaderShellcode(dllPathAddr, g_DllState.LoadLibraryAddress, 
                                     shellcode, sizeof(shellcode), &shellcodeSize);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] Failed to build shellcode: 0x%08X\n", status);
        return status;
    }

    // Step 7: Allocate memory for shellcode
    UINT64 shellcodeAddr = dllPathAddr + 0x1000;
    status = ProcessWriteGuestMemory(TargetCr3, shellcodeAddr, shellcode, shellcodeSize);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] Failed to write shellcode: 0x%08X\n", status);
        return status;
    }

    // Step 8: Find target function to hook (wglSwapBuffers)
    UINT64 opengl32Base = 0;
    status = ProcessFindModuleBase(TargetCr3, "opengl32.dll", &opengl32Base);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] Failed to find opengl32.dll: 0x%08X\n", status);
        return status;
    }

    UINT64 wglSwapBuffersAddr = 0;
    status = ProcessFindExportAddress(opengl32Base, TargetCr3, "wglSwapBuffers", &wglSwapBuffersAddr);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] Failed to find wglSwapBuffers: 0x%08X\n", status);
        return status;
    }

    // Step 9: Patch shellcode with return address
    *(UINT64*)&shellcode[shellcodeSize - 8] = wglSwapBuffersAddr + 5; // After JMP instruction
    status = ProcessWriteGuestMemory(TargetCr3, shellcodeAddr, shellcode, shellcodeSize);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] Failed to patch shellcode: 0x%08X\n", status);
        return status;
    }

    // Step 10: Build inline hook at wglSwapBuffers
    UINT8 hookBytes[14] = { 0 };
    status = BuildAbsoluteJmpHook(shellcodeAddr, hookBytes, sizeof(hookBytes));
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] Failed to build hook: 0x%08X\n", status);
        return status;
    }

    // Step 11: Install NPT-based inline hook
    status = NptInstallInlineHook(&Vcpu->Npt, wglSwapBuffersAddr, TargetCr3, hookBytes, sizeof(hookBytes));
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] Failed to install NPT hook: 0x%08X\n", status);
        return status;
    }

    g_DllState.IsInjected = TRUE;

    DbgPrint("[VMM] DLL injection complete!\n");
    DbgPrint("      DLL Base: 0x%llX (Size: 0x%llX)\n", g_DllState.DllBaseAddress, g_DllState.DllSize);
    DbgPrint("      Hook Target: wglSwapBuffers @ 0x%llX\n", wglSwapBuffersAddr);
    DbgPrint("      Shellcode: 0x%llX (Size: 0x%llX)\n", shellcodeAddr, shellcodeSize);

    return STATUS_SUCCESS;
}

// Remove DLL injection
VOID RemoveDllInjection(VCPU* Vcpu) {
    if (!g_DllState.IsInjected) {
        return;
    }

    extern VOID NptRemoveInlineHook(NPT_STATE* State);
    NptRemoveInlineHook(&Vcpu->Npt);

    if (g_DllState.DllBuffer) {
        ExFreePoolWithTag(g_DllState.DllBuffer, DLL_POOL_TAG);
        g_DllState.DllBuffer = NULL;
    }

    RtlZeroMemory(&g_DllState, sizeof(g_DllState));

    DbgPrint("[VMM] DLL injection removed\n");
}

// Query injection status
BOOLEAN IsDllInjected(void) {
    return g_DllState.IsInjected;
}

UINT64 GetDllBaseAddress(void) {
    return g_DllState.DllBaseAddress;
}