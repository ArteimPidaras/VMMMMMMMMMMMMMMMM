#include <ntifs.h>
#include <limits.h>
#include "../../include/injection.h"

// DIRECT INJECTION PAYLOAD - Embedded in Driver
// This shellcode will be injected into the target process via NPT view-switching
// No external DLL required

// Pre-built shellcode bytes for inline hook
// This is a minimal trampoline that saves/restores registers and jumps to original

// Shellcode buffer - position-independent code
// Format: Save regs -> Custom logic -> Restore regs -> JMP original
static UINT8 g_ShellcodeTemplate[] = {
    // Save registers (push all)
    0x50,                   // push rax
    0x51,                   // push rcx
    0x52,                   // push rdx
    0x53,                   // push rbx
    0x54,                   // push rsp
    0x55,                   // push rbp
    0x56,                   // push rsi
    0x57,                   // push rdi
    0x41, 0x50,             // push r8
    0x41, 0x51,             // push r9
    0x41, 0x52,             // push r10
    0x41, 0x53,             // push r11
    0x41, 0x54,             // push r12
    0x41, 0x55,             // push r13
    0x41, 0x56,             // push r14
    0x41, 0x57,             // push r15
    0x9C,                   // pushfq
    
    // Custom hook logic would go here (NOPs for now)
    0x90, 0x90, 0x90, 0x90, 0x90,
    
    // Restore registers (pop all)
    0x9D,                   // popfq
    0x41, 0x5F,             // pop r15
    0x41, 0x5E,             // pop r14
    0x41, 0x5D,             // pop r13
    0x41, 0x5C,             // pop r12
    0x41, 0x5B,             // pop r11
    0x41, 0x5A,             // pop r10
    0x41, 0x59,             // pop r9
    0x41, 0x58,             // pop r8
    0x5F,                   // pop rdi
    0x5E,                   // pop rsi
    0x5D,                   // pop rbp
    0x5C,                   // pop rsp
    0x5B,                   // pop rbx
    0x5A,                   // pop rdx
    0x59,                   // pop rcx
    0x58,                   // pop rax
    
    // Absolute JMP to original function (14 bytes)
    0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,  // jmp [rip+0]
    0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41  // Placeholder address
};

// Get shellcode buffer and size
PVOID GetShellcodeBuffer(SIZE_T* OutSize) {
    *OutSize = sizeof(g_ShellcodeTemplate);
    return (PVOID)g_ShellcodeTemplate;
}

// Alternative: Pre-built shellcode bytes for inline hook
// JMP hook: E9 XX XX XX XX (5 bytes)
// This will be used for simple inline hooks

typedef struct _INLINE_HOOK_TEMPLATE {
    UINT8 JmpOpcode;        // 0xE9
    INT32 RelativeOffset;   // Relative offset to hook handler
} INLINE_HOOK_TEMPLATE;

NTSTATUS BuildInlineHook(UINT64 TargetAddress, UINT64 HookAddress, UINT8* OutBuffer, SIZE_T BufferSize) {
    if (BufferSize < sizeof(INLINE_HOOK_TEMPLATE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    INLINE_HOOK_TEMPLATE* hook = (INLINE_HOOK_TEMPLATE*)OutBuffer;
    hook->JmpOpcode = 0xE9;
    
    // Calculate relative offset: HookAddress - (TargetAddress + 5)
    INT64 offset = (INT64)HookAddress - (INT64)(TargetAddress + 5);
    
    if (offset > INT_MAX || offset < INT_MIN) {
        DbgPrint("[VMM] Hook offset out of range: 0x%llX\n", offset);
        return STATUS_INTEGER_OVERFLOW;
    }

    hook->RelativeOffset = (INT32)offset;

    DbgPrint("[VMM] Built inline hook: JMP 0x%08X (Target=0x%llX, Hook=0x%llX)\n", 
             hook->RelativeOffset, TargetAddress, HookAddress);

    return STATUS_SUCCESS;
}

// Alternative: Absolute JMP hook (14 bytes) - more reliable for long distances
// FF 25 00 00 00 00 [8-byte address]

typedef struct _ABSOLUTE_JMP_HOOK {
    UINT8 JmpOpcode[2];     // FF 25
    INT32 RipOffset;        // 00 00 00 00 (RIP-relative)
    UINT64 TargetAddress;   // Absolute address
} ABSOLUTE_JMP_HOOK;

NTSTATUS BuildAbsoluteJmpHook(UINT64 HookAddress, UINT8* OutBuffer, SIZE_T BufferSize) {
    if (BufferSize < sizeof(ABSOLUTE_JMP_HOOK)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    ABSOLUTE_JMP_HOOK* hook = (ABSOLUTE_JMP_HOOK*)OutBuffer;
    hook->JmpOpcode[0] = 0xFF;
    hook->JmpOpcode[1] = 0x25;
    hook->RipOffset = 0x00000000;  // RIP + 0 (address follows immediately)
    hook->TargetAddress = HookAddress;

    DbgPrint("[VMM] Built absolute JMP hook: Target=0x%llX\n", HookAddress);

    return STATUS_SUCCESS;
}

// Trampoline builder - saves original bytes and creates jump back
NTSTATUS BuildTrampoline(UINT64 OriginalAddress, const UINT8* OriginalBytes, SIZE_T OriginalSize, 
                         TRAMPOLINE* OutTrampoline) {
    if (OriginalSize > 16) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlCopyMemory(OutTrampoline->OriginalBytes, OriginalBytes, OriginalSize);
    OutTrampoline->OriginalSize = OriginalSize;

    // Build absolute JMP back to original function + hook size
    ABSOLUTE_JMP_HOOK* jmpBack = (ABSOLUTE_JMP_HOOK*)OutTrampoline->JmpBack;
    jmpBack->JmpOpcode[0] = 0xFF;
    jmpBack->JmpOpcode[1] = 0x25;
    jmpBack->RipOffset = 0x00000000;
    jmpBack->TargetAddress = OriginalAddress + 5; // Skip our hook

    DbgPrint("[VMM] Built trampoline: OriginalSize=%llu, JmpBack=0x%llX\n", 
             OriginalSize, jmpBack->TargetAddress);

    return STATUS_SUCCESS;
}
