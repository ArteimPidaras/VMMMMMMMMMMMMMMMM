#pragma once
#include <ntifs.h>
#include "vcpu.h"

// CR3 History Entry for anti-cheat detection
typedef struct _CR3_HISTORY_ENTRY {
    UINT64 Cr3Value;
    UINT64 Timestamp;
    UINT32 SwitchCount;
} CR3_HISTORY_ENTRY;

// Process Attachment API

NTSTATUS ProcessAttachTarget(HANDLE Pid, UINT64 ImageBase);
BOOLEAN ProcessIsTargetAttached(void);
BOOLEAN ProcessIsCr3Captured(void);
UINT64 ProcessGetCapturedCr3(void);
VOID ProcessInterceptCr3Load(VCPU* Vcpu, UINT64 NewCr3);
NTSTATUS ProcessManualCaptureCr3(HANDLE Pid);

// Enhanced CR3 Tracking and Anti-Cheat Detection
BOOLEAN ProcessIsAntiCheatDetected(void);
UINT32 ProcessGetCr3ChangeCount(void);
UINT64 ProcessGetPrimaryCr3(void);
UINT64 ProcessGetSecondaryCr3(void);
VOID ProcessGetCr3History(CR3_HISTORY_ENTRY* OutHistory, UINT32 MaxEntries, UINT32* OutCount);
NTSTATUS ProcessReinjectOnAllCr3s(VCPU* Vcpu);
NTSTATUS ProcessCaptureCr3FromEprocess(HANDLE Pid, UINT64* OutCr3, UINT16* OutAsid);
