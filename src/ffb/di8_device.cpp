#include "ffb/di8_device.hpp"

#include <algorithm>
#include <cstdio>


// Here be dragons. Thx Claude Opus 4.7 for helping me through this.

namespace kalmua::ffb {

namespace {

struct EnumCtx {
    std::vector<Di8DeviceInfo>* out;
};

BOOL CALLBACK enum_devices_cb(const DIDEVICEINSTANCEW* inst, VOID* user) {
    auto* ctx = static_cast<EnumCtx*>(user);
    Di8DeviceInfo info;
    info.instance_guid = inst->guidInstance;
    info.product_name  = inst->tszProductName;
    ctx->out->push_back(std::move(info));
    return DIENUM_CONTINUE;
}

// If anyone know a better way to get error messages out of DirectInput, please tell me. 
// Even if I do a lookup table for common HRESULTs,there will always be some weird error code that I didn't account for.
// If something go wrong, just grep the error code in the directx .h files and hope for the best.
//
// DIERR_NOTEXCLUSIVEACQUIRED (0x80040205) is the one we usually see when focus toggles between KALMUA and LMU
// So i'll write a special message for that one, but for other errors, just print the code.
void log_fail(const char* step, HRESULT hr) {
    if (hr == DIERR_NOTEXCLUSIVEACQUIRED) {
        std::fprintf(stderr,
                     "[di8] %s failed: DIERR_NOTEXCLUSIVEACQUIRED (0x80040205)\n"
                     "      -- This should be autorecovered gracefuly as this is the most common issue when LMU require exclusivity on focus switch.\n"
                     "      -- Still printing an error anyway for debugging purpose. If the next debug isn't about recovery, then something went wrong.\n",
                     step);
        return;
    }
    std::fprintf(stderr, "[di8] %s failed: hr=0x%08lX\n",
                 step, static_cast<unsigned long>(hr));
}

} // namespace

Di8Device::~Di8Device() {
    release();
}

// 
std::vector<Di8DeviceInfo> Di8Device::enumerate(DeviceFilter filter) {
    std::vector<Di8DeviceInfo> result;

    IDirectInput8W* di = nullptr;
    HRESULT hr = DirectInput8Create(
        GetModuleHandleW(nullptr),
        DIRECTINPUT_VERSION,
        IID_IDirectInput8W,
        reinterpret_cast<void**>(&di),
        nullptr);
    if (FAILED(hr) || di == nullptr) {
        return result;
    }

    DWORD flags = DIEDFL_ATTACHEDONLY;
    if (filter == DeviceFilter::FFB) flags |= DIEDFL_FORCEFEEDBACK;

    EnumCtx ctx{ &result };
    di->EnumDevices(DI8DEVCLASS_GAMECTRL, enum_devices_cb, &ctx, flags);

    di->Release();
    return result;
}

bool Di8Device::acquire(const GUID& instance_guid, HWND hwnd, AcquireMode mode) {
    release();

    HRESULT hr = DirectInput8Create(
        GetModuleHandleW(nullptr),
        DIRECTINPUT_VERSION,
        IID_IDirectInput8W,
        reinterpret_cast<void**>(&di_),
        nullptr);
    if (FAILED(hr) || di_ == nullptr) {
        log_fail("DirectInput8Create", hr);
        return false;
    }

    hr = di_->CreateDevice(instance_guid, &device_, nullptr);
    if (FAILED(hr) || device_ == nullptr) {
        log_fail("CreateDevice", hr);
        release();
        return false;
    }

    hr = device_->SetDataFormat(&c_dfDIJoystick2);
    if (FAILED(hr)) {
        log_fail("SetDataFormat", hr);
        release();
        return false;
    }

    // EXCLUSIVE is required for FFB; InputOnly uses NONEXCLUSIVE so other apps
    // (the sim) can still read the device. BACKGROUND lets us drive/poll the
    // device even when the sim has foreground focus.
    DWORD coop = (mode == AcquireMode::FFB)
                     ? (DISCL_EXCLUSIVE | DISCL_BACKGROUND)
                     : (DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    hr = device_->SetCooperativeLevel(hwnd, coop);
    if (FAILED(hr)) {
        log_fail("SetCooperativeLevel", hr);
        release();
        return false;
    }

    if (mode == AcquireMode::FFB) {
        // Disable auto-centering spring so our constant force is what the
        // wheel does, not a fight against a built-in centering torque.
        // Non-fatal: some drivers don't expose this property.
        DIPROPDWORD prop = {};
        prop.diph.dwSize       = sizeof(DIPROPDWORD);
        prop.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        prop.diph.dwObj        = 0;
        prop.diph.dwHow        = DIPH_DEVICE;
        prop.dwData            = FALSE;
        device_->SetProperty(DIPROP_AUTOCENTER, &prop.diph);
    }

    hr = device_->Acquire();
    if (FAILED(hr)) {
        log_fail("Acquire", hr);
        release();
        return false;
    }

    if (mode == AcquireMode::InputOnly) {
        return true; // no FFB effect to build
    }

    // Build a constant-force effect, X axis only, infinite duration.
    DWORD           axes[1]       = { DIJOFS_X };
    LONG            direction[1]  = { 0 };
    DICONSTANTFORCE cf            = { 0 };

    DIEFFECT eff = {};
    eff.dwSize                  = sizeof(DIEFFECT);
    eff.dwFlags                 = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    eff.dwDuration              = INFINITE;
    eff.dwSamplePeriod          = 0;
    eff.dwGain                  = DI_FFNOMINALMAX;
    eff.dwTriggerButton         = DIEB_NOTRIGGER;
    eff.dwTriggerRepeatInterval = 0;
    eff.cAxes                   = 1;
    eff.rgdwAxes                = axes;
    eff.rglDirection            = direction;
    eff.lpEnvelope              = nullptr;
    eff.cbTypeSpecificParams    = sizeof(DICONSTANTFORCE);
    eff.lpvTypeSpecificParams   = &cf;
    eff.dwStartDelay            = 0;

    hr = device_->CreateEffect(GUID_ConstantForce, &eff, &effect_, nullptr);
    if (FAILED(hr) || effect_ == nullptr) {
        log_fail("CreateEffect(ConstantForce)", hr);
        release();
        return false;
    }

    hr = effect_->Start(1, 0);
    if (FAILED(hr)) {
        log_fail("Effect::Start", hr);
        release();
        return false;
    }

    return true;
}

bool Di8Device::set_force(float n11) {
    if (effect_ == nullptr || device_ == nullptr) return false;

    n11 = std::clamp(n11, -1.0f, 1.0f);
    DICONSTANTFORCE cf = { static_cast<LONG>(n11 * DI_FFNOMINALMAX) };

    // SetParameters with DIEP_TYPESPECIFICPARAMS only reads dwSize and the
    // type-specific size/pointer; other DIEFFECT fields are ignored.
    DIEFFECT eff = {};
    eff.dwSize                = sizeof(DIEFFECT);
    eff.dwFlags               = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    eff.cbTypeSpecificParams  = sizeof(DICONSTANTFORCE);
    eff.lpvTypeSpecificParams = &cf;

    HRESULT hr = effect_->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS);
    if (SUCCEEDED(hr)) {
        if (last_setparams_hr_ != S_OK) {
            std::fprintf(stderr, "[di8] SetParameters recovered\n");
            last_setparams_hr_ = S_OK;
        }
        return true;
    }

    if (hr != last_setparams_hr_) {
        std::fprintf(stderr, "[di8] SetParameters -> 0x%08lX\n",
                     static_cast<unsigned long>(hr));
        last_setparams_hr_ = hr;
    }
    return recover_and_replay(eff);
}

int Di8Device::button_count() const {
    if (device_ == nullptr) return 0;
    DIDEVCAPS caps = {};
    caps.dwSize = sizeof(caps);
    if (FAILED(device_->GetCapabilities(&caps))) return 0;
    return static_cast<int>(caps.dwButtons);
}

bool Di8Device::poll_buttons(std::array<bool, 128>& out) {
    if (device_ == nullptr) return false;

    HRESULT hr = device_->Poll();
    if (FAILED(hr)) {
        // Lost device; re-acquire once and retry.
        device_->Acquire();
        hr = device_->Poll();
        if (FAILED(hr)) return false;
    }

    DIJOYSTATE2 st = {};
    hr = device_->GetDeviceState(sizeof(st), &st);
    if (FAILED(hr)) return false;

    for (int i = 0; i < 128; ++i) {
        out[i] = (st.rgbButtons[i] & 0x80) != 0;
    }
    return true;
}

bool Di8Device::recover_and_replay(const DIEFFECT& eff) {
    // Force a clean re-arbitration with DI8. Without Unacquire() first, our
    // handle's "acquired" flag stays set after the sim takes the device, so
    // Acquire() returns S_FALSE (no-op) instead of actually re-arbitrating.
    device_->Unacquire();
    HRESULT ahr = device_->Acquire();
    if (ahr != last_acquire_hr_) {
        std::fprintf(stderr, "[di8] Acquire -> 0x%08lX\n",
                     static_cast<unsigned long>(ahr));
        last_acquire_hr_ = ahr;
    }
    if (FAILED(ahr)) return false;

    // The effect's device-side slot may have been evicted while the sim
    // owned the device. Re-download and restart before replaying.
    effect_->Download();
    effect_->Start(1, 0);
    return SUCCEEDED(effect_->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS));
}

void Di8Device::release() {
    if (effect_) {
        effect_->Stop();
        effect_->Release();
        effect_ = nullptr;
    }
    if (device_) {
        device_->Unacquire();
        device_->Release();
        device_ = nullptr;
    }
    if (di_) {
        di_->Release();
        di_ = nullptr;
    }
}

} // namespace kalmua::ffb
