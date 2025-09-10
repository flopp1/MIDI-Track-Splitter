#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <iomanip>
#include <cstdint>
#include <memory>

#ifdef _WIN32
    #include <windows.h>
    #include <commdlg.h>
    #include <shlobj.h>
#endif

namespace fs = std::filesystem;

class MIDISplitter {
private:
    struct TrackInfo {
        uint16_t number;
        std::string name;
        uint32_t size;
        std::streampos position;
    };

    // Convert big-endian bytes to uint32_t
    uint32_t bytesToUInt32(const std::vector<uint8_t>& bytes, size_t offset = 0) {
        if (offset + 4 > bytes.size()) return 0;
        return (static_cast<uint32_t>(bytes[offset]) << 24) |
               (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
               (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
               static_cast<uint32_t>(bytes[offset + 3]);
    }

    // Convert big-endian bytes to uint16_t
    uint16_t bytesToUInt16(const std::vector<uint8_t>& bytes, size_t offset = 0) {
        if (offset + 2 > bytes.size()) return 0;
        return (static_cast<uint16_t>(bytes[offset]) << 8) |
               static_cast<uint16_t>(bytes[offset + 1]);
    }

    // Convert uint32_t to big-endian bytes
    std::vector<uint8_t> uint32ToBytes(uint32_t value) {
        return {
            static_cast<uint8_t>((value >> 24) & 0xFF),
            static_cast<uint8_t>((value >> 16) & 0xFF),
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>(value & 0xFF)
        };
    }

    // Convert uint16_t to big-endian bytes
    std::vector<uint8_t> uint16ToBytes(uint16_t value) {
        return {
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>(value & 0xFF)
        };
    }

    // Simple search instead of KMP for reliability
    std::vector<size_t> simpleSearch(const std::vector<uint8_t>& text, const std::vector<uint8_t>& pattern) {
        std::vector<size_t> result;
        if (pattern.empty() || text.size() < pattern.size()) return result;

        for (size_t i = 0; i <= text.size() - pattern.size(); i++) {
            bool match = true;
            for (size_t j = 0; j < pattern.size(); j++) {
                if (text[i + j] != pattern[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                result.push_back(i);
                i += pattern.size() - 1; // Skip ahead
            }
        }
        return result;
    }

    // Extract track name from track data
    std::string extractTrackName(std::istream& stream, uint16_t trackNumber, uint32_t trackSize) {
        const size_t MAX_SEARCH_SIZE = 1024; // Reduced for safety
        size_t searchSize = std::min(static_cast<size_t>(trackSize), MAX_SEARCH_SIZE);
        
        std::vector<uint8_t> searchBuffer;
        try {
            searchBuffer.resize(searchSize);
        } catch (const std::bad_alloc& e) {
            return "Track " + std::to_string(trackNumber);
        }
        
        std::streampos currentPos = stream.tellg();
        if (currentPos == -1) {
            return "Track " + std::to_string(trackNumber);
        }

        stream.read(reinterpret_cast<char*>(searchBuffer.data()), searchSize);
        if (!stream) {
            stream.clear(); // Clear error state
            stream.seekg(currentPos);
            return "Track " + std::to_string(trackNumber);
        }
        
        stream.seekg(currentPos); // Rewind the stream
        stream.clear(); // Ensure stream is in good state

        std::vector<uint8_t> pattern = {0xFF, 0x03}; // Track name meta event
        auto matches = simpleSearch(searchBuffer, pattern);

        if (!matches.empty()) {
            for (auto matchPos : matches) {
                size_t nameIndex = matchPos + 2; // Skip the meta event bytes
                if (nameIndex + 1 < searchBuffer.size()) {
                    uint8_t nameLength = searchBuffer[nameIndex];
                    if (nameIndex + 1 + nameLength <= searchBuffer.size()) {
                        std::string trackName;
                        for (size_t i = 0; i < nameLength; i++) {
                            trackName += static_cast<char>(searchBuffer[nameIndex + 1 + i]);
                        }
                        if (!trackName.empty()) {
                            return trackName;
                        }
                    }
                }
            }
        }

        return "Track " + std::to_string(trackNumber);
    }

    // Get safe filename
    std::string getSafeFilename(const std::string& name) {
        std::string safe = name;
        std::string invalid = "<>:\"/\\|?*";
        for (char c : invalid) {
            std::replace(safe.begin(), safe.end(), c, '_');
        }
        return safe;
    }

    // Copy data from one stream to another in chunks
    void copyStream(std::istream& in, std::ostream& out, size_t size) {
        const size_t BUFFER_SIZE = 4096;
        std::vector<char> buffer(BUFFER_SIZE);
        
        while (size > 0) {
            size_t bytesToRead = std::min(size, BUFFER_SIZE);
            in.read(buffer.data(), bytesToRead);
            if (in.gcount() == 0) break; // No more data to read
            
            out.write(buffer.data(), in.gcount());
            if (!out) {
                throw std::runtime_error("Error writing to output stream.");
            }
            
            size -= in.gcount();
            if (in.eof()) break; // Reached end of file
        }
    }

#ifdef _WIN32
    // Windows file dialog
    std::string openFileDialog() {
        OPENFILENAMEA ofn;
        char szFile[260] = {0};

        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = sizeof(szFile);
        ofn.lpstrFilter = "MIDI Files\0*.mid;*.midi\0All Files\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.lpstrTitle = "Select MIDI File to Split";
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

        if (GetOpenFileNameA(&ofn)) {
            return std::string(szFile);
        }
        return "";
    }

    std::string selectFolderDialog() {
        BROWSEINFOA bi = {0};
        bi.lpszTitle = "Select Output Folder";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

        LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
        if (pidl != nullptr) {
            char path[MAX_PATH];
            if (SHGetPathFromIDListA(pidl, path)) {
                CoTaskMemFree(pidl);
                return std::string(path);
            }
            CoTaskMemFree(pidl);
        }
        return "";
    }
#endif

public:
    void splitMIDIFile(const std::string& inputFile, const std::string& outputDir) {
        std::cout << "Reading MIDI file: " << inputFile << std::endl;

        // Use RAII for file handling
        std::ifstream file(inputFile, std::ios::binary);
        if (!file) {
            throw std::runtime_error("Cannot open file: " + inputFile);
        }

        // Read and validate MIDI header
        std::vector<uint8_t> headerData(14);
        file.read(reinterpret_cast<char*>(headerData.data()), 14);
        if (!file || file.gcount() != 14) {
            throw std::runtime_error("Error reading MIDI header.");
        }

        std::string header(headerData.begin(), headerData.begin() + 4);
        if (header != "MThd") {
            throw std::runtime_error("Not a valid MIDI file (missing MThd header)");
        }

        uint32_t headerSize = bytesToUInt32(headerData, 4);
        if (headerSize != 6) {
            throw std::runtime_error("Invalid MIDI header size");
        }

        uint16_t format = bytesToUInt16(headerData, 8);
        if (format != 1) {
            throw std::runtime_error("Not a Format 1 MIDI file");
        }

        uint16_t totalTracks = bytesToUInt16(headerData, 10);
        std::vector<uint8_t> division(headerData.begin() + 12, headerData.end());

        std::cout << "Found " << totalTracks << " tracks to split" << std::endl;

        std::vector<TrackInfo> tracks;
        tracks.reserve(totalTracks); // Reserve space for ALL tracks including primary

        // Process ALL tracks including the primary track
        for (uint16_t i = 0; i < totalTracks; i++) {
            std::streampos trackStartPos = file.tellg();
            if (trackStartPos == -1) {
                throw std::runtime_error("Invalid file position at track " + std::to_string(i + 1));
            }

            std::vector<uint8_t> trackHeader(8);
            file.read(reinterpret_cast<char*>(trackHeader.data()), 8);
            if (!file || file.gcount() != 8) {
                throw std::runtime_error("Error reading track header " + std::to_string(i + 1));
            }

            std::string trackHeaderStr(trackHeader.begin(), trackHeader.begin() + 4);
            if (trackHeaderStr != "MTrk") {
                throw std::runtime_error("Invalid track header for track " + std::to_string(i + 1));
            }

            uint32_t trackSize = bytesToUInt32(trackHeader, 4);

            TrackInfo track;
            track.number = i + 1;
            track.size = trackSize;
            track.position = trackStartPos;
            
            // Extract track name with proper error handling
            try {
                track.name = extractTrackName(file, track.number, trackSize);
            } catch (...) {
                if (i == 0) {
                    track.name = "Tempo Track"; // Special name for primary track
                } else {
                    track.name = "Track " + std::to_string(track.number);
                }
            }
            
            tracks.push_back(track);
            
            if (i == 0) {
                std::cout << "Primary Track: " << track.name << " (" << track.size << " bytes)" << std::endl;
            } else {
                std::cout << "Track " << track.number << ": " << track.name << " (" << track.size << " bytes)" << std::endl;
            }

            // Seek to next track
            file.seekg(trackSize, std::ios::cur);
            if (!file) {
                throw std::runtime_error("Error seeking to next track " + std::to_string(i + 1));
            }
        }

        // Prepare output header for Format 1 (single track)
        std::vector<uint8_t> outputHeader;
        outputHeader.reserve(14);
        
        // MThd header
        outputHeader.insert(outputHeader.end(), {'M', 'T', 'h', 'd'});
        
        // Header size (6 bytes)
        auto headerSizeBytes = uint32ToBytes(6);
        outputHeader.insert(outputHeader.end(), headerSizeBytes.begin(), headerSizeBytes.end());
        
        // Format (1 = format 1 - single track)
        auto formatBytes = uint16ToBytes(1);
        outputHeader.insert(outputHeader.end(), formatBytes.begin(), formatBytes.end());
        
        // Number of tracks (1 - single track)
        auto trackCountBytes = uint16ToBytes(1);
        outputHeader.insert(outputHeader.end(), trackCountBytes.begin(), trackCountBytes.end());
        
        // Division (unchanged)
        outputHeader.insert(outputHeader.end(), division.begin(), division.end());

        fs::path inputPath(inputFile);
        std::string baseName = inputPath.stem().string();

        // Create output files - each containing only ONE track
        int splitCount = 0;
        for (const auto& track : tracks) {
            std::string trackType = (track.number == 1) ? "Tempo" : "Track";
            std::cout << "Splitting: " << trackType << " " << track.number << std::endl;
            
            std::string safeTrackName = getSafeFilename(track.name);
            std::string outputFile;
            
            outputFile = baseName + " - " + safeTrackName + ".mid";
            
            fs::path outputPath = fs::path(outputDir) / outputFile;
            
            int counter = 1;
            while (fs::exists(outputPath)) {
                outputFile = baseName + " - " + safeTrackName + " (Copy " + std::to_string(counter) + ").mid";
            }
            outputPath = fs::path(outputDir) / outputFile;
            counter++;

            std::ofstream outFile(outputPath, std::ios::binary);
            if (!outFile) {
                throw std::runtime_error("Cannot create output file: " + outputPath.string());
            }

            // Write header (Format 1, single track)
            outFile.write(reinterpret_cast<const char*>(outputHeader.data()), outputHeader.size());
            if (!outFile) {
                throw std::runtime_error("Error writing header to: " + outputPath.string());
            }

            // Write ONLY this track (no other tracks included)
            file.clear();
            file.seekg(track.position);
            if (!file) {
                throw std::runtime_error("Error seeking to track " + std::to_string(track.number));
            }
            
            // Write the track header and data (8 bytes header + track data)
            copyStream(file, outFile, 8 + track.size);

            outFile.close();
            splitCount++;
            
            std::cout << "  -> Created: " << outputFile << std::endl;
        }

        std::cout << "\nSuccessfully split " << splitCount << " tracks!" << std::endl;
    }

    void run() {
#ifdef _WIN32
        // Initialize COM for Windows dialogs
        CoInitialize(NULL);
#endif

        try {
            std::string inputFile, outputDir;

#ifdef _WIN32
            // Use Windows dialogs
            std::cout << "Select MIDI file to split..." << std::endl;
            inputFile = openFileDialog();
            if (inputFile.empty()) {
                std::cout << "No file selected. Exiting." << std::endl;
                return;
            }

            std::cout << "Select output folder..." << std::endl;
            outputDir = selectFolderDialog();
            if (outputDir.empty()) {
                std::cout << "No output folder selected. Exiting." << std::endl;
                return;
            }
#else
            // Command line input for non-Windows
            std::cout << "Enter MIDI file path: ";
            std::getline(std::cin, inputFile);
            
            std::cout << "Enter output directory: ";
            std::getline(std::cin, outputDir);
#endif

            // Validate input file
            if (!fs::exists(inputFile)) {
                throw std::runtime_error("Input file does not exist: " + inputFile);
            }

            // Validate/create output directory
            if (!fs::exists(outputDir)) {
                if (!fs::create_directories(outputDir)) {
                    throw std::runtime_error("Cannot create output directory: " + outputDir);
                }
            }

            splitMIDIFile(inputFile, outputDir);

        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }

#ifdef _WIN32
        CoUninitialize();
#endif

        std::cout << "\nPress Enter to exit...";
        std::cin.get();
    }
};

int main() {
    std::cout << "MIDI Splitter C++ v1.0" << std::endl;
    std::cout << "======================" << std::endl << std::endl;

    MIDISplitter splitter;
    splitter.run();

    return 0;
}