// bass_backend.hpp — BassMIDI pre-render / real-time audio backend
#pragma once
#ifdef _WIN32

#include <string>
#include <vector>
#include <cstdint>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>

// ── Audio backend mode ────────────────────────────────────────────────────────
enum class AudioMode : uint8_t {
    KDMAPI           = 0,  // KDMAPI / OmniMIDI
    BassMIDI_RT,           // BassMIDI Real-Time
    BassMIDI_PreRender,    // BassMIDI Pre-Render
    SpectatorAudio,        // External audio file (MP3/OGG/FLAC/WAV)
};

// ── Soundfont list entry ──────────────────────────────────────────────────────
struct SoundFontEntry {
    std::string path;
    bool        enabled = true;
    int         bank    = -1;   
    int         preset  = -1;   
    uint32_t    handle  = 0;    
};

// ── Per-mode config (persists across mode switches) ──────────────────────────
struct BassConfig {
    AudioMode mode              = AudioMode::KDMAPI;

    // Pre-render
    float   preRenderBufferSec  = 60.0f;  
    int     voices              = 512;    
    uint8_t velocityIgnore      = 2;      
    bool    lowBufferMode       = false;  
    bool    sfxEnabled          = true;   

    // Audio output device settings (Applied at BASS_Init)
    uint32_t sampleRate         = 48000;  // <-- 48kHz Default
    int      latencyMs          = 10;     // <-- 10ms Latency Default

    // Low-buffer voice scaling: linearly reduces voices from cfg.voices (at 2s health)
    // down to lowBufferMinVoices (at 0s health) so decode catches up faster.
    int      lowBufferMinVoices = 16;

    // Low-buffer velocity ignore scaling (RT mode only).
    // At health < lowVelScaleMaxSec, GetDynamicVelIgnore scales up toward 127.
    // Set to 0 to disable.
    float    lowVelScaleMaxSec  = 0.5f;
};

// ── Pre-render progress ───────────────────────────────────────────────────────
struct PreRenderStatus {
    bool    busy     = false;
    float   progress = 0.0f;   
    bool    done     = false;
    bool    error    = false;
    std::string errorMsg;
};

class BassPreRenderEngine {
public:
    BassPreRenderEngine();
    ~BassPreRenderEngine();

    bool    Init(void* hwnd);
    void    Shutdown();
    bool    IsInitialized() const;

    void    ApplyConfig(const BassConfig& cfg);
    const   BassConfig& GetConfig() const;

    void    SetMode(AudioMode m);
    void    SetVoices(int v);
    void    SetVelocityIgnore(uint8_t v);
    void    SetPreRenderBufferSec(float sec);
    void    SetLowBufferMode(bool on);
    void    SetSfxEnabled(bool on);
    void    SetPlaybackSpeed(float speed);

    AudioMode GetActiveMode() const;

    bool    AddSoundFont(const std::string& path);      
    void    RemoveSoundFont(size_t index);
    void    MoveSoundFontUp(size_t index);              
    void    MoveSoundFontDown(size_t index);            
    void    SetSoundFontEnabled(size_t index, bool on);
    const   std::vector<SoundFontEntry>& GetSoundFonts() const;
    void    ReloadAllSoundFonts();

    // Spectator Audio (MP3/OGG/FLAC/WAV)
    bool    LoadSpectatorAudioFile(const std::string& path);

    void    StartPreRender(const void* rawEvents, size_t eventCount,
                           int ppq, uint32_t initialTempoBPM,
                           uint64_t totalMicros);
    void    CancelPreRender();
    PreRenderStatus GetPreRenderStatus() const;
    double  GetBufferHealthSeconds() const;

    void    SendMidiData(uint32_t msg);
    void    SendSysEx(const void* data, size_t length);

    void    Play();
    void    Pause();
    void    Stop();
    void    SeekTo(uint64_t seekMicros);

    bool     IsPlaying()  const;
    bool     IsPaused()   const;
    uint64_t GetPositionMicros() const;

    void     SetVolume(float v);
    float    GetVolume() const;

    struct Impl;
private:
    Impl* impl = nullptr;
};

extern BassPreRenderEngine g_BassEngine;

#ifndef BASS_DISPATCH_DEFINED
#define BASS_DISPATCH_DEFINED
extern "C" void SendDirectData(unsigned long data);

// Route MIDI correctly preventing double playback
inline void DispatchMidiOut(uint32_t msg) {
    if (g_BassEngine.IsInitialized()) {
        AudioMode mode = g_BassEngine.GetActiveMode();
        if (mode == AudioMode::BassMIDI_RT) {
            g_BassEngine.SendMidiData(msg);
        } else if (mode == AudioMode::KDMAPI) {
            SendDirectData((unsigned long)msg);
        }
    } else {
        SendDirectData((unsigned long)msg);
    }
}

void DispatchSysExOut(const void* data, size_t length);
#endif // BASS_DISPATCH_DEFINED

#endif // _WIN32