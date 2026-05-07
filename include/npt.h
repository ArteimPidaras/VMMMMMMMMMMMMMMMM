#pragma once
#include <ntifs.h>

#pragma warning(push)
#pragma warning(disable: 4201) // nonstandard extension used: nameless struct/union

#define PAGE_PRESENT     1ULL
#define PAGE_WRITE       (1ULL << 1)
#define PAGE_USER        (1ULL << 2)
#define PAGE_NX          (1ULL << 63)

// NPT Permission Constants (PART 1.7)
#define NPT_RWX          (PAGE_PRESENT | PAGE_WRITE)
#define NPT_RW_NO_EXEC   (PAGE_PRESENT | PAGE_WRITE | PAGE_NX)
#define NPT_EXEC_NO_RW   (PAGE_PRESENT)

// Pool Tag for Shadow Paging (PART 1.9)
#define SHADOW_POOL_TAG  'SHPT'

typedef union _NPT_ENTRY
{
    UINT64 Value;
    struct {
        UINT64 Present : 1;
        UINT64 Write : 1;
        UINT64 User : 1;
        UINT64 WriteThrough : 1;
        UINT64 CacheDisable : 1;
        UINT64 Accessed : 1;
        UINT64 Dirty : 1;
        UINT64 LargePage : 1;
        UINT64 Global : 1;
        UINT64 Reserved1 : 3;
        UINT64 PageFrame : 40;
        UINT64 Reserved2 : 11;
        UINT64 Nx : 1;
    };
} NPT_ENTRY;

// Shadow Page Tracking Entry (PART 1.4)
typedef struct _SHADOW_PAGE_ENTRY
{
    LIST_ENTRY ListEntry;
    UINT64 GuestPhysicalPage;
    UINT64 OriginalHostPage;
    UINT64 InjectedHostPage;
    UINT64 TargetCr3;
    UINT16 Asid;
    BOOLEAN IsActive;
    BOOLEAN IsExecuteView;
    UINT8 Reserved[6];
} SHADOW_PAGE_ENTRY;

// Split Page Table Entry (PART 1.5)
typedef struct _SPLIT_PT_ENTRY
{
    UINT64 OriginalPageFrame;
    UINT64 InjectedPageFrame;
    BOOLEAN IsSplit;
    UINT8 Reserved[7];
} SPLIT_PT_ENTRY;

typedef struct _NPT_STATE
{
    NPT_ENTRY* Pml4;
    PHYSICAL_ADDRESS Pml4Pa;

    // Shadow PML4 for Execute Views (PART 1.2)
    NPT_ENTRY* ShadowPml4;
    PHYSICAL_ADDRESS ShadowPml4Pa;

    UINT64 ShadowCr3;

    // Shadow Page Tracking List (PART 1.4)
    LIST_ENTRY ShadowPageList;
    KSPIN_LOCK ShadowPageLock;
    ULONG ShadowPageCount;

    // Target Process Tracking (PART 1.8)
    UINT64 TargetCr3;
    UINT16 TargetAsid;
    BOOLEAN Cr3MonitorActive;

    struct
    {
        UINT64 GpaPage;
        UINT64 OriginalPageFrame;
        BOOLEAN Armed;
        BOOLEAN UsingFakePage;
    } Apic, Acpi, Smm, Mmio;

  
    struct
    {
        UINT64 GpaPage;
        UINT64 LastMessage;
        BOOLEAN Active;
    } Mailbox;

  
    PVOID FakePageVa[2];
    PHYSICAL_ADDRESS FakePagePa[2];
    ULONG FakePageIndex;

    struct
    {
        UINT64 TargetGpaPage;
        UINT64 NewHpaPage;
        BOOLEAN Active;
    } ShadowHook;
} NPT_STATE;

NTSTATUS NptInitialize(NPT_STATE* State);
VOID NptDestroy(NPT_STATE* State);

PHYSICAL_ADDRESS NptTranslateGvaToHpa(NPT_STATE* State, UINT64 Gva);
PHYSICAL_ADDRESS NptTranslateGpaToHpa(NPT_STATE* State, UINT64 Gpa);
BOOLEAN NptHookPage(NPT_STATE* State, UINT64 GuestPhysical, UINT64 NewHostPhysical);
VOID NptUpdateShadowCr3(NPT_STATE* State, UINT64 GuestCr3);
BOOLEAN NptInstallShadowHook(NPT_STATE* State, UINT64 TargetGpa, UINT64 NewHpa);
VOID NptClearShadowHook(NPT_STATE* State);


BOOLEAN NptSetupHardwareTriggers(NPT_STATE* State, UINT64 apicGpa, UINT64 acpiGpa, UINT64 smmGpa, UINT64 mmioGpa);
BOOLEAN NptHandleHardwareTriggers(NPT_STATE* State, UINT64 faultGpa, UINT64* mailboxValue);
VOID NptRearmHardwareTriggers(NPT_STATE* State);

// Shadow Paging API (PART 1-4)
NTSTATUS NptShadowPageInstall(NPT_STATE* State, UINT64 Gpa, UINT64 OriginalHpa, UINT64 InjectedHpa, UINT64 TargetCr3, UINT16 Asid);
VOID NptShadowPageRemove(NPT_STATE* State, UINT64 Gpa);
BOOLEAN NptHandleNpfViolation(NPT_STATE* State, UINT64 FaultGpa, UINT64 ErrorCode, UINT64 GuestRip);
BOOLEAN NptSplitLargePage(NPT_STATE* State, UINT64 Gpa);
VOID NptSetTargetProcess(NPT_STATE* State, UINT64 Cr3, UINT16 Asid);
VOID NptInvalidateTlb(UINT64 Gva, UINT16 Asid);

#pragma warning(pop)
