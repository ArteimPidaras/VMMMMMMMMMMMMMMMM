#include <Windows.h>
#include <stdio.h>
#include "shared_memory_client.h"

#define SHARED_MEMORY_NAME "Global\\SvmHypervisorShm"
#define SHARED_MEMORY_SIZE 0x10000

// Command codes (must match kernel)
#define SHM_CMD_NONE                0
#define SHM_CMD_TEST_CONNECTION     1
#define SHM_CMD_ATTACH_PROCESS      2
#define SHM_CMD_QUERY_CR3           3
#define SHM_CMD_INJECT_DLL          4
#define SHM_CMD_QUERY_CR3_CHANGES   5
#define SHM_CMD_QUERY_ANTICHEAT     6
#define SHM_CMD_REINJECT_ALL_CR3    7

// Status codes (must match kernel)
#define SHM_STATUS_IDLE             0
#define SHM_STATUS_REQUEST_PENDING  1
#define SHM_STATUS_PROCESSING       2
#define SHM_STATUS_RESPONSE_READY   3
#define SHM_STATUS_ERROR            4

// Shared memory structure (must match kernel)
typedef struct _SHARED_MEMORY_BLOCK {
    volatile ULONG Status;
    volatile ULONG Command;
    volatile ULONG ProcessId;
    volatile UINT64 Param1;
    volatile UINT64 Param2;
    volatile UINT64 Result;
    volatile LONG NtStatus;
    UCHAR Reserved[0x100];
} SHARED_MEMORY_BLOCK;

static HANDLE g_MappingHandle = INVALID_HANDLE_VALUE;
static SHARED_MEMORY_BLOCK* g_SharedMemory = NULL;

BOOL ShmConnect(void)
{
    if (g_SharedMemory != NULL)
    {
        return TRUE; // Already connected
    }
    
    // Open existing section created by kernel
    g_MappingHandle = OpenFileMappingA(
        FILE_MAP_ALL_ACCESS,
        FALSE,
        SHARED_MEMORY_NAME
    );
    
    if (g_MappingHandle == NULL)
    {
        DWORD error = GetLastError();
        printf("[!] Failed to open shared memory: %s (Error: %lu)\n", SHARED_MEMORY_NAME, error);
        
        if (error == ERROR_FILE_NOT_FOUND)
        {
            printf("[!] Shared memory not found - driver may not be loaded\n");
        }
        else if (error == ERROR_ACCESS_DENIED)
        {
            printf("[!] Access denied - run as Administrator\n");
        }
        
        return FALSE;
    }
    
    // Map view of section
    g_SharedMemory = (SHARED_MEMORY_BLOCK*)MapViewOfFile(
        g_MappingHandle,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        SHARED_MEMORY_SIZE
    );
    
    if (g_SharedMemory == NULL)
    {
        DWORD error = GetLastError();
        printf("[!] Failed to map view of file (Error: %lu)\n", error);
        CloseHandle(g_MappingHandle);
        g_MappingHandle = INVALID_HANDLE_VALUE;
        return FALSE;
    }
    
    printf("[+] Connected to hypervisor via shared memory\n");
    return TRUE;
}

VOID ShmDisconnect(void)
{
    if (g_SharedMemory != NULL)
    {
        UnmapViewOfFile(g_SharedMemory);
        g_SharedMemory = NULL;
    }
    
    if (g_MappingHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_MappingHandle);
        g_MappingHandle = INVALID_HANDLE_VALUE;
    }
    
    printf("[+] Disconnected from hypervisor\n");
}

static BOOL ShmSendCommand(ULONG command, ULONG processId, UINT64 param1, UINT64 param2, UINT64* outResult)
{
    if (g_SharedMemory == NULL)
    {
        printf("[!] Not connected to shared memory\n");
        return FALSE;
    }
    
    // Wait for idle state (with timeout)
    int timeout = 100; // 1 second
    while (g_SharedMemory->Status != SHM_STATUS_IDLE && timeout > 0)
    {
        Sleep(10);
        timeout--;
    }
    
    if (g_SharedMemory->Status != SHM_STATUS_IDLE)
    {
        printf("[!] Shared memory not idle (status: %lu)\n", g_SharedMemory->Status);
        return FALSE;
    }
    
    // Set command parameters
    g_SharedMemory->Command = command;
    g_SharedMemory->ProcessId = processId;
    g_SharedMemory->Param1 = param1;
    g_SharedMemory->Param2 = param2;
    g_SharedMemory->Result = 0;
    g_SharedMemory->NtStatus = 0;
    
    // Signal request pending
    InterlockedExchange((volatile LONG*)&g_SharedMemory->Status, SHM_STATUS_REQUEST_PENDING);
    
    // Wait for response (with timeout)
    timeout = 500; // 5 seconds
    while (g_SharedMemory->Status != SHM_STATUS_RESPONSE_READY && timeout > 0)
    {
        Sleep(10);
        timeout--;
    }
    
    if (g_SharedMemory->Status != SHM_STATUS_RESPONSE_READY)
    {
        printf("[!] Command timeout (status: %lu)\n", g_SharedMemory->Status);
        // Reset to idle
        InterlockedExchange((volatile LONG*)&g_SharedMemory->Status, SHM_STATUS_IDLE);
        return FALSE;
    }
    
    // Get result
    if (outResult)
    {
        *outResult = g_SharedMemory->Result;
    }
    
    LONG ntStatus = g_SharedMemory->NtStatus;
    
    // Reset to idle
    InterlockedExchange((volatile LONG*)&g_SharedMemory->Status, SHM_STATUS_IDLE);
    
    return (ntStatus >= 0); // NT_SUCCESS
}

BOOL ShmTestConnection(UINT64* OutResult)
{
    return ShmSendCommand(SHM_CMD_TEST_CONNECTION, 0, 0, 0, OutResult);
}

BOOL ShmAttachProcess(ULONG ProcessId, UINT64 ImageBase, UINT64* OutResult)
{
    return ShmSendCommand(SHM_CMD_ATTACH_PROCESS, ProcessId, ImageBase, 0, OutResult);
}

BOOL ShmQueryCr3(ULONG ProcessId, UINT64* OutCr3)
{
    return ShmSendCommand(SHM_CMD_QUERY_CR3, ProcessId, 0, 0, OutCr3);
}

BOOL ShmInjectDll(UINT64* OutResult)
{
    return ShmSendCommand(SHM_CMD_INJECT_DLL, 0, 0, 0, OutResult);
}

BOOL ShmQueryCr3Changes(UINT64* OutCount)
{
    return ShmSendCommand(SHM_CMD_QUERY_CR3_CHANGES, 0, 0, 0, OutCount);
}

BOOL ShmQueryAntiCheatStatus(UINT64* OutStatus)
{
    return ShmSendCommand(SHM_CMD_QUERY_ANTICHEAT, 0, 0, 0, OutStatus);
}

BOOL ShmReinjectAllCr3(UINT64* OutResult)
{
    return ShmSendCommand(SHM_CMD_REINJECT_ALL_CR3, 0, 0, 0, OutResult);
}
