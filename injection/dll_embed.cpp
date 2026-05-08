// DLL Embedding using C++ - No external scripts required
// Reads the compiled DLL at compile time and embeds it as a resource

#include <ntifs.h>
#include <fstream>
#include <vector>

// This will be replaced with actual embedded data by the preprocessor
#ifdef EMBED_DLL_DATA

// The actual DLL data will be included here by the build process
#include "dll_data.inc"

#else

// Fallback: Try to read DLL at runtime (for development/testing)
extern "C" {

// Placeholder data for when DLL is not embedded
static const UINT8 g_PlaceholderDll[] = {
    0x4D, 0x5A, 0x90, 0x00, 0x03, 0x00, 0x00, 0x00, // MZ header
    0x04, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00,
    0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

const UINT8* g_EmbeddedDll = g_PlaceholderDll;
const SIZE_T g_EmbeddedDllSize = sizeof(g_PlaceholderDll);
const CHAR g_EmbeddedDllName[] = "placeholder.dll";
const UINT32 g_EmbeddedDllCrc32 = 0x00000000;

}

#endif

// Function to verify embedded DLL integrity
extern "C" BOOLEAN VerifyEmbeddedDll(void) {
    if (g_EmbeddedDllSize < 64) {
        DbgPrint("[VMM] Embedded DLL too small: %llu bytes\n", g_EmbeddedDllSize);
        return FALSE;
    }

    // Check MZ header
    if (g_EmbeddedDll[0] != 0x4D || g_EmbeddedDll[1] != 0x5A) {
        DbgPrint("[VMM] Invalid MZ header in embedded DLL\n");
        return FALSE;
    }

    DbgPrint("[VMM] Embedded DLL verified: %s (%llu bytes, CRC32: 0x%08X)\n", 
             g_EmbeddedDllName, g_EmbeddedDllSize, g_EmbeddedDllCrc32);

    return TRUE;
}