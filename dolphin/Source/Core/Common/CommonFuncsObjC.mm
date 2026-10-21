// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/CommonFuncs.h"

#include <Foundation/Foundation.h>
#include <objc/message.h>

namespace Common
{
MacOSVersion GetMacOSVersion()
{
  const NSOperatingSystemVersion ver = [[NSProcessInfo processInfo] operatingSystemVersion];
  return {ver.majorVersion, ver.minorVersion, ver.patchVersion};
}

void HideFromDock()
{
  // [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited] (2), through the runtime:
  // Common links Foundation only, and AppKit is there once Qt's application exists.
  Class app_class = NSClassFromString(@"NSApplication");
  if (!app_class)
    return;
  id app = ((id(*)(id, SEL))objc_msgSend)((id)app_class, sel_registerName("sharedApplication"));
  if (app)
    ((BOOL(*)(id, SEL, long))objc_msgSend)(app, sel_registerName("setActivationPolicy:"), 2);
}
}  // namespace Common
