#include <ntifs.h>
#include "svm.h"
#include "vcpu.h"
#include "smp.h"

// Forward declarations
NTSTATUS IoctlInterfaceCreate(PDRIVER_OBJECT DriverObject);
VOID IoctlInterfaceDestroy(void);
NTSTATUS SharedMemoryCreate(void);
VOID SharedMemoryDestroy(void);

//        ,
//        `-._           __
//         \\  `-..____,.'  `.
//          :`.         /    `.
//          :  )       :      : \
//           ;'        '   ;  |  :
//           )..      .. .:.`.;  :
//          /::...  .:::...   ` ;
//          ; _ '    __        /:\
//          `:o>   /\o_>      ;:. `.
//         `-`.__ ;   __..--- /:.   \
//         === \_/   ;=====_.':.     ;
//          ,/'`--'...`--....        ;
//               ;                    ;
//             .'                      ;
//           .'                        ;
//         .'     ..     ,      .       ;
//        :       ::..  /      ;::.     |
//       /      `.;::.  |       ;:..    ;
//      :         |:.   :       ;:.    ;
//      :         ::     ;:..   |.    ;
//       :       :;      :::....|     |
//       /\     ,/ \      ;:::::;     ;
//     .:. \:..|    :     ; '.--|     ;
//    ::.  :''  `-.,,;     ;'   ;     ;
//  .-'. _.'\      / `;      \,__:      \
//  `---'    `----'   ;      /    \,.,,,/
//                    `----`              sad



static SMP_STATE g_Smp = { 0 };

#define SMP_INIT_MAX_VCPUS SMP_MAX_VCPUS_ALL

// Export SMP state for IOCTL interface
SMP_STATE* GetSmpState(void)
{
    return &g_Smp;
}

VOID DriverUnload(PDRIVER_OBJECT D)
{
    UNREFERENCED_PARAMETER(D);
    
    DbgPrint("[VMM] Driver unloading...\n");
    
    // Destroy communication interfaces
    IoctlInterfaceDestroy();
    SharedMemoryDestroy();
    
    // Shutdown hypervisor
    if (g_Smp.Vcpus)
        SmpShutdown(&g_Smp);

    DbgPrint("[VMM] Driver unloaded\n");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT D, PUNICODE_STRING R)
{
    UNREFERENCED_PARAMETER(R);
    
    DbgPrint("[VMM] ========================================\n");
    DbgPrint("[VMM] SVM Hypervisor Driver Loading...\n");
    DbgPrint("[VMM] ========================================\n");

    if (D)
    {
        D->DriverUnload = DriverUnload;
    }
    else
    {
        DbgPrint("[VMM] DriverEntry called without DriverObject (kdmapper load)\n");
    }

    // Initialize hypervisor
    DbgPrint("[VMM] Initializing hypervisor...\n");
	NTSTATUS st = SmpInitialize(&g_Smp, SMP_INIT_MAX_VCPUS);
    if (!NT_SUCCESS(st))
    {
        DbgPrint("[VMM] SmpInitialize failed: 0x%X\n", st);
        if (HV_STATUS_IS_RESOURCE(st))
        {
            DbgPrint("[VMM] Retrying with single VCPU\n");
            st = SmpInitialize(&g_Smp, 1);
        }

        if (!NT_SUCCESS(st))
        {
            DbgPrint("[VMM] Failed to initialize hypervisor\n");
            return st;
        }
    }

    DbgPrint("[VMM] Launching hypervisor on all CPUs...\n");
    st = SmpLaunch(&g_Smp);
    if (!NT_SUCCESS(st))
    {
        DbgPrint("[VMM] SmpLaunch failed: 0x%X\n", st);
        SmpShutdown(&g_Smp);
        return st;
    }
    
    DbgPrint("[VMM] Hypervisor launched successfully\n");
    
    // Create communication interface
    if (D)
    {
        // Normal driver load - use IOCTL interface
        DbgPrint("[VMM] Creating IOCTL interface (normal driver mode)...\n");
        st = IoctlInterfaceCreate(D);
        if (!NT_SUCCESS(st))
        {
            DbgPrint("[VMM] Failed to create IOCTL interface: 0x%X\n", st);
            DbgPrint("[VMM] Falling back to shared memory...\n");
            
            // Fallback to shared memory
            st = SharedMemoryCreate();
            if (!NT_SUCCESS(st))
            {
                DbgPrint("[VMM] Failed to create shared memory interface: 0x%X\n", st);
                SmpShutdown(&g_Smp);
                return st;
            }
        }
    }
    else
    {
        // kdmapper load - use shared memory interface
        DbgPrint("[VMM] Creating shared memory interface (kdmapper mode)...\n");
        st = SharedMemoryCreate();
        if (!NT_SUCCESS(st))
        {
            DbgPrint("[VMM] Failed to create shared memory interface: 0x%X\n", st);
            SmpShutdown(&g_Smp);
            return st;
        }
        DbgPrint("[VMM] Shared memory interface created successfully\n");
    }
    
    DbgPrint("[VMM] ========================================\n");
    DbgPrint("[VMM] Driver loaded successfully\n");
    DbgPrint("[VMM] ========================================\n");

    return STATUS_SUCCESS;
}
