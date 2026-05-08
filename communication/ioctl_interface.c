#include <ntifs.h>
#include "../../include/communication.h"
#include "../../include/vcpu.h"
#include "../../include/hooks.h"
#include "../../include/process_manager.h"
#include "../../include/process_attach.h"
#include "../../include/smp.h"

#define DEVICE_NAME L"\\Device\\SvmHypervisor"
#define SYMLINK_NAME L"\\DosDevices\\SvmHypervisor"

// IOCTL codes
#define IOCTL_HV_TEST_CONNECTION    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HV_ATTACH_PROCESS     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HV_QUERY_CR3          CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HV_INJECT_DLL         CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HV_QUERY_CR3_CHANGES  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HV_QUERY_ANTICHEAT    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HV_REINJECT_ALL_CR3   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Request/Response structures
typedef struct _HV_ATTACH_REQUEST {
    ULONG ProcessId;
    UINT64 ImageBase;
} HV_ATTACH_REQUEST;

typedef struct _HV_QUERY_CR3_REQUEST {
    ULONG ProcessId;
} HV_QUERY_CR3_REQUEST;

typedef struct _HV_RESPONSE {
    NTSTATUS Status;
    UINT64 Result;
} HV_RESPONSE;

static PDEVICE_OBJECT g_DeviceObject = NULL;

// Forward declaration
extern SMP_STATE* GetSmpState(void);

NTSTATUS IoctlDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG_PTR information = 0;
    
    PVOID inputBuffer = Irp->AssociatedIrp.SystemBuffer;
    PVOID outputBuffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG inputLength = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
    
    switch (stack->Parameters.DeviceIoControl.IoControlCode)
    {
    case IOCTL_HV_TEST_CONNECTION:
    {
        DbgPrint("[IOCTL] Test connection request\n");
        
        if (outputLength >= sizeof(HV_RESPONSE))
        {
            HV_RESPONSE* response = (HV_RESPONSE*)outputBuffer;
            response->Status = STATUS_SUCCESS;
            response->Result = 0xDEADBEEFCAFEBABEULL; // Magic value to confirm connection
            information = sizeof(HV_RESPONSE);
            DbgPrint("[IOCTL] Test connection successful\n");
        }
        else
        {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;
    }
    
    case IOCTL_HV_ATTACH_PROCESS:
    {
        DbgPrint("[IOCTL] Attach process request\n");
        
        if (inputLength >= sizeof(HV_ATTACH_REQUEST) && outputLength >= sizeof(HV_RESPONSE))
        {
            HV_ATTACH_REQUEST* request = (HV_ATTACH_REQUEST*)inputBuffer;
            HV_RESPONSE* response = (HV_RESPONSE*)outputBuffer;
            
            DbgPrint("[IOCTL] Attaching to PID: %lu, ImageBase: 0x%llX\n", 
                     request->ProcessId, request->ImageBase);
            
            NTSTATUS attachStatus = ProcessAttachTarget((HANDLE)(ULONG_PTR)request->ProcessId, request->ImageBase);
            
            if (NT_SUCCESS(attachStatus))
            {
                // Try to capture CR3 immediately from EPROCESS
                ProcessManualCaptureCr3((HANDLE)(ULONG_PTR)request->ProcessId);
            }
            
            response->Status = attachStatus;
            response->Result = NT_SUCCESS(attachStatus) ? 1 : 0;
            information = sizeof(HV_RESPONSE);
            
            DbgPrint("[IOCTL] Attach result: 0x%X\n", attachStatus);
        }
        else
        {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;
    }
    
    case IOCTL_HV_QUERY_CR3:
    {
        DbgPrint("[IOCTL] Query CR3 request\n");
        
        if (inputLength >= sizeof(HV_QUERY_CR3_REQUEST) && outputLength >= sizeof(HV_RESPONSE))
        {
            HV_QUERY_CR3_REQUEST* request = (HV_QUERY_CR3_REQUEST*)inputBuffer;
            HV_RESPONSE* response = (HV_RESPONSE*)outputBuffer;
            
            if (ProcessIsCr3Captured())
            {
                response->Status = STATUS_SUCCESS;
                response->Result = ProcessGetCapturedCr3();
                DbgPrint("[IOCTL] CR3 captured: 0x%llX\n", response->Result);
            }
            else
            {
                // Try to capture from EPROCESS
                UINT64 cr3 = 0;
                UINT16 asid = 0;
                NTSTATUS captureStatus = ProcessCaptureCr3FromEprocess(
                    (HANDLE)(ULONG_PTR)request->ProcessId, &cr3, &asid);
                
                if (NT_SUCCESS(captureStatus))
                {
                    response->Status = STATUS_SUCCESS;
                    response->Result = cr3;
                    DbgPrint("[IOCTL] CR3 from EPROCESS: 0x%llX\n", cr3);
                }
                else
                {
                    response->Status = STATUS_NOT_FOUND;
                    response->Result = 0;
                    DbgPrint("[IOCTL] CR3 not captured yet\n");
                }
            }
            
            information = sizeof(HV_RESPONSE);
        }
        else
        {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;
    }
    
    case IOCTL_HV_INJECT_DLL:
    {
        DbgPrint("[IOCTL] Inject DLL request\n");
        
        if (outputLength >= sizeof(HV_RESPONSE))
        {
            HV_RESPONSE* response = (HV_RESPONSE*)outputBuffer;
            
            // TODO: Implement DLL injection
            // For now, return success as placeholder
            response->Status = STATUS_SUCCESS;
            response->Result = 1;
            information = sizeof(HV_RESPONSE);
            
            DbgPrint("[IOCTL] DLL injection placeholder\n");
        }
        else
        {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;
    }
    
    case IOCTL_HV_QUERY_CR3_CHANGES:
    {
        DbgPrint("[IOCTL] Query CR3 changes request\n");
        
        if (outputLength >= sizeof(HV_RESPONSE))
        {
            HV_RESPONSE* response = (HV_RESPONSE*)outputBuffer;
            response->Status = STATUS_SUCCESS;
            response->Result = ProcessGetCr3ChangeCount();
            information = sizeof(HV_RESPONSE);
            
            DbgPrint("[IOCTL] CR3 changes: %llu\n", response->Result);
        }
        else
        {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;
    }
    
    case IOCTL_HV_QUERY_ANTICHEAT:
    {
        DbgPrint("[IOCTL] Query anti-cheat status request\n");
        
        if (outputLength >= sizeof(HV_RESPONSE))
        {
            HV_RESPONSE* response = (HV_RESPONSE*)outputBuffer;
            response->Status = STATUS_SUCCESS;
            response->Result = ProcessIsAntiCheatDetected() ? 1 : 0;
            information = sizeof(HV_RESPONSE);
            
            DbgPrint("[IOCTL] Anti-cheat detected: %s\n", response->Result ? "YES" : "NO");
        }
        else
        {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;
    }
    
    case IOCTL_HV_REINJECT_ALL_CR3:
    {
        DbgPrint("[IOCTL] Re-inject all CR3 request\n");
        
        if (outputLength >= sizeof(HV_RESPONSE))
        {
            HV_RESPONSE* response = (HV_RESPONSE*)outputBuffer;
            
            // Get first VCPU for re-injection
            SMP_STATE* smp = GetSmpState();
            if (smp && smp->Vcpus && smp->ProcessorCount > 0)
            {
                NTSTATUS reinjectStatus = ProcessReinjectOnAllCr3s(smp->Vcpus[0]);
                response->Status = reinjectStatus;
                response->Result = NT_SUCCESS(reinjectStatus) ? 1 : 0;
            }
            else
            {
                response->Status = STATUS_UNSUCCESSFUL;
                response->Result = 0;
            }
            
            information = sizeof(HV_RESPONSE);
            DbgPrint("[IOCTL] Re-inject result: 0x%X\n", response->Status);
        }
        else
        {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;
    }
    
    default:
        DbgPrint("[IOCTL] Unknown IOCTL code: 0x%X\n", stack->Parameters.DeviceIoControl.IoControlCode);
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    
    return status;
}

NTSTATUS CreateCloseDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    
    return STATUS_SUCCESS;
}

NTSTATUS IoctlInterfaceCreate(PDRIVER_OBJECT DriverObject)
{
    NTSTATUS status;
    UNICODE_STRING deviceName;
    UNICODE_STRING symlinkName;
    
    RtlInitUnicodeString(&deviceName, DEVICE_NAME);
    RtlInitUnicodeString(&symlinkName, SYMLINK_NAME);
    
    // Create device
    status = IoCreateDevice(
        DriverObject,
        0,
        &deviceName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &g_DeviceObject
    );
    
    if (!NT_SUCCESS(status))
    {
        DbgPrint("[IOCTL] Failed to create device: 0x%X\n", status);
        return status;
    }
    
    DbgPrint("[IOCTL] Device created: %wZ\n", &deviceName);
    
    // Create symbolic link
    status = IoCreateSymbolicLink(&symlinkName, &deviceName);
    if (!NT_SUCCESS(status))
    {
        DbgPrint("[IOCTL] Failed to create symbolic link: 0x%X\n", status);
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = NULL;
        return status;
    }
    
    DbgPrint("[IOCTL] Symbolic link created: %wZ -> %wZ\n", &symlinkName, &deviceName);
    
    // Set dispatch routines
    DriverObject->MajorFunction[IRP_MJ_CREATE] = CreateCloseDispatch;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = CreateCloseDispatch;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = IoctlDispatch;
    
    DbgPrint("[IOCTL] Interface initialized successfully\n");
    
    return STATUS_SUCCESS;
}

VOID IoctlInterfaceDestroy(void)
{
    if (g_DeviceObject)
    {
        UNICODE_STRING symlinkName;
        RtlInitUnicodeString(&symlinkName, SYMLINK_NAME);
        
        IoDeleteSymbolicLink(&symlinkName);
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = NULL;
        
        DbgPrint("[IOCTL] Interface destroyed\n");
    }
}