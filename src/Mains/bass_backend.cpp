#ifdef _WIN32

// Prevent Windows GDI and USER API name collisions with Raylib
#define NOGDI
#define NOUSER
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <bass.h>
#include <bassmidi.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>

#include "bass_backend.hpp"
#include "visualizer.hpp"

#ifndef BASS_ATTRIB_MIDI_VOICES
#  define BASS_ATTRIB_MIDI_VOICES    0x12000
#endif
#ifndef BASS_ATTRIB_MIDI_SRC
#  define BASS_ATTRIB_MIDI_SRC       0x12001
#endif
#ifndef BASS_MIDI_EVENTS_RAW
#  define BASS_MIDI_EVENTS_RAW       0x10000
#endif

static constexpr DWORD kDecodeChunk = 1920u;

struct BassPreRenderEngine::Impl {
    bool        initialized  = false;
    BassConfig  cfg;
    void*       hwnd         = nullptr;

    HSTREAM     midiStream   = 0;
    HSTREAM     pushStream   = 0;

    std::vector<SoundFontEntry> fonts;
    mutable std::mutex          fontMutex;
    float volume = 1.0f;

    std::thread               prThread;
    std::atomic<bool>         prRunning{false};
    std::atomic<float>        prProgress{0.0f};
    std::atomic<bool>         prDone{false};
    std::atomic<bool>         prError{false};
    std::atomic<bool>         prSeekFlush{false};
    
    // Dynamic Rebuild Cache
    std::vector<MidiEvent>    cachedEvents;
    int                       cachedPpq = 480;
    uint32_t                  cachedInitialTempo = 500000;
    uint64_t                  cachedTotalMicros = 0;
    float                     playbackSpeed = 1.0f;
    float                     lastRenderedSpeed = 1.0f;
    std::atomic<bool>         prNeedsRebuild{false};
    std::atomic<bool>         prNeedsResize{false};

    // Circular Ring Buffer logic
    std::vector<float>        pcm;
    mutable std::mutex        pcmMutex;
    std::condition_variable   pcmCV;
    uint64_t                  pcmWritePos = 0; 
    uint64_t                  pcmReadPos  = 0; 
    std::atomic<bool>         seekReq{false};
    std::atomic<uint64_t>     seekTargetMicros{0};

    std::string               prErrorMsg;
    mutable std::mutex        prMsgMutex;

    bool MakeStream(DWORD flags) {
        if (midiStream) { BASS_StreamFree(midiStream); midiStream = 0; }
        midiStream = BASS_MIDI_StreamCreate(16, flags | BASS_SAMPLE_FLOAT, cfg.sampleRate);
        if (!midiStream) {
            std::cerr << "[BassEngine] BASS_MIDI_StreamCreate failed: " << BASS_ErrorGetCode() << "\n";
            return false;
        }
        BASS_ChannelSetAttribute(midiStream, BASS_ATTRIB_MIDI_VOICES, (float)cfg.voices);
        BASS_ChannelSetAttribute(midiStream, BASS_ATTRIB_VOL, volume);
        
        std::lock_guard<std::mutex> lk(fontMutex);
        ApplyFontsLocked(); 
        return true;
    }

    void ApplyFontsLocked() {
        std::vector<BASS_MIDI_FONT> active;
        for (auto& fe : fonts) {
            if (!fe.enabled || !fe.handle) continue;
            BASS_MIDI_FONT bfont{};
            bfont.font   = (HSOUNDFONT)fe.handle;
            bfont.preset = fe.preset;
            bfont.bank   = (fe.bank < 0) ? 0 : fe.bank;
            active.push_back(bfont);
        }
        BASS_MIDI_StreamSetFonts(midiStream, active.empty() ? nullptr : active.data(), (DWORD)active.size());
    }

    bool LoadFont(SoundFontEntry& fe) {
        if (fe.handle) { BASS_MIDI_FontFree((HSOUNDFONT)fe.handle); fe.handle = 0; }
        HSOUNDFONT h = BASS_MIDI_FontInit(fe.path.c_str(), 0);
        if (!h) return false;
        fe.handle = (uint32_t)h;
        return true;
    }

    void UnloadFont(SoundFontEntry& fe) {
        if (fe.handle) { BASS_MIDI_FontFree((HSOUNDFONT)fe.handle); fe.handle = 0; }
    }

    uint8_t GetDynamicVelIgnore(double bufHealthSec) const {
        const float scaleMax = cfg.lowVelScaleMaxSec;
        if (scaleMax <= 0.0f) return cfg.velocityIgnore; 
        if (bufHealthSec <= 0.0) return 127; 
        if (bufHealthSec >= (double)scaleMax) return cfg.velocityIgnore; 
        float t = (float)(bufHealthSec / (double)scaleMax); 
        float result = 127.0f + t * ((float)cfg.velocityIgnore - 127.0f);
        return (uint8_t)std::clamp((int)result, (int)cfg.velocityIgnore, 127);
    }
};

static DWORD CALLBACK PreRenderStreamProc(HSTREAM handle, void *buffer, DWORD length, void *user) {
    auto* impl = static_cast<BassPreRenderEngine::Impl*>(user);
    std::lock_guard<std::mutex> lk(impl->pcmMutex);
    
    uint64_t availableSamples = impl->pcmWritePos - impl->pcmReadPos;
    uint64_t requestedSamples = length / sizeof(float);
    
    if (availableSamples == 0) {
        if (impl->prRunning.load() && !impl->prDone.load()) {
            memset(buffer, 0, length);
            return length;
        } else {
            return BASS_STREAMPROC_END; 
        }
    }
    
    uint64_t samplesToRead = std::min(availableSamples, requestedSamples);
    float* outBuf = static_cast<float*>(buffer);
    
    uint64_t readIdx = impl->pcmReadPos % impl->pcm.size();
    uint64_t firstChunk = std::min(samplesToRead, (uint64_t)impl->pcm.size() - readIdx);
    
    memcpy(outBuf, &impl->pcm[readIdx], firstChunk * sizeof(float));
    if (firstChunk < samplesToRead) {
        memcpy(outBuf + firstChunk, &impl->pcm[0], (samplesToRead - firstChunk) * sizeof(float));
    }
    
    impl->pcmReadPos += samplesToRead;
    impl->pcmCV.notify_all();

    if (samplesToRead < requestedSamples) {
        memset(outBuf + samplesToRead, 0, (requestedSamples - samplesToRead) * sizeof(float));
        if (impl->prDone.load()) {
            return (DWORD)(samplesToRead * sizeof(float)) | BASS_STREAMPROC_END;
        }
    }

    return length;
}

BassPreRenderEngine g_BassEngine;

BassPreRenderEngine::BassPreRenderEngine() {
    impl = new Impl();
}

BassPreRenderEngine::~BassPreRenderEngine() {
    Shutdown(); 
    delete impl; 
    impl = nullptr;
}

bool BassPreRenderEngine::Init(void* hwnd) {
    if (!impl) return false;
    if (impl->initialized) return true;
    impl->hwnd = hwnd;

    int updateMs = std::max(1, impl->cfg.latencyMs);
    BASS_SetConfig(BASS_CONFIG_UPDATEPERIOD, updateMs);
    BASS_SetConfig(BASS_CONFIG_BUFFER, updateMs * 2);

    if (!BASS_Init(-1, impl->cfg.sampleRate, 0, (HWND)hwnd, nullptr)) {
        DWORD err = BASS_ErrorGetCode();
        if (err != BASS_ERROR_ALREADY) {
            std::cerr << "[BassEngine] BASS_Init failed: " << err << "\n";
            return false;
        }
    }
    impl->initialized = true;

    if (impl->cfg.mode == AudioMode::BassMIDI_RT) {
        impl->MakeStream(0);
    }
    return true;
}

void BassPreRenderEngine::Shutdown() {
    if (!impl || !impl->initialized) return;
    CancelPreRender();
    if (impl->pushStream) { BASS_StreamFree(impl->pushStream); impl->pushStream = 0; }
    if (impl->midiStream) { BASS_StreamFree(impl->midiStream); impl->midiStream = 0; }
    {
        std::lock_guard<std::mutex> lk(impl->fontMutex);
        for (auto& fe : impl->fonts) impl->UnloadFont(fe);
        impl->fonts.clear();
    }
    BASS_Free();
    impl->initialized = false;
}

bool BassPreRenderEngine::IsInitialized() const {
    return impl && impl->initialized;
}

void BassPreRenderEngine::ApplyConfig(const BassConfig& cfg) {
    if (!impl) return;
    bool modeChanged   = (cfg.mode    != impl->cfg.mode);
    bool voiceChanged  = (cfg.voices  != impl->cfg.voices);
    impl->cfg = cfg;

    if (impl->initialized) {
        if (impl->midiStream && voiceChanged) {
            BASS_ChannelSetAttribute(impl->midiStream, BASS_ATTRIB_MIDI_VOICES, (float)cfg.voices);
        }
        if (modeChanged && cfg.mode == AudioMode::BassMIDI_RT) {
            impl->MakeStream(0);
        }
    }
}

const BassConfig& BassPreRenderEngine::GetConfig() const {
    static BassConfig dummy{};
    return impl ? impl->cfg : dummy;
}

void BassPreRenderEngine::SetMode(AudioMode m) {
    if (!impl) return;
    impl->cfg.mode = m;
    if (impl->initialized) {
        if (m == AudioMode::BassMIDI_RT) {
            if (!impl->midiStream) impl->MakeStream(0);
        } else if (m == AudioMode::BassMIDI_PreRender) {
            if (impl->midiStream) BASS_ChannelStop(impl->midiStream);
        }
    }
}

void BassPreRenderEngine::SetVoices(int v) {
    if (!impl) return;
    impl->cfg.voices = std::clamp(v, 1, 262144);
    if (impl->cfg.mode == AudioMode::BassMIDI_PreRender && impl->prRunning.load()) {
        impl->prNeedsRebuild.store(true);
        impl->pcmCV.notify_all();
    } else if (impl->midiStream) {
        BASS_ChannelSetAttribute(impl->midiStream, BASS_ATTRIB_MIDI_VOICES, (float)impl->cfg.voices);
    }
}

void BassPreRenderEngine::SetVelocityIgnore(uint8_t v) { 
    if (!impl) return;
    impl->cfg.velocityIgnore = v; 
    if (impl->cfg.mode == AudioMode::BassMIDI_PreRender && impl->prRunning.load()) {
        impl->prNeedsRebuild.store(true);
        impl->pcmCV.notify_all();
    }
}
void BassPreRenderEngine::SetSfxEnabled(bool on) { 
    if (!impl) return;
    impl->cfg.sfxEnabled = on; 
    if (impl->cfg.mode == AudioMode::BassMIDI_PreRender && impl->prRunning.load()) {
        impl->prNeedsRebuild.store(true);
        impl->pcmCV.notify_all();
    }
}
void BassPreRenderEngine::SetPlaybackSpeed(float speed) {
    if (!impl) return;
    float old = impl->playbackSpeed;
    impl->playbackSpeed = std::max(0.01f, speed);
    if (impl->cfg.mode == AudioMode::BassMIDI_PreRender && impl->prRunning.load() && old != impl->playbackSpeed) {
        impl->prNeedsRebuild.store(true);
        impl->pcmCV.notify_all();
    }
}
void BassPreRenderEngine::SetPreRenderBufferSec(float sec) { 
    if (!impl) return;
    float old = impl->cfg.preRenderBufferSec;
    impl->cfg.preRenderBufferSec = std::clamp(sec, 1.0f, 1800.0f); 
    if (impl->cfg.mode == AudioMode::BassMIDI_PreRender && impl->prRunning.load() && old != impl->cfg.preRenderBufferSec) {
        impl->prNeedsResize.store(true);
        impl->pcmCV.notify_all();
    }
}
void BassPreRenderEngine::SetLowBufferMode(bool on) { if (impl) impl->cfg.lowBufferMode = on; }

AudioMode BassPreRenderEngine::GetActiveMode() const { return impl ? impl->cfg.mode : AudioMode::KDMAPI; }

bool BassPreRenderEngine::AddSoundFont(const std::string& path) {
    if (!impl) return false;
    SoundFontEntry fe;
    fe.path    = path;
    fe.enabled = true;
    if (!impl->LoadFont(fe)) {
        std::cerr << "[BassEngine] SoundFont load failed: " << path << "\n";
        return false;
    }
    std::lock_guard<std::mutex> lk(impl->fontMutex);
    impl->fonts.push_back(std::move(fe));
    if (impl->cfg.mode == AudioMode::BassMIDI_PreRender && impl->prRunning.load()) {
        impl->prNeedsRebuild.store(true);
        impl->pcmCV.notify_all();
    } else if (impl->midiStream) {
        impl->ApplyFontsLocked(); 
    }
    return true;
}

void BassPreRenderEngine::RemoveSoundFont(size_t index) {
    if (!impl) return;
    std::lock_guard<std::mutex> lk(impl->fontMutex);
    if (index >= impl->fonts.size()) return;
    impl->UnloadFont(impl->fonts[index]);
    impl->fonts.erase(impl->fonts.begin() + (ptrdiff_t)index);
    if (impl->cfg.mode == AudioMode::BassMIDI_PreRender && impl->prRunning.load()) {
        impl->prNeedsRebuild.store(true);
        impl->pcmCV.notify_all();
    } else if (impl->midiStream) {
        impl->ApplyFontsLocked(); 
    }
}

void BassPreRenderEngine::MoveSoundFontUp(size_t index) {
    if (!impl || index == 0) return;
    std::lock_guard<std::mutex> lk(impl->fontMutex);
    if (index >= impl->fonts.size()) return;
    std::swap(impl->fonts[index], impl->fonts[index - 1]);
    if (impl->cfg.mode == AudioMode::BassMIDI_PreRender && impl->prRunning.load()) {
        impl->prNeedsRebuild.store(true);
        impl->pcmCV.notify_all();
    } else if (impl->midiStream) {
        impl->ApplyFontsLocked(); 
    }
}

void BassPreRenderEngine::MoveSoundFontDown(size_t index) {
    if (!impl) return;
    std::lock_guard<std::mutex> lk(impl->fontMutex);
    if (index + 1 >= impl->fonts.size()) return;
    std::swap(impl->fonts[index], impl->fonts[index + 1]);
    if (impl->cfg.mode == AudioMode::BassMIDI_PreRender && impl->prRunning.load()) {
        impl->prNeedsRebuild.store(true);
        impl->pcmCV.notify_all();
    } else if (impl->midiStream) {
        impl->ApplyFontsLocked(); 
    }
}

void BassPreRenderEngine::SetSoundFontEnabled(size_t index, bool on) {
    if (!impl) return;
    {
        std::lock_guard<std::mutex> lk(impl->fontMutex);
        if (index >= impl->fonts.size()) return;
        impl->fonts[index].enabled = on;
    }
    if (impl->cfg.mode == AudioMode::BassMIDI_PreRender && impl->prRunning.load()) {
        impl->prNeedsRebuild.store(true);
        impl->pcmCV.notify_all();
    } else if (impl->midiStream) {
        impl->ApplyFontsLocked(); 
    }
}

const std::vector<SoundFontEntry>& BassPreRenderEngine::GetSoundFonts() const {
    static const std::vector<SoundFontEntry> empty;
    return impl ? impl->fonts : empty;
}

void BassPreRenderEngine::ReloadAllSoundFonts() {
    if (!impl) return;
    std::lock_guard<std::mutex> lk(impl->fontMutex);
    for (auto& fe : impl->fonts) impl->LoadFont(fe);
    if (impl->cfg.mode == AudioMode::BassMIDI_PreRender && impl->prRunning.load()) {
        impl->prNeedsRebuild.store(true);
        impl->pcmCV.notify_all();
    } else if (impl->midiStream) {
        impl->ApplyFontsLocked(); 
    }
}

void BassPreRenderEngine::StartPreRender(const void* rawEvents, size_t eventCount, int ppq, uint32_t initialTempo, uint64_t totalMicros) {
    if (!impl || impl->cfg.mode != AudioMode::BassMIDI_PreRender) return;
    CancelPreRender();

    const MidiEvent* evPtr = static_cast<const MidiEvent*>(rawEvents);
    impl->cachedEvents.assign(evPtr, evPtr + eventCount);
    impl->cachedPpq = ppq;
    impl->cachedInitialTempo = initialTempo;
    impl->cachedTotalMicros = totalMicros;
    impl->lastRenderedSpeed = impl->playbackSpeed;

    const uint32_t sr = impl->cfg.sampleRate;
    DWORD device = BASS_GetDevice(); 

    // Setup Ring Buffer
    {
        std::lock_guard<std::mutex> lk(impl->pcmMutex);
        size_t bufferSize = (size_t)(impl->cfg.preRenderBufferSec * sr * 2); 
        if (bufferSize < sr * 2) bufferSize = sr * 2; 
        impl->pcm.assign(bufferSize, 0.0f);
        impl->pcmWritePos = 0;
        impl->pcmReadPos = 0;
        impl->seekReq.store(false);
    }

    if (impl->pushStream) { BASS_StreamFree(impl->pushStream); }
    impl->pushStream = BASS_StreamCreate(sr, 2, BASS_SAMPLE_FLOAT, PreRenderStreamProc, impl);
    
    if (impl->pushStream) {
        BASS_ChannelSetAttribute(impl->pushStream, BASS_ATTRIB_VOL, impl->volume);
    }

    impl->prProgress.store(0.0f);
    impl->prDone.store(false);
    impl->prError.store(false);
    impl->prRunning.store(true);
    impl->prSeekFlush.store(false);
    impl->prNeedsRebuild.store(false);
    impl->prNeedsResize.store(false);

    impl->prThread = std::thread([this, device, sr]() mutable {
        BASS_SetDevice(device);

        auto buildStream = [&]() -> HSTREAM {
            const uint8_t velIgnore  = impl->cfg.velocityIgnore;
            const bool    sfxEnabled = impl->cfg.sfxEnabled;
            const float   speed      = impl->playbackSpeed;

            auto writeVlq = [](std::vector<uint8_t>& buf, uint32_t v) {
                uint8_t tmp[4]; int n = 0;
                do { tmp[n++] = static_cast<uint8_t>(v & 0x7F); v >>= 7; } while (v);
                for (int i = n - 1; i >= 0; --i) buf.push_back(tmp[i] | (i ? 0x80u : 0u));
            };

            std::vector<uint8_t> trackData;
            trackData.reserve(impl->cachedEvents.size() * 4);

            writeVlq(trackData, 0);
            trackData.push_back(0xFF); trackData.push_back(0x51); trackData.push_back(0x03);
            uint32_t initialT = (uint32_t)(impl->cachedInitialTempo / speed);
            trackData.push_back(static_cast<uint8_t>((initialT >> 16) & 0xFF));
            trackData.push_back(static_cast<uint8_t>((initialT >>  8) & 0xFF));
            trackData.push_back(static_cast<uint8_t>( initialT        & 0xFF));

            uint32_t lastWrittenTick = 0;
            for (const auto& ev : impl->cachedEvents) {
                auto et = static_cast<EventType>(ev.type);
                bool skip = false;
                if (et == EventType::NOTE_ON) {
                    uint8_t vel = ev.getVelocity();
                    if (vel > 0 && vel <= velIgnore) skip = true;
                } else if (!sfxEnabled) {
                    if (et == EventType::CC || et == EventType::PITCH_BEND || et == EventType::PROGRAM_CHANGE || et == EventType::CHANNEL_PRESSURE) skip = true;
                }

                if (skip) continue;

                uint32_t delta = ev.tick - lastWrittenTick;
                lastWrittenTick = ev.tick;

                if (et == EventType::TEMPO) {
                    uint32_t t = (uint32_t)(ev.getTempo() / speed);
                    writeVlq(trackData, delta);
                    trackData.push_back(0xFF); trackData.push_back(0x51); trackData.push_back(0x03);
                    trackData.push_back((uint8_t)((t >> 16) & 0xFF));
                    trackData.push_back((uint8_t)((t >>  8) & 0xFF));
                    trackData.push_back((uint8_t)( t        & 0xFF));
                } else if (et == EventType::NOTE_ON) {
                    writeVlq(trackData, delta);
                    trackData.push_back(static_cast<uint8_t>(0x90 | ev.channel));
                    trackData.push_back(ev.getNote());
                    trackData.push_back(ev.getVelocity());
                } else if (et == EventType::NOTE_OFF) {
                    writeVlq(trackData, delta);
                    trackData.push_back(static_cast<uint8_t>(0x80 | ev.channel));
                    trackData.push_back(ev.getNote());
                    trackData.push_back(ev.getVelocity());
                } else if (et == EventType::CC) {
                    writeVlq(trackData, delta);
                    trackData.push_back(static_cast<uint8_t>(0xB0 | ev.channel));
                    trackData.push_back(ev.getCCController());
                    trackData.push_back(ev.getCCValue());
                } else if (et == EventType::PITCH_BEND) {
                    writeVlq(trackData, delta);
                    trackData.push_back(static_cast<uint8_t>(0xE0 | ev.channel));
                    trackData.push_back(ev.getPitchBendLSB());
                    trackData.push_back(ev.getPitchBendMSB());
                } else if (et == EventType::PROGRAM_CHANGE) {
                    writeVlq(trackData, delta);
                    trackData.push_back(static_cast<uint8_t>(0xC0 | ev.channel));
                    trackData.push_back(ev.getValue());
                }
            }

            // Append silent EOT padding
            {
                uint32_t lastTempo = impl->cachedInitialTempo;
                for (const auto& ev : impl->cachedEvents)
                    if (static_cast<EventType>(ev.type) == EventType::TEMPO)
                        lastTempo = ev.getTempo();

                double secsPerTick = (lastTempo / 1000000.0) / impl->cachedPpq;
                uint32_t tailTicks = (secsPerTick > 0.0)
                    ? (uint32_t)(3.0 / secsPerTick)
                    : (impl->cachedPpq * 6);

                for (uint8_t ch = 0; ch < 16; ++ch) {
                    writeVlq(trackData, 0); trackData.push_back(0xB0 | ch); trackData.push_back(123); trackData.push_back(0);
                    writeVlq(trackData, 0); trackData.push_back(0xB0 | ch); trackData.push_back(64);  trackData.push_back(0);
                }
                writeVlq(trackData, 0); trackData.push_back(0xBF); trackData.push_back(7); trackData.push_back(0);
                writeVlq(trackData, 0); trackData.push_back(0x9F); trackData.push_back(60); trackData.push_back(1);
                writeVlq(trackData, tailTicks);
                trackData.push_back(0x8F); trackData.push_back(60); trackData.push_back(0);
                writeVlq(trackData, 0);
                trackData.push_back(0xFF); trackData.push_back(0x2F); trackData.push_back(0x00); 
            }

            std::vector<uint8_t> midiFile;
            midiFile.reserve(14 + 8 + trackData.size());
            const uint8_t hdr[] = { 'M','T','h','d', 0,0,0,6, 0,0, 0,1, (uint8_t)((impl->cachedPpq >> 8) & 0xFF), (uint8_t)(impl->cachedPpq & 0xFF) };
            midiFile.insert(midiFile.end(), std::begin(hdr), std::end(hdr));

            uint32_t tlen = static_cast<uint32_t>(trackData.size());
            const uint8_t tkhdr[] = { 'M','T','r','k', (uint8_t)((tlen >> 24) & 0xFF), (uint8_t)((tlen >> 16) & 0xFF), (uint8_t)((tlen >>  8) & 0xFF), (uint8_t)(tlen & 0xFF) };
            midiFile.insert(midiFile.end(), std::begin(tkhdr), std::end(tkhdr));
            midiFile.insert(midiFile.end(), trackData.begin(), trackData.end());

            HSTREAM s = BASS_MIDI_StreamCreateFile(TRUE, midiFile.data(), 0, static_cast<QWORD>(midiFile.size()), BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT, sr);
            if (s) {
                BASS_ChannelSetAttribute(s, BASS_ATTRIB_MIDI_VOICES, static_cast<float>(impl->cfg.voices));
                std::lock_guard<std::mutex> lk(impl->fontMutex);
                std::vector<BASS_MIDI_FONT> active;
                for (auto& fe : impl->fonts) {
                    if (!fe.enabled || !fe.handle) continue;
                    BASS_MIDI_FONT bf{};
                    bf.font   = static_cast<HSOUNDFONT>(fe.handle);
                    bf.preset = fe.preset;
                    bf.bank   = (fe.bank < 0) ? 0 : fe.bank;
                    active.push_back(bf);
                }
                BASS_MIDI_StreamSetFonts(s, active.empty() ? nullptr : active.data(), static_cast<DWORD>(active.size()));
            }
            return s;
        };

        HSTREAM decStream = buildStream();
        if (!decStream) {
            std::lock_guard<std::mutex> lk(impl->prMsgMutex);
            impl->prErrorMsg = "BASS_MIDI_StreamCreateFile failed: " + std::to_string(BASS_ErrorGetCode());
            impl->prError.store(true);
            impl->prRunning.store(false);
            return;
        }

        QWORD totalBytes = BASS_ChannelGetLength(decStream, BASS_POS_BYTE);
        if (totalBytes == (QWORD)-1) totalBytes = (QWORD)(((double)impl->cachedTotalMicros / 1000000.0 / impl->lastRenderedSpeed) * sr * 2 * sizeof(float));

        std::vector<float> chunk(kDecodeChunk / sizeof(float));
        QWORD decoded = 0;
        
        while (impl->prRunning.load()) {
            
            // Dynamic Stream Rebuild (Settings or Speed) -> Flushes audio
            if (impl->prNeedsRebuild.exchange(false)) {
                uint64_t currentVirtualMicros = this->GetPositionMicros(); 
                
                if (decStream) BASS_StreamFree(decStream);
                decStream = buildStream();
                impl->lastRenderedSpeed = impl->playbackSpeed;

                if (!decStream) {
                    impl->prError.store(true);
                    break;
                }
                
                totalBytes = BASS_ChannelGetLength(decStream, BASS_POS_BYTE);
                if (totalBytes == (QWORD)-1) totalBytes = (QWORD)(((double)impl->cachedTotalMicros / 1000000.0 / impl->lastRenderedSpeed) * sr * 2 * sizeof(float));

                uint64_t targetPhysicalMicros = (uint64_t)(currentVirtualMicros / impl->lastRenderedSpeed);
                QWORD bytePos = (QWORD)((targetPhysicalMicros / 1000000.0) * sr * 2 * sizeof(float));
                BASS_ChannelSetPosition(decStream, bytePos, BASS_POS_BYTE);
                
                QWORD actualBytePos = BASS_ChannelGetPosition(decStream, BASS_POS_BYTE);
                
                std::lock_guard<std::mutex> lk(impl->pcmMutex);
                std::fill(impl->pcm.begin(), impl->pcm.end(), 0.0f);
                impl->pcmWritePos = actualBytePos / sizeof(float);
                impl->pcmReadPos = impl->pcmWritePos;
                
                impl->prSeekFlush.store(true); 
                
                decoded = actualBytePos;
                impl->prDone.store(false);
                impl->seekReq.store(false);
                if (totalBytes > 0) impl->prProgress.store(std::min(1.0f, (float)decoded / (float)totalBytes));
                continue;
            }
            
            // Dynamic Ring Buffer Resize -> Gapless transfer
            if (impl->prNeedsResize.exchange(false)) {
                size_t desiredCapacity = (size_t)(impl->cfg.preRenderBufferSec * sr * 2);
                if (desiredCapacity < sr * 2) desiredCapacity = sr * 2;
                
                std::lock_guard<std::mutex> lk(impl->pcmMutex);
                if (impl->pcm.size() != desiredCapacity) {
                    std::vector<float> newPcm(desiredCapacity, 0.0f);
                    uint64_t avail = impl->pcmWritePos - impl->pcmReadPos;
                    if (avail > desiredCapacity) {
                        impl->pcmReadPos = impl->pcmWritePos - desiredCapacity;
                        avail = desiredCapacity;
                    }
                    for (uint64_t i = 0; i < avail; ++i) {
                        uint64_t absPos = impl->pcmReadPos + i;
                        newPcm[absPos % desiredCapacity] = impl->pcm[absPos % impl->pcm.size()];
                    }
                    impl->pcm = std::move(newPcm);
                }
            }

            // Seek event processing
            if (impl->seekReq.exchange(false)) {
                uint64_t targetVirtualMicros = impl->seekTargetMicros.load();
                uint64_t targetPhysicalMicros = (uint64_t)(targetVirtualMicros / impl->lastRenderedSpeed);
                QWORD bytePos = (QWORD)((targetPhysicalMicros / 1000000.0) * sr * 2 * sizeof(float));
                BASS_ChannelSetPosition(decStream, bytePos, BASS_POS_BYTE);
                
                QWORD actualBytePos = BASS_ChannelGetPosition(decStream, BASS_POS_BYTE);
                std::lock_guard<std::mutex> lk(impl->pcmMutex);
                impl->pcmWritePos = actualBytePos / sizeof(float);
                impl->pcmReadPos = impl->pcmWritePos;
                decoded = actualBytePos;
                impl->prDone.store(false);
            }

            uint64_t space = 0;
            uint64_t capacity = impl->pcm.size();
            const uint64_t minSpace = kDecodeChunk / sizeof(float);
            {
                std::unique_lock<std::mutex> lk(impl->pcmMutex);
                impl->pcmCV.wait(lk, [&]{ 
                    return !impl->prRunning.load() || impl->seekReq.load() || impl->prNeedsRebuild.load() || impl->prNeedsResize.load() || (capacity - (impl->pcmWritePos - impl->pcmReadPos) >= minSpace); 
                });
                
                if (!impl->prRunning.load()) break;
                if (impl->seekReq.load() || impl->prNeedsRebuild.load() || impl->prNeedsResize.load()) continue; 
                space = capacity - (impl->pcmWritePos - impl->pcmReadPos);
            }

            if (space == 0) continue;

            uint64_t floatsToRead = std::min((uint64_t)(kDecodeChunk / sizeof(float)), space);
            DWORD got = BASS_ChannelGetData(decStream, chunk.data(), (DWORD)(floatsToRead * sizeof(float)) | BASS_DATA_FLOAT);
            
            if (got == (DWORD)-1 || got == 0) {
                impl->prDone.store(true);
                impl->prProgress.store(1.0f);
                std::unique_lock<std::mutex> lk(impl->pcmMutex);
                impl->pcmCV.wait(lk, [&]{ return !impl->prRunning.load() || impl->seekReq.load() || impl->prNeedsRebuild.load() || impl->prNeedsResize.load(); });
                continue;
            }
            
            uint64_t gotFloats = got / sizeof(float);
            {
                std::lock_guard<std::mutex> lk(impl->pcmMutex);
                uint64_t writeIdx = impl->pcmWritePos % capacity;
                uint64_t firstPart = std::min(gotFloats, capacity - writeIdx);
                
                memcpy(&impl->pcm[writeIdx], chunk.data(), firstPart * sizeof(float));
                if (firstPart < gotFloats) {
                    memcpy(&impl->pcm[0], chunk.data() + firstPart, (gotFloats - firstPart) * sizeof(float));
                }
                impl->pcmWritePos += gotFloats;
            }
            
            decoded += got;
            if (totalBytes > 0) impl->prProgress.store(std::min(1.0f, (float)decoded / (float)totalBytes));

            {
                uint64_t capacity = impl->pcm.size();
                uint64_t used     = 0;
                { std::lock_guard<std::mutex> lk(impl->pcmMutex); used = impl->pcmWritePos - impl->pcmReadPos; }

                if (used > capacity * 8 / 10)
                    std::this_thread::yield(); 

                if (decStream) {
                    double health = (double)used / 2.0 / impl->cfg.sampleRate;
                    const double fullSec = 2.0;
                    int minV = impl->cfg.lowBufferMinVoices;
                    int maxV = impl->cfg.voices;
                    int targetVoices;
                    if (health >= fullSec) {
                        targetVoices = maxV;
                    } else {
                        float t = (float)(health / fullSec); 
                        targetVoices = (int)(minV + t * (maxV - minV));
                        targetVoices = std::clamp(targetVoices, minV, maxV);
                    }
                    static int s_lastVoices = -1;
                    if (targetVoices != s_lastVoices) {
                        BASS_ChannelSetAttribute(decStream, BASS_ATTRIB_MIDI_VOICES, (float)targetVoices);
                        s_lastVoices = targetVoices;
                    }
                }
            }
        }
        BASS_StreamFree(decStream);

        if (!impl->prRunning.load()) return;
        impl->prRunning.store(false);
    });
}

void BassPreRenderEngine::CancelPreRender() {
    if (!impl) return;
    impl->prRunning.store(false);
    impl->pcmCV.notify_all(); 
    if (impl->prThread.joinable()) impl->prThread.join();
}

PreRenderStatus BassPreRenderEngine::GetPreRenderStatus() const {
    PreRenderStatus s{};
    if (!impl) return s;
    s.busy     = impl->prRunning.load();
    s.progress = impl->prProgress.load();
    s.done     = impl->prDone.load();
    s.error    = impl->prError.load();
    if (s.error) {
        std::lock_guard<std::mutex> lk(impl->prMsgMutex);
        s.errorMsg = impl->prErrorMsg;
    }
    return s;
}

double BassPreRenderEngine::GetBufferHealthSeconds() const {
    if (!impl || impl->cfg.mode != AudioMode::BassMIDI_PreRender) return 0.0;
    std::lock_guard<std::mutex> lk(impl->pcmMutex);
    if (impl->pcmWritePos >= impl->pcmReadPos) {
        return (double)(impl->pcmWritePos - impl->pcmReadPos) / 2.0 / impl->cfg.sampleRate;
    }
    return 0.0;
}

void BassPreRenderEngine::SendMidiData(uint32_t msg) {
    if (!impl || !impl->midiStream) return;
    const uint8_t status  = msg & 0xFF;
    const uint8_t data1   = (msg >> 8) & 0xFF;
    const uint8_t data2   = (msg >> 16) & 0xFF;
    double bufHealth = GetBufferHealthSeconds();
    uint8_t velIgnore = impl->GetDynamicVelIgnore(bufHealth);
    if ((status & 0xF0) == 0x90 && data2 <= velIgnore && data2 > 0) return;
    if (!impl->cfg.sfxEnabled) {
        uint8_t type = status & 0xF0;
        if (type == 0xB0 || type == 0xE0 || type == 0xC0 || type == 0xD0) return;
    }
    BASS_MIDI_StreamEvents(impl->midiStream, BASS_MIDI_EVENTS_RAW, &msg, 3);
}

void BassPreRenderEngine::Play() {
    if (!impl) return;
    if (impl->cfg.mode == AudioMode::BassMIDI_PreRender) {
        if (!impl->pushStream) return;

        if (BASS_ChannelIsActive(impl->pushStream) == BASS_ACTIVE_STOPPED) {
            const double minFillSec = 1.0;
            double health = GetBufferHealthSeconds();
            if (health < minFillSec && impl->prRunning.load()) {
                for (int tries = 0; tries < 3000 && impl->prRunning.load(); ++tries) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    health = GetBufferHealthSeconds();
                    if (health >= minFillSec) break;
                }
            }
        }

        bool restart = impl->prSeekFlush.exchange(false);
        BASS_ChannelPlay(impl->pushStream, restart ? TRUE : FALSE);
    } else {
        if (impl->midiStream) BASS_ChannelPlay(impl->midiStream, FALSE);
    }
}

void BassPreRenderEngine::Pause() {
    if (!impl) return;
    HSTREAM s = (impl->cfg.mode == AudioMode::BassMIDI_PreRender) ? impl->pushStream : impl->midiStream;
    if (s) BASS_ChannelPause(s);
}

void BassPreRenderEngine::Stop() {
    if (!impl) return;
    HSTREAM s = (impl->cfg.mode == AudioMode::BassMIDI_PreRender) ? impl->pushStream : impl->midiStream;
    if (s) { 
        BASS_ChannelStop(s); 
        if (impl->cfg.mode == AudioMode::BassMIDI_PreRender) {
            impl->seekTargetMicros.store(0);
            impl->seekReq.store(true);
            impl->pcmCV.notify_all();
            impl->prSeekFlush.store(true);
        } else {
            BASS_ChannelSetPosition(s, 0, BASS_POS_BYTE); 
        }
    }
}

void BassPreRenderEngine::SeekTo(uint64_t seekVirtualMicros) {
    if (!impl) return;
    if (impl->cfg.mode == AudioMode::BassMIDI_PreRender) {
        if (impl->pushStream) {
            impl->seekTargetMicros.store(seekVirtualMicros);
            impl->seekReq.store(true);
            impl->pcmCV.notify_all(); 
            impl->prSeekFlush.store(true);
        }
    } else if (impl->midiStream) {
        QWORD bytePos = BASS_ChannelSeconds2Bytes(impl->midiStream, (double)seekVirtualMicros / 1'000'000.0);
        BASS_ChannelSetPosition(impl->midiStream, bytePos, BASS_POS_BYTE);
    }
}

bool BassPreRenderEngine::IsPlaying() const {
    if (!impl) return false;
    HSTREAM s = (impl->cfg.mode == AudioMode::BassMIDI_PreRender) ? impl->pushStream : impl->midiStream;
    return s && (BASS_ChannelIsActive(s) == BASS_ACTIVE_PLAYING);
}

bool BassPreRenderEngine::IsPaused() const {
    if (!impl) return false;
    HSTREAM s = (impl->cfg.mode == AudioMode::BassMIDI_PreRender) ? impl->pushStream : impl->midiStream;
    return s && (BASS_ChannelIsActive(s) == BASS_ACTIVE_PAUSED);
}

uint64_t BassPreRenderEngine::GetPositionMicros() const {
    if (!impl) return 0;
    if (impl->cfg.mode == AudioMode::BassMIDI_PreRender && impl->pushStream) {
        DWORD queuedBytes = BASS_ChannelGetData(impl->pushStream, NULL, BASS_DATA_AVAILABLE);
        int64_t floatPos = 0;
        {
            std::lock_guard<std::mutex> lk(impl->pcmMutex);
            floatPos = (int64_t)impl->pcmReadPos - (queuedBytes / sizeof(float));
        }
        if (floatPos < 0) floatPos = 0;
        uint64_t physicalMicros = (uint64_t)((double)floatPos / 2.0 / impl->cfg.sampleRate * 1'000'000.0);
        return (uint64_t)(physicalMicros * impl->lastRenderedSpeed);
    } else if (impl->midiStream) {
        QWORD pos = BASS_ChannelGetPosition(impl->midiStream, BASS_POS_BYTE);
        return (uint64_t)(BASS_ChannelBytes2Seconds(impl->midiStream, pos) * 1'000'000.0);
    }
    return 0;
}

void BassPreRenderEngine::SetVolume(float v) {
    if (!impl) return;
    impl->volume = std::clamp(v, 0.0f, 1.0f);
    if (impl->midiStream) BASS_ChannelSetAttribute(impl->midiStream, BASS_ATTRIB_VOL, impl->volume);
    if (impl->pushStream) BASS_ChannelSetAttribute(impl->pushStream, BASS_ATTRIB_VOL, impl->volume);
}

float BassPreRenderEngine::GetVolume() const {
    return impl ? impl->volume : 1.0f;
}

#endif // _WIN32