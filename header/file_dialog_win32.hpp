#pragma once
#include <string>

// Native Windows "Open File" dialog for picking a .mid/.midi file.
// Returns the selected path, or an empty string if the dialog was
// cancelled or failed.
//
// Implemented in file_dialog_win32.cpp so THIS header (and everything
// that includes it) never needs to pull in <windows.h>/<commdlg.h> —
// keeps the rest of the codebase free of windows.h's macro pollution
// (min/max colliding with std::min/std::max, etc.), matching how
// visualizer.cpp already avoids including it directly (it manually
// forward-declares the two WinAPI calls it needs for memory stats
// instead of including <psapi.h>/<windows.h> wholesale).
std::string OpenMidiFileDialog();
