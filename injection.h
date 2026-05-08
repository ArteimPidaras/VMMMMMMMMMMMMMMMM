#pragma once
#include <ntifs.h>
#include "vcpu.h"

// Direct Injection API - No DLL Required

typedef struct _TRAMPOLINE {
    UINT8 OriginalBytes[16];
    UINT8 JmpBack[14];
    SIZE_T OriginalSize;
} TRAMPOLINE;

// Shellcode building
PVOID GetShellcodeBuffer(SIZE_T* OutSize);
NTSTATUS BuildInlineHook(UINT64 TargetAddress, UINT64 HookAddress, UINT8* OutBuffer, SIZE_T BufferSize);
NTSTATUS BuildAbsoluteJmpHook(UINT64 HookAddress, UINT8* OutBuffer, SIZE_T BufferSize);
NTSTATUS BuildTrampoline(UINT64 OriginalAddress, const UINT8* OriginalBytes, SIZE_T OriginalSize, TRAMPOLINE* OutTrampoline);

// Direct injection
NTSTATUS InjectDirectly(VCPU* Vcpu, UINT64 TargetCr3);
VOID RemoveInjection(VCPU* Vcpu);
BOOLEAN IsInjected(void);
UINT64 GetInjectedAddress(void);
