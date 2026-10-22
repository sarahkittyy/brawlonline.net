// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/AsyncRequests.h"

#include "Core/Rollback/MachineSession.h"
#include "Core/Rollback/RollbackManager.h"
#include "Core/System.h"

#include "VideoCommon/Fifo.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoEvents.h"

AsyncRequests AsyncRequests::s_singleton;

AsyncRequests::AsyncRequests() = default;

void AsyncRequests::PullEvents()
{
  if (m_queue.Empty())
    return;

  // This is only called if the queue isn't empty.
  // So just flush the pipeline to get accurate results.
  g_vertex_manager->Flush();

  while (!m_queue.Empty())
  {
    std::invoke(std::move(m_queue.Front()));
    m_queue.Pop();
  }
}

void AsyncRequests::QueueEvent(Event&& event)
{
  m_queue.Push(std::move(event));

  auto& system = Core::System::GetInstance();
  // Not while a whole-machine snapshot is saved or loaded (its video state is restored on the video
  // thread from here): RunGpu schedules the GPU-sync event when syncing is suspended, an event the
  // saved state did not have, and its ForceExceptionCheck cut the slice the load had just restored.
  // Nor while a machine image is saved or loaded: the image then held a GPU-sync event (and the
  // event order one higher) that the players' own snapshots of that frame did not, so a player
  // who rolled back past the epoch and one who did not (or a joiner) saved different images at the
  // next epoch, and the joiner refused it (bug 90: "player 2 does not vouch for this machine").
  if (!Rollback::InSnapshotDoState() && !Machine::InImageDoState())
    system.GetFifo().RunGpu();
  system.GetFifo().WakeGpuThread();
}

void AsyncRequests::WaitForEmptyQueue()
{
  m_queue.WaitForEmpty();
}

void AsyncRequests::SetPassthrough(bool enable)
{
  m_passthrough = enable;
}
