#include "visualizer.hpp"
#include "midi_timing_alt.hpp"
#include "midioutput.hpp"
#include "Lagsimulatorpanel.hpp"
#include "build_info.hpp"
#include "smtc_bridge.hpp"
#include "bass_backend.hpp"       // BassMIDI pre-render engine + DispatchMidiOut
#include "AudioConfigPanel.hpp"   // DrawAudioConfigPanel() / ToggleAudioConfigPanel()
#include <fstream>
#include <iostream>
#include <algorithm>
#include <random>
#include <chrono>
#include <map>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <cstring>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <ctime>
#include <cmath>
#include <queue>
#include <deque>
#include <tuple>
#include "raylib.h"
#include "reasings.h"
#include "icon_loader.hpp"
#include "rlgl.h"
#include "rlImGui.h"   // raylib-extras/rlImGui
#include "imgui.h"     // bundled with rlImGui

// ===================================================================
// PLATFORM AND EXTERNAL DEFS
// ===================================================================
#ifdef _WIN32
inline uint32_t ntohl(uint32_t n) { return ((n & 0xFF000000) >> 24) | ((n & 0x00FF0000) >> 8) | ((n & 0x0000FF00) << 8) | ((n & 0x000000FF) << 24); }
inline uint16_t ntohs(uint16_t n) { return ((n & 0xFF00) >> 8) | ((n & 0x00FF) << 8); }
#else
#include <arpa/inet.h>
#endif
extern "C" {
    bool InitializeKDMAPIStream();
    void TerminateKDMAPIStream();
    void SendDirectData(unsigned long data);
}
extern "C" {
    struct _PROCESS_MEMORY_COUNTERS_EX {
        unsigned long cb;
        unsigned long PageFaultCount;
        size_t PeakWorkingSetSize;
        size_t WorkingSetSize;
        size_t QuotaPeakPagedPoolUsage;
        size_t QuotaPagedPoolUsage;
        size_t QuotaPeakNonPagedPoolUsage;
        size_t QuotaNonPagedPoolUsage;
        size_t PagefileUsage;
        size_t PeakPagefileUsage;
        size_t PrivateUsage;
    };
    typedef _PROCESS_MEMORY_COUNTERS_EX PROCESS_MEMORY_COUNTERS_EX;

    __declspec(dllimport) int __stdcall GetProcessMemoryInfo(void* Process, PROCESS_MEMORY_COUNTERS_EX* ppsmemCounters, unsigned long cb);
    __declspec(dllimport) void* __stdcall GetCurrentProcess();
}
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
struct MemoryUsage {
    uint64_t workingSetMB;
    uint64_t privateUsageMB;
};
MemoryUsage GetMemoryUsage() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    MemoryUsage result{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        result.workingSetMB   = pmc.WorkingSetSize / (1024 * 1024);
        result.privateUsageMB = pmc.PrivateUsage   / (1024 * 1024);
    }
    return result;
}
MemoryUsage MidiLoadUsage;
MemoryUsage IndexLoadUsage;
MemoryUsage TotalLoadUsage;
MidiOutputEngine g_AudioEngine;
// Global state variables (static keyword removed)
bool showGuide = true; // Toggle for guide
bool showBeats = true; // Toggle for beats
bool showDebug = false; // Toggle for debug
bool showPerformance = false; // Toggle for Performance
bool showOptions = false;
bool g_enableOverlapRemove = false; // Toggle for render overlap removal
ViewerType g_viewerType = ViewerType::TickLayer; // T key toggles
static bool firstPause = true; // Loads do first pause (kept static since it is only used here)
static AppState currentState = STATE_MENU;
static std::string selectedMidiFile = "Empty"; 
float ScrollSpeed = 0.5f;
float MidiSpeed = 1.00f;
int cursorPos = 0;
std::atomic<uint64_t> renderNotes{0};
std::atomic<uint64_t> maxRenderNotes{0};
bool isHUD = true;

// Custom background color (RGBA, normalized [0,1] for ImGui; converted to raylib Color on use)
float g_bgColorF[4] = { 0.031f, 0.031f, 0.031f, 1.0f }; // default: black
Color g_backgroundColor = { 8, 8, 8, 255 };

// ── Background Particle System ──────────────────────────
struct BgParticle { float x, y, speedFactor, sizeFactor; };
bool                    g_particleShow    = true;
int                     g_particleCount   = 120;
float                   g_particleSpeed   = 120.0f;  // px/sec at 120 BPM
bool                    g_particleBpm     = true;    // scale speed with live BPM
float                   g_particleSize    = 2.0f;
float                   g_particleColorF[4] = { 1.0f, 1.0f, 1.0f, 0.25f };
Color                   g_particleColor   = { 255, 255, 255, 64 };
static std::vector<BgParticle> g_particles; // internal tracking remains static

// ── Background Image ─────────────────────────────────────────────────
bool        g_bgImageShow   = false;
Texture2D   g_bgImageTex    = { 0 };
char        g_bgImagePath[512] = "";
float       g_bgImageTintF[4]  = { 1.0f, 1.0f, 1.0f, 1.0f };
Color       g_bgImageTint      = { 255, 255, 255, 255 };
BgImageFit  g_bgImageFit       = BgImageFit::Fill;

bool inputActive = false;
bool isLoop = false;
bool isAntiSlowdown = false;

// Global definition of lag simulation EPS
int64_t s_lagSimEps = 65536;

// Loop A/B points (UINT64_MAX = not set)
static uint64_t g_loopPointA    = UINT64_MAX;
static uint64_t g_loopPointB    = UINT64_MAX;
// Beat-snap for A/B: when enabled, J/K/Set-A/Set-B snap to the beat grid
static bool g_loopSnapToBeats   = true;
static int  g_loopBeatOffsetA   = 0;   // beat offset applied when setting A  (can be negative)
static int  g_loopBeatOffsetB   = 0;   // beat offset applied when setting B
// Convert a raw tick to a beat-snapped tick + integer beat offset
// ppqIn = pulses-per-quarter-note; offsetBeats is added AFTER snapping to beat
static uint64_t LoopSnapToBeat(uint64_t tick, int offsetBeats, uint16_t ppqIn,
                                uint16_t denominator)
{
    if (ppqIn == 0) return tick;
    uint64_t tpb = (static_cast<uint64_t>(ppqIn) * 4u) / (denominator ? denominator : 4u);
    if (tpb == 0) tpb = ppqIn;
    // Round to nearest beat boundary (instead of floor, for better feel)
    uint64_t beat    = (tick + tpb / 2) / tpb;
    int64_t  target  = static_cast<int64_t>(beat) + offsetBeats;
    if (target < 0) target = 0;
    return static_cast<uint64_t>(target) * tpb;
}
std::string inputBuffer;
uint16_t ppq = 0;
uint16_t timeSigNumerator   = 4; // overwritten from MIDI meta 0x58 at load time
uint16_t timeSigDenominator = 4; // overwritten from MIDI meta 0x58 at load time
static int beatSubdivisions = 4;
uint64_t ticksPerBeat = (ppq * 4) / timeSigDenominator;
uint64_t ticksPerMeasure = 0;
float DWidth = 300.0f, DHeight = 125.0f;
uint64_t noteCounter = 0, noteTotal = 0;
static LoadProgress g_LoadProgress;
static std::thread g_LoaderThread;
static std::vector<CCEvent> g_loadedCCEvents;
static std::vector<uint32_t> g_sortedNoteStartTicks;
static std::vector<uint32_t> g_sortedNoteEndTicks;   // parallel sorted end-ticks for polyphony
static uint32_t g_songLastTick = 0;    // max endTick across all notes — used for duration
extern uint32_t g_totalTicks;
static double   g_songDurationSec = 0.0; // total song duration in seconds (computed once at load)

// Tempo segment table for O(log M) tick→seconds. Built once at load time.
struct VisualizerTempoSeg {
    uint32_t tick;
    double   accumSec;
    double   usPerTick;
};
static std::vector<VisualizerTempoSeg> g_tempoSegs;
static uint64_t g_currentNps  = 0;    // NPS at current tick (updated each frame)
static uint64_t g_maxNps      = 0;    // peak NPS seen so far this file

// Bottom progress bar: 20-cell NPS grid baked once at load time
static constexpr int   kNpsCellPx    = 10;       // fixed cell width in pixels
static int             g_npsGridCells = 0;        // computed from bar width at load/resize
static std::vector<float> g_npsGrid;              // normalized 0..1 per cell, dynamic size
static bool            g_npsGridReady = false;
static int             g_npsGridBuiltWidth = 0;   // bar width used when grid was last built
static uint64_t g_currentPoly = 0;    // polyphony at current tick
static uint64_t g_maxPoly     = 0;    // peak polyphony seen so far this file

// ===================================================================
// CHUNK SLIDING-WINDOW RENDERER variables (Synchronized & Thread-Safe)
// ===================================================================

static std::atomic<bool>  g_seekInvalidate{ false };
static constexpr int      PIX_H     = 128;
static constexpr int      N_CHUNKS  = 4;   // 1 current + 3 ahead

static Texture2D             g_tex        = { 0 };
static int                   g_texW       = 0;   // = N_CHUNKS * screenWidth
static int                   g_chunkW     = 0;   // = screenWidth
static double                g_pixPerTick = 0.0;
static uint32_t              g_bufOriginTick = 0;  // tick at pixel col 0
static std::vector<uint32_t> g_pixBuf;             // [PIX_H rows][g_texW cols]

static bool     g_chunkPainted[N_CHUNKS]    = {};
static uint32_t g_chunkOriginTick[N_CHUNKS] = {};  

// Background thread paints one chunk at a time using immutable bounds
struct ChunkJob { 
    int chunkIdx; 
    uint32_t tickStart; 
    uint32_t tickEnd; 
};

static std::thread              g_paintThread;
static std::atomic<bool>        g_paintStop{ false };
static std::mutex               g_paintMtx;
static std::condition_variable  g_paintCV;
static std::queue<ChunkJob>     g_paintQueue;
static std::atomic<int>         g_lastPaintedChunk{ -1 };

// Guards raw g_pixBuf memory itself (writes from the background paint
// thread vs. reads from the main thread when uploading to the GPU texture).
// g_paintMtx above only protects the job queue, NOT the pixel data, so it
// is not sufficient on its own to prevent a torn read of g_pixBuf.
static std::mutex               g_pixBufMtx;

static const std::vector<OptimizedTrackData>* g_tracks      = nullptr;
static ViewerType                              g_bgViewerType = ViewerType::TrackLayer;

static bool     g_rtNeedsFullRedraw = true;
static uint32_t g_ticksPerChunk     = 0;  
static double   g_ticksPerChunkExact = 0.0; 

static inline uint32_t ExactChunkOrigin(uint32_t bufOrigin, int c) {
    return bufOrigin + (uint32_t)std::round((double)c * g_ticksPerChunkExact);
}

// Contiguous chunk boundaries helper functions
static inline uint32_t GetChunkStartTick(int c, uint32_t bufOrigin, uint64_t windowOffset) {
    return ExactChunkOrigin(bufOrigin, windowOffset + c);
}

static inline uint32_t GetChunkEndTick(int c, uint32_t bufOrigin, uint64_t windowOffset) {
    if (c < N_CHUNKS - 1) {
        return ExactChunkOrigin(bufOrigin, windowOffset + c + 1);
    }
    return ExactChunkOrigin(bufOrigin, windowOffset + c) + g_ticksPerChunk;
}

static std::atomic<bool> g_paintBusy{ false };
static std::atomic<bool> g_paintCancel{ false };

// ---- helpers ---------------------------------------------------------------
static inline uint32_t ToRGBA8(Color c) {
    return (uint32_t)c.r | ((uint32_t)c.g << 8) | ((uint32_t)c.b << 16) | (0xFFu << 24);
}
inline Color GetTrackColorPFA(int track, int channel);

// ===================================================================
// FIX: Sliding Window Smooth Scroll & TickLayer Tracking Colors
// Replace everything from "PaintChunkRange" exactly down to the end of
// "DrawStreamingVisualizerNotes".
// ===================================================================

static uint64_t g_windowOffsetChunks = 0;

struct RowSpan {
    int x0, x1;
    uint32_t rgba;
};

// ===================================================================
// SEAMLESS CONTIGUOUS RENDERING & LOD OCCLUSION RASTERIZER
// ===================================================================

struct ReverseCursor {
    std::vector<NoteEvent>::const_iterator cur;
    std::vector<NoteEvent>::const_iterator begin;
    size_t trackIdx;

    // For max-heap (largest startTick at the top)
    bool operator<(const ReverseCursor& other) const {
        return cur->startTick < other.cur->startTick;
    }
};

struct ForwardCursor {
    std::vector<NoteEvent>::const_iterator cur;
    std::vector<NoteEvent>::const_iterator end;
    size_t trackIdx;

    // For min-heap (smallest startTick at the top)
    bool operator<(const ForwardCursor& other) const {
        return cur->startTick > other.cur->startTick; // Inverted for min-heap
    }
};

static void PaintChunkRange(int chunkIdx, uint32_t tickStart, uint32_t tickEnd)
{
    if (!g_tracks || g_texW == 0 || tickEnd <= tickStart) return;
    const int    W    = g_chunkW;
    const int    base = chunkIdx * W;
    const double ppt  = g_pixPerTick;

    // 1. Clear chunk memory segment
    for (int y = 0; y < PIX_H; ++y)
        std::memset(&g_pixBuf[(size_t)y * g_texW + base], 0, (size_t)W * sizeof(uint32_t));

    // 2. Pre-allocate flat rows (thread-local to avoid runtime reallocation)
    thread_local std::vector<uint32_t> rowColors[128];
    for (int y = 0; y < 128; ++y) {
        rowColors[y].assign(W, 0u);
    }

    uint64_t count = 0;
    int cancelCheckCounter = 0;

    if (g_bgViewerType == ViewerType::TickLayer) {
        if (g_enableOverlapRemove) {
            // ---- TICK LAYER (Overlap Remove Enabled): Heap-Optimized Reverse K-Way Merge ----
            thread_local std::vector<ReverseCursor> heap;
            heap.clear();

            for (size_t t = 0; t < g_tracks->size(); ++t) {
                const auto& track = (*g_tracks)[t];
                if (track.notes.empty()) continue;

                auto ri_start = std::lower_bound(track.notes.begin(), track.notes.end(), tickStart,
                    [](const NoteEvent& n, uint32_t v){ return n.startTick < v; });

                auto ri = ri_start;
                int backLimit = 0;
                while (ri != track.notes.begin() && backLimit < 2000) {
                    --ri; backLimit++;
                    if (ri->endTick <= tickStart) { ++ri; break; }
                }
                ri_start = ri;

                auto ri_end = std::lower_bound(ri_start, track.notes.end(), tickEnd,
                    [](const NoteEvent& n, uint32_t v){ return n.startTick < v; });

                if (ri_start != ri_end) {
                    heap.push_back({ ri_end - 1, ri_start, t });
                }
            }

            std::make_heap(heap.begin(), heap.end());

            while (!heap.empty()) {
                if (++cancelCheckCounter % 8192 == 0) {
                    if (g_paintCancel.load(std::memory_order_relaxed)) return;
                }

                // Extract track cursor with the largest startTick
                std::pop_heap(heap.begin(), heap.end());
                ReverseCursor top = heap.back();
                heap.pop_back();

                const NoteEvent& n = *top.cur;
                size_t t = top.trackIdx;

                uint32_t rawEnd = (n.endTick > n.startTick) ? n.endTick : n.startTick + 1;
                uint32_t ds = (n.startTick > tickStart) ? n.startTick : tickStart;
                uint32_t de = (rawEnd < tickEnd)        ? rawEnd      : tickEnd;

                if (ds < de) {
                    int px0 = (int)((double)(ds - tickStart) * ppt);
                    int px1 = (int)((double)(de - tickStart) * ppt);
                    if (px1 > px0 + 1) px1--;
                    if (px0 < 0)  px0 = 0;
                    if (px1 > W)  px1 = W;

                    if (px0 < px1) {
                        int y = (PIX_H - 1) - (int)n.note;
                        if ((unsigned)y < (unsigned)PIX_H) {
                            Color col = GetTrackColorPFA((int)t, n.channel);
                            uint32_t rgba = ToRGBA8(col);

                            // Reverse Occlusion: write only if pixel is empty
                            for (int px = px0; px < px1; ++px) {
                                if (rowColors[y][px] == 0) {
                                    rowColors[y][px] = rgba;
                                }
                            }
                            count++;
                        }
                    }
                }

                if (top.cur != top.begin) {
                    top.cur--;
                    heap.push_back(top);
                    std::push_heap(heap.begin(), heap.end());
                }
            }
        }
        else {
            // ---- TICK LAYER (Overlap Remove Disabled): Heap-Optimized Forward K-Way Merge ----
            thread_local std::vector<ForwardCursor> heapF;
            heapF.clear();

            for (size_t t = 0; t < g_tracks->size(); ++t) {
                const auto& track = (*g_tracks)[t];
                if (track.notes.empty()) continue;

                auto ri_start = std::lower_bound(track.notes.begin(), track.notes.end(), tickStart,
                    [](const NoteEvent& n, uint32_t v){ return n.startTick < v; });

                auto ri = ri_start;
                int backLimit = 0;
                while (ri != track.notes.begin() && backLimit < 2000) {
                    --ri; backLimit++;
                    if (ri->endTick <= tickStart) { ++ri; break; }
                }
                ri_start = ri;

                auto ri_end = std::lower_bound(ri_start, track.notes.end(), tickEnd,
                    [](const NoteEvent& n, uint32_t v){ return n.startTick < v; });

                if (ri_start != ri_end) {
                    heapF.push_back({ ri_start, ri_end, t });
                }
            }

            std::make_heap(heapF.begin(), heapF.end());

            while (!heapF.empty()) {
                if (++cancelCheckCounter % 8192 == 0) {
                    if (g_paintCancel.load(std::memory_order_relaxed)) return;
                }

                // Extract track cursor with the smallest startTick
                std::pop_heap(heapF.begin(), heapF.end());
                ForwardCursor top = heapF.back();
                heapF.pop_back();

                const NoteEvent& n = *top.cur;
                size_t t = top.trackIdx;

                uint32_t rawEnd = (n.endTick > n.startTick) ? n.endTick : n.startTick + 1;
                uint32_t ds = (n.startTick > tickStart) ? n.startTick : tickStart;
                uint32_t de = (rawEnd < tickEnd)        ? rawEnd      : tickEnd;

                if (ds < de) {
                    int px0 = (int)((double)(ds - tickStart) * ppt);
                    int px1 = (int)((double)(de - tickStart) * ppt);
                    if (px1 > px0 + 1) px1--;
                    if (px0 < 0)  px0 = 0;
                    if (px1 > W)  px1 = W;

                    if (px0 < px1) {
                        int y = (PIX_H - 1) - (int)n.note;
                        if ((unsigned)y < (unsigned)PIX_H) {
                            Color col = GetTrackColorPFA((int)t, n.channel);
                            uint32_t rgba = ToRGBA8(col);

                            std::fill_n(rowColors[y].data() + px0, px1 - px0, rgba);
                            count++;
                        }
                    }
                }

                top.cur++;
                if (top.cur != top.end) {
                    heapF.push_back(top);
                    std::push_heap(heapF.begin(), heapF.end());
                }
            }
        }
    }
    else {
        // ---- TRACK LAYER: Track-Priority Layering (Domino Style) ----
        for (size_t t = 0; t < g_tracks->size(); ++t) {
            if (g_paintCancel.load(std::memory_order_relaxed)) return;
            const auto& track = (*g_tracks)[t];
            if (track.notes.empty()) continue;

            auto ri_start = std::lower_bound(track.notes.begin(), track.notes.end(), tickStart,
                [](const NoteEvent& n, uint32_t v){ return n.startTick < v; });

            auto ri = ri_start;
            int backLimit = 0;
            while (ri != track.notes.begin() && backLimit < 2000) {
                --ri; backLimit++;
                if (ri->endTick <= tickStart) { ++ri; break; }
            }
            ri_start = ri;

            auto ri_end = std::lower_bound(ri_start, track.notes.end(), tickEnd,
                [](const NoteEvent& n, uint32_t v){ return n.startTick < v; });

            for (auto it = ri_start; it != ri_end; ++it) {
                if (++cancelCheckCounter % 8192 == 0) {
                    if (g_paintCancel.load(std::memory_order_relaxed)) return;
                }

                const NoteEvent& n = *it;
                uint32_t rawEnd = (n.endTick > n.startTick) ? n.endTick : n.startTick + 1;
                uint32_t ds = (n.startTick > tickStart) ? n.startTick : tickStart;
                uint32_t de = (rawEnd < tickEnd)        ? rawEnd      : tickEnd;
                if (ds >= de) continue;

                int px0 = (int)((double)(ds - tickStart) * ppt);
                int px1 = (int)((double)(de - tickStart) * ppt);
                if (px1 > px0 + 1) px1--;
                if (px0 < 0)  px0 = 0;
                if (px1 > W)  px1 = W;
                if (px0 >= px1) continue;

                int y = (PIX_H - 1) - (int)n.note;
                if ((unsigned)y >= (unsigned)PIX_H) continue;

                Color col = GetTrackColorPFA((int)t, 0); 
                uint32_t rgba = ToRGBA8(col);

                std::fill_n(rowColors[y].data() + px0, px1 - px0, rgba);
                count++;
            }
        }
    }

    // 3. Thread-safe copy to active global pixel buffer
    {
        std::lock_guard<std::mutex> lk(g_pixBufMtx);
        for (int y = 0; y < PIX_H; ++y) {
            uint32_t* dst = g_pixBuf.data() + (size_t)y * g_texW + base;
            std::memcpy(dst, rowColors[y].data(), W * sizeof(uint32_t));
        }
    }

    // 4. Thread-safe update of atomic counters
    renderNotes.store(count, std::memory_order_relaxed);
    uint64_t currentMax = maxRenderNotes.load(std::memory_order_relaxed);
    while (count > currentMax && !maxRenderNotes.compare_exchange_weak(currentMax, count, std::memory_order_relaxed));
}

static void EnqueueChunk(int chunkIdx, uint32_t tickStart, uint32_t tickEnd, bool clearQueue = false)
{
    std::lock_guard<std::mutex> lk(g_paintMtx);
    if (clearQueue) {
        while (!g_paintQueue.empty()) g_paintQueue.pop();
    }
    g_paintQueue.push({ chunkIdx, tickStart, tickEnd });
    g_paintCV.notify_one();
}

static void BgPaintThreadFunc()
{
    while (!g_paintStop.load(std::memory_order_relaxed)) {
        ChunkJob job;
        {
            std::unique_lock<std::mutex> lk(g_paintMtx);
            g_paintCV.wait(lk, []{ return !g_paintQueue.empty() || g_paintStop.load(); });
            if (g_paintStop.load()) break;
            job = g_paintQueue.front();
            g_paintQueue.pop();
        }
        
        g_paintBusy.store(true, std::memory_order_seq_cst);
        g_paintCancel.store(false, std::memory_order_seq_cst);

        PaintChunkRange(job.chunkIdx, job.tickStart, job.tickEnd);

        if (!g_paintCancel.load(std::memory_order_acquire)) {
            g_chunkPainted[job.chunkIdx] = true;
            g_lastPaintedChunk.store(job.chunkIdx, std::memory_order_release);
        }
        g_paintBusy.store(false, std::memory_order_release);
    }
}

// ===================================================================
// SCROLL VISUALIZER — thread-safe sliding window
// ===================================================================

void UpdateBuffers(uint64_t currentTick) {}

void StartNoteRenderThread() {
    g_rtNeedsFullRedraw = true;
    g_paintStop.store(false);
    g_paintBusy.store(false);
    g_paintCancel.store(false);
    if (g_paintThread.joinable()) g_paintThread.join();
    g_paintThread = std::thread(BgPaintThreadFunc);
}

void StopNoteRenderThread() {
    g_paintCancel.store(true);
    g_paintStop.store(true);
    g_paintCV.notify_all();
    if (g_paintThread.joinable()) g_paintThread.join();
    g_paintBusy.store(false);
    if (g_tex.id != 0) { UnloadTexture(g_tex); g_tex = {0}; }
    g_texW = 0; g_chunkW = 0;
    g_pixBuf.clear(); g_pixBuf.shrink_to_fit();
    g_tracks = nullptr;
}

void DrawStreamingVisualizerNotes(
    const std::vector<OptimizedTrackData>& tracks,
    uint64_t currentTick, int ppq, uint32_t currentTempo,
    ViewerType viewerType)
{
    const int   sw = GetScreenWidth();
    const int   sh = GetScreenHeight();
    const float top = 30.f, bot = 30.f;
    const float uh = (float)sh - top - bot;

    ticksPerBeat = (ppq * 4) / timeSigDenominator;
    ticksPerMeasure = (ppq * 4 * timeSigNumerator) / timeSigDenominator;

    double uspt = MidiTiming::CalculateMicrosecondsPerTick(
        MidiTiming::DEFAULT_TEMPO_MICROSECONDS, ppq);
    const uint32_t viewWindow = std::max(1U,
        static_cast<uint32_t>((ScrollSpeed * 1500000.0) / uspt));
    const float  plx = (float)sw * 0.5f;
    const double ppt = (double)(sw - plx) / (double)viewWindow;

    // --- Strict Hardware Texture Cap Implementation ---
    constexpr int MAX_GPU_TEXTURE_WIDTH = 16384;
    int newChunkW = sw * 2;
    if (newChunkW * N_CHUNKS > MAX_GPU_TEXTURE_WIDTH) {
        newChunkW = MAX_GPU_TEXTURE_WIDTH / N_CHUNKS; // Clamps chunk to 4096 px (16384 px total)
    }
    const int newTexW = N_CHUNKS * newChunkW;

    // Calculate scale ratio between internal texture width and screen pixel width
    const float texScale = (float)newChunkW / (float)(sw * 2);
    const double scaledPpt = ppt * (double)texScale;
    const uint64_t newTicksPerChunk = (uint64_t)((double)newChunkW / scaledPpt) + 1;

    bool changed = (g_tex.id == 0 || g_texW != newTexW ||
        std::fabs(g_pixPerTick - scaledPpt) > 1e-9);
    if (changed) {
        if (g_tex.id != 0) UnloadTexture(g_tex);
        g_chunkW = newChunkW;
        g_texW = newTexW;
        g_pixPerTick = scaledPpt;
        g_ticksPerChunk = newTicksPerChunk;
        g_ticksPerChunkExact = (double)newChunkW / scaledPpt;
        g_pixBuf.assign((size_t)newTexW * PIX_H, 0u);
        Image img = {};
        img.data = g_pixBuf.data();
        img.width = newTexW;
        img.height = PIX_H;
        img.mipmaps = 1;
        img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        g_tex = LoadTextureFromImage(img);
        SetTextureFilter(g_tex, TEXTURE_FILTER_POINT);
        for (int i = 0; i < N_CHUNKS; ++i) g_chunkPainted[i] = false;
        g_rtNeedsFullRedraw = true;
    }

    g_tracks = &tracks;
    g_bgViewerType = viewerType;

    // Visible tick window boundaries
    int64_t  sLeft = (int64_t)currentTick - (int64_t)(plx / ppt);
    int64_t  sRight = (int64_t)currentTick + (int64_t)((sw - plx) / ppt) + 1;
    uint64_t leftTick = (uint64_t)std::max((int64_t)0, sLeft);

    bool seeked = g_seekInvalidate.exchange(false);

    // ---- On seek or init: CONTIGUOUS SEAMLESS RENDERING (100% Asynchronous) ----
    if (seeked || g_rtNeedsFullRedraw) {
        g_rtNeedsFullRedraw = false;

        {
            std::lock_guard<std::mutex> lk(g_paintMtx);
            while (!g_paintQueue.empty()) g_paintQueue.pop();
        }
        g_paintCancel.store(true, std::memory_order_release);

        // Aborts background worker rapidly due to micro-interval cancel checks
        while (g_paintBusy.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        g_windowOffsetChunks = 0;
        g_bufOriginTick = (leftTick / g_ticksPerChunk) * g_ticksPerChunk;
        for (int i = 0; i < N_CHUNKS; ++i) {
            g_chunkPainted[i] = false;
            g_chunkOriginTick[i] = ExactChunkOrigin(g_bufOriginTick, i);
        }
        int visibleChunk = 0;
        for (int c = 0; c < N_CHUNKS; ++c) {
            if (g_chunkOriginTick[c] <= leftTick) visibleChunk = c;
            else break;
        }

        g_paintCancel.store(false, std::memory_order_release);

        std::memset(g_pixBuf.data(), 0, g_pixBuf.size() * sizeof(uint32_t));
        UpdateTexture(g_tex, g_pixBuf.data());

        // Enqueue visible and future look-ahead chunks asynchronously
        for (int nc = visibleChunk; nc < N_CHUNKS; ++nc) {
            uint32_t cts = GetChunkStartTick(nc, g_bufOriginTick, 0);
            uint32_t cte = GetChunkEndTick(nc, g_bufOriginTick, 0);
            EnqueueChunk(nc, cts, cte, false);
        }
    }
    else {
        int lp = g_lastPaintedChunk.exchange(-1, std::memory_order_acquire);
        if (lp >= 0) {
            std::lock_guard<std::mutex> lk(g_pixBufMtx);
            UpdateTexture(g_tex, g_pixBuf.data());
        }

        // SLIDING WINDOW SHIFT - Delay shift until the next buffer is fully painted
        if (leftTick >= g_chunkOriginTick[1] && g_chunkPainted[1]) {
            {
                std::lock_guard<std::mutex> lk(g_paintMtx);
                while (!g_paintQueue.empty()) g_paintQueue.pop();
            }
            g_paintCancel.store(true, std::memory_order_release);
            while (g_paintBusy.load(std::memory_order_acquire)) { std::this_thread::yield(); }

            for (int y = 0; y < PIX_H; ++y) {
                uint32_t* row = g_pixBuf.data() + (size_t)y * g_texW;
                std::memmove(row, row + g_chunkW, (g_texW - g_chunkW) * sizeof(uint32_t));
                std::memset(row + (g_texW - g_chunkW), 0, g_chunkW * sizeof(uint32_t));
            }

            g_windowOffsetChunks++;
            for (int i = 0; i < N_CHUNKS - 1; ++i) {
                g_chunkOriginTick[i] = g_chunkOriginTick[i + 1];
                g_chunkPainted[i] = g_chunkPainted[i + 1];
            }
            g_chunkOriginTick[N_CHUNKS - 1] = ExactChunkOrigin(g_bufOriginTick, g_windowOffsetChunks + N_CHUNKS - 1);
            g_chunkPainted[N_CHUNKS - 1] = false;

            UpdateTexture(g_tex, g_pixBuf.data());

            g_paintCancel.store(false, std::memory_order_release);
            for (int c = 0; c < N_CHUNKS; ++c) {
                if (!g_chunkPainted[c]) {
                    uint32_t ts = GetChunkStartTick(c, g_bufOriginTick, g_windowOffsetChunks);
                    uint32_t te = GetChunkEndTick(c, g_bufOriginTick, g_windowOffsetChunks);
                    EnqueueChunk(c, ts, te, false);
                }
            }
        }
        else if (leftTick < g_chunkOriginTick[1]) {
            // Panic Reset
            uint64_t bufEnd = g_chunkOriginTick[N_CHUNKS - 1] + g_ticksPerChunk;
            bool fellBehind = (leftTick < g_chunkOriginTick[0]) || ((uint64_t)sRight > bufEnd);
            if (fellBehind) {
                {
                    std::lock_guard<std::mutex> lk(g_paintMtx);
                    while (!g_paintQueue.empty()) g_paintQueue.pop();
                }
                g_paintCancel.store(true, std::memory_order_release);
                while (g_paintBusy.load(std::memory_order_acquire)) { std::this_thread::yield(); }

                g_windowOffsetChunks = 0;
                g_bufOriginTick = (leftTick / g_ticksPerChunk) * g_ticksPerChunk;
                for (int i = 0; i < N_CHUNKS; ++i) {
                    g_chunkPainted[i] = false;
                    g_chunkOriginTick[i] = ExactChunkOrigin(g_bufOriginTick, i);
                }
                int vc = 0;
                for (int c = 0; c < N_CHUNKS; ++c) {
                    if (g_chunkOriginTick[c] <= leftTick) vc = c;
                    else break;
                }

                g_paintCancel.store(false, std::memory_order_release);
                
                std::memset(g_pixBuf.data(), 0, g_pixBuf.size() * sizeof(uint32_t));
                UpdateTexture(g_tex, g_pixBuf.data());

                for (int nc = vc; nc < N_CHUNKS; ++nc) {
                    uint32_t ts = GetChunkStartTick(nc, g_bufOriginTick, 0);
                    uint32_t te = GetChunkEndTick(nc, g_bufOriginTick, 0);
                    EnqueueChunk(nc, ts, te, false);
                }
            }
        }
    }

    // ---- Blit ----
    {
        float dstX = (sLeft < 0) ? (float)(-(double)sLeft * ppt) : 0.f;
        float blitW = (float)sw - dstX;
        if (blitW > 0.f) {
            int64_t viewLeftTick = (sLeft >= 0) ? sLeft : 0;
            int64_t viewRightTick = sRight;

            // Determine visible chunk
            int viewChunk = 0;
            for (int c = N_CHUNKS - 1; c >= 0; --c) {
                if ((int64_t)g_chunkOriginTick[c] <= viewLeftTick) {
                    viewChunk = c; 
                    break;
                }
            }

            // Calculate active scaling factor for texture coordinates
            float activeTexScale = (float)g_chunkW / (float)(sw * 2);

            double intraChunkPx = (double)(viewLeftTick - (int64_t)g_chunkOriginTick[viewChunk]) * g_pixPerTick;
            float srcX = (float)(viewChunk * g_chunkW) + (float)intraChunkPx;
            if (srcX < 0.f) srcX = 0.f;

            int rightChunk = viewChunk;
            if (viewChunk + 1 < N_CHUNKS) {
                if (viewRightTick > (int64_t)g_chunkOriginTick[viewChunk + 1]) {
                    rightChunk = viewChunk + 1;
                }
            }

            if (rightChunk == viewChunk) {
                float screenW = blitW;
                float texSrcW = screenW * activeTexScale;
                if (srcX + texSrcW > (float)g_texW) {
                    texSrcW = (float)g_texW - srcX;
                    screenW = texSrcW / activeTexScale;
                }
                if (texSrcW > 0.f && screenW > 0.f) {
                    DrawTexturePro(g_tex, { srcX, 0.f, texSrcW, (float)PIX_H }, { dstX, top, screenW, uh }, { 0,0 }, 0.f, WHITE);
                }
            }
            else {
                float chunk1EndSrcX = (float)((viewChunk + 1) * g_chunkW);
                float texW1 = chunk1EndSrcX - srcX;
                float screenW1 = texW1 / activeTexScale;
                if (screenW1 > blitW) {
                    screenW1 = blitW;
                    texW1 = screenW1 * activeTexScale;
                }
                if (texW1 > 0.f && screenW1 > 0.f) {
                    DrawTexturePro(g_tex, { srcX, 0.f, texW1, (float)PIX_H }, { dstX, top, screenW1, uh }, { 0,0 }, 0.f, WHITE);
                }

                float srcX2 = (float)(rightChunk * g_chunkW);
                float screenW2 = blitW - screenW1;
                float texW2 = screenW2 * activeTexScale;
                if (srcX2 + texW2 > (float)g_texW) {
                    texW2 = (float)g_texW - srcX2;
                    screenW2 = texW2 / activeTexScale;
                }
                if (texW2 > 0.f && screenW2 > 0.f) {
                    DrawTexturePro(g_tex, { srcX2, 0.f, texW2, (float)PIX_H }, { dstX + screenW1, top, screenW2, uh }, { 0,0 }, 0.f, WHITE);
                }
            }
        }
    }

    // ---- Beat lines ----
    if (showBeats) {
        uint64_t tpm = (ppq * 4 * timeSigNumerator) / timeSigDenominator;
        uint64_t tpb = (ppq * 4) / timeSigDenominator;
        uint64_t le = (uint64_t)std::max((int64_t)0, sLeft);
        uint64_t fm = (le / tpm) * tpm;
        for (uint64_t mTick = fm; mTick <= (uint64_t)sRight; mTick += tpm) {
            for (int i = 0; i < timeSigNumerator; ++i) {
                uint64_t bTick = mTick + (uint64_t)(i * tpb);
                if (bTick < le) continue;
                float bx = plx + (float)((int64_t)bTick - (int64_t)currentTick) * (float)ppt;
                if (bx < -1.f || bx > sw + 1.f) continue;
                Color c = (i == 0) ? Color{ 255,255,192,40 } : Color{ 255,255,255,20 };
                DrawRectangleRec({ bx, top, 1.f, uh }, c);
            }
        }
    }

    // ---- Guide lines ----
    if (showGuide) {
        const uint8_t keys[] = { 0,12,24,36,48,60,72,84,96,108,120 };
        for (uint8_t key : keys) {
            float ny = (float)sh - bot - ((float)key / 128.f) * uh;
            if (ny < top || ny > sh - bot) continue;
            Color lc = (key == 60) ? Color{ 255,255,128,64 } : Color{ 128,128,128,64 };
            DrawLine(0, (int)ny, sw, (int)ny, lc);
            if (key == 60) DrawText("C4", 5, (int)ny - 10, 10, Color{ 255,255,128,192 });
            else DrawText(TextFormat("C%d", (key / 12) - 1), 5, (int)ny - 10, 10, Color{ 255,255,255,128 });
        }
    }

    // ---- Playhead + borders ----
    DrawLine((int)plx, (int)top, (int)plx, sh - (int)bot, Color{ 255,128,128,192 });
    DrawLine(0, (int)top, sw, (int)top, Color{ 255,255,255,40 });
    DrawLine(0, sh - (int)bot, sw, sh - (int)bot, Color{ 255,255,255,40 });
}

std::string FormatWithCommas(uint64_t value) {
    // Build the string right-to-left into a fixed buffer to avoid repeated insertions
    char buf[32];
    int pos = 31;
    buf[pos] = '\0';
    uint64_t v = value;
    int digits = 0;
    do {
        if (digits && digits % 3 == 0) buf[--pos] = ',';
        buf[--pos] = '0' + (char)(v % 10);
        v /= 10;
        digits++;
    } while (v);
    return std::string(buf + pos);
}

// ===================================================================
// EASING FUNCTIONS IMPLEMENTATION
// ===================================================================
float EaseInBack(float t) {
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    return c3 * t * t * t - c1 * t * t;
}
float EaseOutBack(float t) {
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    return 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
}

// ===================================================================
// NOTIFICATION SYSTEM IMPLEMENTATION
// ===================================================================
NotificationManager g_NotificationManager;
Notification::Notification(const std::string& txt, Color bgColor, float w, float h, float dur)
    : text(txt), backgroundColor(bgColor), width(w), height(h), duration(dur),
      targetY(0), currentY(-h), isVisible(true), isDismissing(false) {
    startTime = std::chrono::steady_clock::now();
    dismissTime = startTime + std::chrono::milliseconds(static_cast<int>(dur * 1000));
}
void NotificationManager::SendNotification(float width, float height, Color backgroundColor, const std::string& text, float seconds) {
    float newY = TOP_MARGIN;
    for (const auto& notification : notifications) {
        if (notification.isVisible) {
            newY += notification.height + NOTIFICATION_SPACING;
        }
    }
    Notification newNotification(text, backgroundColor, width, height, seconds);
    newNotification.targetY = newY;
    newNotification.currentY = -height;    
    notifications.push_back(newNotification);
}
void NotificationManager::Update() {
    auto currentTime = std::chrono::steady_clock::now();
    for (auto it = notifications.begin(); it != notifications.end();) {
        auto& notification = *it;
        if (!notification.isDismissing && currentTime >= notification.dismissTime) {
            notification.isDismissing = true;
        }
        float animationProgress = 0.0f;
        if (notification.isDismissing) {
            auto dismissStartTime = notification.dismissTime;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - dismissStartTime);
            animationProgress = elapsed.count() / (ANIMATION_DURATION * 1000.0f);            
            if (animationProgress >= 1.0f) {
                it = notifications.erase(it);
                continue;
            }
            float startY = notification.targetY;
            float endY = -notification.height;
            notification.currentY = startY + (endY - startY) * EaseInBack(animationProgress);            
        } else {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - notification.startTime);
            animationProgress = elapsed.count() / (ANIMATION_DURATION * 1000.0f);
            if (animationProgress >= 1.0f) {
                animationProgress = 1.0f;
            }
            float startY = -notification.height;
            float endY = notification.targetY;
            notification.currentY = startY + (endY - startY) * EaseOutBack(animationProgress);
        }        
        ++it;
    }
    float currentTargetY = TOP_MARGIN;
    for (auto& notification : notifications) {
        if (!notification.isDismissing) {
            notification.targetY = currentTargetY;
            currentTargetY += notification.height + NOTIFICATION_SPACING;
        }
    }
}
void NotificationManager::Draw() {
    const int fontSize = 20;
    const float padding = 15.0f;
    const float cornerRadius = 0.75f;
    static std::unordered_map<std::string, std::vector<std::string>> wrapCache;
    for (const auto& notification : notifications) {
        if (!notification.isVisible) continue;
        float centerX = GetScreenWidth() / 2.0f;
        float notificationX = centerX - notification.width / 2.0f;
        float notificationY = notification.currentY;
        Rectangle notificationRect = {
            notificationX,
            notificationY,
            notification.width,
            notification.height
        };
        Color BGColor = {
            static_cast<unsigned char>(notification.backgroundColor.r),
            static_cast<unsigned char>(notification.backgroundColor.g),
            static_cast<unsigned char>(notification.backgroundColor.b),
            192
        };
        DrawRectangleRounded(notificationRect, cornerRadius, 16, BGColor);
        float lineThickness = 2.0f;
        DrawRectangleRoundedLinesEx(notificationRect, cornerRadius, 16, lineThickness, Color {255,255,255,64});
        // Cache wrapped text to avoid re-measuring every frame
        std::string cacheKey = notification.text + "|" + std::to_string((int)notification.width);
        auto cit = wrapCache.find(cacheKey);
        if (cit == wrapCache.end()) {
            wrapCache[cacheKey] = WrapText(notification.text, fontSize, notification.width - 2 * padding);
            cit = wrapCache.find(cacheKey);
        }
        const std::vector<std::string>& wrappedLines = cit->second;
        float textY = notificationY + padding;
        for (const auto& line : wrappedLines) {
            float textWidth = MeasureText(line.c_str(), fontSize);
            float textX = centerX - textWidth / 2.0f;
            DrawText(line.c_str(), static_cast<int>(textX + 1), static_cast<int>(textY + 1), fontSize, BLACK);
            DrawText(line.c_str(), static_cast<int>(textX), static_cast<int>(textY), fontSize, WHITE);
            textY += fontSize + 2;
        }
    }
    if (notifications.empty()) wrapCache.clear();
}
std::vector<std::string> NotificationManager::WrapText(const std::string& text, int fontSize, float maxWidth) {
    std::vector<std::string> lines;
    std::string currentLine = "";
    std::string word = "";
    for (size_t i = 0; i <= text.length(); ++i) {
        char c = (i < text.length()) ? text[i] : ' ';
        if (c == ' ' || c == '\n' || i == text.length()) {
            if (!word.empty()) {
                std::string testLine = currentLine.empty() ? word : currentLine + " " + word;
                if (MeasureText(testLine.c_str(), fontSize) <= maxWidth) {
                    currentLine = testLine;
                } else {
                    if (!currentLine.empty()) {
                        lines.push_back(currentLine);
                        currentLine = word;
                    } else {
                        lines.push_back(word);
                        currentLine = "";
                    }
                }
                word = "";
            }
            if (c == '\n') {
                if (!currentLine.empty()) {
                    lines.push_back(currentLine);
                    currentLine = "";
                }
            }
        } else {
            word += c;
        }
    }
    if (!currentLine.empty()) {
        lines.push_back(currentLine);
    }
    return lines;
}
Rectangle NotificationManager::MeasureTextBounds(const std::string& text, int fontSize, float maxWidth) {
    std::vector<std::string> lines = WrapText(text, fontSize, maxWidth);
    float maxLineWidth = 0;
    for (const auto& line : lines) {
        float lineWidth = MeasureText(line.c_str(), fontSize);
        if (lineWidth > maxLineWidth) {
            maxLineWidth = lineWidth;
        }
    }
    float height = lines.size() * (fontSize + 2) - 2;
    return Rectangle{0, 0, maxLineWidth, height};
}
void NotificationManager::ClearAll() {
    notifications.clear();
}
void SendNotification(float width, float height, Color backgroundColor, const std::string& text, float seconds) {
    g_NotificationManager.SendNotification(width, height, backgroundColor, text, seconds);
}

// ===================================================================
// OTHER MODELS
// ===================================================================

void InvalidateNoteBuffer() {
    // Cancel any in-progress paint job and drain the queue so the bg thread
    // doesn't finish writing old chunk data AFTER we've already cleared and
    // re-enqueued new chunks. Without this, old jobs corrupt new chunk pixels.
    g_paintCancel.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(g_paintMtx);
        while (!g_paintQueue.empty()) g_paintQueue.pop();
    }
    // Spin-wait for thread to finish its current PaintChunkRange call (~1 frame max)
    while (g_paintBusy.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    g_paintCancel.store(false, std::memory_order_release);

    g_rtNeedsFullRedraw = true;   // force full texture repaint next frame
    g_seekInvalidate.store(true); // also snap scroll position
}

// ===================================================================
// IMPROVED COLOR MANAGEMENT
// ===================================================================
#define MAX_TRACKS 65535

static Color extendedColors[] = {
    // Original 16 colors
    {51, 102, 255, 255}, {255, 102, 51, 255}, {51, 255, 102, 255}, {255, 51, 129, 255},
    {51, 255, 255, 255}, {228, 51, 255, 255}, {153, 255, 51, 255}, {75, 51, 255, 255},
    {255, 204, 51, 255}, {51, 180, 255, 255}, {255, 51, 51, 255}, {51, 255, 177, 255},
    {255, 51, 204, 255}, {78, 255, 51, 255}, {153, 51, 255, 255}, {231, 255, 51, 255},
    // Additional colors (lighter variants)
    {102, 153, 255, 255}, {255, 153, 102, 255}, {102, 255, 153, 255}, {255, 102, 180, 255},
    {102, 255, 255, 255}, {255, 102, 255, 255}, {204, 255, 102, 255}, {126, 102, 255, 255},
    // Additional colors (darker variants)
    {25, 51, 128, 255}, {128, 51, 25, 255}, {25, 128, 51, 255}, {128, 25, 64, 255},
    {25, 128, 128, 255}, {114, 25, 128, 255}, {76, 128, 25, 255}, {37, 25, 128, 255},
    // More vibrant colors
    {255, 0, 127, 255}, {127, 255, 0, 255}, {0, 127, 255, 255}, {255, 127, 0, 255},
    {127, 0, 255, 255}, {0, 255, 127, 255}, {255, 255, 0, 255}, {0, 255, 255, 255},
    // Pastel variants
    {255, 192, 203, 255}, {173, 216, 230, 255}, {144, 238, 144, 255}, {255, 182, 193, 255},
    {221, 160, 221, 255}, {176, 196, 222, 255}, {255, 160, 122, 255}, {152, 251, 152, 255},
    // Final set
    {255, 105, 180, 255}, {64, 224, 208, 255}, {255, 215, 0, 255}, {138, 43, 226, 255},
    {50, 205, 50, 255}, {255, 69, 0, 255}, {30, 144, 255, 255}, {255, 20, 147, 255}
};

static Color currentTrackColors[MAX_TRACKS];
static int maxTracksUsed = MAX_TRACKS;
static bool colorsInitialized = false;

void InitializeTrackColors(int numTracks = 16) {
    maxTracksUsed = std::min(numTracks * 16, MAX_TRACKS);
    const int numExtendedColors = sizeof(extendedColors) / sizeof(extendedColors[0]);
    for (int i = 0; i < maxTracksUsed; i++) {
        currentTrackColors[i] = extendedColors[i % numExtendedColors];
    }
    colorsInitialized = true;
    std::cout << "Initialized colors for " << numTracks << " tracks x 16 channels (" << maxTracksUsed << " slots)" << std::endl;
}

inline Color GetTrackColorPFA(int track, int channel) {
    if (!colorsInitialized) InitializeTrackColors();
    int colorIndex = (track * 16 + channel) % maxTracksUsed;
    return currentTrackColors[colorIndex];
}

void ResetTrackColors() {
    if (!colorsInitialized) InitializeTrackColors();
    const int numExtendedColors = sizeof(extendedColors) / sizeof(extendedColors[0]);
    for (int i = 0; i < maxTracksUsed; i++) {
        currentTrackColors[i] = extendedColors[i % numExtendedColors];
    }
    InvalidateNoteBuffer();
    std::cout << "- Channel color change to default (" << maxTracksUsed << " tracks)" << std::endl;
}

void RandomizeTrackColors() {
    if (!colorsInitialized) InitializeTrackColors();
    const int numExtendedColors = sizeof(extendedColors) / sizeof(extendedColors[0]);
    std::vector<Color> colorPool;
    for (int i = 0; i < numExtendedColors; i++) {
        colorPool.push_back(extendedColors[i]);
    }
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(colorPool.begin(), colorPool.end(), g);
    for (int i = 0; i < maxTracksUsed; i++) {
        currentTrackColors[i] = colorPool[i % numExtendedColors];
    }
    InvalidateNoteBuffer();
    std::cout << "- Channel color change to randomized (" << maxTracksUsed << " tracks)" << std::endl;
}

void GenerateRandomTrackColors() {
    if (!colorsInitialized) InitializeTrackColors();
    std::random_device rd;
    std::mt19937 g(rd());
    std::uniform_int_distribution<int> colorDist(0, 255);
    for (int i = 0; i < maxTracksUsed; i++) {
        currentTrackColors[i] = {
            static_cast<unsigned char>(colorDist(g)),
            static_cast<unsigned char>(colorDist(g)),
            static_cast<unsigned char>(colorDist(g)),
            255
        };
    }
    InvalidateNoteBuffer();
    std::cout << "- Channel color change to Generate random (" << maxTracksUsed << " tracks)" << std::endl;
}

bool LoadColorsFromPianoFromAbove() {
    const char* appdata = std::getenv("APPDATA");
    if (!appdata) {
        std::cout << "- APPDATA env not found" << std::endl;
        return false;
    }
    std::string configPath = std::string(appdata) + "\\Piano From Above\\Config.xml";
    std::ifstream file(configPath);
    if (!file.is_open()) {
        std::cout << "- Piano From Above config not found: " << configPath << std::endl;
        return false;
    }
    std::vector<Color> pfaColors;
    std::string line;
    bool inColorsBlock = false;
    while (std::getline(file, line)) {
        if (line.find("<Colors>") != std::string::npos) { inColorsBlock = true; continue; }
        if (line.find("</Colors>") != std::string::npos) { inColorsBlock = false; break; }
        if (!inColorsBlock) continue;
        auto parseAttr = [&](const std::string& attr) -> int {
            size_t pos = line.find(attr + "=\"");
            if (pos == std::string::npos) return -1;
            pos += attr.size() + 2;
            size_t end = line.find('"', pos);
            if (end == std::string::npos) return -1;
            return std::stoi(line.substr(pos, end - pos));
        };
        int r = parseAttr("R"), g = parseAttr("G"), b = parseAttr("B");
        if (r >= 0 && g >= 0 && b >= 0)
            pfaColors.push_back({ (unsigned char)r, (unsigned char)g, (unsigned char)b, 255 });
    }
    if (pfaColors.empty()) {
        std::cout << "- No colors found in Piano From Above config" << std::endl;
        return false;
    }
    if (!colorsInitialized) InitializeTrackColors();
    int numPFA = (int)pfaColors.size();
    for (int i = 0; i < maxTracksUsed; i++) {
        int track   = i / 16;
        int channel = i % 16;
        int trackColor  = (track * 7) % numPFA;
        int colorIndex  = (trackColor + channel * 3) % numPFA;
        currentTrackColors[i] = pfaColors[colorIndex];
    }
    InvalidateNoteBuffer();
    std::cout << "+ Loaded " << numPFA << " PFA colors" << std::endl;
    return true;
}

// ===================================================================
// INFORMATION VERSION
// ===================================================================
void InformationVersion()
{
    int fontSize = 10;
    int positionY = GetScreenHeight() - 35;
    DrawText("Version: 1.0.3A (Pre-Release)", 10, positionY, fontSize, GRAY);
    positionY += 15;
    DrawText("Graphic: raylib 5.5", 10, positionY, fontSize, GRAY);
    DrawText("NOTICE: The same keys hit after sound issue", GetScreenWidth() / 2 - MeasureText("NOTICE: The same keys hit after sound issue", 10) / 2, GetScreenHeight() - 30, 10, Color {255,255,128,128});
    DrawText("Check terminal after load midi", GetScreenWidth() / 2 - MeasureText("Check terminal after load midi", 10) / 2, GetScreenHeight() - 15, 10, Color {255,255,255,192});
}

// ===================================================================
// GUI FUNCTIONS
// ===================================================================
bool DrawButton(Rectangle bounds, const char* text, Color colors) {
    bool isHovered = CheckCollisionPointRec(GetMousePosition(), bounds);
    DrawRectangleRounded(bounds, 0.5f, 48, isHovered ? JBG1C : colors);
    DrawRectangleRoundedLinesEx(bounds, 0.5f, 48, 2.0f, DARKGRAY);
    int textWidth = MeasureText(text, 20);
    DrawText(text, (int)(bounds.x + (bounds.width - textWidth) / 2), (int)(bounds.y + (bounds.height - 20) / 2), 20, WHITE);
    return isHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

bool DrawInputBox(Rectangle box, std::string &inputBuffer, int &cursorPos, bool &inputActive, int fontSize = 20, int padding = 5) {
    DrawRectangle(0,0,GetScreenWidth(), GetScreenHeight(), Color {16,24,32,128});
    DrawText("Input patch with '*.mid' file", GetScreenWidth() / 2 - MeasureText("Input patch with '*.mid' file", 20)/2, GetScreenHeight() - 100, 20, WHITE);
    DrawText("This enter key be may because crash on after type.", GetScreenWidth() / 2 - MeasureText("This enter key be may because crash on after type.", 10)/2, GetScreenHeight() - 115, 10, RED);
    DrawText("If you want put drop '*.mid' or '*.midi' file", GetScreenWidth() / 2 - MeasureText("If you want put drop '*.mid' or '*.midi' file", 10)/2, GetScreenHeight() - 130, 10, WHITE);
    DrawRectangleRec(box, GRAY);
    static double blinkTimer = 0.0;
    blinkTimer += GetFrameTime();
    bool showCursor = fmod(blinkTimer, 1.0) < 0.5;
    int cursorPixelPos = MeasureText(inputBuffer.substr(0, cursorPos).c_str(), fontSize);
    static int scrollOffset = 0;
    if (cursorPixelPos - scrollOffset > (int)box.width - 2*padding) {
        scrollOffset = cursorPixelPos - ((int)box.width - 2*padding);
    }
    if (cursorPixelPos - scrollOffset < 0) {
        scrollOffset = cursorPixelPos;
    }
    std::string visibleText;
    int visibleStart = 0;
    for (int i = 0; i < (int)inputBuffer.size(); i++) {
        int w = MeasureText(inputBuffer.substr(0, i+1).c_str(), fontSize);
        if (w >= scrollOffset) {
            visibleStart = i;
            break;
        }
    }
    for (int i = visibleStart; i < (int)inputBuffer.size(); i++) {
        int w = MeasureText(inputBuffer.substr(visibleStart, i - visibleStart + 1).c_str(), fontSize);
        if (w > (int)box.width - 2*padding) break;
        visibleText = inputBuffer.substr(visibleStart, i - visibleStart + 1);
    }
    int textY = box.y + (box.height/2 - fontSize/2);
    DrawText(visibleText.c_str(), box.x + padding, textY, fontSize, WHITE);
    if (inputActive && showCursor) {
        int beforeW = MeasureText(inputBuffer.substr(visibleStart, cursorPos - visibleStart).c_str(), fontSize);
        int cursorX = box.x + padding + beforeW;
        DrawLine(cursorX, box.y + 5, cursorX, box.y + box.height - 5, WHITE);
    }
    if (inputActive) {
        int key = GetCharPressed();
        while (key > 0) {
            if (key >= 32 && key <= 125) {
                inputBuffer.insert(cursorPos, 1, (char)key);
                cursorPos++;
                blinkTimer = 0.0;
            }
            key = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && cursorPos > 0) {
            inputBuffer.erase(cursorPos - 1, 1);
            cursorPos--;
            blinkTimer = 0.0;
        }
        if (IsKeyPressed(KEY_DELETE) && cursorPos < (int)inputBuffer.size()) {
            inputBuffer.erase(cursorPos, 1);
            blinkTimer = 0.0;
        }
        if (IsKeyPressed(KEY_LEFT) && cursorPos > 0) {
            cursorPos--;
            blinkTimer = 0.0;
        }
        if (IsKeyPressed(KEY_RIGHT) && cursorPos < (int)inputBuffer.size()) {
            cursorPos++;
            blinkTimer = 0.0;
        }
        if (IsKeyPressed(KEY_HOME)) {
            cursorPos = 0;
            blinkTimer = 0.0;
        }
        if (IsKeyPressed(KEY_END)) {
            cursorPos = inputBuffer.size();
            blinkTimer = 0.0;
        }
        if ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) && IsKeyPressed(KEY_V)) {
            const char* clip_cstr = GetClipboardText();
            if (clip_cstr != nullptr) {
                std::string clip_str(clip_cstr);             
                if (!clip_str.empty()) {
                    inputBuffer.insert(cursorPos, clip_str);
                    cursorPos += clip_str.length(); 
                    blinkTimer = 0.0;
                }
            }
        }
        if (IsKeyPressed(KEY_ENTER)) {
            if (!inputBuffer.empty() && inputBuffer.front() == '"' && inputBuffer.back() == '"') {
                inputBuffer = inputBuffer.substr(1, inputBuffer.size() - 2);
            }
            return true;
        }
        if (IsKeyPressed(KEY_ESCAPE)) {
            inputBuffer.clear();
            SendNotification(360, 50, SERROR, "Select input file cancelled", 5.0f);
            inputActive = false;
        }
    }
    return false;
}

void DrawModeSelectionMenu() {
    static std::string inputBuffer;
    static int cursorPos = 0;
    static bool inputActive = false;
    static bool showInputBox = false;
    if (IsFileDropped()) {
        FilePathList droppedFiles = LoadDroppedFiles();
        if (droppedFiles.count > 0) {
            std::string filePath = droppedFiles.paths[0];
            const char* ext = GetFileExtension(filePath.c_str());
            if (TextIsEqual(ext, ".mid") || TextIsEqual(ext, ".midi") || TextIsEqual(ext, ".MID") || TextIsEqual(ext, ".MIDI")) {
                inputBuffer = filePath;
                selectedMidiFile = inputBuffer;
                cursorPos = (int)inputBuffer.length();
                SendNotification(320, 50, SSUCCESS, "File loaded with Drag file", 5.0f);
            } else {
                SendNotification(400, 75, SERROR, "File can't open other file\n Use '*.mid' or '*.midi'", 5.0f);
            }
        }
        UnloadDroppedFiles(droppedFiles);
    }
    if (IsKeyPressed(KEY_ENTER) && !inputActive) currentState = STATE_LOADING;
    ClearBackground(JBG1A);
    DrawText("JIDI Player", 10, 10, 20, WHITE);
    DrawText(TextFormat("File: %s", GetFileName(selectedMidiFile.c_str())), GetScreenWidth()/2 - MeasureText(TextFormat("File: %s", GetFileName(selectedMidiFile.c_str())), 20)/2, 160, 20, LIGHTGRAY);
    if (DrawButton({(float)GetScreenWidth() / 2 - 120, 200, 240, 50}, "Load midi input", JBG1B)) {
        showInputBox = true;
        inputActive = true;
    }
    if (DrawButton({(float)GetScreenWidth() / 2 - 120, 265, 240, 50}, "Start Playback", JBG1B)) {
        currentState = STATE_LOADING;
    }
    InformationVersion();
    if (showInputBox) {
        Rectangle inputRect = { GetScreenWidth()/2 - 320.0f, GetScreenHeight() - 60.0f, 640.0f, 40 };
        if (DrawInputBox(inputRect, inputBuffer, cursorPos, inputActive, 20)) {
            selectedMidiFile = inputBuffer;
            inputActive = false;
            showInputBox = false;
        }
        if (!inputActive) {
            showInputBox = false;
        }
    }
}

void DrawDetailedLoadingScreen() {
    ClearBackground(JGRAY);
    DrawText("Loading File...", GetScreenWidth() / 2 - MeasureText("Loading File...", 40) / 2, 80, 40, WHITE);
    
    float percentage = 0.0f;
    if (g_LoadProgress.totalBytes > 0) {
        percentage = (float)g_LoadProgress.bytesRead / (float)g_LoadProgress.totalBytes;
    }

    // Draw Progress Bar
    int barW = 400;
    int barH = 20;
    int barX = GetScreenWidth() / 2 - barW / 2;
    int barY = 150;
    DrawRectangle(barX, barY, barW, barH, DARKGRAY);
    DrawRectangle(barX, barY, (int)(barW * percentage), barH, LIME);
    
    // Draw Stats
    int textY = 200;
    DrawText(TextFormat("Read Bytes: %zu / %zu", g_LoadProgress.bytesRead.load(), g_LoadProgress.totalBytes.load()), barX, textY, 20, LIGHTGRAY); textY += 25;
    DrawText(TextFormat("Track: %d / %d", g_LoadProgress.currentTrack.load(), g_LoadProgress.totalTracks.load()), barX, textY, 20, LIGHTGRAY); textY += 25;
    DrawText(TextFormat("Notes Parsed: %llu", g_LoadProgress.currentNotes.load()), barX, textY, 20, LIGHTGRAY); textY += 25;
    
    const char* phaseText = "Starting...";
    if (g_LoadProgress.loadPhase == 1) phaseText = "Parsing MIDI stream...";
    if (g_LoadProgress.loadPhase == 2) phaseText = "Optimizing and Sorting...";
    DrawText(phaseText, barX, textY, 20, LIGHTGRAY); textY += 25;
    
    MemoryUsage mem = GetMemoryUsage();
    DrawText(TextFormat("Memory Usage: %llu MB (Committed: %llu MB)", mem.workingSetMB, mem.privateUsageMB), barX, textY, 20, LIGHTGRAY); 
}

static void BuildTempoSegs(int ppq) {
    g_tempoSegs.clear();
    double usPerTick = MidiTiming::CalculateMicrosecondsPerTick(MidiTiming::DEFAULT_TEMPO_MICROSECONDS, ppq);
    g_tempoSegs.push_back({ 0, 0.0, usPerTick });
    double   accumSec = 0.0;
    uint32_t lastTick = 0;
    for (const auto& ev : GetGlobalMidiEvents()) {
        if (ev.type != (uint8_t)EventType::TEMPO) continue;
        accumSec += (ev.tick - lastTick) * usPerTick / 1000000.0;
        lastTick  = ev.tick;
        usPerTick = MidiTiming::CalculateMicrosecondsPerTick(ev.getTempo(), ppq); // Corrected here
        g_tempoSegs.push_back({ ev.tick, accumSec, usPerTick });
    }
}
// Bake the 20-cell NPS grid from all note tracks. Called once after load.
// Each cell covers an equal time slice of the song; value = notes/sec in that slice.
static double TicksToSeconds(uint64_t tick); // forward decl for BuildNpsGrid

static void BuildNpsGrid(const std::vector<OptimizedTrackData>& tracks, int barWidthPx = 0) {
    g_npsGrid.clear();
    g_npsGridReady = false;
    if (g_songDurationSec <= 0.0 || g_tempoSegs.empty()) return;

    // Cell count from bar width: fixed 10px per cell
    int cells = (barWidthPx > 0) ? std::max(1, barWidthPx / kNpsCellPx) : 126;
    g_npsGridCells      = cells;
    g_npsGridBuiltWidth = barWidthPx;
    g_npsGrid.assign(cells, 0.f);

    double cellSec = g_songDurationSec / cells;
    std::vector<float> counts(cells, 0.f);

    for (const auto& track : tracks) {
        for (const auto& n : track.notes) {
            double sec = TicksToSeconds(n.startTick);
            int cell = (int)(sec / cellSec);
            if (cell >= 0 && cell < cells) counts[cell]++;
        }
    }
    float maxNps = 0.f;
    for (int i = 0; i < cells; ++i) {
        g_npsGrid[i] = (cellSec > 0.0) ? counts[i] / (float)cellSec : 0.f;
        maxNps = std::max(maxNps, g_npsGrid[i]);
    }
    if (maxNps > 0.f)
        for (int i = 0; i < cells; ++i) g_npsGrid[i] /= maxNps;

    g_npsGridReady = true;
}

static double TicksToSeconds(uint64_t tick) {
    if (g_tempoSegs.empty()) return 0.0;
    size_t lo = 0, hi = g_tempoSegs.size();
    while (lo + 1 < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_tempoSegs[mid].tick <= (uint32_t)tick) lo = mid;
        else hi = mid;
    }
    const auto& seg = g_tempoSegs[lo];
    return seg.accumSec + (tick - seg.tick) * seg.usPerTick / 1000000.0;
}

// Returns the MIDI tick that is exactly 'seconds' of real time before targetTick,
// properly walking the tempo map backward so BPM changes don't warp the window.
// Used by the NPS calculation so the 1-second lookback window is always correct
// even when the song crosses a tempo boundary inside that window.
static uint32_t TicksMinusSeconds(uint64_t targetTick, double seconds) {
    if (g_tempoSegs.empty() || seconds <= 0.0) return 0;

    double targetSec      = TicksToSeconds(targetTick);
    double windowStartSec = targetSec - seconds;
    if (windowStartSec <= 0.0) return 0;

    // Binary-search for the tempo segment that contains windowStartSec
    size_t lo = 0, hi = g_tempoSegs.size();
    while (lo + 1 < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_tempoSegs[mid].accumSec <= windowStartSec) lo = mid;
        else hi = mid;
    }
    const auto& seg     = g_tempoSegs[lo];
    double      remSec  = windowStartSec - seg.accumSec;   // seconds past seg start
    // Convert remaining seconds back to ticks using this segment's usPerTick
    // ticks = remSec / (usPerTick / 1e6) = remSec * 1e6 / usPerTick
    uint64_t resultTick = (seg.usPerTick > 0.0)
        ? (uint64_t)seg.tick + (uint64_t)(remSec * 1000000.0 / seg.usPerTick)
        : (uint64_t)seg.tick;
    return (uint32_t)std::min(resultTick, (uint64_t)UINT32_MAX);
}

// ===================================================================
// DEBUG PANEL (AKA INFORMATION)
// ===================================================================
void DrawDebugPanel(uint64_t currentVisualizerTick, int ppq, uint32_t currentTempo, size_t eventListPos, size_t totalEvents, bool isPaused, float scrollSpeed, const std::vector<OptimizedTrackData>& tracks, bool isFinished) {
    float panelX = (GetScreenWidth() - DWidth) - 10.0f;
    float panelY = 40.0f;
    float lineHeight = 12.0f;
    float padding = 10.0f;
    uint64_t barNumber = (currentVisualizerTick / ticksPerMeasure) + 1;
    uint64_t tickIntoMeasure = currentVisualizerTick % ticksPerMeasure;
    uint64_t beatNumber = 0;
    uint64_t tickIntoBeat = 0;
    if (ticksPerBeat > 0) {
        beatNumber   = (tickIntoMeasure / ticksPerBeat) + 1;
        tickIntoBeat = tickIntoMeasure % ticksPerBeat;
    }
    DrawRectangleRounded(Rectangle{panelX, panelY, DWidth, DHeight}, 0.25f, 32, Color{64, 64, 64, 128});
	DrawRectangleRoundedLinesEx(Rectangle{panelX, panelY, DWidth, DHeight}, 0.25f, 32, 2.0f, Color{32, 32, 32, 128});
    DrawText("Information", (int)(panelX + padding), (int)(panelY + padding), 20, WHITE);
    float currentY = panelY + padding + 25.0f;
    const char* statusText;
    Color statusColor;
    if (isFinished) {
        statusText = "FINISHED";
        statusColor = YELLOW;
    } else {
        statusText = isPaused ? "PAUSED" : "PLAYING";
        statusColor = isPaused ? RED : GREEN;
    }
    DrawText(TextFormat("Playback status: %s", statusText), (int)(panelX + padding), (int)currentY, 15, statusColor);
    currentY += lineHeight + 10.0f;
    DrawText(TextFormat("Measures: %llu:%llu (%u/%u) ~ Ticks: %llu (PPQ: %d)", barNumber, beatNumber, timeSigNumerator, timeSigDenominator, currentVisualizerTick, ppq), (int)(panelX + padding), (int)currentY, 10, WHITE);
    currentY += lineHeight;
    DrawText(TextFormat("Tempo: %u us ~ Speed: %.2fx", currentTempo, MidiSpeed), (int)(panelX + padding), (int)currentY, 10, WHITE);
    currentY += lineHeight;
    float progress = totalEvents > 0 ? ((float)eventListPos / (float)totalEvents) * 100.0f : 0.0f;
    DrawText(TextFormat("Event: %zu / %zu (%.3f%%)", eventListPos, totalEvents, progress), (int)(panelX + padding), (int)currentY, 10, WHITE);
    currentY += lineHeight;
    DrawText(TextFormat("Scroll speed: %.2fx", scrollSpeed), (int)(panelX + padding), (int)currentY, 10, WHITE);
    currentY += lineHeight;
    DrawText(TextFormat("Render notes: %llu / %llu (Textures)", renderNotes.load(), maxRenderNotes.load()), (int)(panelX + padding), (int)currentY, 10, WHITE);
}

// ===================================================================
// PERFORMANCE DEBUG (NEW)
// ===================================================================

#define MAX_PERF_HISTORY 400

static int perfFpsHistory[MAX_PERF_HISTORY] = {0};
static float perfFtHistory[MAX_PERF_HISTORY] = {0.0f};
static int perfTpsHistory[MAX_PERF_HISTORY] = {0};
static float perfBufHistory[MAX_PERF_HISTORY] = {0.0f};

// Rolling min / max / avg computed from the history window
static int   perfFpsMin = 0, perfFpsMax = 0;
static float perfFpsAvg = 0.0f;
static float perfFtMin  = 0.0f, perfFtMax = 0.0f, perfFtAvg = 0.0f;
static int   perfTpsMin = 0, perfTpsMax = 0;
static float perfTpsAvg = 0.0f;
static uint64_t perfEvpsHistory[MAX_PERF_HISTORY] = {0};
static uint64_t perfEvpsMin = 0, perfEvpsMax = 0;
static double   perfEvpsAvg = 0.0;
static uint64_t lastCurrentVisualizerTick = 0;

void UpdatePerformanceHistory(int fps, float ft, int tps, float bufHealth, uint64_t evps) {
    for (int i = 0; i < MAX_PERF_HISTORY - 1; i++) {
        perfFpsHistory[i]  = perfFpsHistory[i + 1];
        perfFtHistory[i]   = perfFtHistory[i + 1];
        perfTpsHistory[i]  = perfTpsHistory[i + 1];
        perfBufHistory[i]  = perfBufHistory[i + 1];
        perfEvpsHistory[i] = perfEvpsHistory[i + 1];
    }
    perfFpsHistory[MAX_PERF_HISTORY - 1]  = fps;
    perfFtHistory[MAX_PERF_HISTORY - 1]   = ft;
    perfTpsHistory[MAX_PERF_HISTORY - 1]  = tps;
    perfBufHistory[MAX_PERF_HISTORY - 1]  = bufHealth;
    perfEvpsHistory[MAX_PERF_HISTORY - 1] = evps;

    int    fpsMin = perfFpsHistory[0], fpsMax = perfFpsHistory[0];
    float  ftMin  = perfFtHistory[0],  ftMax  = perfFtHistory[0];
    int    tpsMin = perfTpsHistory[0], tpsMax = perfTpsHistory[0];
    uint64_t evpsMin = perfEvpsHistory[0], evpsMax = perfEvpsHistory[0];
    double fpsSum = 0.0, ftSum = 0.0, tpsSum = 0.0, evpsSum = 0.0;
    for (int i = 0; i < MAX_PERF_HISTORY; i++) {
        fpsSum  += perfFpsHistory[i];
        ftSum   += perfFtHistory[i];
        tpsSum  += perfTpsHistory[i];
        evpsSum += (double)perfEvpsHistory[i];
        if (perfFpsHistory[i]  < fpsMin)  fpsMin  = perfFpsHistory[i];
        if (perfFpsHistory[i]  > fpsMax)  fpsMax  = perfFpsHistory[i];
        if (perfFtHistory[i]   < ftMin)   ftMin   = perfFtHistory[i];
        if (perfFtHistory[i]   > ftMax)   ftMax   = perfFtHistory[i];
        if (perfTpsHistory[i]  < tpsMin)  tpsMin  = perfTpsHistory[i];
        if (perfTpsHistory[i]  > tpsMax)  tpsMax  = perfTpsHistory[i];
        if (perfEvpsHistory[i] < evpsMin) evpsMin = perfEvpsHistory[i];
        if (perfEvpsHistory[i] > evpsMax) evpsMax = perfEvpsHistory[i];
    }
    perfFpsMin = fpsMin; perfFpsMax = fpsMax; perfFpsAvg = (float)(fpsSum / MAX_PERF_HISTORY);
    perfFtMin  = ftMin;  perfFtMax  = ftMax;  perfFtAvg  = (float)(ftSum  / MAX_PERF_HISTORY);
    perfTpsMin = tpsMin; perfTpsMax = tpsMax; perfTpsAvg = (float)(tpsSum / MAX_PERF_HISTORY);
    perfEvpsMin = evpsMin; perfEvpsMax = evpsMax; perfEvpsAvg = evpsSum / MAX_PERF_HISTORY;
}

// ===================================================================
// PERFORMANCE DEBUG PANEL (NEW)
// ===================================================================
struct PerfTier {
    float threshold;
    Color color;
};

// Custom helper: Draws a fixed-height 1px vertical line that stacks two colors
// to represent the progression percentage between the current tier and the next.
void Draw100PercentStackedColumn(int x, int y, int height, float val, const std::vector<PerfTier>& tiers) {
    if (tiers.empty()) return;
    
    int i = 0;
    while (i < (int)tiers.size() - 2 && val >= tiers[i + 1].threshold) {
        i++;
    }
    
    float t0 = tiers[i].threshold;
    float t1 = tiers[i + 1].threshold;
    Color cTop = tiers[i].color;     // Current Tier
    Color cBottom = tiers[i + 1].color; // Next Tier

    float percent = 0.0f;
    if (t1 > t0) {
        percent = (val - t0) / (t1 - t0);
    }
    
    // Clamp to [0.0, 1.0]
    if (percent < 0.0f) percent = 0.0f;
    if (percent > 1.0f) percent = 1.0f;

    // Bottom portion represents progress towards the NEXT tier
    int bottomH = (int)(percent * (float)height);
    if (bottomH < 0) bottomH = 0;
    if (bottomH > height) bottomH = height;
    
    // Top portion is the CURRENT tier
    int topH = height - bottomH;

    // Draw the top remainder
    if (topH > 0) {
        DrawRectangle(x, y, 1, topH, cTop);
    }
    // Draw the progressing bottom portion
    if (bottomH > 0) {
        DrawRectangle(x, y + topH, 1, bottomH, cBottom);
    }
}

void DrawPerformanceDebugPanel() {
    int width = 420;
    int height = 295;
    int px = GetScreenWidth() - width - 10;
    int py = GetScreenHeight() - height - 40;
    DrawRectangleRounded(Rectangle{(float)px, (float)py, (float)width, (float)height}, 0.1f, 32, Color{64, 64, 64, 128});
    DrawRectangleRoundedLinesEx(Rectangle{(float)px, (float)py, (float)width, (float)height}, 0.1f, 32, 2.0f, Color{32, 32, 32, 128});
	
    int cx = px + 10;
    int cy = py + 10;

    const int GW = 400; // graph width (1px each)
    const int GH = 45;  // graph height in pixels

    DrawText("Performance", cx, cy, 20, WHITE);
	cy += 25;

    // Tiers definitions mapping your thresholds from GetPerfColorList
    std::vector<PerfTier> fpsTiers = {
        {0.0f, BLACK}, {0.5f, PDarkerRed}, {1.0f, PDarkerRed}, {5.0f, PDarkRed}, {10.0f, PRed}, 
        {30.0f, POrange}, {60.0f, PYellow}, {240.0f, PGreen}, {960.0f, PBlue}, 
        {1920.0f, PCyan}, {3000.0f, PMagenta}, {10000.0f, PWhite}
    };
    std::vector<PerfTier> ftTiers = {
        {0.0f, PWhite}, {0.5f, PWhite}, {1.0f, PMagenta}, {2.5f, PCyan}, 
        {5.0f, PBlue}, {10.0f, PGreen}, {30.0f, PYellow}, {100.0f, POrange}, 
        {500.0f, PRed}, {1000.0f, PDarkRed}, {30000.0f, PDarkerRed}, {60000.0f, BLACK}
    };
	std::vector<PerfTier> bufTiers = {
        {0.0f, BLACK}, {0.5f, PDarkRed}, {1.0f, PRed}, {5.0f, POrange},
        {10.0f, PYellow}, {30.0f, PGreen}, {60.0f, PBlue}, {150.0f, PCyan},
        {300.0f, PMagenta}, {600.0f, PWhite}
    };
    std::vector<PerfTier> tpsTiers = {
        {0.0f, BLACK}, {30.0f, PDarkerRed}, {60.0f, PDarkerRed}, {120.0f, PDarkRed},
		{240.0f, PRed}, {480.0f, POrange}, {960.0f, PYellow}, {1920.0f, PGreen}, {3840.0f, PBlue},
        {7680.0f, PCyan}, {15360.0f, PMagenta}, {30720.0f, PWhite}
    };
	std::vector<PerfTier> evpsTiers = {
        {0.0f, BLACK}, {10.0f, PRed}, {100.0f, POrange}, {1000.0f, PYellow},
		{10000.0f, PGreen}, {100000.0f, PBlue}, {1000000.0f, PCyan}, {10000000.0f, PMagenta},
		{100000000.0f, PWhite}
    };

    // ---- FPS graph ----
    DrawText(TextFormat("Graphics - Frames Per Second (Real-Time): %d  (%d / %d / %.0f)", perfFpsHistory[MAX_PERF_HISTORY - 1], perfFpsMin, perfFpsMax, perfFpsAvg), cx, cy, 10, WHITE);
    cy += 13;

    DrawRectangle(cx, cy, GW, GH, Color{0, 0, 0, 128});
    for (int i = 0; i < MAX_PERF_HISTORY; i++) {
        Draw100PercentStackedColumn(cx + i, cy, GH, (float)perfFpsHistory[i], fpsTiers);
    }

    // ---- Frame Time graph ----
    cy += GH + 5;
    DrawText(TextFormat("Graphics - Frame Time: %.2f ms  (%.2f ms / %.2f ms / %.2f ms)", perfFtHistory[MAX_PERF_HISTORY - 1], perfFtMin, perfFtMax, perfFtAvg), cx, cy, 10, WHITE);
    cy += 13;

    DrawRectangle(cx, cy, GW, GH, Color{0, 0, 0, 128});
    for (int i = 0; i < MAX_PERF_HISTORY; i++) {
        Draw100PercentStackedColumn(cx + i, cy, GH, perfFtHistory[i], ftTiers);
    }

    // ---- Bottom graph: TPS or Buffer Health ----
    cy += GH + 5;
    
    if (g_BassEngine.IsInitialized() && g_BassEngine.GetActiveMode() == AudioMode::BassMIDI_PreRender) {
        auto prSt = g_BassEngine.GetPreRenderStatus();
        float curHealth = perfBufHistory[MAX_PERF_HISTORY - 1];
        float maxHealth = g_BassEngine.GetConfig().preRenderBufferSec;
        {
            const BassConfig& cfg = g_BassEngine.GetConfig();
            int minV = cfg.lowBufferMinVoices;
            int maxV = cfg.voices;
            int dispVoices;
            if (curHealth >= 2.0f) dispVoices = maxV;
            else {
                float t = curHealth / 2.0f;
                dispVoices = (int)(minV + t * (maxV - minV));
                dispVoices = std::clamp(dispVoices, minV, maxV);
            }
            DrawText(TextFormat("Pre-Render - Buffer: %.2f s / %.0f s (Progress: %.2f%% ~ Voices: %d/%d)",
                curHealth, maxHealth, prSt.progress * 100.f, dispVoices, maxV),
                cx, cy, 10, WHITE);
        }
        cy += 13;
        DrawRectangle(cx, cy, GW, GH, Color{0, 0, 0, 128});
        for (int i = 0; i < MAX_PERF_HISTORY; i++) {
            Draw100PercentStackedColumn(cx + i, cy, GH, perfBufHistory[i], bufTiers);
        }
    } else {
        DrawText(TextFormat("MIDI - Ticks Per Second: %d  (%d / %d / %.0f)", perfTpsHistory[MAX_PERF_HISTORY - 1], perfTpsMin, perfTpsMax, perfTpsAvg), cx, cy, 10, WHITE);
        cy += 13;
        DrawRectangle(cx, cy, GW, GH, Color{0, 0, 0, 128});
        for (int i = 0; i < MAX_PERF_HISTORY; i++) {
            Draw100PercentStackedColumn(cx + i, cy, GH, (float)perfTpsHistory[i], tpsTiers);
        }
    }
	cy += GH + 5;
	bool evpsRecordingOn = g_AudioEngine.IsEventCounterRecordEnabled();
	if (evpsRecordingOn) {
		DrawText(TextFormat("MIDI - Events Per Second: %llu  (%llu / %llu / %.0f)", perfEvpsHistory[MAX_PERF_HISTORY - 1], perfEvpsMin, perfEvpsMax, perfEvpsAvg), cx, cy, 10, WHITE);
	} else {
		DrawText("MIDI - Events Per Second: [ DISABLED ]", cx, cy, 10, WHITE);
	}
    cy += 13;
    if (evpsRecordingOn) {
        DrawRectangle(cx, cy, GW, GH, Color{0, 0, 0, 128});
        for (int i = 0; i < MAX_PERF_HISTORY; i++) {
            Draw100PercentStackedColumn(cx + i, cy, GH, (float)perfEvpsHistory[i], evpsTiers);
        }
    } else {
        DrawRectangle(cx, cy, GW, GH, Color{64, 64, 64, 128});
        DrawText("[ DISABLED ]", cx + ( GW/2 - MeasureText("[ DISABLED ]", 10)/2 ), cy + (GH / 2) - 5, 10, LIGHTGRAY);
    }
}


// ===================================================================
// BACKGROUND PARTICLE SYSTEM
// ===================================================================
static void SpawnParticle(BgParticle& p, bool randomX) {
    int sw = std::max(GetScreenWidth(),  1);
    int sh = std::max(GetScreenHeight(), 1);
    p.x           = randomX ? (float)(rand() % sw) : (float)(sw + rand() % 300);
    p.y           = (float)(rand() % sh);
    p.speedFactor = 0.5f + (rand() % 1000) / 1000.0f; // [0.50 – 1.50]
    p.sizeFactor  = 0.5f + (rand() % 1000) / 1000.0f;
}

// bpmFactor  = (currentBpm / 120.0f) * MidiSpeed  when g_particleBpm is on
//            = 1.0f                                when g_particleBpm is off
// paused     = true  -> every particle freezes in place (speed = 0)
static void UpdateAndDrawParticles(float dt, float bpmFactor, bool paused) {
    if (!g_particleShow) return;

    // Resize pool on-the-fly when count changes
    int count = std::clamp(g_particleCount, 1, 512);
    int old   = (int)g_particles.size();
    if (old != count) {
        g_particles.resize(count);
        for (int i = old; i < count; ++i)
            SpawnParticle(g_particles[i], /*randomX=*/true);
    }

    // Pause = hard freeze; BPM flag = whether bpmFactor modulates speed
    float speedBase = paused ? 0.0f
                     : g_particleSpeed * (g_particleBpm ? bpmFactor : 1.0f);

    for (auto& p : g_particles) {
        p.x -= speedBase * p.speedFactor * dt;
        if (p.x < -(g_particleSize * p.sizeFactor * 4.0f))
            SpawnParticle(p, /*randomX=*/false);
        DrawCircleV({ p.x, p.y }, g_particleSize * p.sizeFactor, g_particleColor);
    }
}

// ===================================================================
// MAIN FUNCTION
// ===================================================================
int main(int argc, char* argv[]) {
    std::cout << "+ Starting..." << std::endl;
    // KDMAPI is the default audio backend. If it fails we continue anyway —
    // the user can switch to BassMIDI from the Audio Config panel at runtime.
    bool kdmapiOk = InitializeKDMAPIStream();
    if (kdmapiOk) std::cout << "+ KDMAPI Initialized!" << std::endl;
    else std::cout << "[warn] KDMAPI init failed - switch to BassMIDI in Audio Config." << std::endl;
    if (argc > 1) {
        selectedMidiFile = argv[1];
        std::cout << "+ File selection alived!" << std::endl;
    }
    std::cout << "+ Opening window..." << std::endl;
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(1280, 720, "JIDI Player - v1.0.4 (Build: " TOSTRING(BUILD_NUMBER) ")");
	auto ib = GetIconPNGBytes();
	if (ib.data && ib.size > 0) {
		Image icon = LoadImageFromMemory(".png", ib.data, ib.size);
		if (icon.data) { SetWindowIcon(icon); UnloadImage(icon); }
	}
    SetWindowMinSize(450, 240);
    SetWindowState(FLAG_VSYNC_HINT);
    SetExitKey(KEY_NULL);
	PreInitAudioConfig();
	if (g_BassEngine.Init(GetWindowHandle())) {
        std::cout << "+ BassMIDI engine ready\n";
        LoadAudioConfig();
    } else {
        std::cout << "[warn] BassMIDI engine init failed - pre-render unavailable.\n";
    }
    rlImGuiSetup(true);
	#ifdef _WIN32
    static std::string imguiPath = GetConfigPath("imgui.ini");
    ImGui::GetIO().IniFilename = imguiPath.c_str();
	#endif
	// ImGui Global Styles (my made this themes)
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowBorderSize = 3.0f;
	style.WindowRounding = 12.0f;
	style.FrameRounding = 12.0f;
	style.GrabRounding = 12.0f;
	style.Colors[ImGuiCol_WindowBg] = ImVec4(0.0625f, 0.09375f, 0.125f, 0.875f);
	style.Colors[ImGuiCol_Button] = ImVec4(0.125f, 0.5f, 0.125f, 1.0f);
	style.Colors[ImGuiCol_Border] = ImVec4(0.03125f, 0.046875f, 0.0625f, 0.875f);
    StartNoteRenderThread();
    std::vector<OptimizedTrackData> noteTracks;
	g_Smtc.Init({
		.onPlay  = [] { g_AudioEngine.Resume(); },
		.onPause = [] { g_AudioEngine.Pause(); },
		.onStop  = [] { g_AudioEngine.Stop(); },
		.onSeek  = [](int64_t targetMicros) {
		g_AudioEngine.SeekAbsolute((uint64_t)targetMicros);
	},});
    // eventList lives in load.cpp; access via GetGlobalMidiEvents()
	uint32_t frameupdate = 0;
    uint16_t ppq = 480;
    uint32_t currentTempo = MidiTiming::DEFAULT_TEMPO_MICROSECONDS;
	uint32_t g_totalTicks = 0;
    bool isFirstCheck = true;
	g_Smtc.UpdateMetadata("No played", "JIDI-Player");
    while (!WindowShouldClose()) {
        switch (currentState) {
            case STATE_MENU: {
                BeginDrawing();
                DrawModeSelectionMenu();
                g_NotificationManager.Update();
                g_NotificationManager.Draw();
                EndDrawing();
                break;
            }
            case STATE_LOADING: {
				// START THE THREAD ONCE
				static bool isThreadStarted = false;
				
				if (!isThreadStarted) {
					isThreadStarted = true;
					
					// Safely reset the progress values
					g_LoadProgress.Reset(); 
					
					// Launch thread
					g_LoaderThread = std::thread([&]() {
						int iPpq = 480, iTempo = (int)MidiTiming::DEFAULT_TEMPO_MICROSECONDS;
						g_loadedCCEvents = loadStreamingMidiData(selectedMidiFile, noteTracks, iPpq, iTempo, noteTotal,
						timeSigNumerator, timeSigDenominator, &g_LoadProgress, g_enableOverlapRemove);
						
						// Assign out variables carefully
						ppq = (uint16_t)iPpq;
						currentTempo = (uint32_t)iTempo;
						
						// Finish Up
						MidiLoadUsage = GetMemoryUsage();
						TotalLoadUsage = GetMemoryUsage();
						
						g_LoadProgress.isFinished = true;
					});
				}

				// DRAW THE LOADING CHUNK (Runs 60FPS to keep OS happy)
				BeginDrawing();
				DrawDetailedLoadingScreen();
				g_NotificationManager.Update();
				g_NotificationManager.Draw();
				EndDrawing();
				
				// CHECK IF DONE
				if (g_LoadProgress.isFinished) {
					g_LoaderThread.join(); // Safely close the thread
					isThreadStarted = false; // reset for next time
					
					// Execute post-load configurations synchronously on the main thread now
					InitializeTrackColors(static_cast<int>(noteTracks.size()));
					
					g_sortedNoteStartTicks.clear();
					g_sortedNoteStartTicks.reserve(noteTotal);
					g_sortedNoteEndTicks.clear();
					g_sortedNoteEndTicks.reserve(noteTotal);
					g_songLastTick = 0;
					g_maxNps  = 0;
					g_maxPoly = 0;
					for (const auto& track : noteTracks)
						for (const auto& note : track.notes) {
							g_sortedNoteStartTicks.push_back(note.startTick);
							g_sortedNoteEndTicks.push_back(note.endTick);
							if (note.endTick > g_songLastTick) g_songLastTick = note.endTick;
						}
						
					std::sort(g_sortedNoteStartTicks.begin(), g_sortedNoteStartTicks.end());
					std::sort(g_sortedNoteEndTicks.begin(),   g_sortedNoteEndTicks.end());
					BuildTempoSegs(ppq);
					g_songDurationSec = TicksToSeconds(g_songLastTick);
					BuildNpsGrid(noteTracks, (int)(GetScreenWidth() - 20)); // bake NPS grid at 10px/cell
					
					if (noteTracks.size() == 0) {
						currentState = STATE_MENU;
						SendNotification(400, 75, SERROR, "You need to load MIDI files first", 5.0f);
						break;
					}
					
				// Start playing immediately!
                firstPause = true;
				currentTempo = MidiTiming::DEFAULT_TEMPO_MICROSECONDS;
				{
					const auto& evs = GetGlobalMidiEvents();
					if (!evs.empty() && evs[0].type == (uint8_t)EventType::TEMPO)
						currentTempo = evs[0].getTempo(); // Corrected here
					g_AudioEngine.Start(evs, ppq, currentTempo);
				}
                g_AudioEngine.SetSpeed(MidiSpeed);
                g_AudioEngine.SetLooping(isLoop);
                g_AudioEngine.Pause();
                std::cout << "+-[ Help controller ]-+" << std::endl << std::endl;

                std::cout << "--[ Playback ]--" << std::endl;
                std::cout << "BACKSPACE = Return menu" << std::endl;
                std::cout << "SPACE = Pause / Resume" << std::endl;
                std::cout << "LEFT = Seek -3 seconds" << std::endl;
                std::cout << "RIGHT = Seek +3 seconds" << std::endl;
                std::cout << "UP = Fast speed (+0.01x)" << std::endl;
                std::cout << "DOWN = Slow speed (-0.01x)" << std::endl;
                std::cout << "S = Normal speed (1.00x)" << std::endl;
                std::cout << "E = Toggle Event Skip" << std::endl;
                std::cout << "R = Restart playback" << std::endl;
                std::cout << "J = Start loop" << std::endl;
                std::cout << "K = End loop" << std::endl;
                std::cout << "L = Enable loop (Or when midi is finish)" << std::endl << std::endl;

                std::cout << "--[ Render ]--" << std::endl;
                std::cout << "O = Slower scroll speed (+0.05x)" << std::endl;
                std::cout << "I = Faster scroll speeds (-0.05x)" << std::endl;
                std::cout << "P = Reset scroll speeds (0.50x)" << std::endl;
                std::cout << "T = Change layer" << std::endl;
                std::cout << "V = Toggle guide" << std::endl;
                std::cout << "B = Toggle beats" << std::endl << std::endl;

                std::cout << "--[ Color ]--" << std::endl;
                std::cout << "Keypad 1 = Randomize track colors" << std::endl;
                std::cout << "Keypad 2 = Generate completely random colors" << std::endl;
                std::cout << "Keypad 3 = Import Piano From Above colors" << std::endl;
                std::cout << "Keypad 0 = Reset track colors to original" << std::endl << std::endl; 

                std::cout << "--[ Misc ]--" << std::endl;
				std::cout << "F1 = Toggle HUD" << std::endl;
                std::cout << "F2 = Take Screenshot" << std::endl;
				std::cout << "F3 = Show Information" << std::endl;
                std::cout << "F4 = Show Performance" << std::endl;
                std::cout << "F8 = Audio Config (Show Options only)" << std::endl;
                std::cout << "F9 = Show Options (ImGui)" << std::endl;
                std::cout << "F10 = Toggle VSync" << std::endl;
                std::cout << "F11 = Toggle Fullscreen (Do not return menu for because broken)" << std::endl;
                std::cout << "M = Reset maximum counter" << std::endl << std::endl;

                std::cout << "+-[ Let's being! ]-+" << std::endl;
                std::cout << "- Scroll speed default set: " << ScrollSpeed << "x" << std::endl;
                std::cout << "+ Midi load:" << GetFileName(selectedMidiFile.c_str()) << std::endl;
                std::cout << "+ Total notes: " << FormatWithCommas(noteTotal).c_str() << " ~ Total tracks: " << noteTracks.size() << std::endl;
                std::cout << "+ Time Signature detected: " << timeSigNumerator << "/" << timeSigDenominator << std::endl;
                std::cout << "- Midi/Parse memory: " << MidiLoadUsage.workingSetMB << " MB (Committed: " << MidiLoadUsage.privateUsageMB << " MB)" << std::endl;
                std::cout << "- Result memory: " << TotalLoadUsage.workingSetMB << " MB (Committed: " << TotalLoadUsage.privateUsageMB << " MB)" << std::endl << std::endl;
                
                SetWindowState(FLAG_WINDOW_RESIZABLE);
                currentState = STATE_PLAYING;
                SetWindowTitle(TextFormat("JIDI Player (Build: " TOSTRING(BUILD_NUMBER) ") - %s", GetFileName(selectedMidiFile.c_str())));
				g_Smtc.UpdateMetadata(GetFileName(selectedMidiFile.c_str()), "JIDI-Player");
                if (isFirstCheck) {SendNotification(420, 50, SINFORMATION, "Check terminal for show help control", 5.0f); isFirstCheck = false;}
				}
				break;
			}
            case STATE_PLAYING: {
                if (IsKeyPressed(KEY_R) && !firstPause) {
					noteCounter = 0;
					maxRenderNotes = 0;
					g_maxNps = 0;
					g_maxPoly = 0;
					InvalidateNoteBuffer(); 
					g_AudioEngine.Stop();
					currentTempo = MidiTiming::DEFAULT_TEMPO_MICROSECONDS;
					{
						const auto& evs = GetGlobalMidiEvents();
						if (!evs.empty() && evs[0].type == (uint8_t)EventType::TEMPO)
							currentTempo = evs[0].getTempo();
						g_AudioEngine.Start(evs, ppq, currentTempo);
					}
					g_AudioEngine.SetSpeed(MidiSpeed);
					g_AudioEngine.SetLooping(isLoop);
					if (!isLoop) std::cout << "- Playback Restarted" << std::endl;
				}
                if (IsKeyPressed(KEY_SPACE)) {
                    if (firstPause) firstPause = false;
                    if (g_AudioEngine.IsPaused()) {
                        g_AudioEngine.Resume();
                    } else {
                        g_AudioEngine.Pause();
                    }
                }
                if (IsKeyPressed(KEY_BACKSPACE) && (!showOptions)) { 
                    std::cout << "- Returning menu..." << std::endl; 
                    InvalidateNoteBuffer(); // reset texture for next song
                    g_AudioEngine.Stop();
                    g_AudioEngine.ClearLoopPoints();
                    g_loopPointA = g_loopPointB = UINT64_MAX;
                    SetWindowState(FLAG_VSYNC_HINT);
                    ClearWindowState(FLAG_WINDOW_RESIZABLE);
                    SetWindowSize(1280, 720);
                    noteTracks.clear();
                    noteTracks.shrink_to_fit();
                    g_sortedNoteStartTicks.clear();
                    g_sortedNoteStartTicks.shrink_to_fit();
                    g_sortedNoteEndTicks.clear();
                    g_sortedNoteEndTicks.shrink_to_fit();
                    g_songLastTick = 0; g_songDurationSec = 0.0; g_tempoSegs.clear(); g_maxNps = 0; g_maxPoly = 0; g_npsGridReady = false;
                    SetWindowTitle("JIDI Player - v1.0.4 (Build: " TOSTRING(BUILD_NUMBER) ")"); 
					g_Smtc.UpdateMetadata("No played", "JIDI-Player");
					currentState = STATE_MENU;
                }
                if (IsFileDropped()) {
                    FilePathList droppedFiles = LoadDroppedFiles();
                    if (droppedFiles.count > 0) {
                        std::string filePath = droppedFiles.paths[0];
                        const char* ext = GetFileExtension(filePath.c_str());
                        if (TextIsEqual(ext, ".mid") || TextIsEqual(ext, ".midi") || 
                            TextIsEqual(ext, ".MID") || TextIsEqual(ext, ".MIDI")) {
                            std::cout << "- Returning menu after file drop files" << std::endl; 
                            g_AudioEngine.Stop();
                            SetWindowState(FLAG_VSYNC_HINT);
                            ClearWindowState(FLAG_WINDOW_RESIZABLE);
                            SetWindowSize(1280, 720);
                            noteTracks.clear();
                            noteTracks.shrink_to_fit();
                            g_sortedNoteStartTicks.clear();
                            g_sortedNoteStartTicks.shrink_to_fit();
                            g_sortedNoteEndTicks.clear();
                            g_sortedNoteEndTicks.shrink_to_fit();
                            g_songLastTick = 0; g_songDurationSec = 0.0; g_tempoSegs.clear(); g_maxNps = 0; g_maxPoly = 0; g_npsGridReady = false;
                            SetWindowTitle("JIDI Player - v1.0.4 (Build: " TOSTRING(BUILD_NUMBER) ")");
							g_Smtc.UpdateMetadata("No played", "JIDI-Player");
                            currentState = STATE_MENU;
                            inputBuffer = filePath;
                            selectedMidiFile = inputBuffer;
                            cursorPos = (int)inputBuffer.length();
                            continue;
                        } else if (TextIsEqual(ext, ".png") || TextIsEqual(ext, ".jpg") ||
                                   TextIsEqual(ext, ".jpeg") || TextIsEqual(ext, ".PNG") ||
                                   TextIsEqual(ext, ".JPG") || TextIsEqual(ext, ".JPEG")) {
                            // Drop image -> set as background
                            if (g_bgImageTex.id != 0) { UnloadTexture(g_bgImageTex); g_bgImageTex = { 0 }; }
                            g_bgImageTex = LoadTexture(filePath.c_str());
                            if (g_bgImageTex.id != 0) {
                                SetTextureFilter(g_bgImageTex, TEXTURE_FILTER_BILINEAR);
                                strncpy(g_bgImagePath, filePath.c_str(), sizeof(g_bgImagePath) - 1);
                                g_bgImageShow = true;
                                SendNotification(300, 50, SSUCCESS, "Background image set!", 3.0f);
                            } else {
                                SendNotification(300, 50, SERROR, "Failed to load image", 3.0f);
                            }
                        } else {
                            SendNotification(400, 75, SERROR, "File can't open other file\n Use '*.mid' or '*.midi'", 5.0f);
                        }
                    }
                    UnloadDroppedFiles(droppedFiles);
                }
                if (!showOptions) {
                    if (IsKeyPressed(KEY_I) || IsKeyPressedRepeat(KEY_I)) { ScrollSpeed = std::max(0.05f, ScrollSpeed - 0.05f); InvalidateNoteBuffer(); }
                    if (IsKeyPressed(KEY_O) || IsKeyPressedRepeat(KEY_O)) { ScrollSpeed += 0.05f; InvalidateNoteBuffer(); }
                    if (IsKeyPressed(KEY_P)) { ScrollSpeed = 0.50f; InvalidateNoteBuffer(); }
                    if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) {
                        g_AudioEngine.Seek(-3'000'000);
                        std::cout << "- Seeked backward 3 seconds" << std::endl;
                        g_seekInvalidate.store(true); // triggers full redraw next frame
                    }
                    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) {
                        g_AudioEngine.Seek(3'000'000);
                        std::cout << "+ Seeked forward 3 seconds" << std::endl;
                    }
                    if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) {
                        MidiSpeed += 0.01f;
                        g_AudioEngine.SetSpeed(MidiSpeed);
                    }
                    if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) {
                        MidiSpeed = std::max(0.01f, MidiSpeed - 0.01f);
                        g_AudioEngine.SetSpeed(MidiSpeed);
                    }
                    if (IsKeyPressed(KEY_S)) {
                        MidiSpeed = 1.00f;
                        g_AudioEngine.SetSpeed(MidiSpeed);
                    }
                    if (IsKeyPressed(KEY_V)) { 
                        showGuide = !showGuide; 
                        std::cout << "- Guide " << (showGuide ? "visible" : "invisible") << std::endl; }
                    if (IsKeyPressed(KEY_B)) { 
                        showBeats = !showBeats; 
                        std::cout << "- Beats " << (showBeats ? "visible" : "invisible") << std::endl; }
                    if (IsKeyPressed(KEY_T)) {
                        g_viewerType = (g_viewerType == ViewerType::TrackLayer) ? ViewerType::TickLayer : ViewerType::TrackLayer;
                        InvalidateNoteBuffer();
                        std::cout << "- Viewer: " << (g_viewerType == ViewerType::TrackLayer ? "Track Layer" : "Tick Layer") << std::endl; }
                    if (IsKeyPressed(KEY_L)) { 
                        isLoop = !isLoop;
                        g_AudioEngine.SetLooping(isLoop);
                        std::cout << "- Loops " << (isLoop ? "enabled" : "disabled") << std::endl; }
                    // J = Set loop point A  /  K = Set loop point B
                    if (IsKeyPressed(KEY_J)) {
                        uint64_t rawTick = g_AudioEngine.GetCurrentTick();
                        g_loopPointA = g_loopSnapToBeats
                            ? LoopSnapToBeat(rawTick, g_loopBeatOffsetA, ppq, timeSigDenominator)
                            : rawTick;
                        if (g_loopPointB != UINT64_MAX && g_loopPointA < g_loopPointB)
                            g_AudioEngine.SetLoopPoints(g_loopPointA, g_loopPointB);
                        else if (g_loopPointB != UINT64_MAX && g_loopPointA >= g_loopPointB)
                            g_AudioEngine.ClearLoopPoints();
                        uint64_t tpb = ppq > 0 ? (static_cast<uint64_t>(ppq)*4u)/(timeSigDenominator?timeSigDenominator:4u) : 1;
                        uint64_t beatNum = tpb > 0 ? g_loopPointA / tpb + 1 : 0;
                        std::cout << "- Loop A set at tick " << g_loopPointA << " (beat " << beatNum << ")" << std::endl;
                        SendNotification(420, 50, SDEBUG, TextFormat("Loop A: beat %llu (tick %llu)", (uint64_t)beatNum, (uint64_t)g_loopPointA), 3.0f);
                    }
                    if (IsKeyPressed(KEY_K)) {
                        uint64_t rawTick = g_AudioEngine.GetCurrentTick();
                        g_loopPointB = g_loopSnapToBeats
                            ? LoopSnapToBeat(rawTick, g_loopBeatOffsetB, ppq, timeSigDenominator)
                            : rawTick;
                        if (g_loopPointA != UINT64_MAX && g_loopPointA < g_loopPointB)
                            g_AudioEngine.SetLoopPoints(g_loopPointA, g_loopPointB);
                        else if (g_loopPointA != UINT64_MAX && g_loopPointA >= g_loopPointB)
                            g_AudioEngine.ClearLoopPoints();
                        uint64_t tpb = ppq > 0 ? (static_cast<uint64_t>(ppq)*4u)/(timeSigDenominator?timeSigDenominator:4u) : 1;
                        uint64_t beatNum = tpb > 0 ? g_loopPointB / tpb + 1 : 0;
                        std::cout << "- Loop B set at tick " << g_loopPointB << " (beat " << beatNum << ")" << std::endl;
                        SendNotification(420, 50, SDEBUG, TextFormat("Loop B: beat %llu (tick %llu)", (uint64_t)beatNum, (uint64_t)g_loopPointB), 3.0f);
                    }
                    if (IsKeyPressed(KEY_E)) {
                        isAntiSlowdown = !isAntiSlowdown;
                        g_AudioEngine.ToggleAntiSlowdown(isAntiSlowdown);
                        std::cout << "- Anti-Slowdown " << (isAntiSlowdown ? "enabled" : "disabled") << std::endl; }
                    if (IsKeyPressed(KEY_F1)) { 
                        isHUD = !isHUD; 
                        std::cout << "- HUD " << (isHUD ? "visible" : "invisible") << std::endl; }
                    if (IsKeyPressed(KEY_KP_1)) {
                        RandomizeTrackColors(); 
                        SendNotification(280, 50, SDEBUG, "Color change to Random", 3.0f); }
                    if (IsKeyPressed(KEY_KP_0)) { 
                        ResetTrackColors(); 
                        SendNotification(300, 50, SDEBUG, "Color changed to Default", 3.0f); }
                    if (IsKeyPressed(KEY_KP_2)) { 
                        GenerateRandomTrackColors(); 
                        SendNotification(400, 50, SDEBUG, "Color changed to Generate random", 3.0f); }
                    if (IsKeyPressed(KEY_KP_3)) {
                        if (LoadColorsFromPianoFromAbove()) {
                            SendNotification(410, 50, SDEBUG, "Color changed to Piano From Above", 3.0f);
                        } else {
                            SendNotification(380, 50, SERROR, "PFA config not found!", 3.0f);
                        }
                    }
                }
                if (IsKeyPressed(KEY_M)) {
                    maxRenderNotes.store(0, std::memory_order_relaxed);
					g_maxNps = 0;
					g_maxPoly = 0;
                    std::cout << "- Max render notes reset" << std::endl; }
                if (IsKeyPressed(KEY_F2)) {
                    time_t now = time(0);
                    struct tm tstruct;
                    char buf[64];
                    localtime_s(&tstruct, &now);
                    strftime(buf, sizeof(buf), "Jidi-Screenshot_%Y-%m-%d_%H-%M-%S.png", &tstruct);
                    TakeScreenshot(buf);
                     SendNotification(300, 50, SINFORMATION, "Screenshot saved files!", 5.0f);
                    std::cout << "+ Screenshot saved files: " << buf << std::endl; }
                if (IsKeyPressed(KEY_F10)) {
                    if (IsWindowState(FLAG_VSYNC_HINT)) {ClearWindowState(FLAG_VSYNC_HINT); std::cout << "- VSync disabled" << std::endl; }
                    else {SetWindowState(FLAG_VSYNC_HINT); std::cout << "+ VSync enabled" << std::endl; } }
                if  (IsKeyPressed(KEY_F11)) {
                    ToggleBorderlessWindowed();
                    SendNotification(320, 50, SDEBUG, "Toggle has now fullscreen!", 5.0f); }
                if (IsKeyPressed(KEY_F3)) { showDebug = !showDebug; 
                    std::cout << "- Debug " << (showDebug ? "enabled" : "disabled") << std::endl; }
				if (IsKeyPressed(KEY_F4)) { showPerformance = !showPerformance; 
                    std::cout << "- Performance " << (showPerformance ? "enabled" : "disabled") << std::endl; }
				if (IsKeyPressed(KEY_F9)) {
					showOptions = !showOptions;
					std::cout << "- Options " << (showOptions ? "shown" : "hidden") << std::endl; }
				if (IsKeyPressed(KEY_F8)) {
					ToggleAudioConfigPanel();
					std::cout << "- Audio Config " << (IsAudioConfigPanelOpen() ? "shown" : "hidden") << std::endl; }
                bool isPaused  = g_AudioEngine.IsPaused();
                bool isFinished = g_AudioEngine.IsFinished();
                uint64_t currentVisualizerTick = g_AudioEngine.GetCurrentTick();
                uint32_t currentTempo = g_AudioEngine.GetCurrentTempo();
				float dt = GetFrameTime();
				uint64_t evps = (!isPaused && !isFinished) ? g_AudioEngine.GetEventsPerSecond() : 0;
				int tps = 0;
                if (!isPaused && !isFinished) {
					if (dt > 0.0f) {
						tps = (int)(((double)currentVisualizerTick - (double)lastCurrentVisualizerTick) / dt);
						if (tps < 0) tps = 0;
					}
				}
                lastCurrentVisualizerTick = currentVisualizerTick;
                
                float currentBufHealth = (float)g_BassEngine.GetBufferHealthSeconds();
                UpdatePerformanceHistory(1 / GetFrameTime(), GetFrameTime() * 1000.0f, tps, currentBufHealth, evps);
                
                static bool finishedPrinted = false;
                if (isFinished && !finishedPrinted) {
                    std::cout << "- Playback Finished" << std::endl;
                    finishedPrinted = true;
                } else if (!isFinished && finishedPrinted) {
                    finishedPrinted = false;
                    InvalidateNoteBuffer();
                }
                static uint64_t lastCounterTick = UINT64_MAX;
                if (currentVisualizerTick != lastCounterTick) {
                    noteCounter = (uint64_t)std::distance(g_sortedNoteStartTicks.begin(), std::upper_bound(g_sortedNoteStartTicks.begin(), g_sortedNoteStartTicks.end(), (uint32_t)currentVisualizerTick));
                    lastCounterTick = currentVisualizerTick;
                    if (ppq > 0 && !g_tempoSegs.empty()) {
                        uint32_t winStart = TicksMinusSeconds(currentVisualizerTick, 1.0);
                        uint32_t winEnd   = (uint32_t)currentVisualizerTick;
                        auto lo = std::lower_bound(g_sortedNoteStartTicks.begin(), g_sortedNoteStartTicks.end(), winStart);
                        auto hi = std::upper_bound(lo, g_sortedNoteStartTicks.end(), winEnd);
                        g_currentNps = (uint64_t)std::distance(lo, hi);
                        if (g_currentNps > g_maxNps) g_maxNps = g_currentNps;
                    }
                    
                    uint64_t started = (uint64_t)std::distance(g_sortedNoteStartTicks.begin(),
                    std::upper_bound(g_sortedNoteStartTicks.begin(), g_sortedNoteStartTicks.end(), (uint32_t)currentVisualizerTick));
                    uint64_t ended   = (uint64_t)std::distance(g_sortedNoteEndTicks.begin(),
                    std::lower_bound(g_sortedNoteEndTicks.begin(), g_sortedNoteEndTicks.end(), (uint32_t)currentVisualizerTick));
                    g_currentPoly = (started > ended) ? (started - ended) : 0;
                    if (g_currentPoly > g_maxPoly) g_maxPoly = g_currentPoly;
                }
				frameupdate++;
				g_Smtc.UpdatePlaybackState( g_AudioEngine.IsFinished() ? false : true,
					g_AudioEngine.IsPaused(),
					g_AudioEngine.IsFinished());
				if (!GetGlobalMidiEvents().empty()) {
					g_totalTicks = GetGlobalMidiEvents().back().tick;
				}
				if (frameupdate % 60 == 0) {
					double curSec    = TicksToSeconds(currentVisualizerTick);
					double totalSec  = TicksToSeconds(g_totalTicks);   
					uint64_t curUs   = (uint64_t)(curSec  * 1'000'000.0);
					uint64_t totalUs = (uint64_t)(totalSec * 1'000'000.0);
					g_Smtc.UpdatePosition(curUs, totalUs);
					frameupdate = 0;
				}
                static float smoothedProgress = 0.000f;
                float targetProgress = (noteTotal > 0) ? (float)noteCounter / (float)noteTotal : 0.000f;
                smoothedProgress += (targetProgress - smoothedProgress) * 0.25f;
                float barWidth = 450.0f * smoothedProgress;
				float bpmFactor = (currentTempo > 0) ? (60000000.0f / (float)currentTempo / 120.0f) * MidiSpeed : MidiSpeed;
                BeginDrawing();
                ClearBackground(g_backgroundColor);
				// ── Background Image ─────────────────────────────────────
                if (g_bgImageShow && g_bgImageTex.id != 0) {
                    float sw = (float)GetRenderWidth();
                    float sh = (float)GetRenderHeight();
                    float iw = (float)g_bgImageTex.width;
                    float ih = (float)g_bgImageTex.height;
                    Rectangle src = { 0.0f, 0.0f, iw, ih };
                    Rectangle dst = { 0.0f, 0.0f, sw, sh };
                    switch (g_bgImageFit) {
                        case BgImageFit::Stretch:
                            dst = { 0.0f, 0.0f, sw, sh };
                            break;
                        case BgImageFit::Fit: {
                            float scale = std::min(sw / iw, sh / ih);
                            float dw = iw * scale, dh = ih * scale;
                            dst = { (sw - dw) * 0.5f, (sh - dh) * 0.5f, dw, dh };
                            break;
                        }
                        case BgImageFit::Fill: {
                            float scale = std::max(sw / iw, sh / ih);
                            float dw = iw * scale, dh = ih * scale;
                            dst = { (sw - dw) * 0.5f, (sh - dh) * 0.5f, dw, dh };
                            break;
                        }
                        case BgImageFit::Center:
                            dst = { (sw - iw) * 0.5f, (sh - ih) * 0.5f, iw, ih };
                            break;
                    }
                    DrawTexturePro(g_bgImageTex, src, dst, { 0.0f, 0.0f }, 0.0f, g_bgImageTint);
                }
                UpdateAndDrawParticles(GetFrameTime(), bpmFactor, isPaused);
                DrawStreamingVisualizerNotes(noteTracks, currentVisualizerTick, ppq, currentTempo, g_viewerType);
                rlImGuiBegin();
                if (isHUD) {
				DrawRectangleRounded({10.0f, 10.0f, 450.0f, 10.0f}, 1.0f, 32, Color{64,96,64,128});
                DrawRectangleRounded({10.0f, 10.0f, barWidth, 10.0f}, 1.0f, 32, JLIGHTLIME);
                {
                    const float sw        = (float)GetRenderWidth();
                    const float sh        = (float)GetRenderHeight();
                    const float barH      = 10.0f;
                    const float barX      = 10.0f;
                    const float barY      = sh - barH - 10.0f;
                    const float barW      = sw - 20.0f;
                    const float roundness = 1.0f;
                    const int   segments  = 32;
                    DrawRectangleRounded({barX, barY, barW, barH}, roundness, segments, Color{64,64,64,128});
                    float timeFrac = (g_songDurationSec > 0.0)
                        ? (float)(TicksToSeconds(currentVisualizerTick) / g_songDurationSec)
                        : smoothedProgress;
                    timeFrac = std::clamp(timeFrac, 0.f, 1.f);
                    float blueW = barW * timeFrac;
                    if (blueW > 0.f) {
                        BeginScissorMode((int)barX, (int)barY, (int)blueW, (int)barH);
                        DrawRectangleRounded({barX, barY, barW, barH}, roundness, segments, Color{128,192,255,255});
                        EndScissorMode();
                    }
                    if (g_BassEngine.IsInitialized() &&
                        g_BassEngine.GetActiveMode() == AudioMode::BassMIDI_PreRender)
                    {
                        double bufHealth = g_BassEngine.GetBufferHealthSeconds();
                        double curSec    = TicksToSeconds(currentVisualizerTick);
                        double ahead     = curSec + (bufHealth * MidiSpeed); // decode has reached this far
                        float  aheadFrac = (g_songDurationSec > 0.0) ? std::clamp((float)(ahead / g_songDurationSec), 0.f, 1.f) : 0.f;
                        float greenX = barX + blueW;
                        float greenW = (barW * aheadFrac) - blueW; // width between playhead and decode head
                        if (greenW > 0.f) {
                            BeginScissorMode((int)greenX, (int)barY, (int)greenW, (int)barH);
                            DrawRectangleRounded({barX, barY, barW, barH}, roundness, segments, Color{96,192,96,128});
                            EndScissorMode();
                        }
                    }
					{
                        int curBarW = (int)barW;
                        if (g_npsGridBuiltWidth != curBarW && g_songDurationSec > 0.0)
                            BuildNpsGrid(noteTracks, curBarW); // rebuild for new width
                    }
                    if (g_npsGridReady && g_npsGridCells > 0) {
                        const float cellW = (float)kNpsCellPx;
                        for (int i = 0; i < g_npsGridCells; ++i) {
                            float cellX   = barX + i * cellW;
                            if (cellX + cellW > barX + barW) break; // don't overdraw
                            uint8_t alpha = (uint8_t)(g_npsGrid[i] * 128.f);
                            if (alpha < 1) continue;
                            Color cellCol = {255, 255, 255, alpha};
                            BeginScissorMode((int)cellX, (int)barY, kNpsCellPx, (int)barH);
                            DrawRectangleRounded({barX, barY, barW, barH}, roundness, segments, cellCol);
                            EndScissorMode();
                        }
                    }
					if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && !ImGui::GetIO().WantCaptureMouse) {
                        Vector2 mp = GetMousePosition();
                        if (mp.x >= barX && mp.x <= barX + barW && mp.y >= barY && mp.y <= barY + barH) {
                            float    seekFrac = std::clamp((mp.x - barX) / barW, 0.f, 1.f);
                            // Use time fraction directly — tick->seconds is non-linear with tempo changes
                            uint64_t seekUs = (uint64_t)((double)seekFrac * g_songDurationSec * 1'000'000.0);
                            g_AudioEngine.SeekAbsolute(seekUs);
                            InvalidateNoteBuffer();
                            lastCounterTick = UINT64_MAX;
                        }
                    }
                } // end bottom progress bar scope
                DrawText(TextFormat("Notes: %s / %s", FormatWithCommas(noteCounter).c_str(), FormatWithCommas(noteTotal).c_str()), 10, 23, 20, JLIGHTBLUE);
                double curSec = TicksToSeconds(currentVisualizerTick);
                double totSec = g_songDurationSec;
                uint64_t curM = (uint64_t)(curSec / 60), curS = (uint64_t)curSec % 60;
                uint64_t totM = (uint64_t)(totSec / 60), totS = (uint64_t)totSec % 60;
                DrawText(TextFormat("%02llu:%02llu / %02llu:%02llu ~ %.3f BPM", curM, curS, totM, totS, MidiTiming::MicrosecondsToBPM(currentTempo) * MidiSpeed), 10, 44, 20, JLIGHTBLUE);
                DrawText(TextFormat("NPS: %s (Max: %s) ~ Poly: %s (Max: %s)",
                    FormatWithCommas(g_currentNps).c_str(), FormatWithCommas(g_maxNps).c_str(),
                    FormatWithCommas(g_currentPoly).c_str(), FormatWithCommas(g_maxPoly).c_str()),
                    10, 65, 10, JLIGHTBLUE);
				if (g_AudioEngine.GetSimulateEventsPerSecond() > 0) {
                    DrawText("[Lag Simulate Mode]", 10, 79, 10, JLIGHTYELLOW);
                }
                if (firstPause) DrawText("Press SPACEBAR to play", GetScreenWidth()/2 - MeasureText("Press SPACEBAR to play", 20)/2, 20, 20, YELLOW);
                else if (isPaused) DrawText("PAUSED", GetScreenWidth()/2 - MeasureText("PAUSED", 20)/2, 20, 20, RED);
                if (showDebug) DrawDebugPanel(currentVisualizerTick, ppq, currentTempo, g_AudioEngine.GetEventPos(), GetGlobalMidiEvents().size(), isPaused, ScrollSpeed, noteTracks, isFinished);
				if (showPerformance) DrawPerformanceDebugPanel();
                const char* fpsTxt = TextFormat("FPS: %llu", GetFPS());
                DrawText(fpsTxt, (GetScreenWidth() - MeasureText(fpsTxt, 20)) - 10, 10, 20, JLIGHTLIME); }
                g_NotificationManager.Update();
                g_NotificationManager.Draw();
				if (showOptions) {
                    // ── Pin window to top-right, resize each frame so it hugs its content ──
                    ImGui::SetNextWindowPos(ImVec2((float)GetScreenWidth() - 372.0f, 40.0f), ImGuiCond_Always);
                    ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_Always); // height = auto
                    
                    ImGuiWindowFlags wflags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
                    
                    if (ImGui::Begin("Options [F9]", &showOptions, wflags)) {
						// ── Playback ─────────────────────────────────────────────────────
						if (ImGui::CollapsingHeader("Playback", ImGuiTreeNodeFlags_DefaultOpen)) {
				 
							// Speed slider (0.01x – 10.0x)
							if (ImGui::SliderFloat("Speed", &MidiSpeed, 0.01f, 10.0f, "%.2fx")) {
								g_AudioEngine.SetSpeed(MidiSpeed);
							}
				 
							// Loop checkbox
							if (ImGui::Checkbox("Enable loop", &isLoop)) {
								g_AudioEngine.SetLooping(isLoop);
							}
							ImGui::SameLine();
							
							// ── Loop A/B Point Controls ──────────────────────────────────────
							ImGui::Separator();
							ImGui::TextUnformatted("Loop A/B  (J = Set A, K = Set B)");

							// ── Beat-snap toggle ─────────────────────────────────────────────
							ImGui::Checkbox("Snap to beat", &g_loopSnapToBeats);
							ImGui::SameLine();
							ImGui::TextDisabled("(?)");
							if (ImGui::IsItemHovered())
								ImGui::SetTooltip("When enabled, A/B points snap to the nearest beat boundary.\nUse the offset fields below to fine-tune.");

							// ── Compute ticks-per-beat for display ───────────────────────────
							uint64_t tpbDisp = (ppq > 0)
								? (static_cast<uint64_t>(ppq) * 4u) / (timeSigDenominator ? timeSigDenominator : 4u)
								: 1u;
							if (tpbDisp == 0) tpbDisp = 1;

							// Current position in beats (1-based)
							uint64_t curBeat = currentVisualizerTick / tpbDisp + 1;
							ImGui::Text("Now: beat %llu  (tick %llu)", (uint64_t)curBeat,
							            (uint64_t)currentVisualizerTick);

							// ── Per-point beat offset ─────────────────────────────────────────
							if (g_loopSnapToBeats) {
								ImGui::SetNextItemWidth(90.0f);
								ImGui::DragInt("Offset A##loopA", &g_loopBeatOffsetA, 1.0f, -256, 256, "%+d beat");
								if (ImGui::IsItemHovered())
									ImGui::SetTooltip("Beat offset applied when setting point A.\n"
									                  "e.g. +1 = snap one beat ahead of where you press J.");
								ImGui::SameLine(0.f, 12.f);
								ImGui::SetNextItemWidth(90.0f);
								ImGui::DragInt("Offset B##loopB", &g_loopBeatOffsetB, 1.0f, -256, 256, "%+d beat");
								if (ImGui::IsItemHovered())
									ImGui::SetTooltip("Beat offset applied when setting point B.\n"
									                  "e.g. +1 = snap one beat ahead of where you press K.");
							}

							ImGui::Spacing();

							// ── Set / Reset buttons ───────────────────────────────────────────
							bool abActive = g_AudioEngine.HasLoopPoints();

							auto applyLoopA = [&]() {
								uint64_t raw = currentVisualizerTick;
								g_loopPointA  = g_loopSnapToBeats
								    ? LoopSnapToBeat(raw, g_loopBeatOffsetA, ppq, timeSigDenominator)
								    : raw;
								if (g_loopPointB != UINT64_MAX && g_loopPointA < g_loopPointB)
									g_AudioEngine.SetLoopPoints(g_loopPointA, g_loopPointB);
								else if (g_loopPointB != UINT64_MAX && g_loopPointA >= g_loopPointB)
									g_AudioEngine.ClearLoopPoints();
								uint64_t bn = tpbDisp > 0 ? g_loopPointA / tpbDisp + 1 : 0;
								std::cout << "- Loop A = tick " << g_loopPointA << " (beat " << bn << ")\n";
							};
							auto applyLoopB = [&]() {
								uint64_t raw = currentVisualizerTick;
								g_loopPointB  = g_loopSnapToBeats
								    ? LoopSnapToBeat(raw, g_loopBeatOffsetB, ppq, timeSigDenominator)
								    : raw;
								if (g_loopPointA != UINT64_MAX && g_loopPointA < g_loopPointB)
									g_AudioEngine.SetLoopPoints(g_loopPointA, g_loopPointB);
								else if (g_loopPointA != UINT64_MAX && g_loopPointA >= g_loopPointB)
									g_AudioEngine.ClearLoopPoints();
								uint64_t bn = tpbDisp > 0 ? g_loopPointB / tpbDisp + 1 : 0;
								std::cout << "- Loop B = tick " << g_loopPointB << " (beat " << bn << ")\n";
							};

							if (ImGui::Button("Set A"))  applyLoopA();
							ImGui::SameLine();
							if (ImGui::Button("Set B"))  applyLoopB();
							ImGui::SameLine();
							if (ImGui::Button("Reset A/B")) {
								g_loopPointA = g_loopPointB = UINT64_MAX;
								g_AudioEngine.ClearLoopPoints();
								std::cout << "- Loop A/B cleared\n";
							}

							// ── A / B position display ────────────────────────────────────────
							ImGui::Spacing();
							if (g_loopPointA != UINT64_MAX) {
								uint64_t beatA = g_loopPointA / tpbDisp + 1;
								ImGui::Text("A: beat %-5llu (tick %llu)",
								            (uint64_t)beatA, (uint64_t)g_loopPointA);
							} else {
								ImGui::TextDisabled("A: (not set)");
							}
							if (g_loopPointB != UINT64_MAX) {
								uint64_t beatB = g_loopPointB / tpbDisp + 1;
								uint64_t spanBeats = (g_loopPointB - (g_loopPointA != UINT64_MAX ? g_loopPointA : 0)) / tpbDisp;
								ImGui::Text("B: beat %-5llu (tick %llu)  span: %llu beat(s)",
								            (uint64_t)beatB, (uint64_t)g_loopPointB, (uint64_t)spanBeats);
							} else {
								ImGui::TextDisabled("B: (not set)");
							}

							// ── Status line ───────────────────────────────────────────────────
							if (abActive && isLoop)
								ImGui::TextColored(ImVec4(0.2f,1.0f,0.4f,1.0f), "Loop A/B active");
							else if (abActive && !isLoop)
								ImGui::TextColored(ImVec4(1.0f,0.8f,0.2f,1.0f), "A/B set — enable loop to activate");
							else
								ImGui::TextDisabled("A/B not set (full-song loop)");
							ImGui::Separator();
				 
							// Event skip checkbox
							if (ImGui::Checkbox("Toggle Event Skip", &isAntiSlowdown)) {
								g_AudioEngine.ToggleAntiSlowdown(isAntiSlowdown);
							}
							ImGui::SameLine();
								// Event counter record (Information only)
								bool eventCounterRecordUI = g_AudioEngine.IsEventCounterRecordEnabled();
								if (ImGui::Checkbox("Event counter record", &eventCounterRecordUI)) {
									g_AudioEngine.ToggleEventCounterRecord(eventCounterRecordUI);
								}
				 
							// Seek buttons
							ImGui::Spacing();
							if (ImGui::Button("« -10s"))  g_AudioEngine.Seek(-10'000'000LL);
							ImGui::SameLine();
							if (ImGui::Button("« -3s"))   g_AudioEngine.Seek( -3'000'000LL);
							ImGui::SameLine();
							if (ImGui::Button("+3s »"))   g_AudioEngine.Seek(  3'000'000LL);
							ImGui::SameLine();
							if (ImGui::Button("+10s »"))  g_AudioEngine.Seek( 10'000'000LL);
						}
						
						// Audio Config window (BassMIDI pre-render) -- toggled by F8
						DrawAudioConfigPanel();
						
						// Lag Simulator -- hidden when BassMIDI audio mode is active
						if (g_BassEngine.GetActiveMode() == AudioMode::KDMAPI)
							DrawLagSimulatorPanel(g_AudioEngine);
						else {
                            ImGui::Separator();
							ImGui::TextDisabled("Lag Simulator disabled (BassMIDI mode active).");
						}
				 
						// ── Render ───────────────────────────────────────────────────────
						if (ImGui::CollapsingHeader("Render", ImGuiTreeNodeFlags_DefaultOpen)) {
				 
							// Scroll speed slider (0.05 – 4.0)
							if (ImGui::SliderFloat("Scroll Speed", &ScrollSpeed, 0.05f, 4.0f, "%.2fx")) {
								// ScrollSpeed is read directly by the renderer — no extra call needed
							}
							ImGui::SameLine();
							if (ImGui::Button("Reset scroll")) ScrollSpeed = 0.5f;
				 
							// Guide / Beats toggles
							ImGui::Checkbox("Show Guide", &showGuide);
							ImGui::SameLine();
							ImGui::Checkbox("Show Beats", &showBeats);
				 
							// Beat subdivisions (only relevant when beats are on, But no change update)
							if (showBeats) {
								int subdiv = beatSubdivisions;
								if (ImGui::SliderInt("Beat Subdivisions", &subdiv, 1, 16)) {
									beatSubdivisions = subdiv;
								}
							}
							
							if (ImGui::Checkbox("Render Overlap Remove", &g_enableOverlapRemove)) {
								InvalidateNoteBuffer();
							}
							if (ImGui::IsItemHovered()) {
								ImGui::SetTooltip("Enable to filter overlaps during parsing.\nNote: Requires reloading the MIDI file to update counters, NPS, and Polyphony.");
							}
				 
							// Layer selector: matches the T key toggle
							int layerIdx = (g_viewerType == ViewerType::TickLayer) ? 1 : 0;
							const char* layers[] = { "Track Layer", "Tick Layer" };
							if (ImGui::Combo("Layer", &layerIdx, layers, IM_ARRAYSIZE(layers))) {
								g_viewerType = (layerIdx == 1) ? ViewerType::TickLayer : ViewerType::TrackLayer;
                                InvalidateNoteBuffer();
							}
						}
				 
						// ── Display ──────────────────────────────────────────────────────
						if (ImGui::CollapsingHeader("Display")) {
							ImGui::Text("Background Color");
							if (ImGui::ColorEdit4("##BgColor", g_bgColorF,
								ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel)) {
								g_backgroundColor = {
									(unsigned char)(g_bgColorF[0] * 255.0f),
									(unsigned char)(g_bgColorF[1] * 255.0f),
									(unsigned char)(g_bgColorF[2] * 255.0f),
									(unsigned char)(g_bgColorF[3] * 255.0f)
								};
							}
							ImGui::SameLine();
							if (ImGui::Button("Reset##Bg")) {
								g_bgColorF[0] = g_bgColorF[1] = g_bgColorF[2] = 0.031f;
								g_bgColorF[3] = 1.0f;
								g_backgroundColor = { 8, 8, 8, 255 };
							}
							
							// ── Background Image Config ────────────────────────
							ImGui::Separator();
							ImGui::Text("Background Image");
							ImGui::Checkbox("Show##BgImg", &g_bgImageShow);
							if (g_bgImageShow) {
								ImGui::SetNextItemWidth(220.0f);
								bool pathEntered = ImGui::InputText("##BgImgPath", g_bgImagePath, sizeof(g_bgImagePath),
									ImGuiInputTextFlags_EnterReturnsTrue);
								ImGui::SameLine();
								bool loadClicked = ImGui::Button("Load##BgImg");
								ImGui::SameLine();
								if (ImGui::Button("Clear##BgImg")) {
									if (g_bgImageTex.id != 0) { UnloadTexture(g_bgImageTex); g_bgImageTex = { 0 }; }
									memset(g_bgImagePath, 0, sizeof(g_bgImagePath));
								}
								if (pathEntered || loadClicked) {
									if (g_bgImageTex.id != 0) { UnloadTexture(g_bgImageTex); g_bgImageTex = { 0 }; }
									if (strlen(g_bgImagePath) > 0) {
										g_bgImageTex = LoadTexture(g_bgImagePath);
										if (g_bgImageTex.id == 0)
											SendNotification(300, 50, SERROR, "Failed to load image", 3.0f);
										else {
											SetTextureFilter(g_bgImageTex, TEXTURE_FILTER_BILINEAR);
											SendNotification(200, 50, SSUCCESS, "Image loaded!", 3.0f);
										}
									}
								}
								if (ImGui::ColorEdit4("Tint##BgImg", g_bgImageTintF,
									ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar |
									ImGuiColorEditFlags_PickerHueWheel)) {
									g_bgImageTint = {
										(unsigned char)(g_bgImageTintF[0] * 255.0f),
										(unsigned char)(g_bgImageTintF[1] * 255.0f),
										(unsigned char)(g_bgImageTintF[2] * 255.0f),
										(unsigned char)(g_bgImageTintF[3] * 255.0f)
									};
								}
								ImGui::SameLine();
								if (ImGui::Button("Reset Tint##BgImg")) {
									g_bgImageTintF[0] = g_bgImageTintF[1] = g_bgImageTintF[2] = g_bgImageTintF[3] = 1.0f;
									g_bgImageTint = { 255, 255, 255, 255 };
								}
								const char* fitModes[] = { "Stretch", "Fit", "Fill", "Center" };
								int fitIdx = (int)g_bgImageFit;
								ImGui::SetNextItemWidth(100.0f);
								if (ImGui::Combo("Mode##BgImg", &fitIdx, fitModes, 4))
									g_bgImageFit = (BgImageFit)fitIdx;
							}
							
							ImGui::Separator();
							ImGui::Text("Background Particles");
							ImGui::Checkbox("Show##Particle", &g_particleShow);
							if (g_particleShow) {
								ImGui::SameLine();
								ImGui::SetNextItemWidth(70.0f);
								if (ImGui::DragInt("Count##P", &g_particleCount, 1, 1, 512))
									g_particleCount = std::clamp(g_particleCount, 1, 512);
								ImGui::SetNextItemWidth(110.0f);
								ImGui::DragFloat("Speed##P", &g_particleSpeed, 1.0f, 10.0f, 2000.0f, "%.0f px/s");
								ImGui::SameLine();
								ImGui::Checkbox("Scale w/ BPM##P", &g_particleBpm);
								ImGui::SetNextItemWidth(110.0f);
								ImGui::DragFloat("Size##P", &g_particleSize, 0.05f, 0.5f, 30.0f, "%.1f px");
								ImGui::SameLine();
								if (ImGui::ColorEdit4("Color##P", g_particleColorF,
									ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar |
									ImGuiColorEditFlags_PickerHueWheel)) {
									g_particleColor = {
										(unsigned char)(g_particleColorF[0] * 255.0f),
										(unsigned char)(g_particleColorF[1] * 255.0f),
										(unsigned char)(g_particleColorF[2] * 255.0f),
										(unsigned char)(g_particleColorF[3] * 255.0f)
									};
								}
							}
							ImGui::Separator();
							
							ImGui::Checkbox("HUD", &isHUD);
							ImGui::SameLine();
							ImGui::Checkbox("Information", &showDebug);
							ImGui::SameLine();
							ImGui::Checkbox("Performance", &showPerformance);
				 
							// VSync
							bool vsync = IsWindowState(FLAG_VSYNC_HINT);
							if (ImGui::Checkbox("VSync", &vsync)) {
								if (vsync) SetWindowState(FLAG_VSYNC_HINT);
								else       ClearWindowState(FLAG_VSYNC_HINT);
							}
							ImGui::SameLine();
				 
							// Fullscreen (Can't work at checked after fullscreen worked)
							bool fsNow = IsWindowFullscreen();
							if (ImGui::Checkbox("Fullscreen", &fsNow)) {
								ToggleBorderlessWindowed();
							}
						}
				 
						// ── Colors ───────────────────────────────────────────────────────
						if (ImGui::CollapsingHeader("Colors")) {
							if (ImGui::Button("Randomize"))      RandomizeTrackColors();
							ImGui::SameLine();
							if (ImGui::Button("Generate Random")) GenerateRandomTrackColors();
							ImGui::SameLine();
							if (ImGui::Button("Default"))           ResetTrackColors();
				 
							if (ImGui::Button("Import Piano From Above")) {
								if (!LoadColorsFromPianoFromAbove())
									SendNotification(410, 50, SERROR, "PFA config not found!", 3.0f);
							}
						}
					}
					// add check "if save automatic after close" and "if load automatic after startup"
					// add load setting and Forgot move to manual save ("Save setting")
					ImGui::End();
				}
                rlImGuiEnd();
                EndDrawing();
                break;
            }
        }
    }
    std::cout << "- Exiting..." << std::endl;
	SaveAudioConfig();
    g_AudioEngine.Stop();
    StopNoteRenderThread();
    g_BassEngine.Shutdown();    // shut down BassMIDI / pre-render before KDMAPI
    TerminateKDMAPIStream();    // KDMAPI last (nothing routes through it after above)
	rlImGuiShutdown();
    CloseWindow();
    return 0;
}