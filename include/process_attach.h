#pragma once
#include <ntifs.h>
#include "vcpu.h"

// Process Attachment API

NTSTATUS ProcessAttachTarget(HANDLE Pid, UINT64 ImageBase);
BOOLEAN ProcessIsTargetAttached(void);
BOOLEAN ProcessIsCr3Captured(void);
UINT64 ProcessGetCapturedCr3(void);
VOID ProcessInterceptCr3Load(VCPU* Vcpu, UINT64 NewCr3);
NTSTATUS ProcessManualCaptureCr3(HANDLE Pid);
