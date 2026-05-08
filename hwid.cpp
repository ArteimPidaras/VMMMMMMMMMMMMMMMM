#include "pch.h"
#include "hwid.h"
#include <windows.h>
#include <wbemidl.h>
#include <wmistr.h>
#include <sstream>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace HWID {
    std::string GetHWID() {
        // Simple HWID generation based on computer name and processor
        char computerName[MAX_COMPUTERNAME_LENGTH + 1];
        DWORD size = sizeof(computerName);
        GetComputerNameA(computerName, &size);
        
        return std::string(computerName);
    }

    bool CheckLicense() {
        // Always return true to allow DLL to load
        // In production, this would check against a license server
        return true;
    }
}
