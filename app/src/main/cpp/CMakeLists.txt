cmake_minimum_required(VERSION 3.22.1)
project("uvccapturecalibration")

set(LIBUSB_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libusb)
find_library(log_lib log)
find_library(m_lib m)

add_library(usb-1.0 STATIC
        ${LIBUSB_ROOT}/libusb/core.c
        ${LIBUSB_ROOT}/libusb/descriptor.c
        ${LIBUSB_ROOT}/libusb/hotplug.c
        ${LIBUSB_ROOT}/libusb/io.c
        ${LIBUSB_ROOT}/libusb/sync.c
        ${LIBUSB_ROOT}/libusb/strerror.c
        ${LIBUSB_ROOT}/libusb/os/linux_usbfs.c
        ${LIBUSB_ROOT}/libusb/os/events_posix.c
        ${LIBUSB_ROOT}/libusb/os/threads_posix.c
        ${LIBUSB_ROOT}/libusb/os/linux_netlink.c)

target_include_directories(usb-1.0 PRIVATE
        ${LIBUSB_ROOT}/android
        ${LIBUSB_ROOT}/libusb
        ${LIBUSB_ROOT}/libusb/os)
target_compile_options(usb-1.0 PRIVATE -fvisibility=hidden -pthread)
target_link_libraries(usb-1.0 ${log_lib})

set(LIBJPEG_TURBO_ROOT
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libjpeg-turbo/${ANDROID_ABI})
add_library(turbojpeg STATIC IMPORTED GLOBAL)
set_target_properties(turbojpeg PROPERTIES
        IMPORTED_LOCATION ${LIBJPEG_TURBO_ROOT}/lib/libturbojpeg.a
        INTERFACE_INCLUDE_DIRECTORIES ${LIBJPEG_TURBO_ROOT}/include)

add_library(${CMAKE_PROJECT_NAME} SHARED
        native-lib.cpp
        calibration_progress.cpp
        measurement_engine.cpp
        pu_auto_calibration.cpp
        capture_solver.cpp
        validation_engine.cpp
        profile_engine.cpp
        uvc_device.cpp
        uvc_stream.cpp
        uvc_mjpeg_decoder.cpp)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${LIBUSB_ROOT}/libusb)
target_link_libraries(${CMAKE_PROJECT_NAME}
        usb-1.0
        turbojpeg
        android
        ${log_lib}
        ${m_lib})
