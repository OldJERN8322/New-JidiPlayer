#include "visualizer.hpp"
#include "midi_timing_alt.hpp"
#include "midioutput.hpp"
#include "Lagsimulatorpanel.hpp"
#include "build_info.hpp"
#include "smtc_bridge.hpp"
#include "bass_backend.hpp"       // BassMIDI pre-render engine + DispatchMidiOut
#include "AudioConfigPanel.hpp"   // DrawAudioConfigPanel() / ToggleAudioConfigPanel()
#include "file_dialog_win32.hpp"  // native "Open File" dialog for loading .mid/.midi
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
bool g_enableOverlapRemove = false; // Toggle for complete overlap removal
bool g_enableRenderOverlapRemove = false; // Toggle for render overlap removal
bool g_enableRoundedNotes = false; // Toggle for rounded note end-caps
ViewerType g_viewerType = ViewerType::TickLayer; // T key toggles
bool g_transparentWindow = false; // Global value before Init Window
static bool firstPause = true; // Loads do first pause (kept static since it is only used here)
static AppState currentState = STATE_MENU;
static std::string selectedMidiFile = "Empty"; 
float ScrollSpeed = 0.5f;
float MidiSpeed = 1.00f;
bool IsTempoOverride = false;
float TempoSet = 120.0f;
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
bool isEventSkip = false;

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
uint64_t ticksPerBeat = (ppq * 4) / timeSigDenominator;
uint64_t ticksPerMeasure = 0;
float DWidth = 360.0f, DHeight = 135.0f;
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
// Need save/load json here i guess
static constexpr int      MAX_CHUNKS = 16; // hard cap = fixed array size backing g_numChunks
static int                 g_numChunks = 6; // runtime-adjustable via Options > Render > "Render Chunks" (2-16)

static Texture2D             g_tex        = { 0 };
static int                   g_texW       = 0;   // = g_numChunks * screenWidth
static int                   g_chunkW     = 0;   // = screenWidth
static double                g_pixPerTick = 0.0;
static uint32_t              g_bufOriginTick = 0;  // tick at pixel col 0
static std::vector<uint32_t> g_pixBuf;             // [PIX_H rows][g_texW cols]
static uint64_t              g_windowOffsetChunks = 0;

// ── Rounded Notes shader ──────────────────────────────────────────────────
static Shader   g_roundShader          = { 0 };
static bool     g_roundShaderLoaded    = false;
static bool     g_roundShaderOk        = false;
static int      g_roundCapTexelsLoc    = -1;
static int      g_roundTexelToPxXLoc   = -1;
static int      g_roundRowHeightPxLoc  = -1;

static constexpr int ROUND_CAP_TEXELS = 6;

static void EnsureRoundedNoteShader() {
    if (g_roundShaderLoaded) return;
    g_roundShaderLoaded = true;

    static const char* fs =
        "#version 330\n"
        "in vec2 fragTexCoord;\n"
        "in vec4 fragColor;\n"
        "uniform sampler2D texture0;\n"
        "uniform vec4 colDiffuse;\n"
        "uniform float capTexels;\n"        // ROUND_CAP_TEXELS
        "uniform float texelToScreenPxX;\n" // screen px per source texel (horizontal)
        "uniform float rowHeightPx;\n"      // screen px spanned by one MIDI pitch row
        "out vec4 finalColor;\n"
        "void main() {\n"
        "    vec4 s = texture(texture0, fragTexCoord);\n"
        "    float code = s.a * 255.0;\n"
        "    if (code < 0.5) { finalColor = vec4(0.0); return; }\n" // background
        "    if (code > 254.5) { finalColor = vec4(s.rgb, 1.0) * colDiffuse; return; }\n" // interior, far from any real edge
        "    float band = floor((code - 1.0) / capTexels);\n"
        "    float localCode = code - band * capTexels;\n"
        "    float edgeDistX = (localCode - 1.0) * texelToScreenPxX;\n" // distance to the real edge, in screen px
        "    float rowFrac = fract(fragTexCoord.y * 128.0);\n"          // 0..1 position within THIS pitch row's band
        "    float edgeDistY;\n"
        "    if (band < 0.5)      edgeDistY = min(rowFrac, 1.0 - rowFrac) * rowHeightPx;\n" // both edges relevant
        "    else if (band < 1.5) edgeDistY = (1.0 - rowFrac) * rowHeightPx;\n"             // only bottom relevant
        "    else                 edgeDistY = rowFrac * rowHeightPx;\n"                     // only top relevant
        "    float R = min(rowHeightPx * 0.5, capTexels * texelToScreenPxX);\n"
        "    if (edgeDistX >= R || edgeDistY >= R) { finalColor = vec4(s.rgb, 1.0) * colDiffuse; return; }\n" // flat body/seam, not in a corner box
        "    vec2 q = vec2(R - edgeDistX, R - edgeDistY);\n" // position relative to the rounding circle's center
        "    float dist = length(q);\n"
        "    float alpha = 1.0 - smoothstep(R - 1.5, R + 1.5, dist);\n"
        "    finalColor = vec4(s.rgb, alpha) * colDiffuse;\n"
        "}\n";

    g_roundShader = LoadShaderFromMemory(nullptr, fs);
    g_roundShaderOk = (g_roundShader.id != 0);
    if (g_roundShaderOk) {
        g_roundCapTexelsLoc   = GetShaderLocation(g_roundShader, "capTexels");
        g_roundTexelToPxXLoc  = GetShaderLocation(g_roundShader, "texelToScreenPxX");
        g_roundRowHeightPxLoc = GetShaderLocation(g_roundShader, "rowHeightPx");
    }
}

static bool     g_chunkPainted[MAX_CHUNKS]    = {};
static uint32_t g_chunkOriginTick[MAX_CHUNKS] = {};  

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
static std::atomic<int>         g_paintQueueDepth{ 0 }; // # of jobs waiting/in-flight, for progress reporting

// Must be called while already holding g_paintMtx. Drains the queue AND
// resets the depth counter together so they can never drift apart. There
// were previously several call sites that cleared g_paintQueue directly
// (window-shift, seek, resize, InvalidateNoteBuffer) without touching
// g_paintQueueDepth — each discarded-but-never-processed job leaked the
// counter upward permanently, since BgPaintThreadFunc only decrements it
// for jobs it actually pops and runs. That leak is what made the
// "Streaming..." status get stuck true forever even at 4/4 chunks painted.
// All queue-clear sites must go through this helper now.
static inline void ClearPaintQueueLocked() {
    while (!g_paintQueue.empty()) g_paintQueue.pop();
    g_paintQueueDepth.store(0, std::memory_order_relaxed);
}

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
    if (c < g_numChunks - 1) {
        return ExactChunkOrigin(bufOrigin, windowOffset + c + 1);
    }
    return ExactChunkOrigin(bufOrigin, windowOffset + c) + g_ticksPerChunk;
}

static std::atomic<bool> g_paintBusy{ false };
static std::atomic<bool> g_paintCancel{ false };

// ---- helpers ---------------------------------------------------------------
static inline uint32_t ToRGBA8(Color c) {
    return (uint32_t)c.r | ((uint32_t)c.g << 8) | ((uint32_t)c.b << 16) | ((uint32_t)c.a << 24);
}
inline Color GetTrackColorPFA(int track, int channel);

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

    struct NoteRun { int px0, px1; bool clipLeft, clipRight; };
    thread_local std::vector<NoteRun> rowRuns[128];
    const bool roundNotes = g_enableRoundedNotes;
    constexpr size_t ROUND_ROW_SAFETY_CAP = 4096;
    if (roundNotes) {
        for (int y = 0; y < 128; ++y) rowRuns[y].clear();
    }

    uint64_t count = 0;
    int cancelCheckCounter = 0;

    if (g_bgViewerType == ViewerType::TickLayer) {
        if (g_enableRenderOverlapRemove) {
            // ---- TICK LAYER (Overlap Remove Enabled): Heap-Optimized Reverse K-Way Merge ----
            thread_local std::vector<ReverseCursor> heap;
            heap.clear();

            uint32_t pitchNextStart[128];
            std::fill(std::begin(pitchNextStart), std::end(pitchNextStart), 0xFFFFFFFFu);

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

                if (n.note < 128) {
                    uint32_t rawEnd = (n.endTick > n.startTick) ? n.endTick : n.startTick + 1;
                    
                    // Truncate based on the start tick of the next note on this pitch
                    uint32_t clippedEnd = std::min(rawEnd, pitchNextStart[n.note]);
                    
                    pitchNextStart[n.note] = n.startTick;

                    if (n.startTick < clippedEnd) {
                        uint32_t ds = (n.startTick > tickStart) ? n.startTick : tickStart;
                        uint32_t de = (clippedEnd < tickEnd)    ? clippedEnd  : tickEnd;

                        if (ds < de) {
                            int px0 = (ds <= tickStart) ? 0 : (int)std::round((double)(ds - tickStart) * ppt);
                            int px1 = (de >= tickEnd)   ? W : (int)std::round((double)(de - tickStart) * ppt);

                            if (px0 < 0) px0 = 0;
                            if (px1 > W) px1 = W;
                            if (px1 <= px0) px1 = px0 + 1;

                            if (px0 < px1 && px0 < W) {
                                if (px1 > W) px1 = W;
                                int y = (PIX_H - 1) - (int)n.note;
                                if ((unsigned)y < (unsigned)PIX_H) {
                                    Color col = GetTrackColorPFA((int)t, n.channel);
                                    uint32_t rgba = ToRGBA8(col);

                                    std::fill_n(rowColors[y].data() + px0, px1 - px0, rgba);
                                    count++;

                                    if (roundNotes && rowRuns[y].size() < ROUND_ROW_SAFETY_CAP) {
                                        rowRuns[y].push_back({ px0, px1, n.startTick < tickStart, rawEnd > tickEnd });
                                    }
                                }
                            }
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
                    int px0 = (ds <= tickStart) ? 0 : (int)std::round((double)(ds - tickStart) * ppt);
                    int px1 = (de >= tickEnd)   ? W : (int)std::round((double)(de - tickStart) * ppt);

                    if (px0 < 0) px0 = 0;
                    if (px1 > W) px1 = W;
                    if (px1 <= px0) px1 = px0 + 1;

                    if (px0 < px1 && px0 < W) {
                        if (px1 > W) px1 = W;
                        int y = (PIX_H - 1) - (int)n.note;
                        if ((unsigned)y < (unsigned)PIX_H) {
                            Color col = GetTrackColorPFA((int)t, n.channel);
                            uint32_t rgba = ToRGBA8(col);

                            std::fill_n(rowColors[y].data() + px0, px1 - px0, rgba);
                            count++;

                            if (roundNotes && rowRuns[y].size() < ROUND_ROW_SAFETY_CAP) {
                                rowRuns[y].push_back({ px0, px1, n.startTick < tickStart, rawEnd > tickEnd });
                            }
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
        // ---- TRACK LAYER: Track-Priority Layering ----
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

                if (ds < de) {
                    int px0 = (ds <= tickStart) ? 0 : (int)std::round((double)(ds - tickStart) * ppt);
                    int px1 = (de >= tickEnd)   ? W : (int)std::round((double)(de - tickStart) * ppt);

                    if (px0 < 0) px0 = 0;
                    if (px1 > W) px1 = W;
                    if (px1 <= px0) px1 = px0 + 1;

                    if (px0 < px1 && px0 < W) {
                        if (px1 > W) px1 = W;
                        int y = (PIX_H - 1) - (int)n.note;
                        if ((unsigned)y < (unsigned)PIX_H) {
                            Color col = GetTrackColorPFA((int)t, n.channel);
                            uint32_t rgba = ToRGBA8(col);

                            std::fill_n(rowColors[y].data() + px0, px1 - px0, rgba);
                            count++;

                            if (roundNotes && rowRuns[y].size() < ROUND_ROW_SAFETY_CAP) {
                                rowRuns[y].push_back({ px0, px1, n.startTick < tickStart, rawEnd > tickEnd });
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- Rounded Notes: encode real edge-distance into the alpha channel ---
    if (roundNotes) {
        struct RoundBlock { int px0, px1; bool leftClip, rightClip; };
        thread_local std::vector<RoundBlock> rowBlocks[128];
        for (int y = 0; y < PIX_H; ++y) rowBlocks[y].clear();

        for (int y = 0; y < PIX_H; ++y) {
            auto& runs = rowRuns[y];
            if (runs.empty()) continue;
            if (runs.size() > ROUND_ROW_SAFETY_CAP) continue; 
            std::sort(runs.begin(), runs.end(),
                [](const NoteRun& a, const NoteRun& b) { return a.px0 < b.px0; });

            size_t i = 0;
            while (i < runs.size()) {
                size_t j = i;
                int blockPx0 = runs[i].px0;
                int blockPx1 = runs[i].px1;
                bool leftClip = runs[i].clipLeft;
                while (j + 1 < runs.size() && runs[j + 1].px0 <= blockPx1 + 1) {
                    ++j;
                    if (runs[j].px1 > blockPx1) blockPx1 = runs[j].px1;
                }
                bool rightClip = runs[j].clipRight;
                rowBlocks[y].push_back({ blockPx0, blockPx1, leftClip, rightClip });
                i = j + 1;
            }
        }
        auto rowHasOverlap = [&](int y, int qpx0, int qpx1) -> bool {
            if (y < 0 || y >= PIX_H) return false;
            auto& blocks = rowBlocks[y];
            if (blocks.empty()) return false;
            auto it = std::upper_bound(blocks.begin(), blocks.end(), qpx1 - 1,
                [](int value, const RoundBlock& b) { return value < b.px0; });
            if (it == blocks.begin()) return false;
            --it;
            return it->px0 < qpx1 && it->px1 > qpx0;
        };

        for (int y = 0; y < PIX_H; ++y) {
            for (auto& blk : rowBlocks[y]) {
                if (blk.px1 <= blk.px0) continue;
                uint32_t* row = rowColors[y].data();
                int cap = std::min(ROUND_CAP_TEXELS, std::max(1, (blk.px1 - blk.px0) / 2));
                if (!blk.leftClip) {
                    bool topExp = !rowHasOverlap(y - 1, blk.px0, blk.px0 + cap);
                    bool botExp = !rowHasOverlap(y + 1, blk.px0, blk.px0 + cap);
                    if (topExp || botExp) {
                        uint32_t band = (topExp && botExp) ? 0u : (botExp ? 1u : 2u);
                        uint32_t bandBase = band * (uint32_t)ROUND_CAP_TEXELS;
                        for (int k = 0; k < cap; ++k) {
                            uint32_t rgb = row[blk.px0 + k] & 0x00FFFFFFu;
                            uint32_t code = bandBase + (uint32_t)(k + 1);
                            row[blk.px0 + k] = rgb | (code << 24);
                        }
                    }
                }
                if (!blk.rightClip) {
                    bool topExp = !rowHasOverlap(y - 1, blk.px1 - cap, blk.px1);
                    bool botExp = !rowHasOverlap(y + 1, blk.px1 - cap, blk.px1);
                    if (topExp || botExp) {
                        uint32_t band = (topExp && botExp) ? 0u : (botExp ? 1u : 2u);
                        uint32_t bandBase = band * (uint32_t)ROUND_CAP_TEXELS;
                        for (int k = 0; k < cap; ++k) {
                            uint32_t rgb = row[blk.px1 - 1 - k] & 0x00FFFFFFu;
                            uint32_t code = bandBase + (uint32_t)(k + 1);
                            row[blk.px1 - 1 - k] = rgb | (code << 24);
                        }
                    }
                }
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
        ClearPaintQueueLocked();
    }
    g_paintQueue.push({ chunkIdx, tickStart, tickEnd });
    g_paintQueueDepth.fetch_add(1, std::memory_order_relaxed);
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
        g_paintQueueDepth.fetch_sub(1, std::memory_order_relaxed);
    }
}

// Real streaming/load progress of the background chunk-paint pipeline:
// how many of the g_numChunks texture chunks are currently painted & valid,
// plus whether the worker thread still has jobs in flight. This is the
// actual "is PaintChunkRange caught up" signal — distinct from renderNotes/
// maxRenderNotes, which only tracks note density within a single chunk.
struct ChunkStreamStatus {
    int  paintedChunks;
    int  totalChunks;
    bool streaming; // true while a chunk is being painted or queued
    float progress; // 0..100
};
static ChunkStreamStatus GetChunkStreamStatus() {
    ChunkStreamStatus s{};
    s.totalChunks = g_numChunks;
    int painted = 0;
    for (int i = 0; i < g_numChunks; ++i) if (g_chunkPainted[i]) painted++;
    s.paintedChunks = painted;
    s.streaming = g_paintBusy.load(std::memory_order_relaxed) ||
                  g_paintQueueDepth.load(std::memory_order_relaxed) > 0;
    s.progress = s.totalChunks > 0 ? ((float)painted / (float)s.totalChunks) * 100.0f : 0.0f;
    return s;
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

    // Defensive clamp: g_numChunks is user-adjustable at runtime via the Options
    // slider (2-16). Clamp here before it's used for any array indexing below —
    // g_chunkPainted/g_chunkOriginTick are fixed at MAX_CHUNKS, so an out-of-range
    // value (e.g. from a future JSON load) must never reach the indexing below.
    g_numChunks = std::clamp(g_numChunks, 2, MAX_CHUNKS);

    // --- Strict Hardware Texture Cap Implementation ---
    constexpr int MAX_GPU_TEXTURE_WIDTH = 16384;
    int newChunkW = sw * 2;
    if (newChunkW * g_numChunks > MAX_GPU_TEXTURE_WIDTH) {
        newChunkW = MAX_GPU_TEXTURE_WIDTH / g_numChunks;
    }
    const int newTexW = g_numChunks * newChunkW;

    const float texScale = (float)newChunkW / (float)(sw * 2);
    const double scaledPpt = ppt * (double)texScale;
    const uint64_t newTicksPerChunk = (uint64_t)((double)newChunkW / scaledPpt) + 1;

    bool changed = (g_tex.id == 0 || g_texW != newTexW ||
        std::fabs(g_pixPerTick - scaledPpt) > 1e-9);
    if (changed) {
        g_paintCancel.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(g_paintMtx);
            ClearPaintQueueLocked();
        }
        while (g_paintBusy.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        if (g_tex.id != 0) UnloadTexture(g_tex);
        g_chunkW = newChunkW;
        g_texW = newTexW;
        g_pixPerTick = scaledPpt;
        g_ticksPerChunk = newTicksPerChunk;
        g_ticksPerChunkExact = (double)newChunkW / scaledPpt;
        
        {
            std::lock_guard<std::mutex> lk(g_pixBufMtx);
            g_pixBuf.assign((size_t)newTexW * PIX_H, 0u);
        }

        Image img = {};
        img.data = g_pixBuf.data();
        img.width = newTexW;
        img.height = PIX_H;
        img.mipmaps = 1;
        img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        g_tex = LoadTextureFromImage(img);
        SetTextureFilter(g_tex, TEXTURE_FILTER_POINT);
        for (int i = 0; i < g_numChunks; ++i) g_chunkPainted[i] = false;
        g_rtNeedsFullRedraw = true;
    }

    g_tracks = &tracks;
    g_bgViewerType = viewerType;

    // Visible tick window boundaries
    int64_t  sLeft = (int64_t)currentTick - (int64_t)(plx / ppt);
    int64_t  sRight = (int64_t)currentTick + (int64_t)((sw - plx) / ppt) + 1;
    uint64_t leftTick = (uint64_t)std::max((int64_t)0, sLeft);

    bool seeked = g_seekInvalidate.exchange(false);

    // Check if visible window is outside our rendered texture buffer bounds
    bool outOfBounds = (leftTick < g_chunkOriginTick[0]) || 
                       (leftTick >= g_chunkOriginTick[g_numChunks - 1]);

    if (seeked || g_rtNeedsFullRedraw || outOfBounds) {
        g_rtNeedsFullRedraw = false;

        // Cancel and wait for background paint thread to safely pause
        g_paintCancel.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(g_paintMtx);
            ClearPaintQueueLocked();
        }
        while (g_paintBusy.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        g_windowOffsetChunks = 0;
        g_bufOriginTick = (uint32_t)leftTick;
        for (int i = 0; i < g_numChunks; ++i) {
            g_chunkPainted[i] = false;
            g_chunkOriginTick[i] = ExactChunkOrigin(g_bufOriginTick, i);
        }

        g_paintCancel.store(false, std::memory_order_release);

        {
            std::lock_guard<std::mutex> lk(g_pixBufMtx);
            std::memset(g_pixBuf.data(), 0, g_pixBuf.size() * sizeof(uint32_t));
        }

        // Synchronously paint Chunk 0 on the main thread so visible notes appear instantly
        uint32_t c0ts = GetChunkStartTick(0, g_bufOriginTick, 0);
        uint32_t c0te = GetChunkEndTick(0, g_bufOriginTick, 0);
        PaintChunkRange(0, c0ts, c0te);
        g_chunkPainted[0] = true;

        {
            std::lock_guard<std::mutex> lk(g_pixBufMtx);
            UpdateTexture(g_tex, g_pixBuf.data());
        }

        // Enqueue remaining lookahead chunks (1..g_numChunks-1) for background thread
        for (int nc = 1; nc < g_numChunks; ++nc) {
            uint32_t cts = GetChunkStartTick(nc, g_bufOriginTick, 0);
            uint32_t cte = GetChunkEndTick(nc, g_bufOriginTick, 0);
            EnqueueChunk(nc, cts, cte, false);
        }
    }
    else {
        // Upload finished chunks to GPU
        int lp = g_lastPaintedChunk.exchange(-1, std::memory_order_acquire);
        if (lp >= 0) {
            std::lock_guard<std::mutex> lk(g_pixBufMtx);
            UpdateTexture(g_tex, g_pixBuf.data());
        }

        // ---- SMOOTH SLIDING WINDOW SHIFT ----
        if (leftTick >= g_chunkOriginTick[1]) {
            // Cancel background painter before modifying pixel memory
            g_paintCancel.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(g_paintMtx);
                ClearPaintQueueLocked();
            }
            while (g_paintBusy.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            size_t shiftPx = g_chunkW;
            size_t keepPx  = g_texW - shiftPx;

            {
                std::lock_guard<std::mutex> lk(g_pixBufMtx);
                for (int y = 0; y < PIX_H; ++y) {
                    uint32_t* row = g_pixBuf.data() + (size_t)y * g_texW;
                    std::memmove(row, row + shiftPx, keepPx * sizeof(uint32_t));
                    std::memset(row + keepPx, 0, shiftPx * sizeof(uint32_t));
                }
            }

            g_bufOriginTick = g_chunkOriginTick[1];
            for (int i = 0; i < g_numChunks - 1; ++i) {
                g_chunkOriginTick[i] = g_chunkOriginTick[i + 1];
                g_chunkPainted[i]    = g_chunkPainted[i + 1];
            }
            g_chunkOriginTick[g_numChunks - 1] = ExactChunkOrigin(g_bufOriginTick, g_numChunks - 1);
            g_chunkPainted[g_numChunks - 1]    = false;

            UpdateTexture(g_tex, g_pixBuf.data());

            g_paintCancel.store(false, std::memory_order_release);

            for (int c = 0; c < g_numChunks; ++c) {
                if (!g_chunkPainted[c]) {
                    uint32_t ts = GetChunkStartTick(c, g_bufOriginTick, 0);
                    uint32_t te = GetChunkEndTick(c, g_bufOriginTick, 0);
                    EnqueueChunk(c, ts, te, false);
                }
            }
        }
    }

    // ---- UNIFIED SINGLE-PASS BLIT (Pixel-Perfect, No Stretching) ----
    {
        float texRatio = (float)(g_pixPerTick / ppt);

        float dstX = 0.f;
        float blitW = (float)sw;
        double srcX = (double)(sLeft - (int64_t)g_bufOriginTick) * g_pixPerTick;

        // At the start of playback (sLeft < 0), Tick 0 aligns at dstX on screen
        if (sLeft < 0) {
            dstX  = (float)(-(double)sLeft * ppt);
            blitW = (float)sw - dstX;
            srcX  = 0.0;
        }

        if (srcX < 0.0) srcX = 0.0;

        // Scale texture sampling width to match visible screen width proportionally
        float srcW = blitW * texRatio;

        if (srcX + (double)srcW > (double)g_texW) {
            srcW  = (float)((double)g_texW - srcX);
            blitW = srcW / texRatio;
        }

        if (srcW > 0.f && blitW > 0.f) {
            Rectangle srcRec = { (float)srcX, 0.f, srcW, (float)PIX_H };
            Rectangle dstRec = { dstX, top, blitW, uh };

            bool useRoundShader = false;
            if (g_enableRoundedNotes) {
                EnsureRoundedNoteShader();
                useRoundShader = g_roundShaderOk;
            }

            if (useRoundShader) {
                float capTexels        = (float)ROUND_CAP_TEXELS;
                float texelToScreenPxX = (srcRec.width > 0.f) ? (dstRec.width / srcRec.width) : 1.0f;
                float rowHeightPx      = dstRec.height / (float)PIX_H;
                SetShaderValue(g_roundShader, g_roundCapTexelsLoc,   &capTexels,        SHADER_UNIFORM_FLOAT);
                SetShaderValue(g_roundShader, g_roundTexelToPxXLoc,  &texelToScreenPxX, SHADER_UNIFORM_FLOAT);
                SetShaderValue(g_roundShader, g_roundRowHeightPxLoc, &rowHeightPx,      SHADER_UNIFORM_FLOAT);
                BeginShaderMode(g_roundShader);
            }

            DrawTexturePro(g_tex, srcRec, dstRec, { 0.f, 0.f }, 0.f, WHITE);

            if (useRoundShader) {
                EndShaderMode();
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
    g_paintCancel.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(g_paintMtx);
        ClearPaintQueueLocked();
    }
    while (g_paintBusy.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    g_paintCancel.store(false, std::memory_order_release);

    g_rtNeedsFullRedraw = true;   
    g_seekInvalidate.store(true); 
}

// ===================================================================
// IMPROVED COLOR MANAGEMENT
// ===================================================================
#define MAX_TRACKS 65535

static Color extendedColors[] = {
    {51, 102, 255, 255}, {255, 102, 51, 255}, {51, 255, 102, 255}, {255, 51, 129, 255},
    {51, 255, 255, 255}, {228, 51, 255, 255}, {153, 255, 51, 255}, {75, 51, 255, 255},
    {255, 204, 51, 255}, {51, 180, 255, 255}, {255, 51, 51, 255}, {51, 255, 177, 255},
    {255, 51, 204, 255}, {78, 255, 51, 255}, {153, 51, 255, 255}, {231, 255, 51, 255},
    {102, 153, 255, 255}, {255, 153, 102, 255}, {102, 255, 153, 255}, {255, 102, 180, 255},
    {102, 255, 255, 255}, {255, 102, 255, 255}, {204, 255, 102, 255}, {126, 102, 255, 255},
    {25, 51, 128, 255}, {128, 51, 25, 255}, {25, 128, 51, 255}, {128, 25, 64, 255},
    {25, 128, 128, 255}, {114, 25, 128, 255}, {76, 128, 25, 255}, {37, 25, 128, 255},
    {255, 0, 127, 255}, {127, 255, 0, 255}, {0, 127, 255, 255}, {255, 127, 0, 255},
    {127, 0, 255, 255}, {0, 255, 127, 255}, {255, 255, 0, 255}, {0, 255, 255, 255},
    {255, 192, 203, 255}, {173, 216, 230, 255}, {144, 238, 144, 255}, {255, 182, 193, 255},
    {221, 160, 221, 255}, {176, 196, 222, 255}, {255, 160, 122, 255}, {152, 251, 152, 255},
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

// Helper: Convert HSV values to RGBA Color
static Color ColorFromHSV(float hue, float saturation, float value) {
    float c = value * saturation;
    float x = c * (1.0f - std::fabs(std::fmod(hue / 60.0f, 2.0f) - 1.0f));
    float m = value - c;
    float r = 0, g = 0, b = 0;
    if (hue >= 0 && hue < 60)        { r = c; g = x; b = 0; }
    else if (hue >= 60 && hue < 120) { r = x; g = c; b = 0; }
    else if (hue >= 120 && hue < 180){ r = 0; g = c; b = x; }
    else if (hue >= 180 && hue < 240){ r = 0; g = x; b = c; }
    else if (hue >= 240 && hue < 300){ r = x; g = 0; b = c; }
    else                             { r = c; g = 0; b = x; }
    return Color{
        (unsigned char)((r + m) * 255),
        (unsigned char)((g + m) * 255),
        (unsigned char)((b + m) * 255),
        255
    };
}

// Generates default rainbow palettes inside AppData folder
static void GenerateDefaultRainbowPalettes() {
    std::string palettesDir = GetConfigPath("Palettes");
    std::error_code ec;
    std::filesystem::create_directories(palettesDir, ec);

    auto makeRainbow = [&](const std::string& name, int w, int h) {
        std::string outPath = palettesDir + "\\" + name;
        if (std::filesystem::exists(outPath, ec)) return; 

        Image img = GenImageColor(w, h, BLANK);
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        Color* pixels = (Color*)img.data;

        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float hue = (x / (float)w) * 360.0f;
                float sat = 1.0f - (y / (float)h) * 0.35f; 
                float val = 0.65f + (y / (float)h) * 0.35f; 
                pixels[y * w + x] = ColorFromHSV(hue, sat, val);
            }
        }

        ExportImage(img, outPath.c_str());
        UnloadImage(img);
    };

    makeRainbow("Rainbow_16x8.png", 16, 8);
    makeRainbow("Rainbow_16x16.png", 16, 16);
}

// Loads a custom or preset image as track color mappings
static bool LoadPaletteImage(const std::string& path) {
    if (!FileExists(path.c_str())) {
        SendNotification(360, 50, SERROR, "Palette file not found!", 3.0f);
        return false;
    }

    Image img = LoadImage(path.c_str());
    if (img.data == nullptr) {
        SendNotification(360, 50, SERROR, "Failed to parse image data!", 3.0f);
        return false;
    }

    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    Color* pixels = (Color*)img.data;
    int numPixels = img.width * img.height;

    if (numPixels == 0) {
        UnloadImage(img);
        return false;
    }

    if (!colorsInitialized) {
        InitializeTrackColors();
    }

    for (int i = 0; i < maxTracksUsed; i++) {
        int track   = i / 16;
        int channel = i % 16;

        if (img.width == 16) {
            if (img.height == 1) {
                currentTrackColors[i] = pixels[(track + channel) % 16];
            } else {
                int px = channel;
                int py = track % img.height;
                currentTrackColors[i] = pixels[py * img.width + px];
            }
        } else {
            currentTrackColors[i] = pixels[i % numPixels];
        }
    }

    UnloadImage(img);
    InvalidateNoteBuffer();
    SendNotification(280, 50, SSUCCESS, "Color palette imported!", 3.0f);
    std::cout << "+ Imported track colors from: " << path << " (" << img.width << "x" << img.height << ")" << std::endl;
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

void DrawModeSelectionMenu() {
    static std::string inputBuffer;
    static int cursorPos = 0;
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
    if (IsKeyPressed(KEY_ENTER)) currentState = STATE_LOADING;
    ClearBackground(JBG1A);
    DrawText("JIDI Player", 10, 10, 20, WHITE);
    DrawText(TextFormat("File: %s", GetFileName(selectedMidiFile.c_str())), GetScreenWidth()/2 - MeasureText(TextFormat("File: %s", GetFileName(selectedMidiFile.c_str())), 20)/2, 160, 20, LIGHTGRAY);
    if (DrawButton({(float)GetScreenWidth() / 2 - 120, 200, 240, 50}, "Load midi", JBG1B)) {
        std::string chosen = OpenMidiFileDialog();
        if (!chosen.empty()) {
            selectedMidiFile = chosen;
            inputBuffer = chosen;
            cursorPos = (int)inputBuffer.length();
            SendNotification(160, 50, SSUCCESS, "File loaded", 3.0f);
        }
    }
    if (DrawButton({(float)GetScreenWidth() / 2 - 120, 265, 240, 50}, "Start Playback", JBG1B)) {
        currentState = STATE_LOADING;
    }
    InformationVersion();
}

void DrawDetailedLoadingScreen() {
    ClearBackground(JGRAY);
    DrawText("Loading File...", GetScreenWidth() / 2 - MeasureText("Loading File...", 40) / 2, 80, 40, WHITE);
    float percentage = 0.0f;
    if (g_LoadProgress.totalBytes > 0) {
        percentage = (float)g_LoadProgress.bytesRead / (float)g_LoadProgress.totalBytes;
    }
    float barW = 400.0f;
    float barH = 20.0f;
    float barX = GetScreenWidth() / 2.0f - barW / 2.0f;
    float barY = 150.0f;
	
    DrawRectangleRounded({barX, barY, barW, barH}, 1.0f, 32, DARKGRAY);
	BeginScissorMode(barX, barY, barW * percentage, barH);
    DrawRectangleRounded({barX, barY, barW, barH}, 1.0f, 32, JLIGHTBLUE);
	EndScissorMode();
    
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
        usPerTick = MidiTiming::CalculateMicrosecondsPerTick(ev.getTempo(), ppq); 
        g_tempoSegs.push_back({ ev.tick, accumSec, usPerTick });
    }
}

static double TicksToSeconds(uint64_t tick); 

static void BuildNpsGrid(const std::vector<OptimizedTrackData>& tracks, int barWidthPx = 0) {
    g_npsGrid.clear();
    g_npsGridReady = false;
    if (g_songDurationSec <= 0.0 || g_tempoSegs.empty()) return;

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

static uint32_t TicksMinusSeconds(uint64_t targetTick, double seconds) {
    if (g_tempoSegs.empty() || seconds <= 0.0) return 0;

    double targetSec      = TicksToSeconds(targetTick);
    double windowStartSec = targetSec - seconds;
    if (windowStartSec <= 0.0) return 0;

    size_t lo = 0, hi = g_tempoSegs.size();
    while (lo + 1 < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_tempoSegs[mid].accumSec <= windowStartSec) lo = mid;
        else hi = mid;
    }
    const auto& seg     = g_tempoSegs[lo];
    double      remSec  = windowStartSec - seg.accumSec;   
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

    uint64_t rnCur = renderNotes.load(std::memory_order_relaxed);
    uint64_t rnMax = maxRenderNotes.load(std::memory_order_relaxed);
    ChunkStreamStatus cs = GetChunkStreamStatus();
    DrawText(TextFormat("Render notes: %llu / %llu ~ Chunks: %d / %d %s", rnCur, rnMax, cs.paintedChunks, cs.totalChunks, cs.streaming ? "(Streaming...)" : "(Ready)"), (int)(panelX + padding), (int)currentY, 10, WHITE);
    currentY += lineHeight;
    float barW = DWidth - padding * 2.0f;
    float barH = 8.0f;
    DrawRectangleRounded({panelX + padding, currentY, barW, barH}, 1.0f, 16, Color{0, 0, 0, 128});
    float fillW = barW * std::clamp(cs.progress / 100.0f, 0.0f, 1.0f);
    Color fillColor = cs.streaming ? YELLOW : (cs.progress >= 99.995f ? GREEN : ORANGE);
    if (fillW > 0.0f) DrawRectangleRounded({panelX + padding, currentY, fillW, barH}, 1.0f, 16, fillColor);
    DrawRectangleRoundedLinesEx({panelX + padding, currentY, barW, barH}, 1.0f, 32, 1.0f, Color{32, 32, 32, 192});
}

// ===================================================================
// PERFORMANCE DEBUG 
// ===================================================================

#define MAX_PERF_HISTORY 400

static int   perfFpsHistory[MAX_PERF_HISTORY] = {0};
static float perfFtHistory[MAX_PERF_HISTORY] = {0.0f};
static int   perfTpsHistory[MAX_PERF_HISTORY] = {0};
static float perfBufHistory[MAX_PERF_HISTORY] = {0.0f};
static float perfRenderNotesHistory[MAX_PERF_HISTORY] = {0.0f}; // % complete (renderNotes / maxRenderNotes)

static int   perfFpsMin = 0, perfFpsMax = 0;
static float perfFpsAvg = 0.0f;
static float perfFtMin  = 0.0f, perfFtMax = 0.0f, perfFtAvg = 0.0f;
static int   perfTpsMin = 0, perfTpsMax = 0;
static float perfTpsAvg = 0.0f;
static float perfRenderNotesMin = 0.0f, perfRenderNotesMax = 0.0f, perfRenderNotesAvg = 0.0f;
static uint64_t perfEvpsHistory[MAX_PERF_HISTORY] = {0};
static uint64_t perfEvpsMin = 0, perfEvpsMax = 0;
static double   perfEvpsAvg = 0.0;
static uint64_t lastCurrentVisualizerTick = 0;

// Ring buffer write cursor. Points at the most-recently-written (newest) slot.
// Starts at MAX_PERF_HISTORY - 1 so the very first write lands on index 0.
static int perfHistHead = MAX_PERF_HISTORY - 1;

// Maps a logical "age" index (0 = oldest sample .. MAX_PERF_HISTORY-1 = newest sample)
// to its physical slot in the ring buffer. Keeps every existing draw loop working
// unmodified (they just index perfXHistory[i] in oldest->newest order via this helper).
static inline int PerfIdx(int age) {
    int idx = perfHistHead + 1 + age;
    idx -= (idx >= MAX_PERF_HISTORY) ? MAX_PERF_HISTORY : 0;
    return idx;
}

// Updates the rolling performance history. Previously this shifted all 5 history
// arrays down by one element every single frame (5 * MAX_PERF_HISTORY memmoves/frame,
// i.e. 2000 element copies/frame just to make room for 1 new sample). A ring buffer
// turns that into a fixed set of O(1) writes; the min/max/avg scan below is the only
// remaining O(MAX_PERF_HISTORY) work, and it's unavoidable for a sliding-window
// min/max without a heavier structure (e.g. a monotonic deque), which isn't worth
// the complexity at N = 400.
void UpdatePerformanceHistory(int fps, float ft, int tps, float bufHealth, uint64_t evps, float renderNotesPct) {
    perfHistHead = (perfHistHead + 1 == MAX_PERF_HISTORY) ? 0 : perfHistHead + 1;

    perfFpsHistory[perfHistHead]          = fps;
    perfFtHistory[perfHistHead]           = ft;
    perfTpsHistory[perfHistHead]          = tps;
    perfBufHistory[perfHistHead]          = bufHealth;
    perfEvpsHistory[perfHistHead]         = evps;
    perfRenderNotesHistory[perfHistHead]  = renderNotesPct;

    int    fpsMin = perfFpsHistory[0], fpsMax = perfFpsHistory[0];
    float  ftMin  = perfFtHistory[0],  ftMax  = perfFtHistory[0];
    int    tpsMin = perfTpsHistory[0], tpsMax = perfTpsHistory[0];
    uint64_t evpsMin = perfEvpsHistory[0], evpsMax = perfEvpsHistory[0];
    float  rnMin  = perfRenderNotesHistory[0], rnMax = perfRenderNotesHistory[0];
    double fpsSum = 0.0, ftSum = 0.0, tpsSum = 0.0, evpsSum = 0.0, rnSum = 0.0;
    for (int i = 0; i < MAX_PERF_HISTORY; i++) {
        fpsSum  += perfFpsHistory[i];
        ftSum   += perfFtHistory[i];
        tpsSum  += perfTpsHistory[i];
        evpsSum += (double)perfEvpsHistory[i];
        rnSum   += perfRenderNotesHistory[i];
        if (perfFpsHistory[i]  < fpsMin)  fpsMin  = perfFpsHistory[i];
        if (perfFpsHistory[i]  > fpsMax)  fpsMax  = perfFpsHistory[i];
        if (perfFtHistory[i]   < ftMin)   ftMin   = perfFtHistory[i];
        if (perfFtHistory[i]   > ftMax)   ftMax   = perfFtHistory[i];
        if (perfTpsHistory[i]  < tpsMin)  tpsMin  = perfTpsHistory[i];
        if (perfTpsHistory[i]  > tpsMax)  tpsMax  = perfTpsHistory[i];
        if (perfEvpsHistory[i] < evpsMin) evpsMin = perfEvpsHistory[i];
        if (perfEvpsHistory[i] > evpsMax) evpsMax = perfEvpsHistory[i];
        if (perfRenderNotesHistory[i] < rnMin) rnMin = perfRenderNotesHistory[i];
        if (perfRenderNotesHistory[i] > rnMax) rnMax = perfRenderNotesHistory[i];
    }
    perfFpsMin = fpsMin; perfFpsMax = fpsMax; perfFpsAvg = (float)(fpsSum / MAX_PERF_HISTORY);
    perfFtMin  = ftMin;  perfFtMax  = ftMax;  perfFtAvg  = (float)(ftSum  / MAX_PERF_HISTORY);
    perfTpsMin = tpsMin; perfTpsMax = tpsMax; perfTpsAvg = (float)(tpsSum / MAX_PERF_HISTORY);
    perfEvpsMin = evpsMin; perfEvpsMax = evpsMax; perfEvpsAvg = evpsSum / MAX_PERF_HISTORY;
    perfRenderNotesMin = rnMin; perfRenderNotesMax = rnMax; perfRenderNotesAvg = (float)(rnSum / MAX_PERF_HISTORY);
}

// ===================================================================
// PERFORMANCE DEBUG PANEL 
// ===================================================================
struct PerfTier {
    float threshold;
    Color color;
};

void Draw100PercentStackedColumn(int x, int y, int height, float val, const std::vector<PerfTier>& tiers) {
    if (tiers.empty()) return;
    
    int i = 0;
    while (i < (int)tiers.size() - 2 && val >= tiers[i + 1].threshold) {
        i++;
    }
    
    float t0 = tiers[i].threshold;
    float t1 = tiers[i + 1].threshold;
    Color cTop = tiers[i].color;     
    Color cBottom = tiers[i + 1].color; 

    float percent = 0.0f;
    if (t1 > t0) {
        percent = (val - t0) / (t1 - t0);
    }
    
    if (percent < 0.0f) percent = 0.0f;
    if (percent > 1.0f) percent = 1.0f;

    int bottomH = (int)(percent * (float)height);
    if (bottomH < 0) bottomH = 0;
    if (bottomH > height) bottomH = height;
    
    int topH = height - bottomH;

    if (topH > 0) {
        DrawRectangle(x, y, 1, topH, cTop);
    }
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

    const int GW = 400; 
    const int GH = 45;  

    DrawText("Performance", cx, cy, 20, WHITE);
	cy += 25;

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

    DrawText(TextFormat("Graphics - Frames Per Second (Real-Time): %d  (%d / %d / %.0f)", perfFpsHistory[perfHistHead], perfFpsMin, perfFpsMax, perfFpsAvg), cx, cy, 10, WHITE);
    cy += 13;

    DrawRectangle(cx, cy, GW, GH, Color{0, 0, 0, 128});
    for (int i = 0; i < MAX_PERF_HISTORY; i++) {
        Draw100PercentStackedColumn(cx + i, cy, GH, (float)perfFpsHistory[PerfIdx(i)], fpsTiers);
    }

    cy += GH + 5;
    DrawText(TextFormat("Graphics - Frame Time: %.2f ms  (%.2f ms / %.2f ms / %.2f ms)", perfFtHistory[perfHistHead], perfFtMin, perfFtMax, perfFtAvg), cx, cy, 10, WHITE);
    cy += 13;

    DrawRectangle(cx, cy, GW, GH, Color{0, 0, 0, 128});
    for (int i = 0; i < MAX_PERF_HISTORY; i++) {
        Draw100PercentStackedColumn(cx + i, cy, GH, perfFtHistory[PerfIdx(i)], ftTiers);
    }

    cy += GH + 5;
    
    if (g_BassEngine.IsInitialized() && g_BassEngine.GetActiveMode() == AudioMode::BassMIDI_PreRender) {
        auto prSt = g_BassEngine.GetPreRenderStatus();
        float curHealth = perfBufHistory[perfHistHead];
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
            Draw100PercentStackedColumn(cx + i, cy, GH, perfBufHistory[PerfIdx(i)], bufTiers);
        }
    } else {
        DrawText(TextFormat("MIDI - Ticks Per Second: %d  (%d / %d / %.0f)", perfTpsHistory[perfHistHead], perfTpsMin, perfTpsMax, perfTpsAvg), cx, cy, 10, WHITE);
        cy += 13;
        DrawRectangle(cx, cy, GW, GH, Color{0, 0, 0, 128});
        for (int i = 0; i < MAX_PERF_HISTORY; i++) {
            Draw100PercentStackedColumn(cx + i, cy, GH, (float)perfTpsHistory[PerfIdx(i)], tpsTiers);
        }
    }
	cy += GH + 5;
	bool evpsRecordingOn = g_AudioEngine.IsEventCounterRecordEnabled();
	if (evpsRecordingOn) {
		DrawText(TextFormat("MIDI - Events Per Second: %llu  (%llu / %llu / %.0f)", perfEvpsHistory[perfHistHead], perfEvpsMin, perfEvpsMax, perfEvpsAvg), cx, cy, 10, WHITE);
	} else {
		DrawText("MIDI - Events Per Second: [ DISABLED ]", cx, cy, 10, WHITE);
	}
    cy += 13;
    if (evpsRecordingOn) {
        DrawRectangle(cx, cy, GW, GH, Color{0, 0, 0, 128});
        for (int i = 0; i < MAX_PERF_HISTORY; i++) {
            Draw100PercentStackedColumn(cx + i, cy, GH, (float)perfEvpsHistory[PerfIdx(i)], evpsTiers);
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
    p.speedFactor = 0.5f + (rand() % 1000) / 1000.0f; 
    p.sizeFactor  = 0.5f + (rand() % 1000) / 1000.0f;
}

static void UpdateAndDrawParticles(float dt, float bpmFactor, bool paused) {
    if (!g_particleShow) return;

    int count = std::clamp(g_particleCount, 1, 512);
    int old   = (int)g_particles.size();
    if (old != count) {
        g_particles.resize(count);
        for (int i = old; i < count; ++i)
            SpawnParticle(g_particles[i], true);
    }

    float speedBase = paused ? 0.0f
                     : g_particleSpeed * (g_particleBpm ? bpmFactor : 1.0f);

    for (auto& p : g_particles) {
        p.x -= speedBase * p.speedFactor * dt;
        if (p.x < -(g_particleSize * p.sizeFactor * 4.0f))
            SpawnParticle(p, false);
        DrawCircleV({ p.x, p.y }, g_particleSize * p.sizeFactor, g_particleColor);
    }
}

// ===================================================================
// MAIN FUNCTION
// ===================================================================
int main(int argc, char* argv[]) {
    std::cout << "+ Starting..." << std::endl;
    PreInitAudioConfig();
    bool kdmapiOk = InitializeKDMAPIStream();
    if (kdmapiOk) std::cout << "+ KDMAPI Initialized!" << std::endl;
    else std::cout << "[warn] KDMAPI init failed - switch to BassMIDI in Audio Config." << std::endl;
    if (argc > 1) {
        selectedMidiFile = argv[1];
        std::cout << "+ File selection alived!" << std::endl;
    }    
    std::cout << "+ Opening window..." << std::endl;
    SetTraceLogLevel(LOG_WARNING);
    if (g_transparentWindow) {
        SetConfigFlags(FLAG_WINDOW_TRANSPARENT);
    }    
    InitWindow(1280, 720, "JIDI Player - v1.0.4 (Build: " TOSTRING(BUILD_NUMBER) ")");
    auto ib = GetIconPNGBytes();
    if (ib.data && ib.size > 0) {
        Image icon = LoadImageFromMemory(".png", ib.data, ib.size);
        if (icon.data) { SetWindowIcon(icon); UnloadImage(icon); }
    }
    SetWindowMinSize(450, 240);
    SetWindowState(FLAG_VSYNC_HINT);
    SetExitKey(KEY_NULL);
    if (g_BassEngine.Init(GetWindowHandle())) {
        std::cout << "+ BassMIDI engine ready\n";
    } else {
        std::cout << "[warn] BassMIDI engine init failed - pre-render unavailable.\n";
    }
    LoadAudioConfig();
    rlImGuiSetup(true);
	#ifdef _WIN32
    static std::string imguiPath = GetConfigPath("imgui.ini");
    ImGui::GetIO().IniFilename = imguiPath.c_str();
	#endif
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
				static bool isThreadStarted = false;
				if (!isThreadStarted) {
					isThreadStarted = true;
					g_LoadProgress.Reset(); 
					g_LoaderThread = std::thread([&]() {
						int iPpq = 480, iTempo = (int)MidiTiming::DEFAULT_TEMPO_MICROSECONDS;
						g_loadedCCEvents = loadStreamingMidiData(selectedMidiFile, noteTracks, iPpq, iTempo, noteTotal,
						timeSigNumerator, timeSigDenominator, &g_LoadProgress, g_enableOverlapRemove);
						ppq = (uint16_t)iPpq;
						currentTempo = (uint32_t)iTempo;
						MidiLoadUsage = GetMemoryUsage();
						TotalLoadUsage = GetMemoryUsage();
						g_LoadProgress.isFinished = true;
					});
				}
                BeginDrawing();
				DrawDetailedLoadingScreen();
				g_NotificationManager.Update();
				g_NotificationManager.Draw();
				EndDrawing();
				if (g_LoadProgress.isFinished) {
					g_LoaderThread.join(); 
					isThreadStarted = false; 
					InitializeTrackColors(static_cast<int>(noteTracks.size()));
					g_sortedNoteStartTicks.clear();
					g_sortedNoteStartTicks.reserve(noteTotal);
					g_sortedNoteEndTicks.clear();
					g_sortedNoteEndTicks.reserve(noteTotal);
					g_songLastTick = 0;
					g_maxNps  = 0;
					g_maxPoly = 0;
					for (const auto& track : noteTracks) {
						for (const auto& note : track.notes) {
							g_sortedNoteStartTicks.push_back(note.startTick);
							g_sortedNoteEndTicks.push_back(note.endTick);
							if (note.endTick > g_songLastTick) g_songLastTick = note.endTick;
						}
                    }
					std::sort(g_sortedNoteStartTicks.begin(), g_sortedNoteStartTicks.end());
					std::sort(g_sortedNoteEndTicks.begin(),   g_sortedNoteEndTicks.end());
					BuildTempoSegs(ppq);
					g_songDurationSec = TicksToSeconds(g_songLastTick);
					BuildNpsGrid(noteTracks, (int)(GetScreenWidth() - 20)); 
					if (noteTracks.size() == 0) {
						currentState = STATE_MENU;
						SendNotification(400, 75, SERROR, "You need to load MIDI files first", 5.0f);
						break;
					}
				firstPause = true;
				currentTempo = MidiTiming::DEFAULT_TEMPO_MICROSECONDS;
				{
					const auto& evs = GetGlobalMidiEvents();
					if (!evs.empty() && evs[0].type == (uint8_t)EventType::TEMPO)
						currentTempo = evs[0].getTempo(); 
					g_AudioEngine.Start(evs, ppq, currentTempo);
				}
                g_AudioEngine.SetSpeed(MidiSpeed);
                g_AudioEngine.SetLooping(isLoop);
                g_AudioEngine.Pause();
                std::cout << "+ - [ Help controller ] - +" << std::endl << std::endl;

                std::cout << "- - [ Playback ] - -" << std::endl;
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

                std::cout << "- - [ Render ] - -" << std::endl;
                std::cout << "O = Slower scroll speed (+0.05x)" << std::endl;
                std::cout << "I = Faster scroll speeds (-0.05x)" << std::endl;
                std::cout << "P = Reset scroll speeds (0.50x)" << std::endl;
                std::cout << "T = Change layer" << std::endl;
                std::cout << "V = Toggle guide" << std::endl;
                std::cout << "B = Toggle beats" << std::endl << std::endl;

                std::cout << "- - [ Color ] - -" << std::endl;
                std::cout << "Keypad 1 = Randomize track colors" << std::endl;
                std::cout << "Keypad 2 = Generate completely random colors" << std::endl;
                std::cout << "Keypad 3 = Import Piano From Above colors" << std::endl;
                std::cout << "Keypad 0 = Reset track colors to original" << std::endl << std::endl; 

                std::cout << "- - [ Misc ] - -" << std::endl;
				std::cout << "F1 = Toggle HUD" << std::endl;
                std::cout << "F2 = Take Screenshot" << std::endl;
				std::cout << "F3 = Show Information" << std::endl;
                std::cout << "F4 = Show Performance" << std::endl;
                std::cout << "F8 = Audio Config (Show Options only)" << std::endl;
                std::cout << "F9 = Show Options (ImGui, See more settings)" << std::endl;
                std::cout << "F10 = Toggle VSync" << std::endl;
                std::cout << "F11 = Toggle Fullscreen (Do not return menu for because broken)" << std::endl;
                std::cout << "M = Reset maximum counter" << std::endl << std::endl;

                std::cout << "+ - [ Let's being! ] - +" << std::endl;
                std::cout << "- Scroll speed default set: " << ScrollSpeed << "x" << std::endl;
                std::cout << "+ Midi load:" << GetFileName(selectedMidiFile.c_str()) << std::endl;
                std::cout << "+ Total notes: " << FormatWithCommas(noteTotal).c_str() << " ~ Total tracks: " << noteTracks.size() << std::endl;
                std::cout << "+ Time Signature detected: " << timeSigNumerator << "/" << timeSigDenominator << std::endl;
                std::cout << "- Midi/Parse memory: " << MidiLoadUsage.workingSetMB << " MB (Committed: " << MidiLoadUsage.privateUsageMB << " MB)" << std::endl;
                std::cout << "- Result memory: " << TotalLoadUsage.workingSetMB << " MB (Committed: " << TotalLoadUsage.privateUsageMB << " MB)" << std::endl << std::endl;
                
                SetWindowState(FLAG_WINDOW_RESIZABLE);
                currentState = STATE_PLAYING;
                if (g_enableOverlapRemove) SetWindowTitle(TextFormat("JIDI Player (Build: " TOSTRING(BUILD_NUMBER) ") - %s (Overlap Removed)", GetFileName(selectedMidiFile.c_str())));
                else SetWindowTitle(TextFormat("JIDI Player (Build: " TOSTRING(BUILD_NUMBER) ") - %s", GetFileName(selectedMidiFile.c_str())));
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
					const auto& evs = GetGlobalMidiEvents();
					if (!evs.empty() && evs[0].type == (uint8_t)EventType::TEMPO)
						currentTempo = evs[0].getTempo();
					g_AudioEngine.Start(evs, ppq, currentTempo);
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
                    InvalidateNoteBuffer(); 
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
                            InvalidateNoteBuffer();
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
                        g_seekInvalidate.store(true); 
                    }
                    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) {
                        g_AudioEngine.Seek(3'000'000);
                        std::cout << "+ Seeked forward 3 seconds" << std::endl;
                    }
                    if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) {
						if (!IsTempoOverride) {
							MidiSpeed += 0.01f;
							g_AudioEngine.SetSpeed(MidiSpeed);
						} else {
							TempoSet += 1.0f;
							g_AudioEngine.SetTempoOverride(true, TempoSet);
						}
					}
					if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) {
						if (!IsTempoOverride) {
							MidiSpeed = std::max(0.01f, MidiSpeed - 0.01f);
							g_AudioEngine.SetSpeed(MidiSpeed);
						} else {
							TempoSet = std::max(20.0f, TempoSet - 1.0f);
							g_AudioEngine.SetTempoOverride(true, TempoSet);
						}
					}
					if (IsKeyPressed(KEY_S)) {
						if (!IsTempoOverride) {
							MidiSpeed = 1.00f;
							g_AudioEngine.SetSpeed(MidiSpeed);
						} else {
							float baseBpm = (currentTempo > 0) ? (60000000.0f / (float)currentTempo) : 120.0f;
							TempoSet = baseBpm;
							g_AudioEngine.SetTempoOverride(true, TempoSet);
						}
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
                    }
                    if (IsKeyPressed(KEY_E)) {
                        isEventSkip = !isEventSkip;
                        g_AudioEngine.ToggleAntiSlowdown(isEventSkip);
                        std::cout << "- Anti-Slowdown " << (isEventSkip ? "enabled" : "disabled") << std::endl; }
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
                    std::cout << "- Information " << (showDebug ? "enabled" : "disabled") << std::endl; }
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

                MidiSpeed = g_AudioEngine.GetPlaybackSpeed();
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
                uint64_t rnCur = renderNotes.load(std::memory_order_relaxed);
                uint64_t rnMax = maxRenderNotes.load(std::memory_order_relaxed);
                float renderNotesPct = rnMax > 0 ? ((float)rnCur / (float)rnMax) * 100.0f : 0.0f;
                UpdatePerformanceHistory(1 / GetFrameTime(), GetFrameTime() * 1000.0f, tps, currentBufHealth, evps, renderNotesPct);
                
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
                        double ahead     = curSec + (bufHealth * MidiSpeed); 
                        float  aheadFrac = (g_songDurationSec > 0.0) ? std::clamp((float)(ahead / g_songDurationSec), 0.f, 1.f) : 0.f;
                        float greenX = barX + blueW;
                        float greenW = (barW * aheadFrac) - blueW; 
                        if (greenW > 0.f) {
                            BeginScissorMode((int)greenX, (int)barY, (int)greenW, (int)barH);
                            DrawRectangleRounded({barX, barY, barW, barH}, roundness, segments, Color{96,192,96,128});
                            EndScissorMode();
                        }
                    }
					{
                        int curBarW = (int)barW;
                        if (g_npsGridBuiltWidth != curBarW && g_songDurationSec > 0.0)
                            BuildNpsGrid(noteTracks, curBarW); 
                    }
                    if (g_npsGridReady && g_npsGridCells > 0) {
                        const float cellW = (float)kNpsCellPx;
                        for (int i = 0; i < g_npsGridCells; ++i) {
                            float cellX   = barX + i * cellW;
                            if (cellX + cellW > barX + barW) break; 
                            uint8_t alpha = (uint8_t)(g_npsGrid[i] * 128.f);
                            if (alpha < 1) continue;
                            Color cellCol = {255, 255, 255, alpha};
                            BeginScissorMode((int)cellX, (int)barY, kNpsCellPx, (int)barH);
                            DrawRectangleRounded({barX, barY, barW, barH}, roundness, segments, cellCol);
                            EndScissorMode();
                        }
                    }
					DrawRectangleRoundedLinesEx({barX, barY, barW, barH}, roundness, segments, 2.0f, Color{32,32,32,128});
					if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && !ImGui::GetIO().WantCaptureMouse) {
                        Vector2 mp = GetMousePosition();
                        if (mp.x >= barX && mp.x <= barX + barW && mp.y >= barY && mp.y <= barY + barH) {
                            float    seekFrac = std::clamp((mp.x - barX) / barW, 0.f, 1.f);
                            uint64_t seekUs = (uint64_t)((double)seekFrac * g_songDurationSec * 1'000'000.0);
                            g_AudioEngine.SeekAbsolute(seekUs);
                            InvalidateNoteBuffer();
                            lastCounterTick = UINT64_MAX;
                        }
                    }
                } 
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
                    DrawText("[Slowdown Mode]", 10, 79, 10, JLIGHTYELLOW);
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
                    ImGui::SetNextWindowPos(ImVec2((float)GetScreenWidth() - 372.0f, 40.0f), ImGuiCond_Always);
                    ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_Always); 
                    ImGuiWindowFlags wflags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
                    if (ImGui::Begin("Options [F9]", &showOptions, wflags)) {
						if (ImGui::CollapsingHeader("Playback", ImGuiTreeNodeFlags_DefaultOpen)) {
				 
								if (ImGui::Checkbox("Enable Tempo Override", &IsTempoOverride)) {
								float baseBpm = (currentTempo > 0) ? (60000000.0f / (float)currentTempo) : 120.0f;
								if (IsTempoOverride) {
									TempoSet = baseBpm * MidiSpeed;
									g_AudioEngine.SetTempoOverride(true, TempoSet);
								} else {
									g_AudioEngine.SetTempoOverride(false, TempoSet);
								}
							}

							if (!IsTempoOverride) {
								ImGui::PushItemWidth(180.0f);
								if (ImGui::SliderFloat("Speed", &MidiSpeed, 0.01f, 10.0f, "%.3fx")) {
									g_AudioEngine.SetSpeed(MidiSpeed);
								}
								ImGui::PopItemWidth();
								ImGui::SameLine();
								if (ImGui::Button("Reset")) { MidiSpeed = 1.0f; g_AudioEngine.SetSpeed(MidiSpeed); }
							} else {
								float baseBpm = (currentTempo > 0) ? (60000000.0f / (float)currentTempo) : 120.0f;
								if (ImGui::SliderFloat("Set Tempo (BPM)", &TempoSet, 20.0f, 512.0f, "%.3f BPM")) {
									g_AudioEngine.SetTempoOverride(true, TempoSet);
								}
							}
							ImGui::Separator();
				 
							if (ImGui::Checkbox("Enable loop", &isLoop)) {
								g_AudioEngine.SetLooping(isLoop);
							}
							ImGui::TextUnformatted("Loop A/B  (J = Set A, K = Set B)");

							ImGui::Checkbox("Snap to beat", &g_loopSnapToBeats);
							ImGui::SameLine();
							ImGui::TextDisabled("(?)");
							if (ImGui::IsItemHovered())
								ImGui::SetTooltip("When enabled, A/B points snap to the nearest beat boundary.\nUse the offset fields below to fine-tune.");

							uint64_t tpbDisp = (ppq > 0)
								? (static_cast<uint64_t>(ppq) * 4u) / (timeSigDenominator ? timeSigDenominator : 4u)
								: 1u;
							if (tpbDisp == 0) tpbDisp = 1;

							uint64_t curBeat = currentVisualizerTick / tpbDisp + 1;
							ImGui::Text("Now: beat %llu  (tick %llu)", (uint64_t)curBeat,
							            (uint64_t)currentVisualizerTick);

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

							if (abActive && isLoop)
								ImGui::TextColored(ImVec4(0.2f,1.0f,0.4f,1.0f), "Loop A/B active");
							else if (abActive && !isLoop)
								ImGui::TextColored(ImVec4(1.0f,0.8f,0.2f,1.0f), "A/B set — enable loop to activate");
							else
								ImGui::TextDisabled("A/B not set (full-song loop)");
							ImGui::Separator();
				 
							if (ImGui::Checkbox("Toggle Event Skip", &isEventSkip)) {
								g_AudioEngine.ToggleAntiSlowdown(isEventSkip);
							}
							ImGui::SameLine();
								bool eventCounterRecordUI = g_AudioEngine.IsEventCounterRecordEnabled();
								if (ImGui::Checkbox("Ev/s Show (Information only)", &eventCounterRecordUI)) {
									g_AudioEngine.ToggleEventCounterRecord(eventCounterRecordUI);
								}
				 
							ImGui::Spacing();
							if (ImGui::Button("« -10s"))  g_AudioEngine.Seek(-10'000'000LL);
							ImGui::SameLine();
							if (ImGui::Button("« -3s"))   g_AudioEngine.Seek( -3'000'000LL);
							ImGui::SameLine();
							if (ImGui::Button("+3s »"))   g_AudioEngine.Seek(  3'000'000LL);
							ImGui::SameLine();
							if (ImGui::Button("+10s »"))  g_AudioEngine.Seek( 10'000'000LL);
						}
						
						if (g_BassEngine.GetActiveMode() == AudioMode::KDMAPI)
							DrawLagSimulatorPanel(g_AudioEngine);
						else {
                            ImGui::Separator();
							ImGui::TextDisabled("Lag Simulator disabled (BassMIDI mode active).");
						}
				 
						if (ImGui::CollapsingHeader("Render", ImGuiTreeNodeFlags_DefaultOpen)) {
							ImGui::PushItemWidth(180.0f);
							if (ImGui::SliderFloat("Scroll Speed", &ScrollSpeed, 0.05f, 4.0f, "%.2fx"))
							ImGui::PopItemWidth();
							ImGui::SameLine();
							if (ImGui::Button("Reset##Render")) ScrollSpeed = 0.5f;
							
							if (ImGui::SliderInt("Render Chunks", &g_numChunks, 2, MAX_CHUNKS, "%d")) {
								g_numChunks = std::clamp(g_numChunks, 2, MAX_CHUNKS);
								InvalidateNoteBuffer();
							}
							if (ImGui::IsItemHovered()) {
								ImGui::SetTooltip("How many texture chunks to keep painted ahead of playback (1 current + lookahead).\nHigher = smoother scrolling on fast songs, more background render work.\nChanging this repaints the whole buffer.");
							}
				 
							ImGui::Checkbox("Show Guide", &showGuide);
							ImGui::SameLine();
							ImGui::Checkbox("Show Beats", &showBeats);
							
							if (ImGui::Checkbox("Complete Overlap Remove", &g_enableOverlapRemove)) {}
                            if (ImGui::IsItemHovered()) {
								ImGui::SetTooltip("Enable to filter overlaps during parsing.\nNote: Requires reloading the MIDI file to update counters, NPS, and Polyphony.");
							}
                            if (ImGui::Checkbox("Render Overlap Remove", &g_enableRenderOverlapRemove)) {
								InvalidateNoteBuffer();
							}
							if (ImGui::Checkbox("Toggle Rounded Notes", &g_enableRoundedNotes)) {
								InvalidateNoteBuffer();
							}

							int layerIdx = (g_viewerType == ViewerType::TickLayer) ? 1 : 0;
							const char* layers[] = { "Track Layer", "Tick Layer" };
							if (ImGui::Combo("Layer", &layerIdx, layers, IM_ARRAYSIZE(layers))) {
								g_viewerType = (layerIdx == 1) ? ViewerType::TickLayer : ViewerType::TrackLayer;
                                InvalidateNoteBuffer();
							}
						}
				 
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

                            ImGui::Checkbox("Transparent Window", &g_transparentWindow);
                            if (ImGui::IsItemHovered()) {
								ImGui::SetTooltip("Enable Transparent Window, Be may requires restart application.");
							}
							
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
				 
							bool vsync = IsWindowState(FLAG_VSYNC_HINT);
							if (ImGui::Checkbox("VSync", &vsync)) {
								if (vsync) SetWindowState(FLAG_VSYNC_HINT);
								else       ClearWindowState(FLAG_VSYNC_HINT);
							}
							ImGui::SameLine();
				 
							bool fsNow = IsWindowFullscreen();
							if (ImGui::Checkbox("Fullscreen", &fsNow)) {
								ToggleBorderlessWindowed();
							}
						}
				 
						if (ImGui::CollapsingHeader("Colors")) {
							if (ImGui::Button("Randomize")) RandomizeTrackColors();
							ImGui::SameLine();
							if (ImGui::Button("Generate Random")) GenerateRandomTrackColors();
							ImGui::SameLine();
							if (ImGui::Button("Default##ColorNotes")) ResetTrackColors();
				 
							if (ImGui::Button("Import Piano From Above")) {
								if (!LoadColorsFromPianoFromAbove())
									SendNotification(410, 50, SERROR, "PFA config not found!", 3.0f);
							}

                            ImGui::Separator();
                            ImGui::TextUnformatted("Image Palettes");

                            std::string palettesDir = GetConfigPath("Palettes");
                            static bool directorySetupDone = false;
                            if (!directorySetupDone) {
                                GenerateDefaultRainbowPalettes();
                                directorySetupDone = true;
                            }

                            static std::vector<std::string> s_paletteFiles;
                            static float s_scanTimer = 5.0f; 
                            s_scanTimer += GetFrameTime();
                            if (s_scanTimer >= 5.0f) {
                                s_scanTimer = 0.0f;
                                s_paletteFiles.clear();
                                std::error_code ec;
                                if (std::filesystem::exists(palettesDir, ec)) {
                                    for (const auto& entry : std::filesystem::directory_iterator(palettesDir, ec)) {
                                        if (entry.is_regular_file(ec)) {
                                            auto ext = entry.path().extension().string();
                                            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp") {
                                                s_paletteFiles.push_back(entry.path().filename().string());
                                            }
                                        }
                                    }
                                }
                            }

                            static int s_selectedPaletteIdx = -1;
                            if (!s_paletteFiles.empty()) {
                                std::vector<const char*> items;
                                for (const auto& f : s_paletteFiles) items.push_back(f.c_str());
                                
                                ImGui::SetNextItemWidth(220.0f);
                                if (ImGui::Combo("Palette Presets", &s_selectedPaletteIdx, items.data(), (int)items.size())) {
                                    if (s_selectedPaletteIdx >= 0 && s_selectedPaletteIdx < (int)s_paletteFiles.size()) {
                                        std::string fullPath = palettesDir + "\\" + s_paletteFiles[s_selectedPaletteIdx];
                                        LoadPaletteImage(fullPath);
                                    }
                                }
                            } else {
                                ImGui::TextDisabled("No preset files found in Palettes folder.");
                            }

                            if (ImGui::Button("Generate Rainbow Presets")) {
                                GenerateDefaultRainbowPalettes();
                                s_scanTimer = 5.0f; 
                                SendNotification(280, 50, SSUCCESS, "Default files generated!", 3.0f);
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Reload Palette Presets")) {
                                s_scanTimer = 5.0f; 
                                if (s_selectedPaletteIdx >= 0 && s_selectedPaletteIdx < (int)s_paletteFiles.size()) {
                                    std::string reloadPath = palettesDir + "\\" + s_paletteFiles[s_selectedPaletteIdx];
                                    LoadPaletteImage(reloadPath);
                                } else {
                                    SendNotification(250, 50, SINFORMATION, "Palettes reloaded!", 3.0f);
                                }
                            }

                            ImGui::Spacing();
                            static char s_customPalettePath[512] = "";
                            ImGui::SetNextItemWidth(220.0f);
							ImGui::Text("Patch Palettes");
                            ImGui::InputText("##CustomPalPath", s_customPalettePath, sizeof(s_customPalettePath));
                            ImGui::SameLine();
                            if (ImGui::Button("Import File")) {
                                if (strlen(s_customPalettePath) > 0) {
                                    LoadPaletteImage(s_customPalettePath);
                                }
                            }
						}
                    }
					ImGui::End();
				}
				if (IsAudioConfigPanelOpen()) {
					DrawAudioConfigPanel();
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
    g_BassEngine.Shutdown();    
    TerminateKDMAPIStream();    
    if (g_roundShaderOk) { UnloadShader(g_roundShader); g_roundShaderOk = false; }
	rlImGuiShutdown();
    CloseWindow();
    return 0;
}