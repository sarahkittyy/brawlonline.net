project(hidapi)

add_library(hidapi STATIC hidapi-src/hidapi/hidapi.h)
dolphin_disable_warnings(hidapi)
target_include_directories(hidapi PUBLIC hidapi-src/hidapi)

if(APPLE)
  target_sources(hidapi PRIVATE hidapi-src/mac/hid.c)
  # SDL3's own copy of hidapi defines the same macOS-only extras. Apple's ld64 lets the first
  # archive win; LLVM's ld64.lld (the macOS cross build) refuses the duplicates. Dolphin never
  # calls these, so this copy's are renamed.
  foreach(fn get_location_id set_open_exclusive get_open_exclusive is_device_open_exclusive)
    target_compile_definitions(hidapi PRIVATE hid_darwin_${fn}=dolphin_hid_darwin_${fn})
  endforeach()
elseif(WIN32)
  target_sources(hidapi PRIVATE hidapi-src/windows/hid.c)
else()
  find_package(LIBUDEV)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND LIBUDEV_FOUND)
    target_sources(hidapi PRIVATE hidapi-src/linux/hid.c)
    target_link_libraries(hidapi PRIVATE udev)
  else()
    target_sources(hidapi PRIVATE hidapi-src/libusb/hid.c)
    target_link_libraries(hidapi PRIVATE LibUSB::LibUSB)
  endif()
endif()

add_library(Hidapi::Hidapi ALIAS hidapi)
