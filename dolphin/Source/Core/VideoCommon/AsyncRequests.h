// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <concepts>
#include <functional>
#include <future>

#include "Common/Functional.h"
#include "Common/SPSCQueue.h"

struct EfbPokeData;
class PointerWrap;

class AsyncRequests
{
public:
  AsyncRequests();

  // Called from the Video thread.
  void PullEvents();

  // The following are called from the CPU thread.
  void WaitForEmptyQueue();

  template <std::invocable<> F>
  void PushEvent(F&& callback)
  {
    if (m_passthrough)
    {
      std::invoke(std::forward<F>(callback));
      return;
    }

    QueueEvent(Event{std::forward<F>(callback)});
  }

  // A request made while emulated time stands still between two frames (a rollback session
  // waiting for its peers, a paused replay): it only wakes the video thread. PushEvent also runs
  // Fifo::RunGpu, which schedules the CPU thread's GPU-sync event when syncing is suspended, so a
  // machine that waited got an event the others did not have, its later GPU syncs and token
  // interrupts came at other times, and the game's own timing parted (bug report #213).
  template <std::invocable<> F>
  void PushEventWhileStopped(F&& callback)
  {
    if (m_passthrough)
    {
      std::invoke(std::forward<F>(callback));
      return;
    }

    QueueEvent(Event{std::forward<F>(callback)}, false);
  }

  template <std::invocable<> F>
  auto PushBlockingEvent(F&& callback) -> std::invoke_result_t<F>
  {
    if (m_passthrough)
      return std::invoke(std::forward<F>(callback));

    std::packaged_task task{std::forward<F>(callback)};
    QueueEvent(Event{[&] { task(); }});

    return task.get_future().get();
  }

  // Not thread-safe. Only set during initialization.
  void SetPassthrough(bool enable);

  static AsyncRequests* GetInstance() { return &s_singleton; }

private:
  using Event = Common::MoveOnlyFunction<void()>;

  void QueueEvent(Event&& event, bool run_gpu = true);

  static AsyncRequests s_singleton;

  Common::WaitableSPSCQueue<Event> m_queue;

  bool m_passthrough = true;
};
