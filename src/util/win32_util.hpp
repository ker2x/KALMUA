#pragma once

#include <windows.h>

#include <string>

namespace kalmua::util {

// UTF-16 -> UTF-8 conversion via WideCharToMultiByte.
std::string to_utf8(const std::wstring& w);

// GUID <-> string round-trip in the canonical "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}"
// form, matching what Win32 produces.
std::string guid_to_string(const GUID& g);
bool        parse_guid(const char* s, GUID& g);

// Resolve "<exe-directory>\KALMUA.ini". WritePrivateProfileString needs an
// absolute path, otherwise it falls back to the Windows directory.
std::string ini_path();

// DI8 wants a real top-level HWND that this process owns. GetConsoleWindow()
// under Windows Terminal hands back a conhost stub DI8 rejects at Acquire()
// time with ERROR_INVALID_WINDOW_HANDLE.
HWND create_hidden_owner_window();

// Minimal RAII wrappers.
struct ScopedHandle {
    HANDLE h = nullptr;
    ~ScopedHandle() { if (h) CloseHandle(h); }
    explicit operator bool() const { return h != nullptr; }
};
struct ScopedMappedView {
    const void* p = nullptr;
    ~ScopedMappedView() { if (p) UnmapViewOfFile(p); }
    explicit operator bool() const { return p != nullptr; }
};
struct ScopedHwnd {
    HWND h = nullptr;
    ~ScopedHwnd() { if (h) DestroyWindow(h); }
    explicit operator bool() const { return h != nullptr; }
};

} // namespace kalmua::util
