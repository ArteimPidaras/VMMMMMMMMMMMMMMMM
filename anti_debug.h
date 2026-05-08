#pragma once
#include <ntifs.h>
#include "vcpu.h"

// Anti-Debug & Anti-Cheat Shield

// TSC Offsetting
VOID StealthEnableTscOffset(VCPU* Vcpu);
VOID StealthAdjustTscOnVmExit(VCPU* Vcpu);
VOID StealthResetTscOffset(VCPU* Vcpu);

// DR Register Spoofing
VOID StealthEnableDrSpoof(VCPU* Vcpu);
BOOLEAN StealthHandleDrRead(VCPU* Vcpu, UINT32 DrNumber, UINT64* OutValue);
BOOLEAN StealthHandleDrWrite(VCPU* Vcpu, UINT32 DrNumber, UINT64 Value);
VOID StealthDisableDrSpoof(VCPU* Vcpu);

// Module Hiding
BOOLEAN StealthVerifyModuleHidden(UINT64 DllBase, UINT64 Cr3);

// CPUID Spoofing
BOOLEAN StealthHandleCpuid(VCPU* Vcpu);

// Full Shield
VOID StealthActivateFullShield(VCPU* Vcpu);
VOID StealthDeactivateFullShield(VCPU* Vcpu);
