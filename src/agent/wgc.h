// Windows.Graphics.Capture of one window (wgc.cpp): works while the window is covered, not when minimized.
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
namespace wgc {
// BGRA pixels of the client area (w x h). Returns an error text or "".
std::string Grab(HWND hwnd, std::vector<uint8_t>& px, int& w, int& h);
}
