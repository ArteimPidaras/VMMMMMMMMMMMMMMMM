#include <ntifs.h>
#include "../../include/communication.h"
#include "../../include/vcpu.h"
#include "../../include/hooks.h"
#include "../../include/process_manager.h"
#include "../../include/process_attach.h"
#include "../../include/smp.h"

// Shared memory communication for kdmapper mode
// Uses a named section object that both kernel and user-mode can access

#define SHARED_MEMORY_NAME L"\\BaseNamedObjects\\SvmHypervisorShm"
#define SHARED_MEMORY_SIZE 0x10000  // 64KB

// Command codes
#define SHM_CMD_NONE                0
#define SHM_CMD_TEST_CONNECTION     1
#define SHM_CMD_ATTACH_PROCESS      2
#define SHM_CMD_QUERY_CR3           3
#define SHM_CMD_INJECT_DLL          4
#define SHM_CMD_QUERY_CR3_CHANGES   5
#define SHM_CMD_QUERY_ANTICHEAT     6
#define SHM_CMD_REINJECT_ALL_CR3    7

// Status codes
#define SHM_STATUS_IDLE             0
#define SHM_STATUS_REQUEST_PENDING  1
#define SHM_STATUS_PROCESSING       2
#define SHM_STATUS_RESPONSE_READY   3
#define SHM_STATUS_ERROR            4

// Shared memory structure
typedef struct _SHARED_MEMORY_BLOCK {
    volatile ULONG Status;          // SHM_STATUS_*
    volatile ULONG Command;         // SHM_CMD_*
    volatile ULONG ProcessId;       // For process-related commands
    volatile UINT64 Param1;         // Generic parameter 1
    volatile UINT64 Param2;         // Generic parameter 2
    volatile UINT64 Result;         // Command result
    volatile NTSTATUS NtStatus;     // NT status code
    UCHAR Reserved[0x100];          // Reserved for future use
} SHARED_MEMORY_BLOCK;

static HANDLE g_SectionHandle = NULL;
static SHARED_MEMORY_BLOCK* g_SharedMemory = NULL;
static PVOID g_WorkerThreadHandle = NULL;
static volatile BOOLEAN g_StopWorker = FALSE;

// Forward declaration
extern SMP_STATE* GetSmpState(void);

// Worker thread that processes commands from shared memory
VOID SharedMemoryWorkerThread(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    
    DbgPrint("[SHM] Worker thread started\n");
    
    while (!g_StopWorker)
    {
        if (!g_SharedMemory)
        {
            break;
        }
        
        // Check if there's a pending request
        if (InterlockedCompareExchange((volatile LONG*)&g_SharedMemory->Status, SHM_STATUS_PROCESSING, SHM_STATUS_REQUEST_PENDING) == SHM_STATUS_REQUEST_PENDING)
        {
            ULONG command = g_SharedMemory->Command;
            
            DbgPrint("[SHM] Processing command: %lu\n", command);
            
            switch (command)
            {
            case SHM_CMD_TEST_CONNECTION:
            {
                DbgPrint("[SHM] Test connection\n");
                g_SharedMemory->Result = 0xDEADBEEFCAFEBABEULL;
                g_SharedMemory->NtStatus = STATUS_SUCCESS;
                break;
            }
            
            case SHM_CMD_ATTACH_PROCESS:
            {
                ULONG pid = g_SharedMemory->ProcessId;
                UINT64 imageBase = g_SharedMemory->Param1;
                
                DbgPrint("[SHM] Attach process: PID=%lu, ImageBase=0x%llX\n", pid, imageBase);
                
                NTSTATUS status = ProcessAttachTarget((HANDLE)(ULONG_PTR)pid, imageBase);
                
                if (NT_SUCCESS(status))
                {
                    // Try to capture CR3 immediately
                    ProcessManualCaptureCr3((HANDLE)(ULONG_PTR)pid);
                }
                
                g_SharedMemory->Result = NT_SUCCESS(status) ? 1 : 0;
                g_SharedMemory->NtStatus = status;
                
                DbgPrint("[SHM] Attach result: 0x%X\n", status);
                break;
            }
            
            case SHM_CMD_QUERY_CR3:
            {
                ULONG pid = g_SharedMemory->ProcessId;
                
                DbgPrint("[SHM] Query CR3: PID=%lu\n", pid);
                
                if (ProcessIsCr3Captured())
                {
                    g_SharedMemory->Result = ProcessGetCapturedCr3();
                    g_SharedMemory->NtStatus = STATUS_SUCCESS;
                    DbgPrint("[SHM] CR3 captured: 0x%llX\n", g_SharedMemory->Result);
                }
                else
                {
                    // Try to capture from EPROCESS
                    UINT64 cr3 = 0;
                    UINT16 asid = 0;
                    NTSTATUS status = ProcessCaptureCr3FromEprocess((HANDLE)(ULONG_PTR)pid, &cr3, &asid);
                    
                    if (NT_SUCCESS(status))
                    {
                        g_SharedMemory->Result = cr3;
                        g_SharedMemory->NtStatus = STATUS_SUCCESS;
                        DbgPrint("[SHM] CR3 from EPROCESS: 0x%llX\n", cr3);
                    }
                    else
                    {
                        g_SharedMemory->Result = 0;
                        g_SharedMemory->NtStatus = STATUS_NOT_FOUND;
                        DbgPrint("[SHM] CR3 not captured yet\n");
                    }
                }
                break;
            }
            
            case SHM_CMD_INJECT_DLL:
            {
                DbgPrint("[SHM] Inject DLL (placeholder)\n");
                
                // TODO: Implement DLL injection
                g_SharedMemory->Result = 1;
                g_SharedMemory->NtStatus = STATUS_SUCCESS;
                break;
            }
            
            case SHM_CMD_QUERY_CR3_CHANGES:
            {
                DbgPrint("[SHM] Query CR3 changes\n");
                
                g_SharedMemory->Result = ProcessGetCr3ChangeCount();
                g_SharedMemory->NtStatus = STATUS_SUCCESS;
                
                DbgPrint("[SHM] CR3 changes: %llu\n", g_SharedMemory->Result);
                break;
            }
            
            case SHM_CMD_QUERY_ANTICHEAT:
            {
                DbgPrint("[SHM] Query anti-cheat status\n");
                
                g_SharedMemory->Result = ProcessIsAntiCheatDetected() ? 1 : 0;
                g_SharedMemory->NtStatus = STATUS_SUCCESS;
                
                DbgPrint("[SHM] Anti-cheat detected: %s\n", g_SharedMemory->Result ? "YES" : "NO");
                break;
            }
            
            case SHM_CMD_REINJECT_ALL_CR3:
            {
                DbgPrint("[SHM] Re-inject all CR3\n");
                
                SMP_STATE* smp = GetSmpState();
                if (smp && smp->Vcpus && smp->ProcessorCount > 0)
                {
                    NTSTATUS status = ProcessReinjectOnAllCr3s(smp->Vcpus[0]);
                    g_SharedMemory->Result = NT_SUCCESS(status) ? 1 : 0;
                    g_SharedMemory->NtStatus = status;
                }
                else
                {
                    g_SharedMemory->Result = 0;
                    g_SharedMemory->NtStatus = STATUS_UNSUCCESSFUL;
                }
                
                DbgPrint("[SHM] Re-inject result: 0x%X\n", g_SharedMemory->NtStatus);
                break;
            }
            
            default:
                DbgPrint("[SHM] Unknown command: %lu\n", command);
                g_SharedMemory->Result = 0;
                g_SharedMemory->NtStatus = STATUS_INVALID_PARAMETER;
                break;
            }
            
            // Mark response as ready
            InterlockedExchange((volatile LONG*)&g_SharedMemory->Status, SHM_STATUS_RESPONSE_READY);
        }
        
        // Sleep for a short time to avoid busy-waiting
        LARGE_INTEGER interval;
        interval.QuadPart = -10000LL; // 1ms
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
    }
    
    DbgPrint("[SHM] Worker thread exiting\n");
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS SharedMemoryCreate(void)
{
    NTSTATUS status;
    UNICODE_STRING sectionName;
    OBJECT_ATTRIBUTES objAttr;
    LARGE_INTEGER maxSize;
    
    RtlInitUnicodeString(&sectionName, SHARED_MEMORY_NAME);
    
    maxSize.QuadPart = SHARED_MEMORY_SIZE;
    
    InitializeObjectAttributes(
        &objAttr,
        &sectionName,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,
        NULL
    );
    
    // Create section object
    status = ZwCreateSection(
        &g_SectionHandle,
        SECTION_ALL_ACCESS,
        &objAttr,
        &maxSize,
        PAGE_READWRITE,
        SEC_COMMIT,
        NULL
    );
    
    if (!NT_SUCCESS(status))
    {
        DbgPrint("[SHM] Failed to create section: 0x%X\n", status);
        return status;
    }
    
    DbgPrint("[SHM] Section created: %wZ\n", &sectionName);
    
    // Map section into kernel space
    SIZE_T viewSize = 0;
    status = ZwMapViewOfSection(
        g_SectionHandle,
        ZwCurrentProcess(),
        (PVOID*)&g_SharedMemory,
        0,
        SHARED_MEMORY_SIZE,
        NULL,
        &viewSize,
        ViewUnmap,
        0,
        PAGE_READWRITE
    );
    
    if (!NT_SUCCESS(status))
    {
        DbgPrint("[SHM] Failed to map section: 0x%X\n", status);
        ZwClose(g_SectionHandle);
        g_SectionHandle = NULL;
        return status;
    }
    
    DbgPrint("[SHM] Section mapped at: 0x%p (size: 0x%llX)\n", g_SharedMemory, (UINT64)viewSize);
    
    // Initialize shared memory
    RtlZeroMemory(g_SharedMemory, sizeof(SHARED_MEMORY_BLOCK));
    g_SharedMemory->Status = SHM_STATUS_IDLE;
    g_SharedMemory->Command = SHM_CMD_NONE;
    
    // Create worker thread
    HANDLE threadHandle;
    status = PsCreateSystemThread(
        &threadHandle,
        THREAD_ALL_ACCESS,
        NULL,
        NULL,
        NULL,
        SharedMemoryWorkerThread,
        NULL
    );
    
    if (!NT_SUCCESS(status))
    {
        DbgPrint("[SHM] Failed to create worker thread: 0x%X\n", status);
        ZwUnmapViewOfSection(ZwCurrentProcess(), g_SharedMemory);
        ZwClose(g_SectionHandle);
        g_SharedMemory = NULL;
        g_SectionHandle = NULL;
        return status;
    }
    
    // Get thread object
    status = ObReferenceObjectByHandle(
        threadHandle,
        THREAD_ALL_ACCESS,
        *PsThreadType,
        KernelMode,
        &g_WorkerThreadHandle,
        NULL
    );
    
    ZwClose(threadHandle);
    
    if (!NT_SUCCESS(status))
    {
        DbgPrint("[SHM] Failed to reference thread: 0x%X\n", status);
        g_StopWorker = TRUE;
        ZwUnmapViewOfSection(ZwCurrentProcess(), g_SharedMemory);
        ZwClose(g_SectionHandle);
        g_SharedMemory = NULL;
        g_SectionHandle = NULL;
        return status;
    }
    
    DbgPrint("[SHM] Shared memory interface initialized successfully\n");
    
    return STATUS_SUCCESS;
}

VOID SharedMemoryDestroy(void)
{
    if (g_WorkerThreadHandle)
    {
        // Signal worker thread to stop
        g_StopWorker = TRUE;
        
        // Wait for thread to exit
        LARGE_INTEGER timeout;
        timeout.QuadPart = -10000000LL; // 1 second
        KeWaitForSingleObject(g_WorkerThreadHandle, Executive, KernelMode, FALSE, &timeout);
        
        ObDereferenceObject(g_WorkerThreadHandle);
        g_WorkerThreadHandle = NULL;
        
        DbgPrint("[SHM] Worker thread stopped\n");
    }
    
    if (g_SharedMemory)
    {
        ZwUnmapViewOfSection(ZwCurrentProcess(), g_SharedMemory);
        g_SharedMemory = NULL;
        DbgPrint("[SHM] Section unmapped\n");
    }
    
    if (g_SectionHandle)
    {
        ZwClose(g_SectionHandle);
        g_SectionHandle = NULL;
        DbgPrint("[SHM] Section closed\n");
    }
    
    DbgPrint("[SHM] Shared memory interface destroyed\n");
}
