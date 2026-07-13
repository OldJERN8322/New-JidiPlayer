// load.cpp
// MIDI file loader — 1:1 memory usage (one streaming pass, no duplicate buffers).
// Tempo stored/read as 3-byte (uint24) exactly as the MIDI spec mandates.
// Populates:
//   std::vector<MidiEvent>          → MidiOutputEngine
//   std::vector<OptimizedTrackData> → visualizer (NoteEvent note-on/off pairing)
//   std::vector<CCEvent>            → CC lane data
//   std::vector<TempoEvent>         → global tempo map
// ──────────────────────────────────────────────────────────────────────────────

#include "visualizer.hpp"
#include "midi_timing_alt.hpp"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <cassert>

namespace {

struct MidiReader {
    std::vector<uint8_t> buf;   
    size_t pos       = 0;
    size_t totalSize = 0;
    std::atomic<size_t>* progressBytes = nullptr;

    explicit MidiReader(const std::string& path, std::atomic<size_t>* pBytes = nullptr)
        : progressBytes(pBytes)
    {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return;

        fseek(f, 0, SEEK_END);
        totalSize = static_cast<size_t>(ftell(f));
        fseek(f, 0, SEEK_SET);

        buf.resize(totalSize);
        if (totalSize > 0)
            fread(buf.data(), 1, totalSize, f); 

        fclose(f);
    }

    bool eof() const { return pos >= totalSize; }

    bool readBytes(void* dst, size_t n) {
        if (pos + n > totalSize) return false;
        std::memcpy(dst, buf.data() + pos, n);
        pos += n;
        if (progressBytes && (pos % 4096 == 0))
            progressBytes->store(pos, std::memory_order_relaxed);
        return true;
    }

    uint8_t readU8() {
        if (pos >= totalSize) return 0;
        uint8_t v = buf[pos++];
        if (progressBytes && (pos % 4096 == 0))
            progressBytes->store(pos, std::memory_order_relaxed);
        return v;
    }

    uint16_t readU16() {
        if (pos + 2 > totalSize) return 0;
        uint16_t v = (static_cast<uint16_t>(buf[pos]) << 8) | buf[pos + 1];
        pos += 2;
        return v;
    }

    uint32_t readU32() {
        if (pos + 4 > totalSize) return 0;
        uint32_t v = (static_cast<uint32_t>(buf[pos    ]) << 24)
                   | (static_cast<uint32_t>(buf[pos + 1]) << 16)
                   | (static_cast<uint32_t>(buf[pos + 2]) <<  8)
                   |  static_cast<uint32_t>(buf[pos + 3]);
        pos += 4;
        return v;
    }

    uint32_t readU24() {
        if (pos + 3 > totalSize) return 0;
        uint32_t v = (static_cast<uint32_t>(buf[pos    ]) << 16)
                   | (static_cast<uint32_t>(buf[pos + 1]) <<  8)
                   |  static_cast<uint32_t>(buf[pos + 2]);
        pos += 3;
        return v;
    }

    uint32_t readVLQ() {
        uint32_t val = 0;
        for (int i = 0; i < 4 && pos < totalSize; ++i) {
            uint8_t b = buf[pos++];
            val = (val << 7) | (b & 0x7F);
            if (!(b & 0x80)) break;
        }
        return val;
    }

    void skip(uint32_t n) {
        pos += n;
        if (pos > totalSize) pos = totalSize;
    }
};

struct PendingNote {
    uint32_t startTick;
    uint8_t  velocity;
    uint8_t  visualTrack; 
};

} // namespace

static std::vector<MidiEvent> s_globalEvents;

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

            if (statusByte & 0x80) {
                if (statusByte < 0xF0) runStatus = statusByte;
            }

            uint8_t status = (statusByte & 0x80) ? statusByte : runStatus;
            uint8_t firstData = (statusByte & 0x80) ? 0xFF : statusByte; 

            if (status == 0xFF) {
                uint8_t  metaType = r.readU8(); consume(1);
                uint32_t metaLen  = r.readVLQ(); consume(0);
                if (metaType == 0x51 && metaLen == 3) {
                    uint32_t tempo = r.readU24(); consume(3);
                    tempos.push_back({ absTick, tempo });
                } else {
                    r.skip(metaLen); consume(metaLen);
                }
            } else if (status == 0xF0 || status == 0xF7) {
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

    std::stable_sort(tempos.begin(), tempos.end(),
        [](const TempoEvent& a, const TempoEvent& b){ return a.tick < b.tick; });
    return tempos;
}

std::vector<CCEvent> loadStreamingMidiData(
    const std::string& filename, std::vector<OptimizedTrackData>& tracks,
    int& ppq, int& initialTempo, uint64_t& totalNoteCount,
    uint16_t& outTimeSigNumerator, uint16_t& outTimeSigDenominator,
    LoadProgress* progress, bool removeOverlaps) {
    if (progress) progress->loadPhase = 1;
    MidiReader r(filename, progress ? &progress->bytesRead : nullptr);
    if (progress) progress->totalBytes = r.totalSize;
    tracks.clear();
    totalNoteCount = 0;
    outTimeSigNumerator   = 4;
    outTimeSigDenominator = 4;
    std::vector<CCEvent> ccEvents;

    s_globalEvents.clear();

    if (r.totalSize > 0) {
        size_t estimatedEvents = r.totalSize / 4; 
        s_globalEvents.reserve(estimatedEvents);
        ccEvents.reserve(estimatedEvents / 8);     
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
        for (auto& td : tracks)
            td.notes.reserve(std::max<size_t>(notesPerTrack, 1024));
    }

    std::vector<PendingNote> pendingNotes[16][128];

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
                }
            } else {
                firstData = statusByte;
                statusByte = runStatus;
            }

            if (statusByte == 0xFF) {
                if (bytesLeft < 1) break;
                uint8_t metaType = r.readU8(); bytesLeft--;

                uint32_t metaLen = 0;
                for (int i = 0; i < 4; ++i) {
                    if (bytesLeft == 0) break;
                    uint8_t b = r.readU8(); bytesLeft--;
                    metaLen = (metaLen << 7) | (b & 0x7F);
                    if (!(b & 0x80)) break;
                }

                if (metaType == 0x51 && metaLen == 3 && bytesLeft >= 3) {
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
                uint32_t sysLen = 0;
                for (int i = 0; i < 4; ++i) {
                    if (bytesLeft == 0) break;
                    uint8_t b = r.readU8(); bytesLeft--;
                    sysLen = (sysLen << 7) | (b & 0x7F);
                    if (!(b & 0x80)) break;
                }
                if (sysLen > 0 && bytesLeft >= sysLen) {
                    r.skip(sysLen); bytesLeft -= sysLen;
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

            auto doNoteOff = [&](uint8_t note) {
			MidiEvent ev(absTick, EventType::NOTE_OFF, channel);
			ev.setNote(note, 0);
			s_globalEvents.push_back(ev);

                auto& list = pendingNotes[channel][note];
                if (!list.empty()) {
                    auto oldest = list.begin();
                    NoteEvent ne{};
                    ne.startTick   = oldest->startTick;
                    ne.endTick     = absTick;
                    ne.note        = note;
                    ne.velocity    = oldest->velocity;
                    ne.channel     = channel;
                    ne.visualTrack = oldest->visualTrack;
                    tracks[vtrack].notes.push_back(ne);
                    totalNoteCount++;
                    if (progress && (totalNoteCount % 500 == 0)) {
                        progress->currentNotes.store(totalNoteCount, std::memory_order_relaxed);
                    }
                    list.erase(oldest);
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
						MidiEvent ev(absTick, EventType::NOTE_ON, channel);
						ev.setNote(note, vel);
						s_globalEvents.push_back(ev);
						
						pendingNotes[channel][note].push_back(PendingNote{ absTick, vel, vtrack });
					}
					break;
				}
				case 0xB0: {   
					uint8_t ctrl = readData();
					uint8_t val  = readData();
					if (ctrl == 120 || ctrl == 121 || ctrl == 123) {
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
				} case 0xA0: {   
                    readData(); readData();
                    break;
                }
                default:
                    if (firstData != 0xFF) {  }
                    else { if (bytesLeft > 0) { r.readU8(); bytesLeft--; } }
                    break;
            }
        }

        // Flush active notes at end-of-track boundary
        for (int ch = 0; ch < 16; ++ch) {
            for (int n = 0; n < 128; ++n) {
                auto& list = pendingNotes[ch][n];
                for (auto& pn : list) {
                    NoteEvent ne{};
                    ne.startTick  = pn.startTick;
                    ne.endTick    = absTick;   
                    ne.note       = n;
                    ne.velocity   = pn.velocity;
                    ne.channel    = ch;
                    ne.visualTrack= pn.visualTrack;
                    if (ne.visualTrack < (uint8_t)tracks.size())
                        tracks[ne.visualTrack].notes.push_back(ne);
                    totalNoteCount++;
                    if (progress && (totalNoteCount % 500 == 0)) {
                        progress->currentNotes.store(totalNoteCount, std::memory_order_relaxed);
                    }
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

    // ── Overlap & Duplicate Remover Filter (Conditional) ────────────────────
    if (removeOverlaps) {
        for (auto& td : tracks) {
            if (td.notes.empty()) continue;

            // Sort by note pitch first, then channel, then startTick, then endTick descending
            std::sort(td.notes.begin(), td.notes.end(),
                [](const NoteEvent& a, const NoteEvent& b){
                    if (a.note != b.note) return a.note < b.note;
                    if (a.channel != b.channel) return a.channel < b.channel;
                    if (a.startTick != b.startTick) return a.startTick < b.startTick;
                    return a.endTick > b.endTick;
                });

            std::vector<NoteEvent> cleanNotes;
            cleanNotes.reserve(td.notes.size());
            cleanNotes.push_back(td.notes[0]);

            for (size_t i = 1; i < td.notes.size(); ++i) {
                const auto& next = td.notes[i];
                auto& last = cleanNotes.back();

                if (last.note == next.note && last.channel == next.channel) {
                    // If they start on the exact same tick, discard the duplicate shorter one
                    if (last.startTick == next.startTick) {
                        continue; 
                    }
                    // Truncate previous note if it extends into/past the start of the next note
                    if (last.endTick > next.startTick) {
                        last.endTick = next.startTick;
                    }
                    // Safety floor clamp
                    if (last.endTick <= last.startTick) {
                        last.endTick = last.startTick + 1;
                    }
                }
                cleanNotes.push_back(next);
            }

            td.notes = std::move(cleanNotes);

            // Restore startTick sorting for visualizer compatibility
            std::sort(td.notes.begin(), td.notes.end(),
                [](const NoteEvent& a, const NoteEvent& b){
                    return a.startTick < b.startTick;
                });
            td.notes.shrink_to_fit(); 
        }

        // Recalculate true visual note count
        totalNoteCount = 0;
        for (const auto& td : tracks) {
            totalNoteCount += td.notes.size();
        }
    } else {
        // If bypass overlap removal, still sort tracks by startTick so binary search functions
        for (auto& td : tracks) {
            if (td.notes.empty()) continue;
            std::sort(td.notes.begin(), td.notes.end(),
                [](const NoteEvent& a, const NoteEvent& b){
                    return a.startTick < b.startTick;
                });
            td.notes.shrink_to_fit();
        }
    }

    std::sort(s_globalEvents.begin(), s_globalEvents.end(),
        [](const MidiEvent& a, const MidiEvent& b) {
            if (a.tick != b.tick) return a.tick < b.tick;
            bool aTempo = (a.type == (uint8_t)EventType::TEMPO);
            bool bTempo = (b.type == (uint8_t)EventType::TEMPO);
            if (aTempo != bTempo) return aTempo > bTempo; 
            
            auto pri = [](uint8_t t) -> int {
				if (t == (uint8_t)EventType::TEMPO)    return 0;
				if (t == (uint8_t)EventType::NOTE_OFF) return 1;
				if (t == (uint8_t)EventType::NOTE_ON)  return 2;
				return 3;
			};
			if (pri(a.type) != pri(b.type)) return pri(a.type) < pri(b.type);
			return false;
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