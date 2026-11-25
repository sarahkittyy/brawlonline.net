// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The engine's SDL gamepads as Dolphin's SDL backend lists them (BrawlOnline/DolphinPads.h): the
// same Gamepad class (InputCommon/ControllerInterface/SDL/SDLGamepad.cpp) over the engine's own
// SDL, which pumps the events. Dolphin's backend runs SDL itself on a thread of its own; here the
// joysticks are compared with SDL's list instead of waiting for its added and removed events.

#include <algorithm>
#include <mutex>
#include <span>
#include <vector>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>

#include "BrawlOnline/DolphinPads.h"
#include "InputCommon/ControllerInterface/SDL/SDLGamepad.h"

namespace BrawlOnline::DolphinPads
{
namespace
{
std::mutex s_sdl_mutex;
std::vector<SDL_JoystickID> s_open;
}  // namespace

void SyncSdlDevices()
{
  std::lock_guard lk(s_sdl_mutex);
  int count = 0;
  SDL_JoystickID* const ids = SDL_GetJoysticks(&count);
  const std::span<SDL_JoystickID> now(ids, ids != nullptr ? count : 0);

  std::vector<SDL_JoystickID> gone;
  for (const SDL_JoystickID id : s_open)
  {
    if (std::find(now.begin(), now.end(), id) == now.end())
      gone.push_back(id);
  }
  if (!gone.empty())
  {
    RemoveDevices([&gone](const ciface::Core::Device* device) {
      return device->GetSource() == "SDL" &&
             std::find(gone.begin(), gone.end(),
                       static_cast<const ciface::SDL::Gamepad*>(device)->GetSDLInstanceID()) !=
                 gone.end();
    });
    std::erase_if(s_open, [&gone](SDL_JoystickID id) {
      return std::find(gone.begin(), gone.end(), id) != gone.end();
    });
  }

  // InputBackend::OpenAndAddDevice, in SDL's order.
  for (const SDL_JoystickID id : now)
  {
    if (std::find(s_open.begin(), s_open.end(), id) != s_open.end())
      continue;
    s_open.push_back(id);
    SDL_Gamepad* const gc = SDL_OpenGamepad(id);
    SDL_Joystick* const js = SDL_OpenJoystick(id);
    if (js == nullptr)
      continue;
    if (SDL_GetNumJoystickButtons(js) > 255 || SDL_GetNumJoystickAxes(js) > 255 ||
        SDL_GetNumJoystickHats(js) > 255 || SDL_GetNumJoystickBalls(js) > 255)
    {
      continue;
    }
    auto gamepad = std::make_shared<ciface::SDL::Gamepad>(gc, js);
    if (!gamepad->Inputs().empty() || !gamepad->Outputs().empty())
      AddDevice(std::move(gamepad));
  }
  SDL_free(ids);
}
}  // namespace BrawlOnline::DolphinPads
