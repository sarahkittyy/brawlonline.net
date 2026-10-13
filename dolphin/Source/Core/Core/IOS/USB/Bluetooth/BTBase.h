// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/IOS.h"
#include "Core/System.h"

class PointerWrap;
class SysConf;

namespace IOS::HLE
{
template <typename T>
static void DoStateForMessage(EmulationKernel& ios, PointerWrap& p, std::unique_ptr<T>& message)
{
  u32 request_address = (message != nullptr) ? message->ios_request.address : 0;
  p.Do(request_address);
  // Saving needs only the address (the message is rebuilt from guest RAM when loading).
  if (!p.IsReadMode())
  {
    if (request_address != 0 && p.IsWriteMode())
    {
      auto& memory = ios.GetSystem().GetMemory();
      if (memory.Read_U32(request_address + 0x10) < 2)
      {
        static int s_reported = 0;
        if (s_reported++ < 5)
        {
          ERROR_LOG_FMT(IOS_WIIMOTE,
                        "Bluetooth state: pending request {:08x} no longer an ioctlv in RAM: "
                        "{:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x}",
                        request_address, memory.Read_U32(request_address),
                        memory.Read_U32(request_address + 4), memory.Read_U32(request_address + 8),
                        memory.Read_U32(request_address + 12), memory.Read_U32(request_address + 16),
                        memory.Read_U32(request_address + 20), memory.Read_U32(request_address + 24));
        }
      }
    }
    return;
  }
  message.reset();
  if (request_address != 0)
  {
    IOCtlVRequest request{ios.GetSystem(), request_address};
    // The state may come from another machine (a machine image), and the request is guest RAM:
    // the message reads two in vectors and one out vector.
    if (request.in_vectors.size() < 2 || request.io_vectors.empty())
    {
      ERROR_LOG_FMT(IOS_WIIMOTE, "Bluetooth state: the pending request at {:08x} is not a USB "
                                 "ioctlv ({} in, {} out vectors)",
                    request_address, request.in_vectors.size(), request.io_vectors.size());
      p.SetMeasureMode();
      return;
    }
    message = std::make_unique<T>(ios, request);
  }
}

void BackUpBTInfoSection(const SysConf* sysconf);
void RestoreBTInfoSection(SysConf* sysconf);

class BluetoothBaseDevice : public EmulationDevice
{
public:
  using EmulationDevice::EmulationDevice;
  virtual void UpdateSyncButtonState(bool is_held) {}
  virtual void TriggerSyncButtonPressedEvent() {}
  virtual void TriggerSyncButtonHeldEvent() {}

protected:
  static constexpr int ACL_PKT_SIZE = 339;
  static constexpr int ACL_PKT_NUM = 10;
  static constexpr int SCO_PKT_SIZE = 64;
  static constexpr int SCO_PKT_NUM = 0;

  enum USBEndpoint
  {
    HCI_CTRL = 0x00,
    HCI_EVENT = 0x81,
    ACL_DATA_IN = 0x82,
    ACL_DATA_OUT = 0x02
  };
};
}  // namespace IOS::HLE
