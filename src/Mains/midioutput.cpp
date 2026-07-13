// midioutput.cpp
#include "midioutput.hpp"
#include "bass_backend.hpp"   

#include <iostream>
#include <algorithm>
#include <cstring>

extern "C" {
    void SendDirectData(unsigned long data);
}

MidiOutputEngine::MidiOutputEngine() : 
    threadRunning(false), isPlaying(false), isPaused(false), isFinished(false), isLooping(false), antiSlowdownEnabled(false),
    eventList(nullptr), currentPpq(480), currentVisualizerTick(0), playbackSpeed(1.0f) {
}

void MidiOutputEngine::BuildTempoIndex() {
    tempoIndex.clear();
    if (!eventList) return;

    uint32_t tick         = 0;
    double   accumMicros  = 0.0;
    uint32_t rawTempo     = MidiTiming::DEFAULT_TEMPO_MICROSECONDS;
    double   microsPerTick = MidiTiming::CalculateMicrosecondsPerTick(rawTempo, currentPpq);

    tempoIndex.push_back({ 0, 0, 0.0, rawTempo });

    for (size_t i = 0; i < eventList->size(); ++i) {
        const auto& ev = (*eventList)[i];
        if (ev.type != (uint8_t)EventType::TEMPO) continue;

        accumMicros  += (ev.tick - tick) * microsPerTick;
        tick          = ev.tick;
        rawTempo      = ev.getTempo(); // Corrected: replaced ev.data.tempo with ev.getTempo()
        microsPerTick = MidiTiming::CalculateMicrosecondsPerTick(rawTempo, currentPpq);

        tempoIndex.push_back({ i, tick, accumMicros, rawTempo });
    }
}

void MidiOutputEngine::ToggleAntiSlowdown(bool enabled) {
    antiSlowdownEnabled = enabled;
}

bool MidiOutputEngine::IsAntiSlowdownEnabled() const {
    return antiSlowdownEnabled.load();
}

void MidiOutputEngine::ToggleEventCounterRecord(bool enabled) {
    eventCounterRecordEnabled.store(enabled, std::memory_order_relaxed);
}

bool MidiOutputEngine::IsEventCounterRecordEnabled() const {
    return eventCounterRecordEnabled.load(std::memory_order_relaxed);
}

void MidiOutputEngine::SetSimulateEventsPerSecond(int64_t eps) {
    simulateEventsPerSecond.store(eps > 0 ? eps : 0);
    if (eps <= 0) simLagActive.store(false);
}

int64_t MidiOutputEngine::GetSimulateEventsPerSecond() const {
    return simulateEventsPerSecond.load();
}

bool MidiOutputEngine::IsSimulateLagActive() const {
    return simLagActive.load();
}

void MidiOutputEngine::SetLagSmoothRender(bool smooth) {
    simLagSmooth.store(smooth);
}

bool MidiOutputEngine::GetLagSmoothRender() const {
    return simLagSmooth.load();
}

MidiOutputEngine::~MidiOutputEngine() {
    Stop();
}

void MidiOutputEngine::Start(const std::vector<MidiEvent>& events, int ppq, uint32_t initialTempo) {
    Stop();
    eventList = &events;
    currentPpq = ppq;
    currentTempo = initialTempo;
    microsecondsPerTick = MidiTiming::CalculateMicrosecondsPerTick(currentTempo, ppq);
    accumulatedMicroseconds = 0.0;
    pauseVirtualMicros = 0.0;
    eventPos = 0;
    lastProcessedTick = 0;
    currentVisualizerTick = 0;
    isFinished = false;
    isPaused = false;

    simTokens    = 0.0;
    simLagActive = false;
    simLastRefill = std::chrono::steady_clock::now();

    BuildTempoIndex();

    if (g_BassEngine.IsInitialized() &&
        g_BassEngine.GetActiveMode() == AudioMode::BassMIDI_PreRender &&
        !events.empty())
    {
        uint64_t totalMicros = 0;
        {
            uint32_t lastTick = 0;
            double   usPerTick = MidiTiming::CalculateMicrosecondsPerTick(initialTempo, ppq);
            for (const auto& ev : events) {
                if (ev.type == (uint8_t)EventType::TEMPO) {
                    totalMicros += (uint64_t)((ev.tick - lastTick) * usPerTick);
                    lastTick     = ev.tick;
                    usPerTick    = MidiTiming::CalculateMicrosecondsPerTick(ev.getTempo(), ppq); // Corrected: replaced ev.data.tempo with ev.getTempo()
                }
            }
            totalMicros += (uint64_t)((events.back().tick - lastTick) * usPerTick);
        }
        g_BassEngine.StartPreRender(events.data(), events.size(),
                                    ppq, initialTempo, totalMicros);
    }

    isPlaying = true;
    threadRunning = true;
    playbackStartTime = std::chrono::steady_clock::now();
    workerThread = std::thread(&MidiOutputEngine::PlaybackThread, this);
}

void MidiOutputEngine::Stop() {
    if (threadRunning) {
        threadRunning = false;
        if (workerThread.joinable()) {
            workerThread.join();
        }
    }
    SilenceAllChannels();
    isPlaying = false;

    if (g_BassEngine.IsInitialized() &&
        g_BassEngine.GetActiveMode() != AudioMode::KDMAPI)
        g_BassEngine.Stop();
}

void MidiOutputEngine::Pause() {
    if (!isPaused && isPlaying) {
        auto now = std::chrono::steady_clock::now();
        uint64_t elapsedRealMicros = std::chrono::duration_cast<std::chrono::microseconds>(now - playbackStartTime).count();
        pauseVirtualMicros = (uint64_t)(elapsedRealMicros * playbackSpeed.load());
        isPaused = true;
        SilenceAllChannelsWithoutCC();

        if (g_BassEngine.IsInitialized() &&
            g_BassEngine.GetActiveMode() != AudioMode::KDMAPI)
            g_BassEngine.Pause();
    }
}

void MidiOutputEngine::Resume() {
    if (isPaused && isPlaying) {
        auto now = std::chrono::steady_clock::now();
        playbackStartTime = now - std::chrono::microseconds((uint64_t)(pauseVirtualMicros / playbackSpeed.load()));
        isPaused = false;

        if (g_BassEngine.IsInitialized() &&
            g_BassEngine.GetActiveMode() != AudioMode::KDMAPI)
            g_BassEngine.Play();
    }
}

void MidiOutputEngine::SilenceAllChannels() {
    for (int ch = 0; ch < 16; ++ch) {
        DispatchMidiOut((0xB0 | ch) | (123 << 8)); 
        DispatchMidiOut((0xB0 | ch) | (121 << 8)); 
    }
    memset(activeNotes, 0, sizeof(activeNotes)); 
}

void MidiOutputEngine::SilenceAllChannelsWithoutCC() {
    for (int ch = 0; ch < 16; ++ch) {
        DispatchMidiOut((0xB0 | ch) | (123 << 8));
    }
    memset(activeNotes, 0, sizeof(activeNotes)); 
}

void MidiOutputEngine::SetSpeed(float newSpeed) {
    if (isPlaying && !isPaused) {
        auto now = std::chrono::steady_clock::now();
        uint64_t elapsedRealMicros = std::chrono::duration_cast<std::chrono::microseconds>(now - playbackStartTime).count();
        uint64_t elapsedVirtualMicros = (uint64_t)(elapsedRealMicros * playbackSpeed.load());
        playbackSpeed = newSpeed;
        playbackStartTime = now - std::chrono::microseconds((uint64_t)(elapsedVirtualMicros / playbackSpeed.load()));
    } else {
        playbackSpeed = newSpeed;
    }
    microsecondsPerTick = MidiTiming::CalculateMicrosecondsPerTick(currentTempo, currentPpq);
    
    if (g_BassEngine.IsInitialized()) {
        g_BassEngine.SetPlaybackSpeed(newSpeed);
    }
}

void MidiOutputEngine::SetLooping(bool loop) {
    isLooping = loop;
}

void MidiOutputEngine::SetLoopPoints(uint64_t startTick, uint64_t endTick) {
    loopStartTick.store(startTick);
    loopEndTick.store(endTick);
    hasLoopPoints.store(true);
}

void MidiOutputEngine::ClearLoopPoints() {
    hasLoopPoints.store(false);
    loopStartTick.store(0);
    loopEndTick.store(UINT64_MAX);
}

bool     MidiOutputEngine::HasLoopPoints()    const { return hasLoopPoints.load(); }
uint64_t MidiOutputEngine::GetLoopStartTick() const { return loopStartTick.load(); }
uint64_t MidiOutputEngine::GetLoopEndTick()   const { return loopEndTick.load(); }

uint64_t MidiOutputEngine::TickToMicros(uint64_t targetTick) const {
    if (tempoIndex.empty()) return 0;
    size_t lo = 0, hi = tempoIndex.size();
    while (lo + 1 < hi) {
        size_t mid = (lo + hi) / 2;
        if ((uint64_t)tempoIndex[mid].tick <= targetTick) lo = mid;
        else hi = mid;
    }
    const auto& seg = tempoIndex[lo];
    double mpt = MidiTiming::CalculateMicrosecondsPerTick(seg.rawTempo, currentPpq);
    return (uint64_t)(seg.accumMicros + (double)(targetTick - seg.tick) * mpt);
}

void MidiOutputEngine::LoopBackToTick(uint64_t loopStart) {
    SilenceAllChannels();

    size_t segIdx = 0;
    {
        size_t lo = 0, hi = tempoIndex.size();
        while (lo + 1 < hi) {
            size_t mid = (lo + hi) / 2;
            if ((uint64_t)tempoIndex[mid].tick <= loopStart) lo = mid;
            else hi = mid;
        }
        segIdx = lo;
    }
    const TempoSegment& seg = tempoIndex[segIdx];

    uint64_t scanAccum  = (uint64_t)seg.accumMicros;
    uint32_t tempTempo  = seg.rawTempo;
    double   tempMPT    = MidiTiming::CalculateMicrosecondsPerTick(tempTempo, currentPpq);
    size_t   newEP      = seg.eventIdx;
    uint32_t newLTick   = seg.tick;

    uint8_t chaseProgram[16];
    uint16_t chasePitchBend[16];
    uint8_t chaseCC[16][128];
    std::memset(chaseProgram, 0xFF, sizeof(chaseProgram));
    std::memset(chasePitchBend, 0xFF, sizeof(chasePitchBend));
    std::memset(chaseCC, 0xFF, sizeof(chaseCC));

    while (newEP < eventList->size()) {
        const auto& ev = (*eventList)[newEP];
        if ((uint64_t)ev.tick >= loopStart) break;
        scanAccum = (uint64_t)(seg.accumMicros + (double)(ev.tick - seg.tick) * tempMPT);
        newLTick  = ev.tick;
        if (ev.type == (uint8_t)EventType::TEMPO) {
            tempTempo = ev.getTempo(); // Corrected: replaced ev.data.tempo with ev.getTempo()
            tempMPT   = MidiTiming::CalculateMicrosecondsPerTick(tempTempo, currentPpq);
        } else if (ev.type == (uint8_t)EventType::CC) {
            chaseCC[ev.channel][ev.getCCController()] = ev.getCCValue();
        } else if (ev.type == (uint8_t)EventType::PROGRAM_CHANGE) {
            chaseProgram[ev.channel] = ev.getValue();
        } else if (ev.type == (uint8_t)EventType::PITCH_BEND) {
            chasePitchBend[ev.channel] = (ev.getPitchBendMSB() << 8) | ev.getPitchBendLSB();
        }
        newEP++;
    }

    for (int ch = 0; ch < 16; ++ch) {
        if (chaseProgram[ch] != 0xFF) {
            DispatchMidiOut((0xC0 | ch) | (chaseProgram[ch] << 8));
        }
        if (chasePitchBend[ch] != 0xFFFF) {
            uint8_t lsb = chasePitchBend[ch] & 0xFF;
            uint8_t msb = (chasePitchBend[ch] >> 8) & 0xFF;
            DispatchMidiOut((0xE0 | ch) | (lsb << 8) | (msb << 16));
        }
        for (int c = 0; c < 128; ++c) {
            if (chaseCC[ch][c] != 0xFF) {
                DispatchMidiOut((0xB0 | ch) | (c << 8) | (chaseCC[ch][c] << 16));
            }
        }
    }

    uint64_t startMicros = scanAccum + (uint64_t)((double)(loopStart - newLTick) * tempMPT);

    accumulatedMicroseconds = (double)scanAccum;
    lastProcessedTick       = newLTick;
    eventPos                = newEP;
    currentTempo            = tempTempo;
    microsecondsPerTick     = tempMPT;
    currentVisualizerTick   = loopStart;

    double spd = (double)playbackSpeed.load();
    uint64_t realOffset = (spd > 0.0) ? (uint64_t)((double)startMicros / spd) : 0ULL;
    playbackStartTime = std::chrono::steady_clock::now() - std::chrono::microseconds(realOffset);

    simTokens     = 0.0;
    simLagActive  = false;
    simLastRefill = std::chrono::steady_clock::now();

    if (g_BassEngine.IsInitialized() && g_BassEngine.GetActiveMode() != AudioMode::KDMAPI)
        g_BassEngine.SeekTo(startMicros);
}

uint64_t MidiOutputEngine::GetCurrentTick() const {
    return currentVisualizerTick.load();
}

size_t MidiOutputEngine::GetEventPos() const {
    return eventPos.load();
}

uint32_t MidiOutputEngine::GetCurrentTempo() const {
    return currentTempo.load();
}

bool MidiOutputEngine::IsFinished() const {
    return isFinished.load();
}

bool MidiOutputEngine::IsPaused() const {
    return isPaused.load();
}

void MidiOutputEngine::Seek(int64_t microsecondOffset) {
    bool wasPlaying = !isPaused.load();
    Pause(); 
    SilenceAllChannelsWithoutCC();
    
    int64_t targetMicros = (int64_t)pauseVirtualMicros + microsecondOffset;
    if (targetMicros < 0) targetMicros = 0;
    
    pauseVirtualMicros = (uint64_t)targetMicros; 
    
    size_t segIdx = 0;
    {
        size_t lo = 0, hi = tempoIndex.size();
        while (lo + 1 < hi) {
            size_t mid = (lo + hi) / 2;
            if (tempoIndex[mid].accumMicros <= (double)targetMicros) lo = mid;
            else hi = mid;
        }
        segIdx = lo;
    }

    const TempoSegment& seg   = tempoIndex[segIdx];
    uint64_t scanAccumulatedMicros = (uint64_t)seg.accumMicros;
    uint32_t tempTempo             = seg.rawTempo;
    double   tempMicrosPerTick     = MidiTiming::CalculateMicrosecondsPerTick(tempTempo, currentPpq);
    eventPos          = seg.eventIdx;
    lastProcessedTick = seg.tick;

    uint8_t chaseProgram[16];
    uint16_t chasePitchBend[16];
    uint8_t chaseCC[16][128];
    std::memset(chaseProgram, 0xFF, sizeof(chaseProgram));
    std::memset(chasePitchBend, 0xFF, sizeof(chasePitchBend));
    std::memset(chaseCC, 0xFF, sizeof(chaseCC));

    while (eventPos < eventList->size()) {
        const auto& event = (*eventList)[eventPos];
        uint64_t eventScheduledTime = scanAccumulatedMicros +
            (uint64_t)((event.tick - lastProcessedTick) * tempMicrosPerTick);
        if (eventScheduledTime > (uint64_t)targetMicros) break;
        scanAccumulatedMicros = eventScheduledTime;
        lastProcessedTick     = event.tick;
        if (event.type == (uint8_t)EventType::TEMPO) {
            tempTempo         = event.getTempo(); // Corrected: replaced event.data.tempo with event.getTempo()
            tempMicrosPerTick = MidiTiming::CalculateMicrosecondsPerTick(tempTempo, currentPpq);
        } else if (event.type == (uint8_t)EventType::CC) {
            chaseCC[event.channel][event.getCCController()] = event.getCCValue();
        } else if (event.type == (uint8_t)EventType::PROGRAM_CHANGE) {
            chaseProgram[event.channel] = event.getValue();
        } else if (event.type == (uint8_t)EventType::PITCH_BEND) {
            chasePitchBend[event.channel] = (event.getPitchBendMSB() << 8) | event.getPitchBendLSB();
        }
        eventPos++;
    }
    
    for (int ch = 0; ch < 16; ++ch) {
        if (chaseProgram[ch] != 0xFF) {
            DispatchMidiOut((0xC0 | ch) | (chaseProgram[ch] << 8));
        }
        if (chasePitchBend[ch] != 0xFFFF) {
            uint8_t lsb = chasePitchBend[ch] & 0xFF;
            uint8_t msb = (chasePitchBend[ch] >> 8) & 0xFF;
            DispatchMidiOut((0xE0 | ch) | (lsb << 8) | (msb << 16));
        }
        for (int c = 0; c < 128; ++c) {
            if (chaseCC[ch][c] != 0xFF) {
                DispatchMidiOut((0xB0 | ch) | (c << 8) | (chaseCC[ch][c] << 16));
            }
        }
    }

    currentTempo        = tempTempo;
    microsecondsPerTick = tempMicrosPerTick;
    accumulatedMicroseconds = scanAccumulatedMicros;
    
    uint64_t microsSinceLastEvent = pauseVirtualMicros - scanAccumulatedMicros;
    if (tempMicrosPerTick > 0.0) {
        currentVisualizerTick = lastProcessedTick + (uint64_t)(microsSinceLastEvent / tempMicrosPerTick);
    }
    
    if (isFinished && eventPos < eventList->size()) {
        isFinished = false;
    }

    simTokens    = 0.0;
    simLagActive = false;
    simLastRefill = std::chrono::steady_clock::now();

    if (g_BassEngine.IsInitialized() &&
        g_BassEngine.GetActiveMode() != AudioMode::KDMAPI)
        g_BassEngine.SeekTo((uint64_t)targetMicros);
    
    if (wasPlaying) Resume();
}

void MidiOutputEngine::SeekAbsolute(uint64_t targetMicros) {
    bool wasPlaying = !isPaused.load();
    Pause();
    int64_t delta = (int64_t)targetMicros - (int64_t)pauseVirtualMicros;
    Seek(delta);
    if (wasPlaying) Resume();
}

void MidiOutputEngine::RecordDispatch(uint64_t count) {
    auto    now     = std::chrono::steady_clock::now();
    int64_t nowMs   = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch()).count();
    int     cur     = m_evpsCurBucket.load(std::memory_order_relaxed);
    int64_t bStart  = m_evpsBuckets[cur].startMs.load(std::memory_order_relaxed);

    if (bStart < 0 || nowMs - bStart >= 10) {
        int next = (cur + 1) % kEvpsBuckets;
        m_evpsBuckets[next].startMs.store(nowMs, std::memory_order_relaxed);
        m_evpsBuckets[next].count.store(count,  std::memory_order_relaxed);
        m_evpsCurBucket.store(next,            std::memory_order_relaxed);
    } else {
        m_evpsBuckets[cur].count.fetch_add(count,  std::memory_order_relaxed);
    }
}

uint64_t MidiOutputEngine::GetEventsPerSecond() const {
    auto    now   = std::chrono::steady_clock::now();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count();
                        
    // If queried within 10ms, return the cached result.
    // This prevents redundant bucket loops when running at high uncapped FPS.
    int64_t lastUpdate = m_lastEpsUpdateMs.load(std::memory_order_relaxed);
    if (nowMs - lastUpdate < 10) {
        return m_cachedEps.load(std::memory_order_relaxed);
    }

    uint64_t total = 0;
    for (int i = 0; i < kEvpsBuckets; i++) {
        int64_t start = m_evpsBuckets[i].startMs.load(std::memory_order_relaxed);
        if (start >= 0 && nowMs - start <= 1000)
            total += m_evpsBuckets[i].count.load(std::memory_order_relaxed);
    }

    m_cachedEps.store(total, std::memory_order_relaxed);
    m_lastEpsUpdateMs.store(nowMs, std::memory_order_relaxed);
    return total;
}

void MidiOutputEngine::PlaybackThread() {
    while (threadRunning) {
        if (isPaused || isFinished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        uint64_t elapsedRealMicros = std::chrono::duration_cast<std::chrono::microseconds>(now - playbackStartTime).count();
        uint64_t elapsedVirtualMicros = (uint64_t)(elapsedRealMicros * playbackSpeed.load());
        
        double microsSinceLastEvent = ((double)elapsedVirtualMicros > accumulatedMicroseconds)
            ? (double)elapsedVirtualMicros - accumulatedMicroseconds : 0.0;
        double effectiveMicrosPerTick = microsecondsPerTick;
        const int64_t eps = simulateEventsPerSecond.load();
        if (eps > 0 && simLagActive.load() && !simLagSmooth.load()) {
            microsSinceLastEvent = 0.0; 
        }
        
        if (effectiveMicrosPerTick > 0.0) {
            uint64_t rawVizTick = lastProcessedTick + (uint64_t)(microsSinceLastEvent / effectiveMicrosPerTick);
            if (hasLoopPoints.load() && isLooping.load())
                currentVisualizerTick = std::min(rawVizTick, loopEndTick.load());
            else
                currentVisualizerTick = rawVizTick;
        }

        if (eps > 0) {
            auto nowSim = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(nowSim - simLastRefill).count();
            simLastRefill = nowSim;
            const double burstCap = (double)eps * 0.002; 
            simTokens = std::min(simTokens + dt * (double)eps, burstCap);
        }

        int processedInBatch = 0;
        uint64_t dispatchAccumulator = 0; // Local counter for batching metrics

        while (eventPos < eventList->size() && threadRunning && !isPaused) {
            const auto& event = (*eventList)[eventPos];

            if (hasLoopPoints.load() && isLooping.load()) {
                if ((uint64_t)event.tick >= loopEndTick.load()) break;
            }

            double scheduledTime = accumulatedMicroseconds + (event.tick - lastProcessedTick) * effectiveMicrosPerTick;    
            if (scheduledTime > (double)elapsedVirtualMicros) {
                double waitTimeMicros = scheduledTime - (double)elapsedVirtualMicros;
                if (waitTimeMicros > 2500.0) {
                    uint64_t sleepTime = (uint64_t)(waitTimeMicros - 1500.0);
                    if (sleepTime > 5000) sleepTime = 5000; 
                    std::this_thread::sleep_for(std::chrono::microseconds(sleepTime));
                } else if (waitTimeMicros > 200.0) {
                    std::this_thread::yield();
                }
                break; 
            }

            const bool bassActive = g_BassEngine.IsInitialized() &&
                                    g_BassEngine.GetActiveMode() != AudioMode::KDMAPI;
            if (eps > 0 && !bassActive) {
                if (simTokens < 1.0) {
                    simLagActive.store(true);
                    break;
                }
                simTokens -= 1.0;
                simLagActive.store(false);
            }
            
            accumulatedMicroseconds = scheduledTime;
            lastProcessedTick = event.tick;
            processedInBatch++;
            
            if (processedInBatch % 4096 == 0) {
                currentVisualizerTick = event.tick;
            }
            
            if (event.type == (uint8_t)EventType::TEMPO) {
                currentTempo = event.getTempo();
                microsecondsPerTick = MidiTiming::CalculateMicrosecondsPerTick(currentTempo, currentPpq);
                effectiveMicrosPerTick = microsecondsPerTick;
            } else if (event.type == (uint8_t)EventType::NOTE_OFF) {
                DispatchMidiOut((0x80 | event.channel) | (event.getNote() << 8) | (event.getVelocity() << 16));
                activeNotes[event.channel][event.getNote()] = false;
            } else if (event.type == (uint8_t)EventType::NOTE_ON) {
                uint8_t ch = event.channel, n = event.getNote(), v = event.getVelocity();
                DispatchMidiOut((0x90 | ch) | (n << 8) | (v << 16));
                activeNotes[ch][n] = (v > 0);
            } else if (event.type == (uint8_t)EventType::CC) {
                DispatchMidiOut((0xB0 | event.channel) | (event.getCCController() << 8) | (event.getCCValue() << 16));
            } else if (event.type == (uint8_t)EventType::PITCH_BEND) {
                DispatchMidiOut((0xE0 | event.channel) | (event.getPitchBendLSB() << 8) | (event.getPitchBendMSB() << 16));
            } else if (event.type == (uint8_t)EventType::PROGRAM_CHANGE) {
                DispatchMidiOut((0xC0 | event.channel) | (event.getValue() << 8));
            }

			// eventPos must ALWAYS advance — this is the playback loop's own
			// read cursor, not a debug counter. Gating it behind the
			// info-only "event counter record" toggle would freeze playback
			// entirely (this exact index never advances) whenever that
			// toggle is off. Only the EVPS/dispatch stats accumulation is
			// optional; advancement is not.
			eventPos++;
			if (eventCounterRecordEnabled.load(std::memory_order_relaxed)) {
				dispatchAccumulator++; // Increment the local batch counter
				if (dispatchAccumulator >= 2048) {
					RecordDispatch(dispatchAccumulator);
					dispatchAccumulator = 0;
				}
			}
        }

        // Flush any remaining accumulated dispatches at the end of the batch
        if (dispatchAccumulator > 0) {
            RecordDispatch(dispatchAccumulator);
            dispatchAccumulator = 0;
        }

        if (eps > 0 && simLagActive.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (hasLoopPoints.load() && isLooping.load() && threadRunning && !isPaused) {
            uint64_t loopEnd = loopEndTick.load();

            bool nextPastB = (eventPos >= eventList->size()) ||
                             ((uint64_t)(*eventList)[eventPos].tick >= loopEnd);

            if (nextPastB) {
                double loopEndMicros = accumulatedMicroseconds +
                    (double)((int64_t)loopEnd - (int64_t)lastProcessedTick) * effectiveMicrosPerTick;

                if ((double)elapsedVirtualMicros >= loopEndMicros) {
                    LoopBackToTick(loopStartTick.load());
                    continue;
                }

                double realWaitUs = (loopEndMicros - (double)elapsedVirtualMicros) / std::max(0.01, (double)playbackSpeed.load());
                if (realWaitUs > 800.0) {
                    uint64_t sleepUs = std::min((uint64_t)(realWaitUs - 400.0), (uint64_t)2000);
                    std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
                }
            }
        }

        if (eventPos >= eventList->size()) {
            if (isLooping.load()) {
                if (hasLoopPoints.load()) {
                    LoopBackToTick(loopStartTick.load());
                } else {
                    SilenceAllChannels();
                    accumulatedMicroseconds = 0.0;
                    lastProcessedTick = 0;
                    currentVisualizerTick = 0;
                    eventPos = 0;

                    uint32_t tempTempo = MidiTiming::DEFAULT_TEMPO_MICROSECONDS;
                    if (!eventList->empty() && (*eventList)[0].type == (uint8_t)EventType::TEMPO)
                        tempTempo = (*eventList)[0].getTempo();
                    currentTempo = tempTempo;
                    microsecondsPerTick = MidiTiming::CalculateMicrosecondsPerTick(currentTempo, currentPpq);

                    simTokens    = 0.0;
                    simLagActive = false;
                    simLastRefill = std::chrono::steady_clock::now();
                    playbackStartTime = std::chrono::steady_clock::now();

                    if (g_BassEngine.IsInitialized() &&
                        g_BassEngine.GetActiveMode() != AudioMode::KDMAPI) {
                        g_BassEngine.SeekTo(0);
                        g_BassEngine.Play();
                    }
                }
            } else {
                isFinished = true;
            }
        }
    }
}