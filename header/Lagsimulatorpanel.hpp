// =============================================================
// LagSimulatorPanel.hpp
// =============================================================
#pragma once
#include "imgui.h"
#include "midioutput.hpp"
#include <cmath>
#include <cstdint>
#include <cstdio>

static constexpr int64_t kLagSimMin     =         512;
static constexpr int64_t kLagSimMax     = 134217728LL;
static constexpr int64_t kLagSimDefault =       65536;

extern int64_t s_lagSimEps;
static void FormatEps(char* buf, size_t bufsz, int64_t v)
{
    if (v == 0) { snprintf(buf, bufsz, "0"); return; }
    char tmp[32]; int pos = 0;
    int64_t n = v; int group = 0;
    while (n > 0) {
        if (group && group % 3 == 0) tmp[pos++] = ',';
        tmp[pos++] = '0' + (char)(n % 10);
        n /= 10; group++;
    }
    size_t w = 0;
    for (int i = pos - 1; i >= 0 && w + 1 < bufsz; --i)
        buf[w++] = tmp[i];
    buf[w] = '\0';
}

inline void DrawLagSimulatorPanel(MidiOutputEngine& engine)
{
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.22f, 0.12f, 0.32f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.32f, 0.18f, 0.46f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.42f, 0.22f, 0.58f, 1.00f));
    bool open = ImGui::CollapsingHeader("Slowdown mode");
    ImGui::PopStyleColor(3);
    if (!open) return;

    ImGui::Indent(8.0f);
    ImGui::Spacing();

    bool enabled = (engine.GetSimulateEventsPerSecond() > 0);
    if (ImGui::Checkbox("Enable slowdown mode", &enabled))
        engine.SetSimulateEventsPerSecond(enabled ? s_lagSimEps : 0);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Throttles MIDI output to N events/second.\n"
            "Inspired by PFA For legit run.");

    ImGui::SameLine();
    bool smooth = engine.GetLagSmoothRender();
    if (ImGui::Checkbox("Smooth Render", &smooth)) {
        engine.SetLagSmoothRender(smooth);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When ON: visualizer scrolls smoothly but audio drifts late.\nWhen OFF: visualizer physically stutters holding with audio.");
    }

    if (!enabled) {
        ImGui::TextDisabled("(disabled - playback runs at full speed)");
        ImGui::Unindent(8.0f);
        return;
    }

    ImGui::Spacing();
    static const int64_t kStep     =  1024;
    static const int64_t kStepFast = 65536; 
    ImGui::PushItemWidth(100.0f); 
    if (ImGui::InputScalar("##eps", ImGuiDataType_S64, &s_lagSimEps, &kStep, &kStepFast, "%lld")) {
        if (s_lagSimEps < kLagSimMin) s_lagSimEps = kLagSimMin;
        if (s_lagSimEps > kLagSimMax) s_lagSimEps = kLagSimMax;
        engine.SetSimulateEventsPerSecond(s_lagSimEps);
    }
    ImGui::PopItemWidth();

    if (ImGui::IsItemHovered()) {
        char fmtMin[32], fmtMax[32], fmtDef[32];
        FormatEps(fmtMin, sizeof(fmtMin), kLagSimMin);
        FormatEps(fmtMax, sizeof(fmtMax), kLagSimMax);
        FormatEps(fmtDef, sizeof(fmtDef), kLagSimDefault);
        ImGui::BeginTooltip();
        ImGui::Text("Range: %s - %s  |  Default: %s", fmtMin, fmtMax, fmtDef);
        ImGui::TextDisabled("Drag: +/- 1,024   Control+drag: +/- 65,536");
        ImGui::EndTooltip();
    }

    ImGui::SameLine();
    ImGui::Text("Events / sec");

    char buf[32];
    FormatEps(buf, sizeof(buf), s_lagSimEps);
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", buf);

    ImGui::Spacing();
    struct Preset { const char* label; int64_t eps; const char* tip; };
    static constexpr Preset kPresets[] = {
        { "Potato",   	512,         "Fishy usage." 						  				},
        { "Lower",   	1024,        "Catastrophic - barely a tick per burst" 				},
        { "Low",   		8192,        "Heavy lag, clear chord smear"           				},
        { "Mid",   		32768,       "Noticeable on dense passages"           				},
        { "Default",  	65536,       "Balanced starting point"                				},
        { "Standard",	262144,  	 "Different than Default preset x4"       				},
        { "Standard+",  524288,      "Near-real-time, light stutter only"     				},
        { "Fast",  		1048576,     "Different than Standard preset x4"      				},
        { "Fast+",  	2097152,     "Different than Standard preset x8"      				},
        { "Faster",  	4194304,     "Different than Fast preset x4"      	  				},
        { "Faster+",  	8388608,     "Different than Faster preset x2"        				},
        { "Powerful",  	16777216LL,  "Different than Faster preset x4"        				},
        { "Powerful+",  20971520LL,  "Synth Powerful Than Syndrv/OmniMIDI (Non-skip event)"	},
        { "Uncapped", 	134217728LL, "Effectively unlimited (2^27)"           				},
    };

    ImGui::TextDisabled("Presets:");
    ImGui::SameLine();
    ImGuiStyle& style = ImGui::GetStyle();
    float window_visible_x2 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;

    for (int i = 0; i < 14; ++i) {
        const auto& p = kPresets[i];
        bool isCurrent = (s_lagSimEps == p.eps);
        
        if (isCurrent) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.18f, 0.52f, 1.0f));
        ImGui::SmallButton(p.label);
        
        if (ImGui::IsItemClicked()) {
            s_lagSimEps = p.eps;
            engine.SetSimulateEventsPerSecond(s_lagSimEps);
        }
        if (isCurrent) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%lld eps", p.tip, (long long)p.eps);

        if (i < 13) {
            float last_button_x2 = ImGui::GetItemRectMax().x;
            float next_button_x2 = last_button_x2 + style.ItemSpacing.x + ImGui::CalcTextSize(kPresets[i+1].label).x + style.FramePadding.x * 2.0f;
            if (next_button_x2 < window_visible_x2) {
                ImGui::SameLine();
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    bool  lagging = engine.IsSimulateLagActive();
    float t       = (float)ImGui::GetTime();

    if (lagging) {
        float pulse = 0.5f + 0.5f * std::sin(t * 10.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.15f + 0.25f * pulse, 0.15f, 1.0f));
        ImGui::TextWrapped("EV/s LIMITED");
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.4f, 1.0f));
        ImGui::TextWrapped("OK");
        ImGui::PopStyleColor();
    }

    ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
        lagging ? ImVec4(0.80f, 0.12f, 0.12f, 1.0f)
                : ImVec4(0.12f, 0.70f, 0.22f, 1.0f));
    ImGui::ProgressBar(lagging ? 0.0f : 1.0f, ImVec2(-1.0f, 5.0f), "");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Tip: low EV/s (< 8,192) + Anti-Slowdown OFF = stuck notes during lag bursts.");
    ImGui::PopStyleColor();

    ImGui::Unindent(8.0f);
}