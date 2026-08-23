#pragma once
#include "visualizer.hpp"
#include "midi_timing_alt.hpp"
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>

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
    void ToggleAntiSlowdown(bool enabled);
    bool IsAntiSlowdownEnabled() const;

    // ---------------------------------------------------------------
    // Tempo Override — hold a fixed real-world BPM as the song's own
    // tempo track changes underneath it. Lives in the engine (not the UI)
    // so it's evaluated on the playback thread the instant a TEMPO event
    // is processed, rather than being polled once per rendered frame —
    // which meant it drifted or froze depending on which UI code path
    // happened to run that frame (Options panel open vs. closed, etc.).
    // ---------------------------------------------------------------
    void  SetTempoOverride(bool enabled, float targetBpm);
    bool  IsTempoOverrideEnabled() const;
    float GetTempoOverrideTarget() const;

    // Event counter record (Information only) — gates the EVPS/dispatch
    // stats accumulator, NOT playback advancement. See PlaybackThread():
    // eventPos must always increment regardless of this flag, or the
    // playback loop stalls when recording is disabled.
    void ToggleEventCounterRecord(bool enabled);
    bool IsEventCounterRecordEnabled() const;

    // ---------------------------------------------------------------
    // Lag Simulator — limits MIDI sends to N events/sec (0 = off).
    // Mimics PFA behaviour on a slow machine: dense chord bursts cause
    // the audio thread to fall behind because the token bucket drains
    // faster than it refills, producing authentic timing drift.
    // ---------------------------------------------------------------
    void    SetSimulateEventsPerSecond(int64_t eps); // 0 disables; range [1024, 134217728]
    int64_t GetSimulateEventsPerSecond() const;
    bool    IsSimulateLagActive() const;             // true = currently throttled
	void    SetLagSmoothRender(bool smooth);
    bool    GetLagSmoothRender() const;

	// ── Events-per-second counter (reset each frame by the render thread) ──
	std::atomic<uint64_t> eventsDispatchedCounter{ 0 };
	uint64_t GetAndResetEventCount() {
		return eventsDispatchedCounter.exchange(0, std::memory_order_relaxed);
	}
	
	// ── EVPS sliding-window ring buffer (100 × 10ms = 1 second) ──────────
	static constexpr int kEvpsBuckets = 100;
	struct EvpsBucket {
		std::atomic<int64_t>  startMs{ -1 };  // wall-clock ms, -1 = unused
		std::atomic<uint64_t> count{ 0 };
	};
	EvpsBucket              m_evpsBuckets[kEvpsBuckets];
	std::atomic<int>        m_evpsCurBucket{ 0 };

	void RecordDispatch(uint64_t count = 1); // Modified to accept a batch count
	uint64_t GetEventsPerSecond() const;

private:
    void PlaybackThread();
    void SilenceAllChannels();
    void SilenceAllChannelsWithoutCC();
    void BuildTempoIndex();
    void ApplyTempoOverride(); // recompute+apply playbackSpeed to hold tempoOverrideTargetBpm

    // Built once in Start(). Each entry marks a tempo change point.
    struct TempoSegment {
        size_t   eventIdx;    // index into *eventList of the TEMPO event
        uint32_t tick;        // tick this segment begins at
        double   accumMicros; // virtual microseconds elapsed at segment start (speed=1)
        uint32_t rawTempo;    // microseconds per beat
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
    // Guards {playbackStartTime, playbackSpeed} as one consistent unit.
    // PlaybackThread() reads this pair every loop iteration while SetSpeed(),
    // Pause(), Resume(), Start(), Seek(), and LoopBackToTick() all write it
    // from other threads (e.g. a UI slider firing many SetSpeed() calls per
    // second while dragging). Without this lock the reader can observe a
    // just-updated speed paired with a stale start time (or vice versa),
    // which makes the tick loop think a huge amount of virtual time has
    // passed and dump a burst of queued events in one go.
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
    std::atomic<bool> eventCounterRecordEnabled{true}; // default on: stats work out of the box
	bool activeNotes[16][128] = {};

    // ---- Lag simulator state ------------------------------------------------
    // simulateEventsPerSecond: int64_t so it can hold up to 134 217 728 (2^27)
    // without overflow.  0 = disabled.  UI writes, PlaybackThread reads.
    std::atomic<int64_t> simulateEventsPerSecond{0};
    std::atomic<bool>    simLagActive{false}; // true while token bucket is empty
    // Token bucket — PlaybackThread-exclusive after Start(); no atomic needed:
    double   simTokens{0.0};
    std::chrono::steady_clock::time_point simLastRefill;
	std::atomic<bool> simLagSmooth{false};
	mutable std::atomic<uint64_t> m_cachedEps{0};
    mutable std::atomic<int64_t>  m_lastEpsUpdateMs{0};
};

// ---------------------------------------------------------------
// Global engine instance — defined in visualizer.cpp as:
//     MidiOutputEngine g_AudioEngine;
// Declared here so every TU that includes this header can reach it.
// ---------------------------------------------------------------
extern MidiOutputEngine g_AudioEngine;