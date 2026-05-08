#ifndef SHARED_MEMORY_CLIENT_H
#define SHARED_MEMORY_CLIENT_H

#include <Windows.h>

// Connect to hypervisor via shared memory
BOOL ShmConnect(void);

// Disconnect from hypervisor
VOID ShmDisconnect(void);

// Test connection to hypervisor
BOOL ShmTestConnection(UINT64* OutResult);

// Attach to target process
BOOL ShmAttachProcess(ULONG ProcessId, UINT64 ImageBase, UINT64* OutResult);

// Query CR3 of attached process
BOOL ShmQueryCr3(ULONG ProcessId, UINT64* OutCr3);

// Inject DLL into target process
BOOL ShmInjectDll(UINT64* OutResult);

// Query CR3 change count
BOOL ShmQueryCr3Changes(UINT64* OutCount);

// Query anti-cheat detection status
BOOL ShmQueryAntiCheatStatus(UINT64* OutStatus);

// Re-inject on all known CR3 values
BOOL ShmReinjectAllCr3(UINT64* OutResult);

#endif // SHARED_MEMORY_CLIENT_H
