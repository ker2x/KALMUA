#include "util/win32_util.hpp"

#include <cstdio>

namespace kalmua::util {

// Don't ask.
std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(
        CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
        nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
        s.data(), n, nullptr, nullptr);
    return s;
}

// Yet another thing I don't understand about Windows. Nice intro to my code isn't it ?
// GUID <-> string round-trip in the canonical "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" form, matching what Win32 produces.
// As long as it works i guess. I don't want to mess with it.
std::string guid_to_string(const GUID& g) {
    char buf[64];
    std::snprintf(buf, sizeof(buf),
        "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        static_cast<unsigned long>(g.Data1),
        static_cast<unsigned int>(g.Data2),
        static_cast<unsigned int>(g.Data3),
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

// Same thing in reverse. Returns false if the string is not in the correct format.
// I just needed something for the ini file. And this is what I got.
bool parse_guid(const char* s, GUID& g) {
    unsigned long d1 = 0;
    unsigned int  d2 = 0, d3 = 0;
    unsigned int  d4[8] = {};
    int n = sscanf_s(s, "{%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}",
                     &d1, &d2, &d3,
                     &d4[0], &d4[1], &d4[2], &d4[3],
                     &d4[4], &d4[5], &d4[6], &d4[7]);
    if (n != 11) return false;
    g.Data1 = d1;
    g.Data2 = static_cast<unsigned short>(d2);
    g.Data3 = static_cast<unsigned short>(d3);
    for (int i = 0; i < 8; ++i) g.Data4[i] = static_cast<unsigned char>(d4[i]);
    return true;
}

// Resolve "<exe-directory>\KALMUA.ini". WritePrivateProfileString needs an absolute path, otherwise it falls back to the Windows directory.
// Don't mess with the windows directory, that's just asking for trouble.
// And yes, GetModuleFileName is the recommended way to get the exe path.
// I'll trust billions of hours of developer's headaches all over the world for this. Especially over my own understanding of Windows internals, which is approximately zero.
std::string ini_path() {
    char buf[MAX_PATH] = {};
    DWORD dir = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf, dir);
    auto slash = path.find_last_of("\\/");
    if (slash != std::string::npos) path.erase(slash + 1);
    path += "KALMUA.ini"; // heh ...
    return path;
}

// DI8 wants a real top-level HWND that this process owns. GetConsoleWindow()
// under Windows Terminal hands back a conhost stub DI8 rejects at Acquire()
// time with ERROR_INVALID_WINDOW_HANDLE.
HWND create_hidden_owner_window() {
    static const wchar_t kClass[] = L"KALMUA_DI8_Owner";
    HINSTANCE h = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = h;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc); // benign if already registered

    return CreateWindowExW(
        0, kClass, L"KALMUA",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
        nullptr, nullptr, h, nullptr);
}

} // namespace kalmua::util
