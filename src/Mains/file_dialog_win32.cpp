#include "file_dialog_win32.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
// GetOpenFileNameW lives in comdlg32.lib — this pragma pulls it in so the
// link works out of the box without needing to edit xmake.lua/CMakeLists
// to add the syslink separately.
#pragma comment(lib, "comdlg32.lib")
#endif

std::string OpenMidiFileDialog() {
#ifdef _WIN32
    wchar_t fileBuf[MAX_PATH] = L"";

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = GetActiveWindow(); // the raylib window is the active window when this is called
    ofn.lpstrFilter = L"MIDI Files (*.mid;*.midi)\0*.mid;*.midi\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFile   = fileBuf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"Open MIDI File";
    // OFN_FILEMUSTEXIST/PATHMUSTEXIST: reject anything that doesn't
    // actually exist, so the app never receives a bogus path. NOCHANGEDIR:
    // GetOpenFileNameW can otherwise silently change the process's current
    // working directory to wherever the user browsed to, which would break
    // any other code in the app relying on relative paths (soundfonts,
    // config, etc.) — this stops that side effect.
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) {
        // The rest of the codebase works in UTF-8 std::string/char*
        // throughout (GetFileName(), std::string filenames passed to the
        // MIDI parser, etc.) — convert the wide-char result back to that
        // rather than pushing wchar_t further into the app.
        int len = WideCharToMultiByte(CP_UTF8, 0, fileBuf, -1, nullptr, 0, nullptr, nullptr);
        if (len > 1) {
            std::string result(len - 1, '\0'); // len includes the null terminator
            WideCharToMultiByte(CP_UTF8, 0, fileBuf, -1, result.data(), len, nullptr, nullptr);
            return result;
        }
    }
#endif
    return std::string();
}