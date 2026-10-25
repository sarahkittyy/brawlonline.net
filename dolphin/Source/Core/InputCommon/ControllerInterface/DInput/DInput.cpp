// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerInterface/DInput/DInput.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "Common/HRWrap.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/DInput/DInputJoystick.h"
#include "InputCommon/ControllerInterface/DInput/DInputKeyboardMouse.h"

#pragma comment(lib, "Dinput8.lib")
#pragma comment(lib, "dxguid.lib")

namespace ciface::DInput
{
static IDirectInput8* s_idi8 = nullptr;

BOOL CALLBACK DIEnumDeviceObjectsCallback(LPCDIDEVICEOBJECTINSTANCE lpddoi, LPVOID pvRef)
{
  ((std::list<DIDEVICEOBJECTINSTANCE>*)pvRef)->push_back(*lpddoi);
  return DIENUM_CONTINUE;
}

BOOL CALLBACK DIEnumDevicesCallback(LPCDIDEVICEINSTANCE lpddi, LPVOID pvRef)
{
  ((std::list<DIDEVICEINSTANCE>*)pvRef)->push_back(*lpddi);
  return DIENUM_CONTINUE;
}

std::string GetDeviceName(const LPDIRECTINPUTDEVICE8 device)
{
  DIPROPSTRING str = {};
  str.diph.dwSize = sizeof(str);
  str.diph.dwHeaderSize = sizeof(str.diph);
  str.diph.dwHow = DIPH_DEVICE;

  std::string result;
  HRESULT hr = device->GetProperty(DIPROP_PRODUCTNAME, &str.diph);
  if (SUCCEEDED(hr))
  {
    result = StripWhitespace(WStringToUTF8(str.wsz));
  }
  else
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "GetProperty(DIPROP_PRODUCTNAME) failed: {}",
                  Common::HRWrap(hr));
  }

  return result;
}

// A USB or Bluetooth device that never answers a HID request holds DirectInput's enumeration
// forever: EnumDevices reads the strings of every HID device on the PC, game controller or not,
// while holding dinput8's DLL-wide lock, which creating, acquiring and reading the keyboard and
// mouse take too (bug report #141: a consumer control collection of an HP device, 03F0:089D, never
// answered HidD_GetProductString; SDL's init and Dolphin's UI thread behind it waited for good, so
// every Dolphin opened "Not responding"). So enumerations run on their own thread and are waited
// for at most this long; past it DirectInput is left alone (no keyboard, mouse or DirectInput
// controllers) until that call returns, and its devices are added then.
constexpr auto ENUM_TIMEOUT = std::chrono::seconds(3);

struct EnumState
{
  std::mutex mutex;
  std::condition_variable done;
  // An enumeration that was given up on hasn't returned yet.
  bool stuck = false;
};
// Never freed: a stuck enumeration's thread can outlive static destruction at exit.
static EnumState* const s_enum = new EnumState;

struct EnumCall
{
  std::list<DIDEVICEINSTANCE> devices;
  bool done = false;
  bool given_up = false;
};

bool IsResponding()
{
  std::lock_guard lk(s_enum->mutex);
  return !s_enum->stuck;
}

std::optional<std::list<DIDEVICEINSTANCE>> EnumGameControllers(IDirectInput8* idi8)
{
  if (!IsResponding())
    return std::nullopt;

  const auto call = std::make_shared<EnumCall>();
  idi8->AddRef();
  std::thread([idi8, call] {
    Common::SetCurrentThreadName("DInput EnumDevices");

    std::list<DIDEVICEINSTANCE> devices;
    idi8->EnumDevices(DI8DEVCLASS_GAMECTRL, DIEnumDevicesCallback, (LPVOID)&devices,
                      DIEDFL_ATTACHEDONLY);
    idi8->Release();

    bool given_up;
    {
      std::lock_guard lk(s_enum->mutex);
      call->devices = std::move(devices);
      call->done = true;
      given_up = call->given_up;
      if (given_up)
        s_enum->stuck = false;
    }
    s_enum->done.notify_all();

    if (given_up)
    {
      NOTICE_LOG_FMT(CONTROLLERINTERFACE, "DirectInput answers again: adding its devices");
      g_controller_interface.PlatformPopulateDevices([] {
        PopulateDevices(
            static_cast<HWND>(g_controller_interface.GetWindowSystemInfo().render_window));
      });
    }
  }).detach();

  std::unique_lock lk(s_enum->mutex);
  if (!s_enum->done.wait_for(lk, ENUM_TIMEOUT, [&call] { return call->done; }))
  {
    call->given_up = true;
    s_enum->stuck = true;
    WARN_LOG_FMT(CONTROLLERINTERFACE,
                 "DirectInput did not list its devices in {} s: a USB or Bluetooth device is not "
                 "answering Windows. DirectInput controllers, keyboard and mouse are off until it "
                 "does (unplugging that device or restarting the PC fixes it).",
                 ENUM_TIMEOUT.count());
    return std::nullopt;
  }
  return std::move(call->devices);
}

static bool CreateDirectInput()
{
  if (s_idi8)
    return true;

  HRESULT hr = DirectInput8Create(GetModuleHandle(nullptr), DIRECTINPUT_VERSION,
                                  IID_IDirectInput8, (LPVOID*)&s_idi8, nullptr);
  if (FAILED(hr))
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "DirectInput8Create failed: {}", Common::HRWrap(hr));
    s_idi8 = nullptr;
    return false;
  }
  return true;
}

void Init()
{
  // Before SDL starts: its own DirectInput enumeration (SDL::InputBackend turns it off if this one
  // didn't return) would wait on a stuck device too, and the UI thread on SDL. One that returns
  // leaves dinput8's HID device list cached (it rebuilds it after 20 s), so SDL's and the first
  // PopulateDevices' enumerations don't ask the devices again.
  if (CreateDirectInput())
    EnumGameControllers(s_idi8);
}

// Assumes hwnd had not changed from the previous call
void PopulateDevices(HWND hwnd)
{
  // Removing, creating or acquiring a device would wait on the stuck enumeration's lock.
  if (!IsResponding() || !CreateDirectInput())
    return;

  // Remove old (invalid) devices. No need to ever remove the KeyboardMouse device.
  // Note that if we have 2+ DInput controllers, not fully repopulating devices
  // will mean that a device with index "2" could persist while there is no device with index "0".
  // This is slightly inconsistent as when we refresh all devices, they will instead reset, and
  // that happens a lot (for uncontrolled reasons, like starting/stopping the emulation).
  g_controller_interface.RemoveDevice(
      [](const auto* dev) { return dev->GetSource() == DINPUT_SOURCE_NAME && !dev->IsValid(); });

  InitKeyboardMouse(s_idi8, hwnd);
  InitJoystick(s_idi8, hwnd);
}

void ChangeWindow(HWND hwnd)
{
  // Has init? Ignore if called before the first PopulateDevices(), or while DirectInput is stuck.
  if (s_idi8 && IsResponding())
  {
    // The KeyboardMouse device is marked as virtual device, so we avoid removing it.
    // We need to force all the DInput joysticks to be destroyed now, or recreation would fail.
    g_controller_interface.RemoveDevice(
        [](const auto* dev) {
          return dev->GetSource() == DINPUT_SOURCE_NAME && !dev->IsVirtualDevice();
        },
        true);

    SetKeyboardMouseWindow(hwnd);
    InitJoystick(s_idi8, hwnd);
  }
}

void DeInit()
{
  if (s_idi8)
  {
    s_idi8->Release();
    s_idi8 = nullptr;
  }
}
}  // namespace ciface::DInput
