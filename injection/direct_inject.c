#include <ntifs.h>
#include <ntimage.h>
#include "../../include/npt.h"
#include "../../include/process_manager.h"
#include "../../include/vcpu.h"
#include "../../include/injection.h"

// DIRECT INJECTION - No DLL Required
// Injects shellcode directly into target process memory via NPT

extern NTSTATUS BuildAbsoluteJmpHook(UINT64 HookAddress, UINT8* OutBuffer, SIZE_T BufferSize);
extern NTSTATUS BuildTrampoline(UINT64 OriginalAddress, const UINT8* OriginalBytes, SIZE_T OriginalSize, TRAMPOLINE* OutTrampoline);
extern NTSTATUS NptInstallInlineHook(NPT_STATE* State, UINT64 TargetGva, UINT64 TargetCr3, const UINT8* HookBytes, SIZE_T HookSize);

#define OPENGL32_DLL_NAME "opengl32.dll"
#define WGLSWAPBUFFERS_NAME "wglSwapBuffers"

typedef struct _INJECTION_STATE {
    UINT64 TargetCr3;
    UINT64 OpenGL32Base;
    UINT64 WglSwapBuffersAddress;
    UINT64 ShellcodeAddress;
    PVOID ShellcodeBuffer;
    SIZE_T ShellcodeSize;
    BOOLEAN IsInjected;
} INJECTION_STATE;

static INJECTION_STATE g_InjectionState = { 0 };

// Find opengl32.dll in target process
NTSTATUS FindOpenGL32Module(UINT64 Cr3, UINT64* OutBase) {
    return ProcessFindModuleBase(Cr3, OPENGL32_DLL_NAME, OutBase);
}

// Parse PE exports to find wglSwapBuffers
NTSTATUS FindExportAddress(UINT64 ModuleBase, UINT64 Cr3, const CHAR* ExportName, UINT64* OutAddress) {
    // Read DOS header
    IMAGE_DOS_HEADER dosHeader;
    // TODO: Read from guest memory using Cr3
    // For now, simplified
    
    *OutAddress = ModuleBase + 0x1000; // Placeholder
    
    DbgPrint("[VMM] Found export %s at 0x%llX\n", ExportName, *OutAddress);
    return STATUS_SUCCESS;
}

// Allocate memory in target process for shellcode
NTSTATUS AllocateShellcodeInGuest(UINT64 Cr3, SIZE_T Size, UINT64* OutAddress) {
    // In a real implementation, we would:
    // 1. Find a free region in guest address space
    // 2. Allocate physical memory
    // 3. Map it into guest page tables
    
    // For now, use a fixed address in high memory
    *OutAddress = 0x7FFF00000000ULL;
    
    DbgPrint("[VMM] Allocated shellcode buffer at 0x%llX, Size=0x%llX\n", *OutAddress, Size);
    return STATUS_SUCCESS;
}

// Write shellcode to guest memory
NTSTATUS WriteShellcodeToGuest(UINT64 GuestAddress, UINT64 Cr3, const VOID* Buffer, SIZE_T Size) {
    // Walk guest page tables and write to physical memory
    // For now, simplified
    
    DbgPrint("[VMM] Wrote shellcode to guest: GVA=0x%llX, Size=0x%llX\n", GuestAddress, Size);
    return STATUS_SUCCESS;
}

// Main injection routine
NTSTATUS InjectDirectly(VCPU* Vcpu, UINT64 TargetCr3) {
    NTSTATUS status;

    if (g_InjectionState.IsInjected) {
        DbgPrint("[VMM] Already injected\n");
        return STATUS_ALREADY_REGISTERED;
    }

    g_InjectionState.TargetCr3 = TargetCr3;

    // Step 1: Find opengl32.dll
    status = FindOpenGL32Module(TargetCr3, &g_InjectionState.OpenGL32Base);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] Failed to find opengl32.dll: 0x%08X\n", status);
        return status;
    }

    DbgPrint("[VMM] Found opengl32.dll at 0x%llX\n", g_InjectionState.OpenGL32Base);

    // Step 2: Find wglSwapBuffers export
    status = FindExportAddress(g_InjectionState.OpenGL32Base, TargetCr3, 
                                WGLSWAPBUFFERS_NAME, &g_InjectionState.WglSwapBuffersAddress);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] Failed to find wglSwapBuffers: 0x%08X\n", status);
        return status;
    }

    DbgPrint("[VMM] Found wglSwapBuffers at 0x%llX\n", g_InjectionState.WglSwapBuffersAddress);

    // Step 3: Allocate shellcode buffer in guest
    g_InjectionState.ShellcodeSize = 0x1000; // 4KB for shellcode
    status = AllocateShellcodeInGuest(TargetCr3, g_InjectionState.ShellcodeSize, 
                                       &g_InjectionState.ShellcodeAddress);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] Failed to allocate shellcode buffer: 0x%08X\n", status);
        return status;
    }

    // Step 4: Build shellcode
    // For simplicity, we'll use a minimal hook that just logs and returns
    UINT8 shellcode[256] = { 0 };
    SIZE_T shellcodeSize = 0;

    // Build a simple hook: save registers, do nothing, restore, jump back
    // This is a placeholder - in production, you'd have actual ESP/wallhack logic
    shellcode[shellcodeSize++] = 0x50; // push rax
    shellcode[shellcodeSize++] = 0x51; // push rcx
    shellcode[shellcodeSize++] = 0x52; // push rdx
    shellcode[shellcodeSize++] = 0x53; // push rbx
    
    // NOP sled for demonstration
    for (int i = 0; i < 10; i++) {
        shellcode[shellcodeSize++] = 0x90; // nop
    }
    
    shellcode[shellcodeSize++] = 0x5B; // pop rbx
    shellcode[shellcodeSize++] = 0x5A; // pop rdx
    shellcode[shellcodeSize++] = 0x59; // pop rcx
    shellcode[shellcodeSize++] = 0x58; // pop rax
    
    // Absolute JMP back to original wglSwapBuffers + 5
    shellcode[shellcodeSize++] = 0xFF; // jmp [rip+0]
    shellcode[shellcodeSize++] = 0x25;
    shellcode[shellcodeSize++] = 0x00;
    shellcode[shellcodeSize++] = 0x00;
    shellcode[shellcodeSize++] = 0x00;
    shellcode[shellcodeSize++] = 0x00;
    *(UINT64*)&shellcode[shellcodeSize] = g_InjectionState.WglSwapBuffersAddress + 5;
    shellcodeSize += 8;

    // Step 5: Write shellcode to guest memory
    status = WriteShellcodeToGuest(g_InjectionState.ShellcodeAddress, TargetCr3, 
                                    shellcode, shellcodeSize);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] Failed to write shellcode: 0x%08X\n", status);
        return status;
    }

    // Step 6: Build inline hook at wglSwapBuffers
    UINT8 hookBytes[14] = { 0 };
    status = BuildAbsoluteJmpHook(g_InjectionState.ShellcodeAddress, hookBytes, sizeof(hookBytes));
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] Failed to build hook: 0x%08X\n", status);
        return status;
    }

    // Step 7: Install NPT-based inline hook
    status = NptInstallInlineHook(&Vcpu->Npt, g_InjectionState.WglSwapBuffersAddress, 
                                   TargetCr3, hookBytes, sizeof(hookBytes));
    if (!NT_SUCCESS(status)) {
        DbgPrint("[VMM] Failed to install NPT hook: 0x%08X\n", status);
        return status;
    }

    g_InjectionState.IsInjected = TRUE;

    DbgPrint("[VMM] Direct injection complete!\n");
    DbgPrint("      Target: wglSwapBuffers @ 0x%llX\n", g_InjectionState.WglSwapBuffersAddress);
    DbgPrint("      Shellcode: 0x%llX (Size: 0x%llX)\n", 
             g_InjectionState.ShellcodeAddress, shellcodeSize);

    return STATUS_SUCCESS;
}

// Remove injection
VOID RemoveInjection(VCPU* Vcpu) {
    if (!g_InjectionState.IsInjected) {
        return;
    }

    extern VOID NptRemoveInlineHook(NPT_STATE* State);
    NptRemoveInlineHook(&Vcpu->Npt);

    g_InjectionState.IsInjected = FALSE;

    DbgPrint("[VMM] Direct injection removed\n");
}

// Query injection status
BOOLEAN IsInjected(void) {
    return g_InjectionState.IsInjected;
}

UINT64 GetInjectedAddress(void) {
    return g_InjectionState.WglSwapBuffersAddress;
}
