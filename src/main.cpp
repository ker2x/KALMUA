// KALMUA dispatcher.
//
//   KALMUA        run FFB (or run setup automatically on first run)
//   KALMUA setup  (re-)capture wheel buttons for scale up/down

#include "ffb/di8_device.hpp"
#include "SharedMemoryInterface/SharedMemoryInterface.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <utility>

#include <conio.h>  

namespace {

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

// Minimal RAII wrappers
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

// The SDK exposes shared mapping "LMU_Data" and event "LMU_Data_Event".
// We only read a single float (FFBTorque) — that's atomic on x64, so the SDK's SharedMemoryLock isn't needed here. 
// Revisit if we start reading multi-field state that has to be internally consistent. 
// This unlikely unless we go to the forbidden land of writing a telemetry based FFB that won't work because we don't get enough data from the SDK to do it right
//
// PS: 
// Don't even think about trying to do it anyway. Trust me bro. 
// I'm not the first person to try it, and the SDK is not designed for it (feel like it's even designed AGAINST it) 
// so you will just end up with a buggy mess that will feel worse than the native FFB.
int run_passthrough() {
    using namespace kalmua::ffb;

    // Initial scale before a car is detected. Per-car value from the INI
    // takes over as soon as the player has a vehicle.
	// New: the ini file now have a default scale that the user can edit. 
    // I just initialize it to a safe value anyway to avoid an "uninitialized variable" going wild and break my wrist. 
    float scale = 1.0f;

	// See SharedMemoryInterface.hpp which is part of the LMU SDK for details on the shared memory layout.
    ScopedHandle hEvent{ OpenEventA(SYNCHRONIZE, FALSE, LMU_SHARED_MEMORY_EVENT) };
    ScopedHandle hMap  { OpenFileMappingA(FILE_MAP_READ, FALSE, LMU_SHARED_MEMORY_FILE) };

	// If either of these fail, the most likely explanation is that LMU isn't running. The user needs to start LMU before starting this tool, so it's worth giving them a heads-up.
	// If LMU is running but theses fail, the hpp files i grabbed from the SDK are probably out of date. 
    // In that case, I'll need to update them, but the user won't be able to do anything about it.
    if (!hEvent || !hMap) {
        std::printf("Could not open '%s' / '%s' — is LMU running?\n",
                    LMU_SHARED_MEMORY_FILE, LMU_SHARED_MEMORY_EVENT);
        return 0;
    }

	// Map the shared memory to read the value. 
    // Again, if this fails while LMU is running, the SDK hpp files are probably out of date.
    ScopedMappedView view{
        MapViewOfFile(hMap.h, FILE_MAP_READ, 0, 0, sizeof(SharedMemoryLayout))
    };
    if (!view) {
        std::printf("MapViewOfFile failed (GLE=%lu).\n", GetLastError());
        return 1;
    }
    const auto* pBuf = static_cast<const SharedMemoryLayout*>(view.p);

	auto devices = Di8Device::enumerate();  // DirectX isn't always a PITA. Sometimes it's just simple and works.
                                            // And yet I still had to write a (relatively simple) method for it.
											// I'm still worried that it might break on some weird system configuration, but I have no way to test that. Hope for the best.

    if (devices.empty()) {
        std::printf("No FFB devices found.\n");
        return 0;
    }

    // Try to honor the FFB device picked during setup. If the INI has a guid
    // and it matches an enumerated device, use it silently. Otherwise fall
    // back to the legacy prompt (which auto-picks when there's only one).
    const std::string ini = ini_path();
    char ffb_guid_buf[64] = {};
    GetPrivateProfileStringA("bindings", "ffb_device_guid", "",
                             ffb_guid_buf, sizeof(ffb_guid_buf), ini.c_str());
    size_t choice    = 0;
    bool   resolved  = false;
    GUID   ffb_pref{};
    if (parse_guid(ffb_guid_buf, ffb_pref)) {
        for (size_t i = 0; i < devices.size(); ++i) {
            if (IsEqualGUID(devices[i].instance_guid, ffb_pref)) {
                choice   = i;
                resolved = true;
                break;
            }
        }
    }
    if (!resolved && devices.size() > 1) {
        std::printf("FFB devices:\n");
        for (size_t i = 0; i < devices.size(); ++i) {
            std::printf("  [%zu] %s\n", i, to_utf8(devices[i].product_name).c_str());
        }
        std::printf("Pick [0-%zu]: ", devices.size() - 1);
        std::fflush(stdout);
        if (scanf_s("%zu", &choice) != 1 || choice >= devices.size()) {
            std::printf("Invalid choice.\n");
            return 1;
        }
    }

    ScopedHwnd hwnd{ create_hidden_owner_window() };
    if (!hwnd) {
        std::printf("Failed to create owner window (GLE=%lu).\n", GetLastError());
        return 1;
    }

    Di8Device dev;
    if (!dev.acquire(devices[choice].instance_guid, hwnd.h)) {
        std::printf("Failed to acquire device.\n");
        return 1;
    }

    int scale_up_btn   = GetPrivateProfileIntA("bindings", "scale_up",   -1, ini.c_str());
    int scale_down_btn = GetPrivateProfileIntA("bindings", "scale_down", -1, ini.c_str());
    char guid_buf[64] = {};
    GetPrivateProfileStringA("bindings", "device_guid", "", guid_buf, sizeof(guid_buf), ini.c_str());

    // [settings] step — user-editable. Seed the INI with 0.02 the first time
    // so the key is discoverable.
    float scale_step = 0.02f;
    char step_buf[32] = {};
    GetPrivateProfileStringA("settings", "step", "", step_buf, sizeof(step_buf), ini.c_str());
    if (step_buf[0] == '\0') {
        WritePrivateProfileStringA("settings", "step", "0.02", ini.c_str());
    } else {
        try { scale_step = std::stof(step_buf); }
        catch (...) { /* keep 0.02 */ }
    }

    // [settings] abs_sound / tc_sound — gate the ABS and TC beeps separately.
    // Default to on. Setup seeds the keys; defaulting here covers "ran
    // passthrough before setup".
    const bool abs_sound_enabled =
        GetPrivateProfileIntA("settings", "abs_sound", 1, ini.c_str()) != 0;
    const bool tc_sound_enabled =
        GetPrivateProfileIntA("settings", "tc_sound",  1, ini.c_str()) != 0;

    // Try to acquire the bindings input device. Failure is non-fatal — FFB
    // still works, you just can't tune scale live.
    Di8Device input_dev;
    bool input_ready = false;
    std::string input_name;
    GUID bindings_guid{};
    if ((scale_up_btn >= 0 || scale_down_btn >= 0) && parse_guid(guid_buf, bindings_guid)) {
        for (const auto& d : Di8Device::enumerate(Di8Device::DeviceFilter::All)) {
            if (IsEqualGUID(d.instance_guid, bindings_guid)) {
                if (input_dev.acquire(d.instance_guid, hwnd.h, Di8Device::AcquireMode::InputOnly)) {
                    input_ready = true;
                    input_name  = to_utf8(d.product_name);
                }
                break;
            }
        }
    }

    std::printf("\nPass-through active.\n");
    std::printf("  Source:   '%s' (SharedMemoryGeneric.FFBTorque, float)\n", LMU_SHARED_MEMORY_FILE);
    std::printf("  Trigger:  '%s' (event-driven, no fixed tick)\n", LMU_SHARED_MEMORY_EVENT);
    std::printf("  Device:   %s\n", to_utf8(devices[choice].product_name).c_str());
    std::printf("  Scale:    %.3f\n", scale);
    // Print "<n>" or "skipped" per side, depending on what's in the INI.
    auto btn_str = [](int b, char* out, size_t n) {
        if (b >= 0) std::snprintf(out, n, "button %d", b);
        else        std::snprintf(out, n, "skipped");
    };
    char up_s[32], down_s[32];
    btn_str(scale_up_btn,   up_s,   sizeof(up_s));
    btn_str(scale_down_btn, down_s, sizeof(down_s));
    if (scale_up_btn < 0 && scale_down_btn < 0) {
        std::printf("  Bindings: none (run 'KALMUA setup')\n");
    } else if (!input_ready) {
        std::printf("  Bindings: scale_up=%s, scale_down=%s  (input device not found / acquire failed)\n",
                    up_s, down_s);
    } else {
        std::printf("  Bindings: scale_up=%s, scale_down=%s on '%s'\n",
                    up_s, down_s, input_name.c_str());
    }

    using clock      = std::chrono::steady_clock;
    const auto start = clock::now();
    auto last_log    = start;

    float         observed_min = +std::numeric_limits<float>::infinity();
    float         observed_max = -std::numeric_limits<float>::infinity();
    double        sum_abs      = 0.0;
    std::uint64_t ticks        = 0;
    std::uint64_t timeouts     = 0;
    std::uint64_t lost         = 0; // ticks where set_force failed

    // Live scale tuning via the bound buttons. Step loaded from INI above.
    std::array<bool, 128> btn_prev{};

    // Cache the player's current car model so we only hit the INI on change.
    char current_car[64] = {};

    while (true) {
        // 50 ms wait so the loop still progresses if LMU stops firing events.
        DWORD wr = WaitForSingleObject(hEvent.h, 50);
        if (wr == WAIT_TIMEOUT) {
            ++timeouts;
            continue;
        }
        if (wr != WAIT_OBJECT_0) {
            std::printf("WaitForSingleObject failed (wr=%lu, GLE=%lu).\n", wr, GetLastError());
            break;
        }

        float v = pBuf->data.generic.FFBTorque;
        if (v < observed_min) observed_min = v;
        if (v > observed_max) observed_max = v;
        sum_abs += std::fabs(v);
        ++ticks;

        // See project_ffb_sign_convention.md: LMU's "Invert FFB" UI toggle
        // does not affect the value in shared memory; negate to match the wheel.
        float out = std::clamp(-v * scale, -1.0f, 1.0f);
        if (!dev.set_force(out)) ++lost;

        auto now = clock::now();

        // ABS edge detection — telemetry is per-vehicle; read the player slot.
        const auto& tel = pBuf->data.telemetry;
        if (tel.playerHasVehicle && tel.playerVehicleIdx < tel.activeVehicles) {
            bool abs_now = tel.telemInfo[tel.playerVehicleIdx].mABSActive;
            if (abs_now && abs_sound_enabled) {
                std::thread([]{ Beep(800, 20); }).detach();
            }
            bool tc_now = tel.telemInfo[tel.playerVehicleIdx].mTCActive;
            if (tc_now && tc_sound_enabled) {
                std::thread([]{ Beep(1200, 20); }).detach();
            }

            // Car change -> reload scale from [scales]<model>, default to 1.0.
            // mVehicleModel collapses liveries/teams together (e.g. all
            // McLaren 720S GT3 entries share one scale); mVehicleName would
            // be one entry per variant.
            const char* car = tel.telemInfo[tel.playerVehicleIdx].mVehicleModel;
            if (car[0] != '\0' && std::strncmp(current_car, car, sizeof(current_car)) != 0) {
                strncpy_s(current_car, sizeof(current_car), car, _TRUNCATE);
                char val[32] = {};
                GetPrivateProfileStringA("scales", current_car, "",
                                         val, sizeof(val), ini.c_str());
                float new_scale = 1.0f;
                if (val[0] != '\0') {
                    try { new_scale = std::stof(val); }
                    catch (...) { new_scale = 1.0f; }
                }
                scale = std::clamp(new_scale, 0.0f, 2.0f);
                std::printf("[car] %s -> scale %.3f\n", current_car, scale);
            }
        }

        // Scale tuning buttons — rising-edge -> adjust + log + persist per car.
        if (input_ready) {
            std::array<bool, 128> btn{};
            if (input_dev.poll_buttons(btn)) {
                bool changed = false;
                if (scale_up_btn >= 0 && scale_up_btn < 128 &&
                    btn[scale_up_btn] && !btn_prev[scale_up_btn]) {
                    scale = std::clamp(scale + scale_step, 0.0f, 2.0f);
                    std::printf("[btn] scale up   -> %.3f\n", scale);
                    changed = true;
                }
                if (scale_down_btn >= 0 && scale_down_btn < 128 &&
                    btn[scale_down_btn] && !btn_prev[scale_down_btn]) {
                    scale = std::clamp(scale - scale_step, 0.0f, 2.0f);
                    std::printf("[btn] scale down -> %.3f\n", scale);
                    changed = true;
                }
                if (changed && current_car[0] != '\0') {
                    char val[32];
                    std::snprintf(val, sizeof(val), "%.3f", scale);
                    WritePrivateProfileStringA("scales", current_car, val, ini.c_str());
                }
                btn_prev = btn;
            }
        }

        if (now - last_log >= std::chrono::seconds(1)) {
            last_log = now;
            float t_s = std::chrono::duration<float>(now - start).count();
            std::printf("  t=%5.1fs  src=% .4f  out=% .4f   range=[% .4f, % .4f]  mean|src|=%.4f  ticks=%llu  timeouts=%llu  lost=%llu\n",
                        t_s, v, out, observed_min, observed_max,
                        ticks ? sum_abs / static_cast<double>(ticks) : 0.0,
                        static_cast<unsigned long long>(ticks),
                        static_cast<unsigned long long>(timeouts),
                        static_cast<unsigned long long>(lost));
        }
    }

    dev.set_force(0.0f);
    dev.release();

    std::printf("\nDone.\n");
    std::printf("  Source range: [% .5f, % .5f]\n", observed_min, observed_max);
    std::printf("  Ticks:        %llu  (timeouts: %llu, lost: %llu)\n",
                static_cast<unsigned long long>(ticks),
                static_cast<unsigned long long>(timeouts),
                static_cast<unsigned long long>(lost));
    std::printf("  Mean |src|:   %.5f\n",
                ticks ? sum_abs / static_cast<double>(ticks) : 0.0);
    return 0;
}

/*
* Setup mode: prompt the user to select their wheel and the buttons to use for scale up/down, then save
*/
int run_setup() {
    using namespace kalmua::ffb;

    // Step 1 — pick the FFB device (wheelbase). Multiple FFB devices might be a thing ?
    auto ffb_devices = Di8Device::enumerate(Di8Device::DeviceFilter::FFB);

	// 1.1 - No FFB devices at all: give up.
    if (ffb_devices.empty()) {
        std::printf("No FFB devices found.\n");
        return 0;
    }
    // 1.2 - Print the list of FFB devices.
    std::printf("FFB devices (wheelbase):\n");
    for (size_t i = 0; i < ffb_devices.size(); ++i) {
        std::printf("  [%zu] %s\n", i, to_utf8(ffb_devices[i].product_name).c_str());
    }

	// 1.3 - If there's more than one, ask the user to pick. Otherwise just auto-pick and print it so they have some feedback and confidence that we found their wheel.
    size_t ffb_choice = 0;
    if (ffb_devices.size() > 1) {
        std::printf("Pick FFB device [0-%zu]: ", ffb_devices.size() - 1);
        std::fflush(stdout);
        if (scanf_s("%zu", &ffb_choice) != 1 || ffb_choice >= ffb_devices.size()) {
            std::printf("Invalid choice.\n");
            return 1;
        }
	}
	else {
		// Auto-pick
		std::printf("Only 1 FFB device found. Using: %s\n", to_utf8(ffb_devices[0].product_name).c_str());
	}

    // Step 2 — pick the input device (rim / button box / pedals) we'll bind
    // scale up/down buttons on.
    auto devices = Di8Device::enumerate(Di8Device::DeviceFilter::All);

	// 2.1 - No input devices at all: give up
    if (devices.empty()) {
        std::printf("No input devices found.\n");
        return 0;
    }

	// 2.2 - Print the list of input devices. It's possible that the wheel's buttons are on a separate HID from the FFB axis, so we want to show them all.
    std::printf("Input devices (most likely steering wheel) to map FFB scale up/down:\n");
    for (size_t i = 0; i < devices.size(); ++i) {
        std::printf("  [%zu] %s\n", i, to_utf8(devices[i].product_name).c_str());
    }

	// 2.3 - Same thing, ask or auto-pick if there's only one. 
    size_t choice = 0;
    if (devices.size() > 1) {
        std::printf("Pick input device (you can skip later if you don't want to map FFB scale up/down) [0-%zu]: ", devices.size() - 1);
        std::fflush(stdout);
        if (scanf_s("%zu", &choice) != 1 || choice >= devices.size()) {
            std::printf("Invalid choice.\n");
            return 1;
        }
	}
    else {
		// Auto-pick
        std::printf("Only 1 input device found. Using: %s\n", to_utf8(devices[0].product_name).c_str());
    }

    // 2.4 - Weird place to suddenly go through this but:
	//  * If we need to map buttons, we need to acquire the device to poll its buttons in the prompts below. 
	//  * The window thing is needed by directinput, you can't properly run DirectX from a console. Some stuff works but it's unclear what break and what works.
    ScopedHwnd hwnd{ create_hidden_owner_window() };
    if (!hwnd) {
        std::printf("Failed to create owner window for DirectInput device acquisition (Error: %lu).\n", GetLastError());
        return 1;
    }

	// 2.5 - Try to acquire the device. If this fails, we won't be able to capture button bindings.
    // For now it completely blocks the setup in case of failure.
    // If this is a problem for some users we might want to go on and just skip the button mapping.
    // Will wait for user feedback before creating a workaround for this.
    Di8Device dev;
    if (!dev.acquire(devices[choice].instance_guid, hwnd.h, Di8Device::AcquireMode::InputOnly)) {
        std::printf("Failed to acquire device.\n");
        return 1;
    }

    std::printf("Using: %s (%d buttons)\n",
                to_utf8(devices[choice].product_name).c_str(),
                dev.button_count());

    // Baseline against current state so a button already held when the prompt
    // appears doesn't auto-select; we wait for a fresh 0->1 transition.
    // (Thx claude, I would have missed that one)
    //
    // Returns the button index, or -1 if the user skipped (any keyboard key)
    // or polling failed -- both treated the same by the caller (no binding).
    auto prompt_button = [&](const char* label) -> int {
        std::printf("Press button for %s (Press enter to skip)...\n", label);
        std::fflush(stdout);

        std::array<bool, 128> baseline{};
        if (!dev.poll_buttons(baseline)) {
            std::printf("poll_buttons failed.\n");
            return -1;
        }
        while (true) {
            if (_kbhit()) {
                (void)_getch(); // consume the key so it doesn't echo / leak
                std::printf("  (skipped)\n");
                return -1;
            }
            std::array<bool, 128> now{};
            if (!dev.poll_buttons(now)) {
                std::printf("poll_buttons failed.\n");
                return -1;
            }
            for (int i = 0; i < 128; ++i) {
                if (now[i] && !baseline[i]) return i;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    };

    int up = prompt_button("SCALE UP");
    if (up >= 0) std::printf("  scale_up   = button %d\n", up);

    int down = prompt_button("SCALE DOWN");
    if (down >= 0) std::printf("  scale_down = button %d\n", down);

	// Step 3 — save the bindings to the INI. 
	// We'll look them up by the device GUID so they still work if the device order changes or a new device is added.
	// No error handling here, that might need a fix or workaround if a user report an error. 
    // (most likely case would be that the app is installed in program files by an installer or if this code end up being bundled in some other tool with improper permission handling)
    // Still it would be the right thing to do to at least give the user a heads up about a problem.
    const std::string ini = ini_path();
    char val[16];
    std::snprintf(val, sizeof(val), "%d", up);    // -1 if skipped
    WritePrivateProfileStringA("bindings", "scale_up", val, ini.c_str());
    std::snprintf(val, sizeof(val), "%d", down);  // -1 if skipped
    WritePrivateProfileStringA("bindings", "scale_down", val, ini.c_str());
    const std::string g = guid_to_string(devices[choice].instance_guid);
    WritePrivateProfileStringA("bindings", "device_guid", g.c_str(), ini.c_str());
    const std::string ffb_g = guid_to_string(ffb_devices[ffb_choice].instance_guid);
    WritePrivateProfileStringA("bindings", "ffb_device_guid", ffb_g.c_str(), ini.c_str());

    // Seed [settings] abs_sound / tc_sound = 1 on first setup. Skip if the key
    // already exists so a user who manually disabled one doesn't get it
    // flipped back on by re-running setup.
    auto seed_if_absent = [&](const char* key, const char* val) {
        char existing[8] = {};
        GetPrivateProfileStringA("settings", key, "", existing, sizeof(existing), ini.c_str());
        if (existing[0] == '\0') {
            WritePrivateProfileStringA("settings", key, val, ini.c_str());
        }
    };
    seed_if_absent("abs_sound", "1");
    seed_if_absent("tc_sound",  "1");

    std::printf("Saved to %s\n", ini.c_str());

    return 0;
}

// Good old boring usage message. No need for fancy CLI parsing.
void print_usage(const char* argv0) {
    std::printf("Usage:\n");
    std::printf("  %s        run FFB (auto-runs setup on first run)\n", argv0);
    std::printf("  %s setup  (re-)capture wheel buttons\n", argv0);
}

} // namespace

/* 
* KALMUA - Keru's Awesome LMU App
* KALMUA - KALMUA Ain't an LMU Universal Adapter 
*/
int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);    // do i even need this ?

	// Too many args -> print usage and exit.
    if (argc > 2) {
        print_usage(argv[0]);
        return 1;
    }

    // Explicit `setup`: re-run capture (e.g. new device, different bindings).
    // re-running setup is safe to rebind wheel/button,  without nuking your saved per-car scales.
    if (argc == 2) {
        if (std::string(argv[1]) == "setup") {
            return run_setup();
        }
        // Unknown arg.
		// Used to have an explicit "passthough mode" in early phase of development. (and a test mode that just vibrate to verify the device works)
        // Now it's the main mode, and setup is just a one-time thing to (re)capture the bindings.
		// This thing isn't even released yet and there is already some legacy stuff. 
        // I keep it in case I want to add more mode, but for now, just print usage if it's not "setup".
        print_usage(argv[0]);
        return 1;
    }

    // No args: if the INI doesn't exist, run setup; otherwise run passthrough.
    // Should I run passthrough once the setup is done ? I assume LMU isn't running when the user is doing the setup.
    // So dropping an error "LMU not running" right after completing the setup would be weird / confusing. It might lead users to think the setup didn't work.
    DWORD attrs = GetFileAttributesA(ini_path().c_str());   // Check if the ini file exists.
	if (attrs == INVALID_FILE_ATTRIBUTES) {                 // run setup if it doesn't
        return run_setup();
    }

	// INI exists, assume setup is done and run passthrough. User can always re-run setup if they want to change bindings.
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        return run_passthrough();
    }

    // Every reachable path above returns. 
	std::printf("Unreachable code reached in main(). This should never happen. this shouldn't even compile if MSVC did its job properly\n");
	std::printf("Please report this to the developer.\n");
	std::printf("argc: %d\n", argc);
	for (int i = 0; i < argc; ++i) {
		std::printf("argv[%d]: %s\n", i, argv[i]);
	}
    std::unreachable();
}
