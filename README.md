# Enhanced Anti-Cheat Bypass System

A sophisticated AMD SVM-based hypervisor for DLL injection with advanced anti-cheat evasion targeting Stalcraft.exe.

## 🎯 Key Features

### Enhanced CR3 Tracking
- **Multi-CR3 Detection**: Tracks up to 16 different CR3 values per process
- **Anti-Cheat Pattern Recognition**: Detects rapid CR3 switching (< 100ms intervals)
- **Automatic Re-injection**: Re-injects on all known CR3 values when anti-cheat is detected
- **Historical Tracking**: Maintains CR3 change history with timestamps

### Persistent Process Monitoring
- **Continuous Operation**: Never terminates after injection
- **Real-time Status Display**: Shows CR3 changes, anti-cheat status, and hook status
- **Automatic Recovery**: Re-establishes hooks when they become inactive
- **Process Restart Detection**: Automatically re-attaches when target process restarts

### Pre-Flight Checks
- **Driver Verification**: Checks if driver service is loaded and running
- **Hypervisor Connection Test**: Verifies hypervisor is responding before injection
- **Graceful Error Handling**: Provides detailed error messages and recovery suggestions

### Advanced Stealth
- **NPT View-Switching**: Dual-view pages (execute/read) for undetectable hooks
- **TSC Offsetting**: Hides VM-Exit overhead from timing checks
- **Debug Register Spoofing**: Masks hardware breakpoints
- **CPUID Masking**: Hides hypervisor presence
- **MSR Filtering**: Conceals SVM activation

## 📋 Requirements

- **CPU**: AMD processor with SVM (AMD-V) support
- **OS**: Windows 10/11 x64
- **Tools**: Visual Studio 2019/2022 with WDK (Windows Driver Kit)
- **Privileges**: Administrator rights
- **BIOS**: Virtualization (AMD-V/SVM) enabled

## 🔧 Build Instructions

### Option 1: Batch Script (Recommended)
```batch
build.bat
```

### Option 2: PowerShell Script
```powershell
.\build.ps1
```

### Option 3: Manual Build
```batch
msbuild SeCodeIntegrityQueryInformation.sln /p:Configuration=Release /p:Platform=x64
msbuild um\process_watchdog.vcxproj /p:Configuration=Release /p:Platform=x64
cd DLL
msbuild oglwh.sln /p:Configuration=Release /p:Platform=x64
cd ..
```

## 🚀 Installation & Usage

### Step 1: Prepare System

**Disable Hyper-V (if enabled):**
```batch
bcdedit /set hypervisorlaunchtype off
```
Then reboot.

**Enable AMD-V in BIOS:**
- Enter BIOS/UEFI settings
- Find "AMD-V", "SVM Mode", or "Virtualization Technology"
- Enable it
- Save and reboot

### Step 2: Load Driver with kdmapper

**Download kdmapper:**
- Get kdmapper from: https://github.com/TheCruZ/kdmapper

**Load the driver:**
```batch
kdmapper.exe x64\Release\SeCodeIntegrityQueryInformation.sys
```

**Expected output:**
```
[+] Loading vulnerable driver...
[+] Allocating memory...
[+] Mapping driver...
[+] Driver entry point called
[+] Success
```

**Note:** kdmapper loads drivers without creating a Windows service, making it stealthier than traditional driver loading methods.

### Step 3: Run Process Watchdog

```batch
bin\x64\Release\process_watchdog.exe
```

The watchdog will:
1. ✅ Test hypervisor connection (verifies driver is loaded)
2. ⏳ Wait for stalcraft.exe to launch
3. 🎯 Attach to process and capture CR3
4. 💉 Inject DLL with OpenGL hooks
5. 📊 Monitor continuously for CR3 changes and anti-cheat activity

### Step 4: Launch the Game

Start `stalcraft.exe` - the watchdog will automatically detect and inject.

## 📊 Monitoring Output

### Normal Operation
```
[INFO] Cycle: 5 | Process: Active | CR3: 0x1234567890ABCDEF | Session Changes: 0 | Total Changes: 0 | Anti-Cheat: Not Detected | Hooks: Active
```

### CR3 Change Detected
```
[!] CR3 CHANGE DETECTED! Old: 0x1234567890ABCDEF -> New: 0x9876543210FEDCBA (Change #1)
[+] Re-attaching to new CR3...
[+] Successfully re-attached to new CR3
[+] Re-injecting DLL...
[+] DLL re-injection successful!
```

### Anti-Cheat Detected
```
[!] CR3 CHANGE DETECTED! Old: 0x1234567890ABCDEF -> New: 0x9876543210FEDCBA (Change #5)
[!] ANTI-CHEAT DETECTED! Rapid CR3 switching pattern identified
[+] Attempting multi-CR3 re-injection...
[+] Multi-CR3 re-injection successful!
```

## 🐛 Troubleshooting

### Error: "Hypervisor NOT responding"
**Possible Causes:**
1. Driver not loaded with kdmapper
2. CPU doesn't support AMD-V/SVM
3. Virtualization disabled in BIOS
4. Another hypervisor is running (Hyper-V, VMware, VirtualBox)
5. Driver initialization failed

**Solution:**
```batch
# Load driver with kdmapper
kdmapper.exe x64\Release\SeCodeIntegrityQueryInformation.sys

# Disable Hyper-V
bcdedit /set hypervisorlaunchtype off
# Then reboot

# Enable AMD-V in BIOS
# Check BIOS settings for "AMD-V" or "SVM Mode"

# Check driver debug output
# Use DebugView to see kernel messages
```

### Error: "VMMCALL instruction not supported"
**Cause:** Driver not loaded or hypervisor not initialized

**Solution:**
1. Verify kdmapper successfully loaded the driver
2. Check DebugView for driver initialization messages
3. Look for "[VMM] Hypervisor initialized" message
4. Ensure no errors during SVM initialization

### Error: "Failed to capture CR3"
**Possible Causes:**
1. Process exited too quickly
2. CR3 interception not working
3. Anti-cheat blocking hypervisor

**Solution:**
- Check kernel debug output with DebugView
- Look for "[VMM] Target Process Locked" message
- Verify process is still running
- Check for anti-cheat interference

### Nothing Appears In-Game
**Possible Causes:**
1. Anti-cheat using multiple CR3 values
2. Hooks being removed by anti-cheat
3. DLL injection failed

**Solution:**
- Monitor for CR3 changes in watchdog output
- Check for "Anti-Cheat: DETECTED" status
- Verify multi-CR3 re-injection is working
- Review kernel debug output

## 📁 Project Structure

```
svm-vmm-main/
├── src/                          # Kernel driver source
│   ├── core/                     # SVM hypervisor core
│   ├── hooks/                    # Hypercall handlers
│   ├── memory/                   # NPT and memory management
│   ├── process/                  # Process attachment and CR3 tracking
│   ├── stealth/                  # Anti-detection features
│   └── injection/                # DLL injection logic
├── um/                           # User-mode components
│   ├── process_watchdog.c        # Enhanced monitoring tool
│   ├── hypercall.h               # Hypercall interface
│   └── hypercall.asm             # VMMCALL wrapper
├── DLL/                          # Injection payload
│   └── oglwh/                    # OpenGL wallhack DLL
├── include/                      # Header files
├── asm/                          # Assembly routines
├── build.bat                     # Build script
├── install_driver.bat            # Driver installation script
├── DEBUG_GUIDE.md                # Detailed debugging guide
└── README.md                     # This file
```

## 🔬 Technical Details

### Hypervisor Architecture
- **Type**: AMD SVM (Secure Virtual Machine)
- **NPT**: Nested Page Tables for memory virtualization
- **ASID**: Address Space ID for TLB isolation
- **Intercepts**: CR3, CPUID, MSR, NPF (Nested Page Fault)

### Injection Method
- **Technique**: NPT view-switching with dual-page setup
- **Target**: wglSwapBuffers in opengl32.dll
- **Payload**: Custom OpenGL hooks for ESP/wallhack
- **Stealth**: Execute-only pages invisible to memory scans

### Anti-Cheat Evasion
- **CR3 Tracking**: Detects and adapts to page table switching
- **Multi-CR3 Injection**: Injects on all known page tables
- **Timing Stealth**: TSC offsetting hides hypervisor overhead
- **Register Masking**: Spoofs debug registers and CPUID
- **Module Hiding**: DLL not visible in PEB module list

## 📝 Hypercall Interface

### Process Management
- `0x500`: Attach to process (PID, ImageBase)
- `0x322`: Query process CR3 by PID
- `0x505`: Query CR3 change count
- `0x506`: Query anti-cheat detection status
- `0x507`: Re-inject on all known CR3 values

### Injection
- `0x501`: Install NPT hook
- `0x502`: Remove NPT hook
- `0x503`: Inject DLL
- `0x504`: Remove DLL

### Stealth
- `0x200`: Enable stealth mode
- `0x201`: Disable stealth mode
- `0x407`: Enable/disable debug register masking
- `0x408`: Enable/disable TSC intercept

## ⚠️ Legal Disclaimer

This software is provided for **educational and research purposes only**. 

- Using this software to cheat in online games violates Terms of Service
- May result in permanent bans from game services
- Could be illegal in some jurisdictions
- Author assumes no responsibility for misuse

**Use at your own risk.**

## 🤝 Contributing

Contributions are welcome! Please:
1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Submit a pull request

## 📄 License

This project is provided as-is without warranty. See LICENSE file for details.

## 🔗 Resources

- [AMD SVM Programming Guide](https://www.amd.com/system/files/TechDocs/24593.pdf)
- [Windows Driver Kit Documentation](https://docs.microsoft.com/en-us/windows-hardware/drivers/)
- [Nested Page Tables Explained](https://en.wikipedia.org/wiki/Second_Level_Address_Translation)
- [DEBUG_GUIDE.md](DEBUG_GUIDE.md) - Detailed troubleshooting guide

## 📧 Support

For issues and questions:
1. Check [DEBUG_GUIDE.md](DEBUG_GUIDE.md)
2. Review kernel debug output with DebugView
3. Open an issue on GitHub with:
   - Error messages
   - Debug output
   - System specifications
   - Steps to reproduce

---

**Version**: 2.0 Enhanced  
**Last Updated**: May 8, 2026  
**Status**: Active Development