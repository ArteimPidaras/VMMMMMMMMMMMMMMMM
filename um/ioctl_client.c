#include <Windows.h>
#include <stdio.h>
#include "ioctl_client.h"

#define DEVICE_PATH "\\\\.\\SvmHypervisor"

// IOCTL codes (must match kernel definitions)
#define IOCTL_HV_TEST_CONNECTION    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HV_ATTACH_PROCESS     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HV_QUERY_CR3          CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HV_INJECT_DLL         CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HV_QUERY_CR3_CHANGES  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HV_QUERY_ANTICHEAT    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HV_REINJECT_ALL_CR3   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)

static HANDLE g_DeviceHandle = INVALID_HANDLE_VALUE;

BOOL HvConnect(void)
{
    if (g_DeviceHandle != INVALID_HANDLE_VALUE)
    {
        return TRUE; // Already connected
    }
    
    g_DeviceHandle = CreateFileA(
        DEVICE_PATH,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    
    if (g_DeviceHandle == INVALID_HANDLE_VALUE)
    {
        DWORD error = GetLastError();
        printf("[!] Failed to open device: %s (Error: %lu)\n", DEVICE_PATH, error);
        
        if (error == ERROR_FILE_NOT_FOUND)
        {
            printf("[!] Device not found - driver may not be loaded\n");
        }
        else if (error == ERROR_ACCESS_DENIED)
        {
            printf("[!] Access denied - run as Administrator\n");
        }
        
        return FALSE;
    }
    
    printf("[+] Connected to hypervisor device\n");
    return TRUE;
}

VOID HvDisconnect(void)
{
    if (g_DeviceHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_DeviceHandle);
        g_DeviceHandle = INVALID_HANDLE_VALUE;
        printf("[+] Disconnected from hypervisor device\n");
    }
}

BOOL HvTestConnection(UINT64* OutResult)
{
    if (g_DeviceHandle == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }
    
    HV_RESPONSE response = { 0 };
    DWORD bytesReturned = 0;
    
    BOOL success = DeviceIoControl(
        g_DeviceHandle,
        IOCTL_HV_TEST_CONNECTION,
        NULL,
        0,
        &response,
        sizeof(response),
        &bytesReturned,
        NULL
    );
    
    if (success && bytesReturned == sizeof(response))
    {
        if (OutResult)
        {
            *OutResult = response.Result;
        }
        return TRUE;
    }
    
    printf("[!] Test connection failed (Error: %lu)\n", GetLastError());
    return FALSE;
}

BOOL HvAttachProcess(ULONG ProcessId, UINT64 ImageBase, UINT64* OutResult)
{
    if (g_DeviceHandle == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }
    
    HV_ATTACH_REQUEST request = { 0 };
    request.ProcessId = ProcessId;
    request.ImageBase = ImageBase;
    
    HV_RESPONSE response = { 0 };
    DWORD bytesReturned = 0;
    
    BOOL success = DeviceIoControl(
        g_DeviceHandle,
        IOCTL_HV_ATTACH_PROCESS,
        &request,
        sizeof(request),
        &response,
        sizeof(response),
        &bytesReturned,
        NULL
    );
    
    if (success && bytesReturned == sizeof(response))
    {
        if (OutResult)
        {
            *OutResult = response.Result;
        }
        return response.Result != 0;
    }
    
    printf("[!] Attach process failed (Error: %lu)\n", GetLastError());
    return FALSE;
}

BOOL HvQueryCr3(ULONG ProcessId, UINT64* OutCr3)
{
    if (g_DeviceHandle == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }
    
    HV_QUERY_CR3_REQUEST request = { 0 };
    request.ProcessId = ProcessId;
    
    HV_RESPONSE response = { 0 };
    DWORD bytesReturned = 0;
    
    BOOL success = DeviceIoControl(
        g_DeviceHandle,
        IOCTL_HV_QUERY_CR3,
        &request,
        sizeof(request),
        &response,
        sizeof(response),
        &bytesReturned,
        NULL
    );
    
    if (success && bytesReturned == sizeof(response))
    {
        if (OutCr3)
        {
            *OutCr3 = response.Result;
        }
        return response.Result != 0;
    }
    
    return FALSE;
}

BOOL HvInjectDll(UINT64* OutResult)
{
    if (g_DeviceHandle == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }
    
    HV_RESPONSE response = { 0 };
    DWORD bytesReturned = 0;
    
    BOOL success = DeviceIoControl(
        g_DeviceHandle,
        IOCTL_HV_INJECT_DLL,
        NULL,
        0,
        &response,
        sizeof(response),
        &bytesReturned,
        NULL
    );
    
    if (success && bytesReturned == sizeof(response))
    {
        if (OutResult)
        {
            *OutResult = response.Result;
        }
        return response.Result != 0;
    }
    
    return FALSE;
}

BOOL HvQueryCr3Changes(UINT64* OutCount)
{
    if (g_DeviceHandle == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }
    
    HV_RESPONSE response = { 0 };
    DWORD bytesReturned = 0;
    
    BOOL success = DeviceIoControl(
        g_DeviceHandle,
        IOCTL_HV_QUERY_CR3_CHANGES,
        NULL,
        0,
        &response,
        sizeof(response),
        &bytesReturned,
        NULL
    );
    
    if (success && bytesReturned == sizeof(response))
    {
        if (OutCount)
        {
            *OutCount = response.Result;
        }
        return TRUE;
    }
    
    return FALSE;
}

BOOL HvQueryAntiCheatStatus(UINT64* OutStatus)
{
    if (g_DeviceHandle == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }
    
    HV_RESPONSE response = { 0 };
    DWORD bytesReturned = 0;
    
    BOOL success = DeviceIoControl(
        g_DeviceHandle,
        IOCTL_HV_QUERY_ANTICHEAT,
        NULL,
        0,
        &response,
        sizeof(response),
        &bytesReturned,
        NULL
    );
    
    if (success && bytesReturned == sizeof(response))
    {
        if (OutStatus)
        {
            *OutStatus = response.Result;
        }
        return TRUE;
    }
    
    return FALSE;
}

BOOL HvReinjectAllCr3(UINT64* OutResult)
{
    if (g_DeviceHandle == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }
    
    HV_RESPONSE response = { 0 };
    DWORD bytesReturned = 0;
    
    BOOL success = DeviceIoControl(
        g_DeviceHandle,
        IOCTL_HV_REINJECT_ALL_CR3,
        NULL,
        0,
        &response,
        sizeof(response),
        &bytesReturned,
        NULL
    );
    
    if (success && bytesReturned == sizeof(response))
    {
        if (OutResult)
        {
            *OutResult = response.Result;
        }
        return response.Result != 0;
    }
    
    return FALSE;
}