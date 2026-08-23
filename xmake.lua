add_rules("mode.debug", "mode.release")
add_requires("raylib")
add_requires("imgui", { configs = { shared = false } })
add_requires("nlohmann_json")

-- ─────────────────────────────────────────────────────────────────────────────
-- Expected file layout for BASS (ship DLLs alongside the .exe):
--
--   external/
--     bass/
--       include/          ← bass.h, bassmidi.h
--       lib/x64/          ← bass.lib, bassmidi.lib
--       bin/x64/          ← bass.dll, bassmidi.dll  (copied to output by after_build)
--   src/Mains/
--     bass_backend.cpp    ← pre-render engine
--   header/
--     bass_backend.hpp
--     AudioConfigPanel.hpp
-- ─────────────────────────────────────────────────────────────────────────────

-- ── Main visualizer target ────────────────────────────────────────────────────
target("jidi-player")
    set_kind("binary")
    set_languages("c99", "c++23")
    add_files("src/Mains/*.cpp")           -- picks up bass_backend.cpp automatically
    add_files("external/rlImGui/rlImGui.cpp")
    if is_plat("windows") then
        if os.isfile("resources/icon.rc") then
            add_files("resources/icon.rc")
        end
    end

    -- ── Build-number header generation ────────────────────────────────────────
    before_build(function(target)
        local build_number_file  = "src/Paths/build_number.txt"
        local output_header_file = "header/build_info.hpp"

        local file = io.open(build_number_file, "r")
        local build_number = 0
        if file then
            build_number = tonumber(file:read("*a")) or 0
            file:close()
        end

        build_number = build_number + 1

        file = io.open(build_number_file, "w")
        if file then
            file:write(tostring(build_number))
            file:close()
        end

        os.mkdir(path.directory(output_header_file))
        file = io.open(output_header_file, "w")
        if file then
            print("Generating build_info.hpp with build number: " .. build_number)
            file:write("#pragma once\n")
            file:write("#define BUILD_NUMBER " .. build_number .. "\n")
            file:close()
        end
    end)

    -- ── Post-build: copy BASS DLLs next to the .exe ───────────────────────────
    -- NOTE: no top-level local helpers — after_build runs in a sandboxed scope
    -- and cannot see locals defined outside the target block.
    after_build(function(target)
		local out_dir  = target:targetdir()
		local bass_bin = "external/bass/bin/x64"
		for _, dll in ipairs({ "bass.dll", "bassmidi.dll", "bassflac.dll" }) do
			local src = bass_bin .. "/" .. dll
			if os.isfile(src) then
				os.cp(src, out_dir)
				print("[BASS] copied " .. dll .. " → " .. out_dir)
			end
		end
	end)

    -- ── Packages ──────────────────────────────────────────────────────────────
    add_packages("raylib", "imgui", "nlohmann_json")

    -- ── Include directories ───────────────────────────────────────────────────
    add_includedirs(
        "external",
        "external/rlImGui",
        "external/bass/include",   -- bass.h, bassmidi.h
        "header"
    )

    -- ── Link directories ──────────────────────────────────────────────────────
    add_linkdirs(
        "external",
        "external/bass/lib/x64"    -- bass.lib, bassmidi.lib
    )

    -- ── Preprocessor defines ──────────────────────────────────────────────────
    add_defines(
        "RAYGUI_STANDALONE",
        "WINRT_LEAN_AND_MEAN",
        "_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNING"
    )

    -- ── Libraries ─────────────────────────────────────────────────────────────
    add_links(
        "OmniMIDI_Win64",          -- KDMAPI (original path; kept for fallback)
        "bass",                    -- BASS audio engine
        "bassmidi"                 -- BASS MIDI plugin
    )
    add_syslinks("winmm", "Psapi", "runtimeobject")

    -- ── Compiler flags ────────────────────────────────────────────────────────
    add_cxxflags("/EHsc", { force = true })
    set_optimize("fastest")

-- ── MIDI core player target ───────────────────────────────────────────────────
target("midicore")
    set_kind("binary")
    set_languages("c++23")
    add_files("src/Test/midicore.cpp")
    add_includedirs("external", "header")
    add_linkdirs("external")
    add_links("OmniMIDI_Win64")
    add_syslinks("winmm")
    set_optimize("fastest")

-- ── Timing test utility ───────────────────────────────────────────────────────
target("timing-test")
    set_kind("binary")
    set_languages("c++23")
    add_files("src/Test/timing_test.cpp")
    add_includedirs("header")
    set_optimize("fastest")

-- ── MIDI hex dump utility ─────────────────────────────────────────────────────
target("midi-hex-dump")
    set_kind("binary")
    set_languages("c++23")
    add_files("src/Test/midi_hex_dump.cpp")
    set_optimize("fastest")

-- ── MIDI file analyzer ────────────────────────────────────────────────────────
target("midi-analyzer")
    set_kind("binary")
    set_languages("c++23")
    add_files("src/Test/midi_analyzer.cpp")
    add_includedirs("header")
    set_optimize("fastest")

-- ── Track loading test ────────────────────────────────────────────────────────
target("track-test")
    set_kind("binary")
    set_languages("c++23")
    add_files("src/Test/track_test.cpp")
    add_includedirs("header")
    set_optimize("fastest")

--
-- If you want to known more usage about xmake, please see https://xmake.io
--