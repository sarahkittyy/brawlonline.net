// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Dolphin's WGInput backend (InputCommon/ControllerInterface/WGInput/WGInput.cpp) for core/standalone,
// whose toolchain (llvm-mingw) has Windows.Gaming.Input's ABI headers but not C++/WinRT. The same
// devices, names and controls as Dolphin's, so a GCPadNew.ini line "Device = WGInput/0/Xbox One Game
// Controller" finds its controller here as it does in Dolphin; it is Dolphin's code with the
// C++/WinRT projection written out as the COM calls under it. Left out: the SimpleHaptics outputs
// ("Rumble 0" and the like on controllers without IGamepad) and the Battery input.

#include "InputCommon/ControllerInterface/WGInput/WGInput.h"

#include <array>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

#include <objbase.h>
#ifdef __MINGW32__
// MinGW's windows.foundation.h declares IReference<boolean> and IReference<BYTE>, one type in
// C++ (both unsigned char), which C++ rejects; nothing here uses either.
#define ____FIReference_1_boolean_INTERFACE_DEFINED__
#endif
#include <roapi.h>
#include <windows.gaming.input.h>
#include <winstring.h>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/WorkQueueThread.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"

namespace WGI = ABI::Windows::Gaming::Input;

namespace ciface::WGInput
{
namespace
{
constexpr std::string_view SOURCE_NAME = "WGInput";

// combase.dll's WinRT calls, looked up at Init (Windows 8 and later; WGI itself is Windows 10).
struct Combase
{
  HMODULE dll = nullptr;
  HRESULT(WINAPI* RoGetActivationFactory)(HSTRING, REFIID, void**) = nullptr;
  HRESULT(WINAPI* WindowsCreateString)(PCNZWCH, UINT32, HSTRING*) = nullptr;
  HRESULT(WINAPI* WindowsDeleteString)(HSTRING) = nullptr;
  PCWSTR(WINAPI* WindowsGetStringRawBuffer)(HSTRING, UINT32*) = nullptr;
  HRESULT(WINAPI* CoIncrementMTAUsage)(CO_MTA_USAGE_COOKIE*) = nullptr;
  HRESULT(WINAPI* CoDecrementMTAUsage)(CO_MTA_USAGE_COOKIE) = nullptr;
};
Combase s_combase;

template <typename F>
bool Load(F& fn, const char* name)
{
  fn = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(s_combase.dll, name)));
  return fn != nullptr;
}

bool LoadCombase()
{
  s_combase.dll = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!s_combase.dll)
    return false;
  return Load(s_combase.RoGetActivationFactory, "RoGetActivationFactory") &&
         Load(s_combase.WindowsCreateString, "WindowsCreateString") &&
         Load(s_combase.WindowsDeleteString, "WindowsDeleteString") &&
         Load(s_combase.WindowsGetStringRawBuffer, "WindowsGetStringRawBuffer") &&
         Load(s_combase.CoIncrementMTAUsage, "CoIncrementMTAUsage") &&
         Load(s_combase.CoDecrementMTAUsage, "CoDecrementMTAUsage");
}

// A COM pointer that releases what it holds (WRL's ComPtr, which MinGW lacks in places).
template <typename T>
class Ptr
{
public:
  Ptr() = default;
  Ptr(T* p, bool add_ref) : m_p(p)
  {
    if (m_p && add_ref)
      m_p->AddRef();
  }
  Ptr(const Ptr& o) : Ptr(o.m_p, true) {}
  Ptr(Ptr&& o) noexcept : m_p(std::exchange(o.m_p, nullptr)) {}
  Ptr& operator=(Ptr o) noexcept
  {
    std::swap(m_p, o.m_p);
    return *this;
  }
  ~Ptr()
  {
    if (m_p)
      m_p->Release();
  }
  T* Get() const { return m_p; }
  T* operator->() const { return m_p; }
  explicit operator bool() const { return m_p != nullptr; }
  T** Put()
  {
    *this = Ptr();
    return &m_p;
  }
  template <typename U>
  Ptr<U> As() const
  {
    Ptr<U> out;
    if (m_p)
      m_p->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out.Put()));
    return out;
  }

private:
  T* m_p = nullptr;
};

template <typename T>
Ptr<T> GetFactory(const wchar_t* class_name)
{
  Ptr<T> out;
  HSTRING name = nullptr;
  if (FAILED(s_combase.WindowsCreateString(class_name, static_cast<UINT32>(wcslen(class_name)),
                                           &name)))
  {
    return out;
  }
  s_combase.RoGetActivationFactory(name, __uuidof(T), reinterpret_cast<void**>(out.Put()));
  s_combase.WindowsDeleteString(name);
  return out;
}

Ptr<WGI::IRawGameControllerStatics> RawStatics()
{
  return GetFactory<WGI::IRawGameControllerStatics>(L"Windows.Gaming.Input.RawGameController");
}

// COM identity, what C++/WinRT's == on two RawGameControllers compares.
IUnknown* Identity(IUnknown* object)
{
  IUnknown* unknown = nullptr;
  if (!object || FAILED(object->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&unknown))))
    return nullptr;
  unknown->Release();
  return unknown;
}

// These names correspond to the values of the GameControllerButtonLabel enum.
// "None" is not used.
// There are some overlapping names assuming no device exposes both
// GameControllerButtonLabel::XboxLeftBumper and GameControllerButtonLabel::LeftBumper.
// If needed we can prepend "Xbox" to relevant input names on conflict in the future.
constexpr std::array wgi_button_names = {
    "None",      "Back",     "Start",      "Menu",     "View",     "Pad N",
    "Pad S",     "Pad W",    "Pad E",      "Button A", "Button B", "Button X",
    "Button Y",  "Bumper L", "Trigger L",  "Thumb L",  "Bumper R", "Trigger R",
    "Thumb R",   "Paddle 1", "Paddle 2",   "Paddle 3", "Paddle 4", "Mode",
    "Select",    "Menu",     "View",       "Back",     "Start",    "Options",
    "Share",     "Pad N",    "Pad S",      "Pad W",    "Pad E",    "Letter A",
    "Letter B",  "Letter C", "Letter L",   "Letter R", "Letter X", "Letter Y",
    "Letter Z",  "Cross",    "Circle",     "Square",   "Triangle", "Bumper L",
    "Trigger L", "Thumb L",  "Left 1",     "Left 2",   "Left 3",   "Bumper R",
    "Trigger R", "Thumb R",  "Right 1",    "Right 2",  "Right 3",  "Paddle 1",
    "Paddle 2",  "Paddle 3", "Paddle 4",   "Plus",     "Minus",    "Down Left Arrow",
    "Dial L",    "Dial R",   "Suspension",
};

template <typename T, typename M>
struct MemberName
{
  M T::* ptr;
  const char* name;
};

constexpr MemberName<WGI::GamepadReading, double> gamepad_trigger_names[] = {
    {&WGI::GamepadReading::LeftTrigger, "Trigger L"},
    {&WGI::GamepadReading::RightTrigger, "Trigger R"}};

constexpr MemberName<WGI::GamepadReading, double> gamepad_axis_names[] = {
    {&WGI::GamepadReading::LeftThumbstickX, "Left X"},
    {&WGI::GamepadReading::LeftThumbstickY, "Left Y"},
    {&WGI::GamepadReading::RightThumbstickX, "Right X"},
    {&WGI::GamepadReading::RightThumbstickY, "Right Y"}};

constexpr MemberName<WGI::GamepadVibration, double> gamepad_motor_names[] = {
    {&WGI::GamepadVibration::LeftMotor, "Motor L"},
    {&WGI::GamepadVibration::RightMotor, "Motor R"},
    {&WGI::GamepadVibration::LeftTrigger, "Trigger L"},
    {&WGI::GamepadVibration::RightTrigger, "Trigger R"}};

class Device : public Core::Device
{
public:
  Device(std::string name, Ptr<WGI::IRawGameController> raw_controller,
         Ptr<WGI::IGamepad> gamepad)
      : m_name(std::move(name)), m_raw_controller(std::move(raw_controller)),
        m_identity(Identity(m_raw_controller.Get())), m_gamepad(std::move(gamepad))
  {
    // Buttons:
    PopulateButtons();

    // Add inputs for IGamepad interface if available.
    if (m_gamepad)
    {
      // Axes:
      for (auto& axis : gamepad_axis_names)
      {
        AddInput(new NamedAxis(&(m_gamepad_reading.*axis.ptr), 0.0, -1.0, axis.name));
        AddInput(new NamedAxis(&(m_gamepad_reading.*axis.ptr), 0.0, +1.0, axis.name));
      }

      // Triggers:
      for (auto& trigger : gamepad_trigger_names)
        AddInput(new NamedTrigger(&(m_gamepad_reading.*trigger.ptr), trigger.name));

      // Motors:
      for (auto& motor : gamepad_motor_names)
        AddOutput(new NamedMotor(&(m_state_out.*motor.ptr), motor.name, this));
    }

    // Add IRawGameController's axes if IGamepad is not available.
    // This may need to change if some devices expose additional axes.
    // Maybe we can determine which additional axes are not in IGamepad's collection.
    const bool use_raw_controller_axes = !m_gamepad;

    if (use_raw_controller_axes)
    {
      INT32 count = 0;
      if (SUCCEEDED(m_raw_controller->get_AxisCount(&count)) && count > 0)
      {
        // Axes:
        m_axes.resize(count);

        u32 i = 0;
        for (auto& axis : m_axes)
        {
          AddFullAnalogSurfaceInputs(new IndexedAxis(&axis, 0.5, +0.5, i),
                                     new IndexedAxis(&axis, 0.5, -0.5, i));
          ++i;
        }
      }
      else if (count < 0)
      {
        ERROR_LOG_FMT(CONTROLLERINTERFACE, "WGInput: Error populating axes");
      }
    }

    // Apparently some devices (e.g. DS4) provide the IGameController interface
    // but expose the dpad only on a switch (IRawGameController interface).
    // We'll need to add switches regardless of available interfaces.
    PopulateSwitches();
  }

  int GetSortPriority() const override { return -1; }

  IUnknown* GetIdentity() const { return m_identity; }

private:
  using ButtonValueType = u8;

  class Button : public Input
  {
  public:
    explicit Button(const ButtonValueType* button) : m_button(*button) {}
    ControlState GetState() const override { return ControlState(m_button != 0); }

  private:
    const ButtonValueType& m_button;
  };

  // A button with one of the "labels" that WGI provides.
  class NamedButton final : public Button
  {
  public:
    NamedButton(const ButtonValueType* button, std::string_view name) : Button(button), m_name(name)
    {
    }
    std::string GetName() const override { return std::string(m_name); }

  private:
    const std::string_view m_name;
  };

  // A button with no label so we name it by its index.
  class IndexedButton final : public Button
  {
  public:
    IndexedButton(const ButtonValueType* button, u32 index) : Button(button), m_index(index) {}
    std::string GetName() const override { return fmt::format("Button {}", m_index); }

  private:
    u32 m_index;
  };

  class Axis : public Input
  {
  public:
    Axis(const double* axis, double base, double range)
        : m_base(base), m_range(range), m_axis(*axis)
    {
    }

    ControlState GetState() const override { return ControlState(m_axis - m_base) / m_range; }

  protected:
    const double m_base;
    const double m_range;

  private:
    const double& m_axis;
  };

  class NamedAxis final : public Axis
  {
  public:
    NamedAxis(const double* axis, double base, double range, std::string_view name)
        : Axis(axis, base, range), m_name(name)
    {
    }
    std::string GetName() const override
    {
      return fmt::format("{}{}", m_name, m_range < 0 ? '-' : '+');
    }

  private:
    const std::string_view m_name;
  };

  class NamedTrigger final : public Axis
  {
  public:
    NamedTrigger(const double* axis, std::string_view name) : Axis(axis, 0.0, 1.0), m_name(name) {}
    std::string GetName() const override { return std::string(m_name); }

  private:
    const std::string_view m_name;
  };

  class NamedMotor final : public Output
  {
  public:
    NamedMotor(double* motor, std::string_view name, Device* parent)
        : m_motor(*motor), m_name(name), m_parent(*parent)
    {
    }
    std::string GetName() const override { return std::string(m_name); }
    void SetState(ControlState state) override
    {
      if (m_motor == state)
        return;

      m_motor = state;
      m_parent.UpdateMotors();
    }

  private:
    double& m_motor;
    const std::string_view m_name;
    Device& m_parent;
  };

  class IndexedAxis final : public Axis
  {
  public:
    IndexedAxis(const double* axis, double base, double range, u32 index)
        : Axis(axis, base, range), m_index(index)
    {
    }
    std::string GetName() const override
    {
      return fmt::format("Axis {}{}", m_index, m_range < 0 ? '-' : '+');
    }

  private:
    const u32 m_index;
  };

  class IndexedSwitch final : public Input
  {
  public:
    IndexedSwitch(const WGI::GameControllerSwitchPosition* swtch, u32 index,
                  WGI::GameControllerSwitchPosition direction)
        : m_switch(*swtch), m_index(index), m_direction(static_cast<int32_t>(direction))
    {
    }
    std::string GetName() const override
    {
      return fmt::format("Switch {} {}", m_index, "NESW"[m_direction / 2]);
    }
    ControlState GetState() const override
    {
      if (m_switch == WGI::GameControllerSwitchPosition_Center)
        return 0.0;

      // All of the "in between" states (e.g. Up-Right) are one-off from the four cardinal
      // directions. This tests that the current switch state value is within 1 of the desired
      // state.
      const auto direction_diff = std::abs(static_cast<int32_t>(m_switch) - m_direction);
      return ControlState(direction_diff <= 1 || direction_diff == 7);
    }

  private:
    const WGI::GameControllerSwitchPosition& m_switch;
    const u32 m_index;
    const int32_t m_direction;
  };

  void PopulateButtons()
  {
    INT32 count = 0;
    if (FAILED(m_raw_controller->get_ButtonCount(&count)) || count < 0)
    {
      // If ButtonCount failed, m_buttons will be zero-sized.
      ERROR_LOG_FMT(CONTROLLERINTERFACE, "WGInput: Error populating buttons");
      return;
    }
    // Using RawGameController for buttons because it gives us a nice array instead of a bitmask.
    m_buttons.resize(count);

    u32 i = 0;
    for (const auto& button : m_buttons)
    {
      WGI::GameControllerButtonLabel lbl = WGI::GameControllerButtonLabel_None;
      if (FAILED(m_raw_controller->GetButtonLabel(static_cast<INT32>(i), &lbl)))
        lbl = WGI::GameControllerButtonLabel_None;

      const int32_t button_name_idx = static_cast<int32_t>(lbl);
      if (lbl != WGI::GameControllerButtonLabel_None && button_name_idx >= 0 &&
          button_name_idx < static_cast<int32_t>(wgi_button_names.size()))
        AddInput(new NamedButton(&button, wgi_button_names[button_name_idx]));
      else
        AddInput(new IndexedButton(&button, i));

      ++i;
    }
  }

  void PopulateSwitches()
  {
    INT32 count = 0;
    if (FAILED(m_raw_controller->get_SwitchCount(&count)) || count < 0)
    {
      ERROR_LOG_FMT(CONTROLLERINTERFACE, "WGInput: Error populating switches");
      return;
    }
    // Accumulate switch kinds first, to ensure no inputs are added if there is any error.
    std::vector<WGI::GameControllerSwitchKind> switch_kinds;
    for (INT32 i = 0; i < count; i++)
    {
      WGI::GameControllerSwitchKind kind{};
      if (FAILED(m_raw_controller->GetSwitchKind(i, &kind)))
      {
        // Safe as no inputs will have been added.
        ERROR_LOG_FMT(CONTROLLERINTERFACE, "WGInput: Error populating switches");
        return;
      }
      switch_kinds.push_back(kind);
    }
    m_switches.resize(count);

    u32 i = 0;
    for (auto& swtch : m_switches)
    {
      AddInput(new IndexedSwitch(&swtch, i, WGI::GameControllerSwitchPosition_Up));
      AddInput(new IndexedSwitch(&swtch, i, WGI::GameControllerSwitchPosition_Down));

      if (switch_kinds[i] != WGI::GameControllerSwitchKind_TwoWay)
      {
        // If it's not a "two-way" switch (up/down only) then add the left/right inputs.
        AddInput(new IndexedSwitch(&swtch, i, WGI::GameControllerSwitchPosition_Left));
        AddInput(new IndexedSwitch(&swtch, i, WGI::GameControllerSwitchPosition_Right));
      }

      ++i;
    }
  }

  std::string GetName() const override { return m_name; }

  std::string GetSource() const override { return std::string(SOURCE_NAME); }

  Core::DeviceRemoval UpdateInput() override
  {
    // IRawGameController (boolean is a byte, as ButtonValueType is):
    static_assert(sizeof(boolean) == sizeof(ButtonValueType));
    UINT64 timestamp = 0;
    const HRESULT raw_hr = m_raw_controller->GetCurrentReading(
        static_cast<UINT32>(m_buttons.size()), reinterpret_cast<boolean*>(m_buttons.data()),
        static_cast<UINT32>(m_switches.size()), m_switches.data(),
        static_cast<UINT32>(m_axes.size()), m_axes.data(), &timestamp);
    if (FAILED(raw_hr))
    {
      ERROR_LOG_FMT(CONTROLLERINTERFACE,
                    "WGInput: IRawGameController::GetCurrentReading failed: {:#x}",
                    static_cast<u32>(raw_hr));
    }

    // IGamepad:
    if (m_gamepad)
    {
      const HRESULT hr = m_gamepad->GetCurrentReading(&m_gamepad_reading);
      if (FAILED(hr))
      {
        ERROR_LOG_FMT(CONTROLLERINTERFACE, "WGInput: IGamepad::GetCurrentReading failed: {:#x}",
                      static_cast<u32>(hr));
      }
    }

    return Core::DeviceRemoval::Keep;
  }

  void UpdateMotors()
  {
    if (m_gamepad)
      m_gamepad->put_Vibration(m_state_out);
  }

  const std::string m_name;

  const Ptr<WGI::IRawGameController> m_raw_controller;
  IUnknown* const m_identity;
  std::vector<ButtonValueType> m_buttons;
  std::vector<WGI::GameControllerSwitchPosition> m_switches;
  std::vector<double> m_axes;

  const Ptr<WGI::IGamepad> m_gamepad;
  WGI::GamepadReading m_gamepad_reading{};
  WGI::GamepadVibration m_state_out{};
};

enum class AddRemoveEventType
{
  AddOrReplace,
  Remove,
};

struct AddRemoveEvent
{
  AddRemoveEventType type;
  Ptr<WGI::IRawGameController> raw_game_controller;
};

bool s_loaded = false;
CO_MTA_USAGE_COOKIE s_mta_cookie = nullptr;
EventRegistrationToken s_event_added{}, s_event_removed{};
Common::WorkQueueThread<AddRemoveEvent> s_device_add_remove_queue;

std::string DeviceName(const Ptr<WGI::IRawGameController>& raw_game_controller)
{
  // Get user-facing device name if available. Otherwise generate a name from vid/pid.
  std::string device_name;
  if (const auto raw2 = raw_game_controller.As<WGI::IRawGameController2>())
  {
    HSTRING name = nullptr;
    if (SUCCEEDED(raw2->get_DisplayName(&name)))
    {
      UINT32 length = 0;
      const wchar_t* text = s_combase.WindowsGetStringRawBuffer(name, &length);
      device_name = StripWhitespace(WStringToUTF8(std::wstring(text ? text : L"", length)));
      s_combase.WindowsDeleteString(name);
    }
  }
  if (device_name.empty())
  {
    UINT16 vid = 0, pid = 0;
    raw_game_controller->get_HardwareVendorId(&vid);
    raw_game_controller->get_HardwareProductId(&pid);
    device_name = fmt::format("Device_{:04x}:{:04x}", vid, pid);
    INFO_LOG_FMT(CONTROLLERINTERFACE, "WGInput: Empty device name, using {}", device_name);
  }
  return device_name;
}

void AddDevice(const Ptr<WGI::IRawGameController>& raw_game_controller)
{
  std::string device_name = DeviceName(raw_game_controller);

  // Note that gamepad may be null here. The Device class will deal with this.
  Ptr<WGI::IGamepad> gamepad;
  const auto statics =
      GetFactory<WGI::IGamepadStatics2>(L"Windows.Gaming.Input.Gamepad");
  const auto game_controller = raw_game_controller.As<WGI::IGameController>();
  if (statics && game_controller)
    statics->FromGameController(game_controller.Get(), gamepad.Put());

  auto dev = std::make_shared<Device>(std::move(device_name), raw_game_controller, gamepad);

  // Only add if it has some inputs/outputs.
  if (dev->Inputs().size() || dev->Outputs().size())
    g_controller_interface.AddDevice(std::move(dev));
}

void RemoveDevice(const Ptr<WGI::IRawGameController>& raw_game_controller)
{
  IUnknown* const identity = Identity(raw_game_controller.Get());
  g_controller_interface.RemoveDevice([&](const auto* dev) {
    if (dev->GetSource() != SOURCE_NAME)
      return false;
    return static_cast<const Device*>(dev)->GetIdentity() == identity;
  });
}

void HandleAddRemoveEvent(AddRemoveEvent evt)
{
  switch (evt.type)
  {
  case AddRemoveEventType::AddOrReplace:
    RemoveDevice(evt.raw_game_controller);
    AddDevice(evt.raw_game_controller);
    break;
  case AddRemoveEventType::Remove:
    RemoveDevice(evt.raw_game_controller);
    break;
  }
}

// RawGameControllerAdded/Removed, invoked from a WGI-managed threadpool.
class Handler final : public ABI::Windows::Foundation::IEventHandler<WGI::RawGameController*>
{
public:
  explicit Handler(AddRemoveEventType type) : m_type(type) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
  {
    if (!object)
      return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IAgileObject) ||
        riid == __uuidof(ABI::Windows::Foundation::IEventHandler<WGI::RawGameController*>))
    {
      *object = this;
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refs); }
  ULONG STDMETHODCALLTYPE Release() override
  {
    const ULONG refs = InterlockedDecrement(&m_refs);
    if (refs == 0)
      delete this;
    return refs;
  }
  HRESULT STDMETHODCALLTYPE Invoke(IInspectable*, WGI::IRawGameController* args) override
  {
    if (args)
      s_device_add_remove_queue.EmplaceItem(AddRemoveEvent{m_type, Ptr(args, true)});
    return S_OK;
  }

private:
  const AddRemoveEventType m_type;
  LONG m_refs = 1;
};
}  // namespace

void Init()
{
  if (!LoadCombase())
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "WGInput: combase.dll lacks WinRT");
    return;
  }
  // Keeps the process's multithreaded apartment up, so every thread that reads the devices (the
  // engine's, whatever their own COM state) and the add/remove thread can use WGI's objects.
  if (FAILED(s_combase.CoIncrementMTAUsage(&s_mta_cookie)))
    s_mta_cookie = nullptr;
  s_loaded = true;

  s_device_add_remove_queue.Reset("WGInput Add/Remove Device Thread", HandleAddRemoveEvent);

  const auto statics = RawStatics();
  if (!statics)
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "WGInput: Failed to register event handlers");
    return;
  }
  // These events will be invoked from WGI-managed threadpool.
  auto* added = new Handler(AddRemoveEventType::AddOrReplace);
  auto* removed = new Handler(AddRemoveEventType::Remove);
  if (FAILED(statics->add_RawGameControllerAdded(added, &s_event_added)) ||
      FAILED(statics->add_RawGameControllerRemoved(removed, &s_event_removed)))
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "WGInput: Failed to register event handlers");
  }
  added->Release();
  removed->Release();
}

void DeInit()
{
  if (!s_loaded)
    return;

  if (const auto statics = RawStatics())
  {
    if (s_event_added.value)
      statics->remove_RawGameControllerAdded(s_event_added);
    if (s_event_removed.value)
      statics->remove_RawGameControllerRemoved(s_event_removed);
  }
  s_event_added = {};
  s_event_removed = {};

  s_device_add_remove_queue.Shutdown();

  if (s_mta_cookie)
  {
    s_combase.CoDecrementMTAUsage(s_mta_cookie);
    s_mta_cookie = nullptr;
  }
  s_loaded = false;
}

void PopulateDevices()
{
  // WGI Interfaces to potentially use:
  // Gamepad: Buttons, 2x Sticks and 2x Triggers, 4x Vibration Motors
  // RawGameController: Buttons, Switches (Hats), Axes, Haptics
  // The following are not implemented:
  // ArcadeStick: Buttons (probably no need to specialize, literally just buttons)
  // FlightStick: Buttons, HatSwitch, Pitch, Roll, Throttle, Yaw
  // RacingWheel: Buttons, Clutch, Handbrake, PatternShifterGear, Throttle, Wheel, WheelMotor
  // UINavigationController: Directions, Scrolling, etc.

  // RawGameController available from Windows 15063.
  //  properties added in 1709: DisplayName, NonRoamableId, SimpleHapticsControllers
  if (!s_loaded)
    return;

  const auto statics = RawStatics();
  Ptr<ABI::Windows::Foundation::Collections::IVectorView<WGI::RawGameController*>> list;
  UINT32 size = 0;
  if (!statics || FAILED(statics->get_RawGameControllers(list.Put())) || !list ||
      FAILED(list->get_Size(&size)))
  {
    // Only reach here if RawGameControllers() failed
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "WGInput: PopulateDevices failed");
    return;
  }
  for (UINT32 i = 0; i < size; ++i)
  {
    Ptr<WGI::IRawGameController> raw_game_controller;
    if (FAILED(list->GetAt(i, raw_game_controller.Put())) || !raw_game_controller)
      continue;
    RemoveDevice(raw_game_controller);
    AddDevice(raw_game_controller);
  }
}

}  // namespace ciface::WGInput
