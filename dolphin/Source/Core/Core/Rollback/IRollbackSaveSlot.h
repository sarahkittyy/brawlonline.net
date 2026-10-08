// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core
{
class System;
}

namespace Rollback
{

class IRollbackSaveSlot
{
public:
  virtual ~IRollbackSaveSlot() = default;

  virtual bool HasState() const = 0;
  virtual void Reset() = 0;
  virtual void Save(Core::System& system) = 0;
  virtual void Load(Core::System& system) = 0;
};

}  // namespace Rollback
