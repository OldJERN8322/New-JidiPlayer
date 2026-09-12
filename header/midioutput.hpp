#pragma once
#include "visualizer.hpp"
#include "midi_timing_alt.hpp"
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <algorithm>

class MidiOutputEngine {
public:
    MidiOutputEngine();
    ~MidiOutputEngine();
    void Start(const std::vector<MidiEvent>& events, int ppq, uint32_t initialTempo);
    void Stop();
    void Pause();
    void Resume();
    void Seek(int64_t microsecondOffset);
    void SeekAbsolute(uint64_t targetMicroseconds);
    void SetSpeed(float newSpeed);
    float GetPlaybackSpeed() const;
    void SetLooping(bool loop);
    uint64_t GetCurrentTick() const;
    size_t GetEventPos() const;
    uint32_t GetCurrentTempo() const;
    bool IsFinished() const;
    bool IsPaused() const;
    void SetLoopPoints(uint64_t startTick, uint64_t endTick);
    void ClearLoopPoints();
    bool HasLoopPoints()    const;
    uint64_t GetLoopStartTick() const;
    uint64_t GetLoopEndTick()   const;
	std::atomic<uint64_t> currentPolyphony{0};
    std::atomic<uint64_t> currentNotesDispatched{0};

    // ---------------------------------------------------------------
    // Event Skip / Anti-Slowdown
    // ---------------------------------------------------------------
    void ToggleAntiSlowdown(bool enabled);
    bool IsAntiSlowdownEnabled() const;

    // ---------------------------------------------------------------
    // Tempo Override
    // ---------------------------------------------------------------
    void  SetTempoOverride(bool enabled, float targetBpm);
    bool  IsTempoOverrideEnabled() const;
    float GetTempoOverrideTarget() const;

    // Event counter record
    void ToggleEventCounterRecord(bool enabled);
    bool IsEventCounterRecordEnabled() const;

    // ---------------------------------------------------------------
    // Slowdown Mode (Events Per Second Limiter)
    // ---------------------------------------------------------------
    void    SetSimulateEventsPerSecond(int64_t eps);
    int64_t GetSimulateEventsPerSecond() const;
    bool    IsSimulateLagActive() const;

    void SetLagSmoothRender(bool smooth);
    bool GetLagSmoothRender() const;

    // ── Events-per-second counter ──
    std::atomic<uint64_t> eventsDispatchedCounter{ 0 };
    uint64_t GetAndResetEventCount() {
        return eventsDispatchedCounter.exchange(0, std::memory_order_relaxed);
    }
    
    // ── EVPS sliding-window ring buffer (100 × 10ms = 1 second) ──────────
    static constexpr int kEvpsBuckets = 100;
    struct EvpsBucket {
        std::atomic<int64_t>  startMs{ -1 };
        std::atomic<uint64_t> count{ 0 };
    };
    EvpsBucket              m_evpsBuckets[kEvpsBuckets];
    std::atomic<int>        m_evpsCurBucket{ 0 };

    void RecordDispatch(uint64_t count = 1);
    uint64_t GetEventsPerSecond() const;

private:
    void PlaybackThread();
    void SilenceAllChannels();
    void SilenceAllChannelsWithoutCC();
    void BuildTempoIndex();
    void ApplyTempoOverride();

    struct TempoSegment {
        size_t   eventIdx;
        uint32_t tick;
        double   accumMicros;
        uint32_t rawTempo;
    };
    std::vector<TempoSegment> tempoIndex;
    std::thread workerThread;
    std::atomic<bool> threadRunning;
    std::atomic<bool> isPlaying;
    std::atomic<bool> isPaused;
    std::atomic<bool> isFinished;
    std::atomic<bool> isLooping;
    const std::vector<MidiEvent>* eventList;
    int currentPpq;
    std::atomic<uint64_t> currentVisualizerTick;
    std::atomic<float> playbackSpeed;
    std::chrono::steady_clock::time_point playbackStartTime;
    std::mutex timingMutex;
    double accumulatedMicroseconds;
    double pauseVirtualMicros;
    std::atomic<size_t> eventPos;
    uint32_t lastProcessedTick;
    double microsecondsPerTick;
    std::atomic<uint32_t> currentTempo;
    std::atomic<bool>  tempoOverrideEnabled{false};
    std::atomic<float> tempoOverrideTargetBpm{120.0f};
    std::atomic<uint64_t> loopStartTick{ 0 };
    std::atomic<uint64_t> loopEndTick{ UINT64_MAX };
    std::atomic<bool>     hasLoopPoints{ false };
    uint64_t TickToMicros(uint64_t targetTick) const;
    void     LoopBackToTick(uint64_t loopStart);
    std::atomic<bool> antiSlowdownEnabled{false};
    std::atomic<bool> eventCounterRecordEnabled{true};
    bool activeNotes[16][128] = {};
	
    // Slowdown mode state
    std::atomic<int64_t>      simulateEventsPerSecond{ 0 };
    std::atomic<bool>         simLagActive{ false };
    std::atomic<bool>         simLagSmooth{ false };
    double                    simTokens{ 0.0 };
    std::chrono::steady_clock::time_point simLastRefill;

    mutable std::atomic<uint64_t> m_cachedEps{0};
    mutable std::atomic<int64_t>  m_lastEpsUpdateMs{0};
};

extern MidiOutputEngine g_AudioEngine;