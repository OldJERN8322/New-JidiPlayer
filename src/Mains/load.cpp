// load.cpp

#include "visualizer.hpp"
#include "midi_timing_alt.hpp"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <cassert>
#include <vector>
#include <unordered_map>
#include <cctype>
#include <filesystem>

#ifdef _WIN32
// Declare Win32 memory-mapping APIs without pulling in <windows.h> to avoid raylib macro collisions
extern "C" {
    __declspec(dllimport) void* __stdcall CreateFileA(
        const char* lpFileName, unsigned long dwDesiredAccess, unsigned long dwShareMode,
        void* lpSecurityAttributes, unsigned long dwCreationDisposition,
        unsigned long dwFlagsAndAttributes, void* hTemplateFile);
    __declspec(dllimport) void* __stdcall CreateFileMappingA(
        void* hFile, void* lpFileMappingAttributes, unsigned long flProtect,
        unsigned long dwMaximumSizeHigh, unsigned long dwMaximumSizeLow, const char* lpName);
    __declspec(dllimport) void* __stdcall MapViewOfFile(
        void* hFileMappingObject, unsigned long dwDesiredAccess,
        unsigned long dwFileOffsetHigh, unsigned long dwFileOffsetLow, size_t dwNumberOfBytesToMap);
    __declspec(dllimport) int   __stdcall UnmapViewOfFile(const void* lpBaseAddress);
    __declspec(dllimport) int   __stdcall CloseHandle(void* hObject);
    __declspec(dllimport) int   __stdcall GetFileSizeEx(void* hFile, int64_t* lpFileSize);
}
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

struct MidiReader {
    const uint8_t* data = nullptr;
    std::vector<uint8_t> heapBuf;
    size_t pos       = 0;
    size_t totalSize = 0;
    std::atomic<size_t>* progressBytes = nullptr;

#ifdef _WIN32
    void* hFile = (void*)(intptr_t)-1;
    void* hMap  = nullptr;
#else
    int fd = -1;
#endif
    bool isMapped = false;

    MidiReader(const MidiReader&) = delete;
    MidiReader& operator=(const MidiReader&) = delete;

    explicit MidiReader(const std::string& path, std::atomic<size_t>* pBytes = nullptr)
        : progressBytes(pBytes)
    {
#ifdef _WIN32
        hFile = CreateFileA(path.c_str(), 0x80000000UL /*GENERIC_READ*/, 1UL /*FILE_SHARE_READ*/,
                            nullptr, 3UL /*OPEN_EXISTING*/, 0x00000080UL /*FILE_ATTRIBUTE_NORMAL*/, nullptr);
        if (hFile != (void*)(intptr_t)-1 && hFile != nullptr) {
            int64_t fsize = 0;
            if (GetFileSizeEx(hFile, &fsize) && fsize > 0) {
                totalSize = static_cast<size_t>(fsize);
                hMap = CreateFileMappingA(hFile, nullptr, 0x02UL /*PAGE_READONLY*/, 0, 0, nullptr);
                if (hMap) {
                    data = static_cast<const uint8_t*>(MapViewOfFile(hMap, 0x0004UL /*FILE_MAP_READ*/, 0, 0, 0));
                    if (data) {
                        isMapped = true;
                        return;
                    }
                }
            }
        }
#else
        fd = open(path.c_str(), O_RDONLY);
        if (fd != -1) {
            struct stat st;
            if (fstat(fd, &st) == 0 && st.st_size > 0) {
                totalSize = static_cast<size_t>(st.st_size);
                void* map = mmap(nullptr, totalSize, PROT_READ, MAP_PRIVATE, fd, 0);
                if (map != MAP_FAILED) {
                    data = static_cast<const uint8_t*>(map);
                    isMapped = true;
                    return;
                }
            }
        }
#endif

        // Fallback: 64-bit standard file reading if mapping is unavailable
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return;

#if defined(_WIN32)
        _fseeki64(f, 0, SEEK_END);
        int64_t sz = _ftelli64(f);
        _fseeki64(f, 0, SEEK_SET);
#else
        fseeko(f, 0, SEEK_END);
        off_t sz = ftello(f);
        fseeko(f, 0, SEEK_SET);
#endif
        if (sz <= 0) {
            std::error_code ec;
            auto fs = std::filesystem::file_size(path, ec);
            if (!ec) sz = static_cast<int64_t>(fs);
        }

        if (sz > 0) {
            totalSize = static_cast<size_t>(sz);
            try {
                heapBuf.resize(totalSize);
                size_t totalRead = 0;
                constexpr size_t CHUNK = 64 * 1024 * 1024;
                while (totalRead < totalSize) {
                    size_t toRead = std::min(CHUNK, totalSize - totalRead);
                    size_t n = fread(heapBuf.data() + totalRead, 1, toRead, f);
                    if (n == 0) break;
                    totalRead += n;
                }
                totalSize = totalRead;
                data = heapBuf.data();
            } catch (...) {
                totalSize = 0;
                data = nullptr;
            }
        }
        fclose(f);
    }

    ~MidiReader() {
#ifdef _WIN32
        if (isMapped && data) {
            UnmapViewOfFile(data);
            data = nullptr;
        }
        if (hMap) {
            CloseHandle(hMap);
            hMap = nullptr;
        }
        if (hFile && hFile != (void*)(intptr_t)-1) {
            CloseHandle(hFile);
            hFile = (void*)(intptr_t)-1;
        }
#else
        if (isMapped && data) {
            munmap((void*)data, totalSize);
            data = nullptr;
        }
        if (fd != -1) {
            close(fd);
            fd = -1;
        }
#endif
    }

    bool eof() const { return pos >= totalSize; }

    bool readBytes(void* dst, size_t n) {
        if (!data || pos + n > totalSize) return false;
        std::memcpy(dst, data + pos, n);
        pos += n;
        if (progressBytes && (pos % 4096 == 0))
            progressBytes->store(pos, std::memory_order_relaxed);
        return true;
    }

    uint8_t readU8() {
        if (!data || pos >= totalSize) return 0;
        uint8_t v = data[pos++];
        if (progressBytes && (pos % 4096 == 0))
            progressBytes->store(pos, std::memory_order_relaxed);
        return v;
    }

    uint16_t readU16() {
        if (!data || pos + 2 > totalSize) return 0;
        uint16_t v = (static_cast<uint16_t>(data[pos]) << 8) | data[pos + 1];
        pos += 2;
        return v;
    }

    uint32_t readU32() {
        if (!data || pos + 4 > totalSize) return 0;
        uint32_t v = (static_cast<uint32_t>(data[pos    ]) << 24)
                   | (static_cast<uint32_t>(data[pos + 1]) << 16)
                   | (static_cast<uint32_t>(data[pos + 2]) <<  8)
                   |  static_cast<uint32_t>(data[pos + 3]);
        pos += 4;
        return v;
    }

    uint32_t readU24() {
        if (!data || pos + 3 > totalSize) return 0;
        uint32_t v = (static_cast<uint32_t>(data[pos    ]) << 16)
                   | (static_cast<uint32_t>(data[pos + 1]) <<  8)
                   |  static_cast<uint32_t>(data[pos + 2]);
        pos += 3;
        return v;
    }

    uint32_t readVLQ() {
        uint32_t val = 0;
        for (int i = 0; i < 4 && data && pos < totalSize; ++i) {
            uint8_t b = data[pos++];
            val = (val << 7) | (b & 0x7F);
            if (!(b & 0x80)) break;
        }
        return val;
    }

    void skip(uint64_t n) {
        pos += n;
        if (pos > totalSize) pos = totalSize;
    }
};

struct PendingNote {
    uint32_t startTick;
    uint8_t  velocity;
    uint8_t  visualTrack; 
};

// High-performance ring pool replacing std::deque to eliminate 17M heap allocations
struct PendingNotePool {
    std::vector<PendingNote> items;
    size_t head = 0;

    bool empty() const { return head >= items.size(); }
    const PendingNote& front() const { return items[head]; }
    
    void push_back(const PendingNote& pn) {
        if (head == items.size()) {
            items.clear();
            head = 0;
        }
        items.push_back(pn);
    }
    
    void pop_front() {
        head++;
        if (head == items.size()) {
            items.clear();
            head = 0;
        }
    }
    
    void clear() {
        items.clear();
        head = 0;
    }
};

}

static bool ParseTitleSetCommand(const char* text, size_t len, std::string& outTitle) {
    if (len < 8) return false;
    std::string s(text, len);
    std::string lower = s;
    for (char& c : lower) c = (char)std::tolower((unsigned char)c);

    size_t pos = lower.find("!titleset");
    if (pos == std::string::npos) pos = lower.find("!title");
    if (pos != std::string::npos) {
        size_t openP = s.find('(', pos);
        if (openP != std::string::npos) {
            size_t closeP = s.rfind(')');
            if (closeP != std::string::npos && closeP > openP) {
                outTitle = s.substr(openP + 1, closeP - openP - 1);
            } else {
                outTitle = s.substr(openP + 1);
            }
            while (!outTitle.empty() && (outTitle.front() == ' ' || outTitle.front() == '\t'))
                outTitle.erase(outTitle.begin());
            while (!outTitle.empty() && (outTitle.back() == ' ' || outTitle.back() == '\t' || outTitle.back() == '\r' || outTitle.back() == '\n'))
                outTitle.pop_back();

            std::cout << "[TextEvent] Found !TitleSet: \"" << outTitle << "\"" << std::endl;
            return true;
        }
    }
    return false;
}

static bool ParseCompressCommand(const char* text, size_t len, int currentTrack, uint64_t& outMultiplier, int& outTargetTrack) {
    if (len < 9) return false;

    std::string s(text, len);
    std::string lower = s;
    for (char& c : lower) c = (char)std::tolower((unsigned char)c);

    size_t posComp = lower.find("!compress");
    size_t posUncomp = lower.find("!uncompress");

    if (posUncomp != std::string::npos) {
        outMultiplier = 1;
        outTargetTrack = -1;
        size_t openP = lower.find('(', posUncomp);
        if (openP != std::string::npos) {
            size_t closeP = lower.find(')', openP);
            std::string inside = lower.substr(openP + 1, (closeP != std::string::npos) ? (closeP - openP - 1) : std::string::npos);
            int trk = 0;
            if (sscanf(inside.c_str(), "%d", &trk) >= 1) {
                outTargetTrack = trk;
            }
        }
        std::cout << "[TextEvent] Detected !UncompressNote -> Target Track: " << outTargetTrack << std::endl;
        return true;
    }

    if (posComp != std::string::npos) {
        size_t openP = lower.find('(', posComp);
        if (openP != std::string::npos) {
            size_t closeP = lower.find(')', openP);
            std::string inside = lower.substr(openP + 1, (closeP != std::string::npos) ? (closeP - openP - 1) : std::string::npos);

            for (char& c : inside) {
                if (c == ',' || c == ':' || c == ';') c = ' ';
            }

            unsigned long long mult = 1;
            int trk = -1;
            int parsed = sscanf(inside.c_str(), "%llu %d", &mult, &trk);
            if (parsed >= 1) {
                outMultiplier = mult > 0 ? (uint64_t)mult : 1ULL;
                outTargetTrack = trk;
                std::cout << "[TextEvent] Found !CompressNote: Multiplier = " << outMultiplier 
                          << ", Target Track = " << outTargetTrack 
                          << " (Current MIDI Track = " << currentTrack << ")" << std::endl;
                return true;
            }
        }
    }
    return false;
}

struct CompressNote {
    uint32_t tick;
    int      targetTrack;
    uint64_t multiplier;
};
static std::vector<CompressNote> s_CompressNote;

static std::vector<MidiEvent> s_globalEvents;
static std::vector<std::vector<uint8_t>> s_sysexPool;

const std::vector<uint8_t>& GetSysExData(uint32_t index) {
    static const std::vector<uint8_t> empty;
    if (index < s_sysexPool.size()) return s_sysexPool[index];
    return empty;
}

std::vector<TempoEvent> collectGlobalTempoEvents(const std::string& filename) {
    std::vector<TempoEvent> tempos;
    MidiReader r(filename);

    uint32_t hdrId  = r.readU32();  
    uint32_t hdrLen = r.readU32();  
    uint16_t format = r.readU16();
    uint16_t nTracks= r.readU16();
    uint16_t ppq    = r.readU16();
    (void)format; (void)ppq;
    if (hdrLen > 6) r.skip(hdrLen - 6);

    for (uint16_t t = 0; t < nTracks && !r.eof(); ++t) {
        uint32_t chunkId  = r.readU32(); 
        uint32_t chunkLen = r.readU32();
        if (chunkId != 0x4D54726B) { r.skip(chunkLen); continue; }

        uint32_t absTick   = 0;
        uint8_t  runStatus = 0;
        size_t   bytesLeft = chunkLen;
        auto consume = [&](size_t n) { if (n <= bytesLeft) bytesLeft -= n; };

        while (bytesLeft > 0 && !r.eof()) {
            uint32_t delta = r.readVLQ(); consume(0); 
            absTick += delta;

            uint8_t statusByte = r.readU8(); consume(1);

            uint8_t firstData = 0xFF;
            if (statusByte & 0x80) {
                if (statusByte < 0xF0) {
                    runStatus = statusByte;
                } else {
                    runStatus = 0;
                }
            } else {
                firstData = statusByte;
                statusByte = runStatus;
            }

            uint8_t status = statusByte;

            if (status == 0xFF) {
                runStatus = 0;
                uint8_t  metaType = r.readU8(); consume(1);
                uint32_t metaLen  = r.readVLQ(); consume(0);
                if (metaType == 0x51 && metaLen == 3) {
                    uint32_t tempo = r.readU24(); consume(3);
                    tempos.push_back({ absTick, tempo });
                } else {
                    r.skip(metaLen); consume(metaLen);
                }
            } else if (status == 0xF0 || status == 0xF7) {
                runStatus = 0;
                uint32_t sysLen = r.readVLQ(); consume(0);
                r.skip(sysLen); consume(sysLen);
            } else {
                uint8_t type = status & 0xF0;
                if (type == 0xC0 || type == 0xD0) {
                    if (firstData == 0xFF) { r.readU8(); consume(1); }
                } else {
                    if (firstData == 0xFF) { r.readU8(); consume(1); }
                    r.readU8(); consume(1);
                }
            }
        }
        if (bytesLeft > 0) r.skip((uint32_t)bytesLeft);
    }

    std::sort(tempos.begin(), tempos.end(),
        [](const TempoEvent& a, const TempoEvent& b){ return a.tick < b.tick; });
    return tempos;
}

std::vector<CCEvent> loadStreamingMidiData(
    const std::string& filename, std::vector<OptimizedTrackData>& tracks,
    int& ppq, int& initialTempo, uint64_t& totalNoteCount,
    uint16_t& outTimeSigNumerator, uint16_t& outTimeSigDenominator,
    LoadProgress* progress, bool removeOverlaps, bool expandCompressedNotes) {
    if (progress) progress->loadPhase = 1;
    MidiReader r(filename, progress ? &progress->bytesRead : nullptr);
    if (progress) progress->totalBytes = r.totalSize;
    tracks.clear();
    totalNoteCount = 0;
    outTimeSigNumerator   = 4;
    outTimeSigDenominator = 4;
    std::vector<CCEvent> ccEvents;

    s_globalEvents.clear();
    s_sysexPool.clear();
    s_CompressNote.clear();
    g_midiTitle.clear();
    bool titleSetExplicitly = false;

    if (r.totalSize > 0) {
        size_t estimatedEvents = r.totalSize / 4;
        try {
            s_globalEvents.reserve(estimatedEvents);
        } catch (...) {}
        try {
            // Cap to 1M to prevent reserving 550MB on Black MIDIs with 0 CC events
            ccEvents.reserve(std::min<size_t>(estimatedEvents / 8, 1000000ULL));
        } catch (...) {}
    }

    if (r.readU32() != 0x4D546864) throw std::runtime_error("Not a MIDI file");
    uint32_t hdrLen = r.readU32();
    uint16_t format  = r.readU16();
    uint16_t nTracks = r.readU16();
    if (progress) progress->totalTracks = nTracks;
    uint16_t ppqRaw  = r.readU16();
    ppq = (int)(ppqRaw & 0x7FFF); 
    initialTempo = (int)MidiTiming::DEFAULT_TEMPO_MICROSECONDS;
    if (hdrLen > 6) r.skip(hdrLen - 6);

    int visualTrackCount = (format == 0) ? 16 : (int)nTracks;
    tracks.resize(visualTrackCount);

    if (r.totalSize > 0 && visualTrackCount > 0) {
        size_t notesPerTrack = (r.totalSize / 16) / (size_t)visualTrackCount;
        for (auto& td : tracks) {
            try {
                td.notes.reserve(std::max<size_t>(notesPerTrack, 1024));
            } catch (...) {}
        }
    }

    PendingNotePool pendingNotes[16][128];

    for (uint16_t trackIdx = 0; trackIdx < nTracks && !r.eof(); ++trackIdx) {
        uint32_t chunkId  = r.readU32();
        uint32_t chunkLen = r.readU32();
        if (progress) progress->currentTrack = trackIdx + 1;

        if (chunkId != 0x4D54726B) {  
            r.skip(chunkLen);
            continue;
        }

        uint32_t absTick   = 0;
        uint8_t  runStatus = 0;
        size_t   bytesLeft = chunkLen;

        bool isFormat0 = (format == 0);

        while (bytesLeft > 0 && !r.eof()) {
            uint32_t delta = 0;
            for (int i = 0; i < 4; ++i) {
                if (bytesLeft == 0) break;
                uint8_t b = r.readU8(); bytesLeft--;
                delta = (delta << 7) | (b & 0x7F);
                if (!(b & 0x80)) break;
            }
            absTick += delta;

            if (bytesLeft == 0) break;
            uint8_t statusByte = r.readU8(); bytesLeft--;

            uint8_t firstData = 0xFF; 
            if (statusByte & 0x80) {
                if (statusByte < 0xF0) {
                    runStatus = statusByte;
                } else {
                    runStatus = 0;
                }
            } else {
                firstData = statusByte;
                statusByte = runStatus;
            }

            if (statusByte == 0xFF) {
                runStatus = 0;
                if (bytesLeft < 1) break;
                uint8_t metaType = r.readU8(); bytesLeft--;

                uint32_t metaLen = 0;
                for (int i = 0; i < 4; ++i) {
                    if (bytesLeft == 0) break;
                    uint8_t b = r.readU8(); bytesLeft--;
                    metaLen = (metaLen << 7) | (b & 0x7F);
                    if (!(b & 0x80)) break;
                }
                if (metaType == 0x03 && !titleSetExplicitly && trackIdx == 0 && metaLen > 0 && bytesLeft >= metaLen) {
                    std::vector<char> titleBuf(metaLen + 1);
                    r.readBytes(titleBuf.data(), metaLen);
                    titleBuf[metaLen] = '\0';
                    bytesLeft -= metaLen;
                    if (titleBuf[0] != '!') {
                        g_midiTitle = titleBuf.data();
                        std::cout << "+ Default MIDI Title: " << g_midiTitle << std::endl;
                    }
                } else if (metaType == 0x51 && metaLen == 3 && bytesLeft >= 3) {
                    uint32_t tempoVal = r.readU24(); bytesLeft -= 3;
                    if (absTick == 0 && s_globalEvents.empty() &&
                        initialTempo == (int)MidiTiming::DEFAULT_TEMPO_MICROSECONDS) {
                        initialTempo = (int)tempoVal;
                    }
                    MidiEvent ev(absTick, EventType::TEMPO, 0);
                    ev.setTempo(tempoVal);   
                    s_globalEvents.push_back(ev);
                } else if (metaType == 0x58 && metaLen == 4 && bytesLeft >= 4) {
                    uint8_t nn = r.readU8(); bytesLeft--;
                    uint8_t dd = r.readU8(); bytesLeft--;
                    r.readU8(); bytesLeft--; 
                    r.readU8(); bytesLeft--; 
                    if (nn < 1)  nn = 1;
                    if (nn > 32) nn = 32;
                    if (dd > 5)  dd = 5; 
                    if (outTimeSigNumerator == 4 && outTimeSigDenominator == 4) {
                        outTimeSigNumerator   = nn;
                        outTimeSigDenominator = (uint16_t)(1u << dd); 
                    }
                } else if (metaType == 0x01 || metaType == 0x02 || metaType == 0x03 || metaType == 0x06 || metaType == 0x07) {
                    if (metaLen > 0 && bytesLeft >= metaLen && metaLen < 1024) {
                        std::vector<char> txtBuf(metaLen + 1);
                        r.readBytes(txtBuf.data(), metaLen);
                        txtBuf[metaLen] = '\0';
                        bytesLeft -= metaLen;

                        std::string customTitle;
                        if (ParseTitleSetCommand(txtBuf.data(), metaLen, customTitle)) {
                            g_midiTitle = customTitle;
                            titleSetExplicitly = true;
                        }

                        if (expandCompressedNotes) {
                            uint64_t multiplier = 1;
                            int targetTrack = -1;
                            if (ParseCompressCommand(txtBuf.data(), metaLen, (int)trackIdx, multiplier, targetTrack)) {
                                s_CompressNote.push_back({ absTick, targetTrack, multiplier });
                            }
                        }
                    } else {
                        if (metaLen > 0 && bytesLeft >= metaLen) {
                            r.skip(metaLen); bytesLeft -= metaLen;
                        }
                    }
                } else if (metaType == 0x2F) {
                    if (metaLen > 0 && bytesLeft >= metaLen) {
                        r.skip(metaLen); bytesLeft -= metaLen;
                    }
                    break;
                } else {
                    if (metaLen > 0 && bytesLeft >= metaLen) {
                        r.skip(metaLen); bytesLeft -= metaLen;
                    }
                }
                continue;
            }

            if (statusByte == 0xF0 || statusByte == 0xF7) {
                runStatus = 0;
                uint32_t sysLen = 0;
                for (int i = 0; i < 4; ++i) {
                    if (bytesLeft == 0) break;
                    uint8_t b = r.readU8(); bytesLeft--;
                    sysLen = (sysLen << 7) | (b & 0x7F);
                    if (!(b & 0x80)) break;
                }

                if (sysLen > 0 && bytesLeft >= sysLen) {
                    std::vector<uint8_t> msg;
                    msg.reserve(sysLen + 1);
                    
                    if (statusByte == 0xF0) {
                        msg.push_back(0xF0);
                    }
                    
                    size_t startPos = msg.size();
                    msg.resize(startPos + sysLen);
                    r.readBytes(msg.data() + startPos, sysLen);
                    bytesLeft -= sysLen;

                    uint32_t sysexIndex = (uint32_t)s_sysexPool.size();
                    s_sysexPool.push_back(std::move(msg));

                    MidiEvent ev(absTick, EventType::SYSEX, 0);
                    ev.setValue(sysexIndex);
                    s_globalEvents.push_back(ev);
                }
                continue;
            }

            uint8_t  evType   = statusByte & 0xF0;
            uint8_t  channel  = statusByte & 0x0F;
            uint8_t  vtrack   = (uint8_t)(isFormat0 ? channel : (trackIdx < (uint16_t)visualTrackCount ? trackIdx : 0));

            auto readData = [&]() -> uint8_t {
                if (firstData != 0xFF) { uint8_t v = firstData; firstData = 0xFF; return v; }
                if (bytesLeft == 0) return 0;
                uint8_t v = r.readU8(); bytesLeft--;
                return v;
            };

            auto GetCompressNote = [&](uint32_t tick, int trk, int ch) -> uint64_t {
                if (!expandCompressedNotes || s_CompressNote.empty()) return 1ULL;
                uint64_t mult = 1ULL;
                for (const auto& m : s_CompressNote) {
                    if (m.tick > tick) break;
                    if (m.targetTrack == -1 || m.targetTrack == trk || m.targetTrack == (ch + 1)) {
                        mult = m.multiplier;
                    }
                }
                return mult;
            };

            auto doNoteOff = [&](uint8_t note) {
                auto& list = pendingNotes[channel][note];
                if (!list.empty()) {
                    const PendingNote& oldest = list.front();
                    uint64_t mult = GetCompressNote(oldest.startTick, (int)trackIdx, channel);

                    uint32_t dur = (absTick > oldest.startTick) ? (absTick - oldest.startTick) : 1u;
                    if (dur > 65535u) dur = 65535u;

                    for (uint64_t m = 0; m < mult; ++m) {
                        MidiEvent ev(absTick, EventType::NOTE_OFF, channel);
                        ev.setNote(note, 0);
                        s_globalEvents.push_back(ev);

                        NoteEvent ne{};
                        ne.startTick   = oldest.startTick;
                        ne.duration    = (uint16_t)dur;
                        ne.note        = note;
                        ne.channel     = channel & 0x0F;
                        ne.visualTrack = (uint8_t)(vtrack & 0x0F);
                        tracks[vtrack].notes.push_back(ne);
                        totalNoteCount++;
                    }

                    if (progress && (totalNoteCount % 10000 == 0)) {
                        progress->currentNotes.store(totalNoteCount, std::memory_order_relaxed);
                    }
                    list.pop_front();
                }
            };

            switch (evType) {
                case 0x80: {   
                    uint8_t note = readData();
                    readData(); 
                    doNoteOff(note);
                    break;
                }
                case 0x90: {   
                    uint8_t note = readData();
                    uint8_t vel  = readData();
                    if (vel == 0) {
                        doNoteOff(note); 
                    } else {
                        uint64_t mult = GetCompressNote(absTick, (int)trackIdx, channel);

                        for (uint64_t m = 0; m < mult; ++m) {
                            MidiEvent ev(absTick, EventType::NOTE_ON, channel);
                            ev.setNote(note, vel);
                            s_globalEvents.push_back(ev);
                        }
                        
                        pendingNotes[channel][note].push_back(PendingNote{ absTick, vel, vtrack });
                    }
                    break;
                }
                case 0xB0: {   
                    uint8_t ctrl = readData();
                    uint8_t val  = readData();
                    
                    if (ctrl == 120 || ctrl == 123) {
                        break;
                    }

                    {
                        MidiEvent ev(absTick, EventType::CC, channel);
                        ev.setCC(ctrl, val);
                        s_globalEvents.push_back(ev);

                        CCEvent cc{};
                        cc.tick       = absTick;
                        cc.channel    = channel;
                        cc.controller = ctrl;
                        cc.value      = val;
                        ccEvents.push_back(cc);
                    }
                    break;
                }
                case 0xE0: {   
                    uint8_t lsb = readData();
                    uint8_t msb = readData();
                    MidiEvent ev(absTick, EventType::PITCH_BEND, channel);
                    ev.setPitchBend(lsb, msb);
                    s_globalEvents.push_back(ev);
                    break;
                }
                case 0xC0: {   
                    uint8_t prog = readData();
                    MidiEvent ev(absTick, EventType::PROGRAM_CHANGE, channel);
                    ev.setValue(prog);
                    s_globalEvents.push_back(ev);
                    break;
                }
                case 0xD0: {   
                    uint8_t pressure = readData();
                    MidiEvent ev(absTick, EventType::CHANNEL_PRESSURE, channel);
                    ev.setValue(pressure);
                    s_globalEvents.push_back(ev);
                    break;
                }
                case 0xA0: {   
                    readData(); readData();
                    break;
                }
                default:
                    if (firstData != 0xFF) {  }
                    else { if (bytesLeft > 0) { r.readU8(); bytesLeft--; } }
                    break;
            }
        }
        for (int ch = 0; ch < 16; ++ch) {
            for (int n = 0; n < 128; ++n) {
                auto& list = pendingNotes[ch][n];
                while (!list.empty()) {
                    const auto& pn = list.front();
                    uint32_t dur = (absTick > pn.startTick) ? (absTick - pn.startTick) : 1u;
                    if (dur > 65535u) dur = 65535u;

                    NoteEvent ne{};
                    ne.startTick   = pn.startTick;
                    ne.duration    = (uint16_t)dur;
                    ne.note        = n;
                    ne.channel     = ch & 0x0F;
                    ne.visualTrack = (uint8_t)(pn.visualTrack & 0x0F);
                    if (pn.visualTrack < (uint8_t)tracks.size())
                        tracks[pn.visualTrack].notes.push_back(ne);
                    totalNoteCount++;
                    if (progress && (totalNoteCount % 10000 == 0)) {
                        progress->currentNotes.store(totalNoteCount, std::memory_order_relaxed);
                    }
                    list.pop_front();
                }
                list.clear();
            }
        }

        if (bytesLeft > 0) r.skip((uint32_t)bytesLeft);
    }
    
    if (progress) {
        progress->currentNotes = totalNoteCount;
        progress->loadPhase = 2; 
    }
    if (removeOverlaps) {
        for (auto& td : tracks) {
            if (td.notes.empty()) continue;

            std::sort(td.notes.begin(), td.notes.end(),
                [](const NoteEvent& a, const NoteEvent& b){
                    if (a.note != b.note) return a.note < b.note;
                    if (a.channel != b.channel) return a.channel < b.channel;
                    if (a.startTick != b.startTick) return a.startTick < b.startTick;
                    return a.endTick() > b.endTick();
                });

            std::vector<NoteEvent> cleanNotes;
            try {
                cleanNotes.reserve(td.notes.size());
            } catch (...) {}
            cleanNotes.push_back(td.notes[0]);

            for (size_t i = 1; i < td.notes.size(); ++i) {
                const auto& next = td.notes[i];
                auto& last = cleanNotes.back();

                if (last.note == next.note && last.channel == next.channel) {
                    if (last.startTick == next.startTick && last.endTick() == next.endTick()) {
                        cleanNotes.push_back(next);
                        continue;
                    }
                    if (last.startTick == next.startTick) {
                        continue; 
                    }
                    if (last.endTick() > next.startTick) {
                        uint32_t newDur = (next.startTick > last.startTick) ? (next.startTick - last.startTick) : 1u;
                        last.duration = (uint16_t)std::min<uint32_t>(newDur, 65535u);
                    }
                }
                cleanNotes.push_back(next);
            }

            td.notes = std::move(cleanNotes);

            std::sort(td.notes.begin(), td.notes.end(),
                [](const NoteEvent& a, const NoteEvent& b){
                    return a.startTick < b.startTick;
                });
            td.notes.shrink_to_fit(); 
        }

        totalNoteCount = 0;
        for (const auto& td : tracks) {
            totalNoteCount += td.notes.size();
        }
    } else {
        for (auto& td : tracks) {
            if (td.notes.empty()) continue;
            std::sort(td.notes.begin(), td.notes.end(),
                [](const NoteEvent& a, const NoteEvent& b){
                    return a.startTick < b.startTick;
                });
            td.notes.shrink_to_fit();
        }
    }

    // In-place sort prevents allocating 4.4GB of duplicate temporary memory
    std::sort(s_globalEvents.begin(), s_globalEvents.end(),
        [](const MidiEvent& a, const MidiEvent& b) {
            if (a.tick != b.tick) return a.tick < b.tick;
            
            auto pri = [](uint8_t t) -> int {
                switch ((EventType)t) {
                    case EventType::TEMPO:            return 0;
                    case EventType::SYSEX:            return 1;
                    case EventType::NOTE_OFF:         return 2;
                    case EventType::CC:               return 3;
                    case EventType::PROGRAM_CHANGE:   return 4;
                    case EventType::PITCH_BEND:       return 5;
                    case EventType::CHANNEL_PRESSURE: return 5;
                    case EventType::NOTE_ON:          return 6;
                    default:                          return 7;
                }
            };
            int pa = pri(a.type);
            int pb = pri(b.type);
            if (pa != pb) return pa < pb;
            if (a.channel != b.channel) return a.channel < b.channel;
            return a.data < b.data;
        });

    s_globalEvents.shrink_to_fit(); 

    std::sort(ccEvents.begin(), ccEvents.end(),
        [](const CCEvent& a, const CCEvent& b){
            return a.tick < b.tick;
        });
    ccEvents.shrink_to_fit();

    return ccEvents;
}

const std::vector<MidiEvent>& GetGlobalMidiEvents() {
    return s_globalEvents;
}