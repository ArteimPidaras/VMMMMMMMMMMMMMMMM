#pragma once

#include <Windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Request structures
typedef struct _HV_ATTACH_REQUEST {
    ULONG ProcessId;
    UINT64 ImageBase;
} HV_ATTACH_REQUEST;

typedef struct _HV_QUERY_CR3_REQUEST {
    ULONG ProcessId;
} HV_QUERY_CR3_REQUEST;

// Response structure
typedef struct _HV_RESPONSE {
    LONG Status; // NTSTATUS
    UINT64 Result;
} HV_RESPONSE;

// Connection management
BOOL HvConnect(void);
VOID HvDisconnect(void);

// Hypervisor operations
BOOL HvTestConnection(UINT64* OutResult);
BOOL HvAttachProcess(ULONG ProcessId, UINT64 ImageBase, UINT64* OutResult);
BOOL HvQueryCr3(ULONG ProcessId, UINT64* OutCr3);
BOOL HvInjectDll(UINT64* OutResult);
BOOL HvQueryCr3Changes(UINT64* OutCount);
BOOL HvQueryAntiCheatStatus(UINT64* OutStatus);
BOOL HvReinjectAllCr3(UINT64* OutResult);

#ifdef __cplusplus
}
#endif