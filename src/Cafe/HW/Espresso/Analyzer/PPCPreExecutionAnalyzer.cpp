#include "Cafe/HW/Espresso/Analyzer/PPCPreExecutionAnalyzer.h"
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include <shared_mutex>

namespace PPCPreExecutionAnalyzer {
    static std::unordered_map<uint32_t, InstructionMetadata> s_db;
    static std::shared_mutex s_dbMutex;

    void ScanExecutableSection(uint32_t guestBase, const uint8_t* bytes, size_t size, const std::string& moduleName) {
        if (!bytes || size == 0) return;
        std::unique_lock lock(s_dbMutex);
        for (size_t offset = 0; offset + 4 <= size; offset += 4) {
            uint32_t raw = (uint32_t(bytes[offset]) << 24) | (uint32_t(bytes[offset+1]) << 16) | (uint32_t(bytes[offset+2]) << 8) | uint32_t(bytes[offset+3]);
            InstructionMetadata meta;
            meta.guestPc = guestBase + offset;
            meta.rawOpcode = raw;
            meta.primaryOpcode = (raw >> 26) & 0x3F;
            meta.secondaryOpcode = 0;
            s_db[meta.guestPc] = meta;
        }
    }
    void InvalidateRange(uint32_t guestStart, uint32_t guestEnd) {
        std::unique_lock lock(s_dbMutex);
        for (auto it = s_db.begin(); it != s_db.end(); ) {
            // Match PPCRecompiler_invalidateRange(): [guestStart, guestEnd).
            if (it->first >= guestStart && it->first < guestEnd) it = s_db.erase(it);
            else ++it;
        }
    }
    void ClearTitle() { std::unique_lock lock(s_dbMutex); s_db.clear(); }
    bool Lookup(uint32_t guestPc, InstructionMetadata& out) {
        std::shared_lock lock(s_dbMutex);
        auto it = s_db.find(guestPc);
        if (it != s_db.end()) { out = it->second; return true; }
        return false;
    }
}
