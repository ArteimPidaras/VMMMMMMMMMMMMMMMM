# NPT-Based Direct Injection System

## Overview

This system implements a sophisticated kernel-level injection mechanism using AMD SVM (Secure Virtual Machine) and NPT (Nested Page Tables) for stealth game hooking without external DLLs.

## Architecture

### Components

1. **Process Watchdog (User-Mode)**
   - `um/process_watchdog.c`
   - Monitors for target process (stalcraft.exe)
   - Sends VMMCALL_ATTACH_PROCESS to VMM
   - Waits for CR3 capture confirmation

2. **Process Attachment (Kernel)**
   - `src/process/process_attach.c`
   - Captures target process CR3 (DirectoryTableBase)
   - Intercepts MOV CR3 or parses EPROCESS
   - Enables NPT shadowing for target process

3. **NPT View-Switching (Kernel)**
   - `src/memory/npt_view_switch.c`
   - Implements dual-view NPT entries
   - Execute View: Points to injected/patched page
   - Read View: Points to original/clean page
   - Handles #NPF (Nested Page Fault) for view switching

4. **Direct Injection (Kernel)**
   - `src/injection/direct_inject.c`
   - Finds opengl32.dll and wglSwapBuffers in target
   - Allocates shellcode in guest memory
   - Installs NPT-based inline hook
   - No external DLL required

5. **Anti-Debug Shield (Kernel)**
   - `src/stealth/anti_debug_shield.c`
   - TSC offsetting to hide VM-Exit latency
   - DR register spoofing (returns 0 for all reads)
   - CPUID spoofing to hide hypervisor presence
   - Module hiding verification

6. **VM-Exit Handler (Kernel)**
   - `src/core/vmexit_handler_npt.c`
   - Handles #NPF for view-switching
   - Handles CR3 writes for process tracking
   - Handles DR reads/writes for anti-debug
   - Handles CPUID for hypervisor hiding

## How It Works

### Phase 1: Process Attachment

```
User-Mode Watchdog
    |
    | 1. CreateToolhelp32Snapshot (wait for stalcraft.exe)
    |
    v
VMMCALL_ATTACH_PROCESS (PID, ImageBase)
    |
    v
VMM: ProcessAttachTarget()
    |
    | 2. Intercept next MOV CR3 or parse EPROCESS
    |
    v
VMM: ProcessInterceptCr3Load()
    |
    | 3. Capture CR3 and ASID
    |
    v
VMM: NptSetTargetProcess()
    |
    | 4. Enable NPT shadowing
    |
    v
[VMM] Target Process Locked: 0x%llX
```

### Phase 2: Direct Injection

```
VMM: InjectDirectly()
    |
    | 1. Find opengl32.dll base
    |
    v
VMM: FindOpenGL32Module()
    |
    | 2. Parse PE exports for wglSwapBuffers
    |
    v
VMM: FindExportAddress()
    |
    | 3. Allocate shellcode buffer in guest
    |
    v
VMM: AllocateShellcodeInGuest()
    |
    | 4. Build shellcode (hook logic)
    |
    v
VMM: BuildAbsoluteJmpHook()
    |
    | 5. Write shellcode to guest memory
    |
    v
VMM: WriteShellcodeToGuest()
    |
    | 6. Install NPT-based inline hook
    |
    v
VMM: NptInstallInlineHook()
    |
    | 7. Configure dual-view NPT
    |
    v
[VMM] Direct injection complete!
```

### Phase 3: NPT View-Switching

```
Guest executes wglSwapBuffers
    |
    | Instruction Fetch
    |
    v
#NPF (Nested Page Fault)
    |
    | EXITINFO1 & FETCH_BIT = 1
    |
    v
VMM: NptHandleViewSwitch()
    |
    | Switch to Execute View (Injected Page)
    |
    v
NPT Entry -> Injected HPA (RX)
    |
    | __invlpga(GVA, ASID)
    |
    v
Guest executes HOOKED code
    |
    | Shellcode runs (ESP/wallhack logic)
    |
    v
Guest reads memory at wglSwapBuffers
    |
    | Data Access
    |
    v
#NPF (Nested Page Fault)
    |
    | EXITINFO1 & FETCH_BIT = 0
    |
    v
VMM: NptHandleViewSwitch()
    |
    | Switch to Read View (Original Page)
    |
    v
NPT Entry -> Original HPA (RW, NX)
    |
    | __invlpga(GVA, ASID)
    |
    v
Guest reads ORIGINAL bytes (anti-cheat sees clean memory)
```

## Anti-Detection Features

### 1. TSC Offsetting
- Measures VM-Exit cycles
- Subtracts from guest TSC view
- Game sees no timing anomalies

### 2. DR Register Spoofing
- Intercepts MOV DR0-DR7 reads
- Returns 0 (no hardware breakpoints)
- Anti-cheat cannot detect VMM breakpoints

### 3. CPUID Spoofing
- Clears hypervisor bit (CPUID.1:ECX.31)
- Returns 0 for hypervisor vendor leaf (0x40000000)
- Game thinks it's running on bare metal

### 4. Module Hiding
- No DLL in PEB->Ldr->InLoadOrderModuleList
- Shellcode is "floating" in memory
- No module base to scan

### 5. NPT View-Switching
- Memory reads see original bytes
- Memory scans find no hooks
- Only instruction fetches execute hooked code

## Building

### Prerequisites
- Windows Driver Kit (WDK)
- Visual Studio 2022
- AMD CPU with SVM support

### Build Steps

```bash
# Open solution
start SeCodeIntegrityQueryInformation.sln

# Build driver (Release x64)
msbuild SeCodeIntegrityQueryInformation.vcxproj /p:Configuration=Release /p:Platform=x64

# Build user-mode tools
msbuild um/um.vcxproj /p:Configuration=Release /p:Platform=x64
```

### Output
- `x64/Release/SeCodeIntegrityQueryInformation.sys` - VMM driver
- `bin/x64/Release/um_demo.exe` - Original demo
- `bin/x64/Release/process_watchdog.exe` - Process monitor (to be built)

## Usage

### 1. Load Driver
```cmd
sc create vmm type= kernel binPath= C:\path\to\SeCodeIntegrityQueryInformation.sys
sc start vmm
```

### 2. Run Process Watchdog
```cmd
process_watchdog.exe
```

### 3. Launch Target Game
```cmd
stalcraft.exe
```

### 4. Verify Injection
```
[+] Process Watchdog Started
[+] Waiting for stalcraft.exe to launch...
[+] Target process found!
    PID: 1234
    Image Base: 0x00007FF700000000
[+] Sending VMMCALL_ATTACH_PROCESS to VMM...
[+] VMM acknowledged process attachment
[+] VMM will now capture CR3 on next context switch
[+] Waiting for VMM to lock target process...
[+] VMM captured CR3: 0x000000001A2B3000
[+] Target Process Locked Successfully
[+] NPT Shadowing Engine is now active
```

## Debugging

### Enable Debug Prints
```cmd
# Set debug filter
bcdedit /set debug on
bcdedit /dbgsettings serial debugport:1 baudrate:115200

# View kernel debug output
windbg -k com:port=COM1,baud=115200
```

### Key Debug Messages
```
[VMM] Target Process Locked: 0x%llX
[VMM] Direct injection complete!
[VMM] NPF: Switched to Execute View (Injected)
[VMM] NPF: Switched to Read View (Original)
[VMM] TSC Offset enabled: Offset=0x%llX
[VMM] DR%u read intercepted, returning 0
```

## Security Considerations

### Strengths
- No user-mode DLL (no PEB entries)
- No IAT hooks (no import table modifications)
- No inline hooks visible to memory scans
- No timing anomalies (TSC offsetting)
- No hardware breakpoint detection (DR spoofing)
- No hypervisor detection (CPUID spoofing)

### Weaknesses
- Requires kernel driver (needs vulnerable driver or test signing)
- AMD-specific (SVM/NPT)
- Detectable via VMCB inspection (if anti-cheat runs in VMM)
- Detectable via #NPF frequency analysis

## Future Enhancements

1. **Intel VT-x Support**
   - Implement EPT view-switching (Intel equivalent of NPT)
   - Handle EPT violations instead of NPF

2. **Advanced Shellcode**
   - Full ESP (wallhack) implementation
   - Aimbot with smoothing
   - Radar overlay

3. **Nested Virtualization**
   - Run VMM inside another VMM
   - Defeat anti-cheat VMM detection

4. **SLAT Shadowing**
   - Multiple shadow page tables
   - Per-thread view switching

## License

Educational purposes only. Do not use for cheating in online games.

## Credits

- AMD SVM Programming Guide
- Intel VT-x Specification
- Windows Internals (Russinovich et al.)
- HyperDbg Project
- SimpleSvm Project
