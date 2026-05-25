#pragma once

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include <array>
#include <string>
#include <vector>

namespace kalmua::ffb {

struct Di8DeviceInfo {
    GUID         instance_guid;
    std::wstring product_name;
};

// RAII wrapper over a single DirectInput8 game controller. Can be acquired in
// FFB mode (exclusive, owns a constant-force effect on the X axis) or in
// input-only mode (non-exclusive, button polling only). Typical use:
//
//   auto ffb = Di8Device::enumerate(Di8Device::DeviceFilter::FFB);
//   Di8Device dev;
//   dev.acquire(ffb[i].instance_guid, hwnd);          // FFB by default
//   dev.set_force(0.3f);                              // ... in a loop ...
//   dev.release();                                    // or let dtor do it
//
//   auto all = Di8Device::enumerate(Di8Device::DeviceFilter::All);
//   Di8Device rim;
//   rim.acquire(all[i].instance_guid, hwnd, Di8Device::AcquireMode::InputOnly);
//   std::array<bool,128> btn{};
//   rim.poll_buttons(btn);
//
// All forces are normalized to [-1.0, +1.0]; sign chooses direction along X.
class Di8Device {
public:
    // FFB: only force-feedback-capable controllers (wheelbase).
    // All: every attached game controller (wheelbase + rim + pedals + ...).
    enum class DeviceFilter { FFB, All };

    // FFB:       exclusive cooperative level, creates constant-force effect.
    // InputOnly: non-exclusive, no effect — for reading buttons only.
    enum class AcquireMode { FFB, InputOnly };

    Di8Device() = default;
    ~Di8Device();

    Di8Device(const Di8Device&)            = delete;
    Di8Device& operator=(const Di8Device&) = delete;

    // List attached game controllers. Default filters to FFB-capable only;
    // pass DeviceFilter::All to include rims, pedals, etc.
    static std::vector<Di8DeviceInfo> enumerate(DeviceFilter filter = DeviceFilter::FFB);

    // Acquire the device. Default mode creates an FFB constant-force effect
    // (exclusive); InputOnly skips that and uses non-exclusive cooperation.
    // hwnd: any valid window handle (cooperative level is BACKGROUND, so the
    //       window does not need to be foreground).
    bool acquire(const GUID& instance_guid, HWND hwnd,
                 AcquireMode mode = AcquireMode::FFB);

    // Update force magnitude. Clamped to [-1.0, +1.0].
    bool set_force(float n11);

    // Poll the device and fill `out` with current button state (DIJOYSTATE2
    // exposes 128 buttons; rotary encoders typically map to button clicks).
    // Returns false if the device is not acquired or polling failed.
    bool poll_buttons(std::array<bool, 128>& out);

    // Number of buttons this device reports (0 if not acquired or query fails).
    // Useful to detect when buttons live on a separate HID (e.g. wheel rim
    // distinct from the wheelbase that owns the FFB axis).
    int button_count() const;

    // Stop the effect, unacquire, release. Safe to call multiple times.
    void release();

private:
    // Try to recover the device + effect after SetParameters fails. Returns
    // true if recovery succeeded and the parameters were replayed.
    bool recover_and_replay(const DIEFFECT& eff);

    IDirectInput8W*       di_     = nullptr;
    IDirectInputDevice8W* device_ = nullptr;
    IDirectInputEffect*   effect_ = nullptr;

    // Transition tracking for diagnostic logging in set_force. Only print
    // when these change, so a steady-state error doesn't spam the console.
    HRESULT last_setparams_hr_ = S_OK;
    HRESULT last_acquire_hr_   = S_OK;
};

} // namespace kalmua::ffb
