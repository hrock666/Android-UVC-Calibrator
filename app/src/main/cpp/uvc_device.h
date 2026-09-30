#pragma once

struct libusb_device_handle;

namespace uvc_device {

// Wrap an Android UsbDeviceConnection file descriptor with libusb,
// discover the required 1280x720 MJPEG 60 fps UVC mode, negotiate/commit it,
// and select the streaming backend/alternate setting.
//
// The Android-owned fd remains owned by UsbDeviceConnection.
// It must stay open until close() has returned.
bool openFromAndroidFd(int fd);

// Return the VideoStreaming interface to alt 0, release it,
// close the libusb wrapper/context, and forget the Android fd.
void close();

// Borrowed handle for calibration controls while the stream is open.
// Ownership remains in uvc_device; callers must not close it.
libusb_device_handle* calibrationHandle();

}  // namespace uvc_device
