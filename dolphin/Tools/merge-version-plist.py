#!/usr/bin/env python3
# What DolphinInjectVersionInfo.cmake does with /usr/libexec/PlistBuddy on a Mac, for builds
# cross-compiled on other hosts: replace the bundle's version keys with VersionInfo.plist's.
#
#   merge-version-plist.py <Info.plist> <VersionInfo.plist>
import plistlib
import sys

info_path, version_path = sys.argv[1], sys.argv[2]
with open(info_path, "rb") as f:
    info = plistlib.load(f)
with open(version_path, "rb") as f:
    version = plistlib.load(f)

for key in ("CFBundleShortVersionString", "CFBundleLongVersionString", "CFBundleVersion"):
    info.pop(key, None)
for key, value in version.items():
    info.setdefault(key, value)

with open(info_path, "wb") as f:
    plistlib.dump(info, f)
