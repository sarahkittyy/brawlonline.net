function(dolphin_inject_version_info target)
  set(INFO_PLIST_PATH "$<TARGET_BUNDLE_DIR:${target}>/Contents/Info.plist")
  if(NOT CMAKE_HOST_APPLE)
    # Cross-compiling (osxcross): no PlistBuddy, the same edit in Python.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_command(TARGET ${target}
      POST_BUILD
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/Tools/merge-version-plist.py"
      "${INFO_PLIST_PATH}" "${CMAKE_BINARY_DIR}/Source/Core/VersionInfo.plist")
    return()
  endif()
  add_custom_command(TARGET ${target}
    POST_BUILD

    COMMAND /usr/libexec/PlistBuddy -c
    "Delete :CFBundleShortVersionString"
    "${INFO_PLIST_PATH}"
    || true

    COMMAND /usr/libexec/PlistBuddy -c
    "Delete :CFBundleLongVersionString"
    "${INFO_PLIST_PATH}"
    || true

    COMMAND /usr/libexec/PlistBuddy -c
    "Delete :CFBundleVersion"
    "${INFO_PLIST_PATH}"
    || true

    COMMAND /usr/libexec/PlistBuddy -c
    "Merge '${CMAKE_BINARY_DIR}/Source/Core/VersionInfo.plist'"
    "${INFO_PLIST_PATH}")
endfunction()
