// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// BrawlOnline/DolphinPads.h outside an engine.
//
//   brawl_online_dolphin_pads_probe <User folder> [seconds]
//     lists the devices Dolphin's input code finds here, loads the folder's GCPadNew.ini and
//     prints each standard controller port's pad whenever it changes.
//   brawl_online_dolphin_pads_probe --selftest <scratch folder>
//     writes a GCPadNew.ini for a scripted keyboard ("Probe/0/Keys": a C-stick with a diagonal key
//     besides its four, a control stick with a modifier, L/R and their analog halves) and checks
//     the pads Dolphin's GCPad makes from it. Exit code 0 when every check passes.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include "BrawlOnline/DolphinPads.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "InputCommon/GCPadStatus.h"

namespace
{
namespace DP = BrawlOnline::DolphinPads;

std::map<std::string, double> s_keys;

class Key final : public ciface::Core::Device::Input
{
public:
  explicit Key(std::string name) : m_name(std::move(name)) {}
  std::string GetName() const override { return m_name; }
  ControlState GetState() const override
  {
    const auto it = s_keys.find(m_name);
    return it == s_keys.end() ? 0.0 : it->second;
  }

private:
  std::string m_name;
};

class Keys final : public ciface::Core::Device
{
public:
  Keys()
  {
    for (const char* k : {"I", "J", "K", "L", "O", "W", "A", "S", "D", "LSHIFT", "Q", "E", "X",
                          "Z", "RETURN", "1"})
    {
      AddInput(new Key(k));
    }
  }
  std::string GetName() const override { return "Keys"; }
  std::string GetSource() const override { return "Probe"; }
};

std::string Describe(const GCPadStatus& s)
{
  char line[160];
  std::snprintf(line, sizeof(line),
                "connected %d buttons %04x stick %3d,%3d c %3d,%3d L %3d R %3d", s.isConnected,
                s.button, s.stickX, s.stickY, s.substickX, s.substickY, s.triggerLeft,
                s.triggerRight);
  return line;
}

void* Window()
{
#ifdef _WIN32
  return GetConsoleWindow();
#else
  return nullptr;
#endif
}

int Probe(const std::string& user, int seconds)
{
  const bool loaded = DP::Load(user, Window());
  std::printf("devices:\n");
  for (const std::string& d : g_controller_interface.GetAllDeviceStrings())
    std::printf("  %s\n", d.c_str());
  if (!loaded)
  {
    std::printf("no %s/Config/GCPadNew.ini\n", user.c_str());
    return 1;
  }
  for (int p = 0; p < 4; ++p)
    std::printf("port %d: SIDevice %d, device %s\n", p + 1, DP::SiDevice(p), DP::DefaultDevice(p).c_str());
  std::string last[4];
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < end)
  {
    DP::UpdateInput(true);
    for (int p = 0; p < 4; ++p)
    {
      if (DP::SiDevice(p) != DP::SI_STANDARD_CONTROLLER)
        continue;
      const std::string now = Describe(DP::GetStatus(p));
      if (now != last[p])
        std::printf("port %d: %s\n", p + 1, now.c_str());
      last[p] = now;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  DP::Shutdown();
  return 0;
}

int s_failures = 0;

void Check(const char* what, bool ok, const GCPadStatus& s)
{
  std::printf("%s %s: %s\n", ok ? "ok  " : "FAIL", what, Describe(s).c_str());
  if (!ok)
    ++s_failures;
}

int SelfTest(const std::filesystem::path& folder)
{
  std::filesystem::create_directories(folder / "Config");
  {
    std::ofstream ini(folder / "Config" / "Dolphin.ini");
    ini << "[Core]\nSIDevice0 = 6\nSIDevice1 = 0\nSIDevice2 = 0\nSIDevice3 = 0\n";
  }
  {
    // As Dolphin's Controllers window writes it (its default keyboard layout, square calibration,
    // and the player's diagonal: O on both C-Stick/Up and C-Stick/Right).
    std::ofstream ini(folder / "Config" / "GCPadNew.ini");
    ini << "[GCPad1]\n"
           "Device = Probe/0/Keys\n"
           "Buttons/A = X\n"
           "Buttons/B = Z\n"
           "Buttons/Start = RETURN\n"
           "Main Stick/Up = W\n"
           "Main Stick/Down = S\n"
           "Main Stick/Left = A\n"
           "Main Stick/Right = D\n"
           "Main Stick/Modifier = LSHIFT\n"
           "Main Stick/Modifier/Range = 50.000000000000000\n"
           "Main Stick/Calibration = 100.00 141.42 100.00 141.42 100.00 141.42 100.00 141.42\n"
           "C-Stick/Up = I | O\n"
           "C-Stick/Down = K\n"
           "C-Stick/Left = J\n"
           "C-Stick/Right = L | O\n"
           "C-Stick/Calibration = 100.00 141.42 100.00 141.42 100.00 141.42 100.00 141.42\n"
           "Triggers/L = Q\n"
           "Triggers/R = E\n"
           "Triggers/L-Analog = Q | `1` & !LSHIFT\n"
           "Triggers/R-Analog = E\n";
  }
  DP::Load(folder.string(), Window());
  DP::AddDevice(std::make_shared<Keys>());
  const auto read = [](std::map<std::string, double> keys) {
    s_keys = std::move(keys);
    DP::UpdateInput(true);
    return DP::GetStatus(0);
  };

  GCPadStatus s = read({});
  Check("at rest", s.isConnected && s.button == 0 && s.stickX == 128 && s.stickY == 128 &&
                       s.substickX == 128 && s.substickY == 128,
        s);
  s = read({{"I", 1}});
  Check("C up alone", s.substickX == 128 && s.substickY > 200, s);
  const int c_full = s.substickY;
  s = read({{"L", 1}});
  Check("C right alone", s.substickY == 128 && s.substickX == c_full, s);
  s = read({{"O", 1}});
  Check("O: C up-right", s.substickX > 128 && s.substickX == s.substickY && s.substickX < c_full, s);
  const GCPadStatus diagonal = s;
  s = read({{"I", 1}, {"L", 1}});
  Check("I+L = O", s.substickX == diagonal.substickX && s.substickY == diagonal.substickY, s);
  s = read({{"D", 1}});
  Check("stick right", s.stickX > 200 && s.stickY == 128, s);
  const int full = s.stickX;
  s = read({{"D", 1}, {"LSHIFT", 1}});
  Check("stick right with the modifier: half", s.stickX > 128 && s.stickX < full &&
                                                   std::abs((s.stickX - 128) * 2 - (full - 128)) <= 1,
        s);
  s = read({{"X", 1}, {"RETURN", 1}});
  Check("A + Start", s.button == (PAD_BUTTON_A | PAD_BUTTON_START), s);
  s = read({{"Q", 1}});
  Check("L: digital and analog", (s.button & PAD_TRIGGER_L) != 0 && s.triggerLeft == 255, s);
  s = read({{"1", 1}});
  Check("analog L from `1` & !LSHIFT", s.triggerLeft == 255, s);
  s = read({{"1", 1}, {"LSHIFT", 1}});
  Check("`1` with LSHIFT: no analog L", s.triggerLeft == 0, s);

  // The gate: without focus (BackgroundInput off) nothing counts.
  s_keys = {{"X", 1}, {"D", 1}};
  DP::UpdateInput(false);
  s = DP::GetStatus(0);
  Check("no focus: at rest", s.button == 0 && s.stickX == 128, s);

  // Dolphin's window saves a change: the next ReloadIfChanged takes it.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  {
    std::ofstream ini(folder / "Config" / "GCPadNew.ini", std::ios::app);
  }
  {
    std::ifstream in(folder / "Config" / "GCPadNew.ini");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto at = text.find("Buttons/A = X");
    text.replace(at, 13, "Buttons/A = Z");
    in.close();
    std::ofstream out(folder / "Config" / "GCPadNew.ini");
    out << text;
  }
  const bool reloaded = DP::ReloadIfChanged();
  s = read({{"Z", 1}});
  Check("reloaded: A is Z now", reloaded && (s.button & PAD_BUTTON_A) != 0, s);

  DP::Shutdown();
  std::printf("%s\n", s_failures == 0 ? "all passed" : "FAILED");
  return s_failures == 0 ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv)
{
  if (argc >= 3 && std::string(argv[1]) == "--selftest")
    return SelfTest(argv[2]);
  if (argc < 2)
  {
    std::fprintf(stderr, "usage: %s <User folder> [seconds] | --selftest <scratch folder>\n", argv[0]);
    return 2;
  }
  return Probe(argv[1], argc >= 3 ? std::atoi(argv[2]) : 0);
}
