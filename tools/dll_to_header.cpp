// DLL to Header Converter
// Converts a DLL file to a C header file at build time

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <iomanip>
#include <filesystem>

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <input.dll> <output.h>" << std::endl;
        return 1;
    }

    std::string inputFile = argv[1];
    std::string outputFile = argv[2];

    // Check if input file exists
    if (!std::filesystem::exists(inputFile)) {
        std::cerr << "Error: Input file does not exist: " << inputFile << std::endl;
        return 1;
    }

    // Read DLL file
    std::ifstream file(inputFile, std::ios::binary);
    if (!file) {
        std::cerr << "Error: Cannot open input file: " << inputFile << std::endl;
        return 1;
    }

    // Get file size
    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    // Read all bytes
    std::vector<uint8_t> dllData(fileSize);
    file.read(reinterpret_cast<char*>(dllData.data()), fileSize);
    file.close();

    if (fileSize == 0) {
        std::cerr << "Error: Input file is empty" << std::endl;
        return 1;
    }

    // Calculate simple CRC32
    uint32_t crc32 = 0;
    for (uint8_t byte : dllData) {
        crc32 ^= byte;
        for (int i = 0; i < 8; i++) {
            if (crc32 & 1) {
                crc32 = (crc32 >> 1) ^ 0xEDB88320;
            } else {
                crc32 >>= 1;
            }
        }
    }

    // Generate header file
    std::ofstream header(outputFile);
    if (!header) {
        std::cerr << "Error: Cannot create output file: " << outputFile << std::endl;
        return 1;
    }

    // Get filename without path
    std::string filename = std::filesystem::path(inputFile).filename().string();

    header << "// Auto-generated DLL embedding header\n";
    header << "// Generated from: " << filename << "\n";
    header << "// Size: " << fileSize << " bytes\n";
    header << "// CRC32: 0x" << std::hex << std::uppercase << crc32 << "\n\n";

    header << "#pragma once\n";
    header << "#include <ntifs.h>\n\n";

    header << "// Embedded DLL data\n";
    header << "extern \"C\" const UINT8 g_EmbeddedDll[] = {\n";

    // Write bytes (16 per line)
    for (size_t i = 0; i < dllData.size(); i += 16) {
        header << "    ";
        for (size_t j = i; j < i + 16 && j < dllData.size(); j++) {
            header << "0x" << std::hex << std::setw(2) << std::setfill('0') 
                   << static_cast<int>(dllData[j]);
            if (j < dllData.size() - 1) header << ",";
            if (j < i + 15 && j < dllData.size() - 1) header << " ";
        }
        header << "\n";
    }

    header << "};\n\n";

    header << "// DLL metadata\n";
    header << "extern \"C\" const SIZE_T g_EmbeddedDllSize = " << std::dec << fileSize << ";\n";
    header << "extern \"C\" const CHAR g_EmbeddedDllName[] = \"" << filename << "\";\n";
    header << "extern \"C\" const UINT32 g_EmbeddedDllCrc32 = 0x" << std::hex << std::uppercase << crc32 << ";\n";

    header.close();

    std::cout << "Successfully converted " << filename << " (" << fileSize << " bytes) to " << outputFile << std::endl;
    std::cout << "CRC32: 0x" << std::hex << std::uppercase << crc32 << std::endl;

    return 0;
}