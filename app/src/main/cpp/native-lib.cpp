#include <jni.h>
#include <libusb.h>

#include "calibration_progress.h"
#include "measurement_engine.h"
#include "pu_auto_calibration.h"
#include "capture_solver.h"
#include "uvc_device.h"
#include "validation_engine.h"
#include "profile_engine.h"
#include "uvc_mjpeg_decoder.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

uint64_t gPreviewSequence = 0;
std::mutex gPreviewMutex;
bool gPreviewEnabled = true;
uint64_t gMeasurementPreviewSequence = 0;

int clampByte(int value) {
    return std::max(0, std::min(255, value));
}

class ScopedProgressCallback {
public:
    ScopedProgressCallback(JNIEnv* env, jobject target) {
        jclass targetClass = env->GetObjectClass(target);
        const jmethodID method = env->GetMethodID(
                targetClass, "onNativeCalibrationProgress", "(ILjava/lang/String;)V");
        env->DeleteLocalRef(targetClass);
        if (method == nullptr) return;
        calibration_progress::setCallback([env, target, method](
                int percent, const std::string& message) {
            jstring javaMessage = env->NewStringUTF(message.c_str());
            env->CallVoidMethod(target, method, static_cast<jint>(percent), javaMessage);
            env->DeleteLocalRef(javaMessage);
        });
        installed_ = true;
    }

    ~ScopedProgressCallback() {
        if (installed_) calibration_progress::clearCallback();
    }

private:
    bool installed_ = false;
};

constexpr unsigned int CONTROL_TIMEOUT_MS = 1000;
constexpr uint8_t USB_DT_CS_INTERFACE = 0x24;
constexpr uint8_t UVC_VC_PROCESSING_UNIT = 0x05;
constexpr uint8_t UVC_SET_CUR = 0x01;
constexpr uint8_t UVC_GET_CUR = 0x81;
constexpr uint8_t UVC_GET_MIN = 0x82;
constexpr uint8_t UVC_GET_MAX = 0x83;
constexpr uint8_t UVC_GET_RES = 0x84;
constexpr uint8_t UVC_GET_LEN = 0x85;
constexpr uint8_t UVC_GET_INFO = 0x86;
constexpr uint8_t UVC_GET_DEF = 0x87;
constexpr uint8_t UVC_REQ_IN = LIBUSB_ENDPOINT_IN |
        LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE;
constexpr uint8_t UVC_REQ_OUT = LIBUSB_ENDPOINT_OUT |
        LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE;

struct ProcessingUnit {
    uint8_t interfaceNumber = 0;
    uint8_t unitId = 0;
    std::vector<uint8_t> controls;
};

struct ControlDefinition {
    const char* name;
    uint8_t selector;
    bool signedValue;
};

constexpr std::array<ControlDefinition, 4> CONTROLS{{
        {"Brightness", 0x02, true},
        {"Contrast", 0x03, false},
        {"Saturation", 0x07, false},
        {"Hue", 0x06, true},
}};

uint16_t readLe16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

std::string rawBytes(const std::vector<uint8_t>& data) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0');
    for (size_t i = 0; i < data.size(); ++i) {
        if (i != 0) out << ' ';
        out << std::setw(2) << static_cast<unsigned int>(data[i]);
    }
    return out.str();
}

std::string usbError(int result) {
    if (result >= 0) return "short transfer (" + std::to_string(result) + ")";
    return std::string(libusb_error_name(result)) + " (" + std::to_string(result) + ")";
}

bool findProcessingUnit(libusb_device* device, ProcessingUnit& result, std::string& error) {
    libusb_config_descriptor* config = nullptr;
    const int status = libusb_get_active_config_descriptor(device, &config);
    if (status != LIBUSB_SUCCESS) {
        error = "Active configuration read failed: " + usbError(status);
        return false;
    }

    bool found = false;
    for (uint8_t i = 0; i < config->bNumInterfaces && !found; ++i) {
        const libusb_interface& iface = config->interface[i];
        for (int a = 0; a < iface.num_altsetting && !found; ++a) {
            const libusb_interface_descriptor& alt = iface.altsetting[a];
            if (alt.bAlternateSetting != 0 ||
                alt.bInterfaceClass != LIBUSB_CLASS_VIDEO ||
                alt.bInterfaceSubClass != 1) {
                continue;
            }

            const uint8_t* cursor = alt.extra;
            int remaining = alt.extra_length;
            while (cursor != nullptr && remaining >= 3) {
                const uint8_t length = cursor[0];
                if (length < 3 || length > remaining) {
                    error = "Malformed VideoControl descriptor";
                    break;
                }

                if (cursor[1] == USB_DT_CS_INTERFACE &&
                    cursor[2] == UVC_VC_PROCESSING_UNIT) {
                    // bControlSize is byte 7 and bmControls begins at byte 8.
                    if (length < 8) {
                        error = "Processing Unit descriptor is too short";
                        break;
                    }
                    const uint8_t controlSize = cursor[7];
                    if (controlSize == 0 || length < 8 + controlSize) {
                        error = "Processing Unit bmControls is malformed";
                        break;
                    }
                    result.interfaceNumber = alt.bInterfaceNumber;
                    result.unitId = cursor[3];
                    result.controls.assign(cursor + 8, cursor + 8 + controlSize);
                    found = true;
                    break;
                }

                cursor += length;
                remaining -= length;
            }
        }
    }

    libusb_free_config_descriptor(config);
    if (!found && error.empty()) error = "UVC Processing Unit descriptor not found";
    return found;
}

bool descriptorSupports(const ProcessingUnit& pu, uint8_t selector) {
    if (selector == 0) return false;
    const size_t bit = static_cast<size_t>(selector - 1);
    const size_t byteIndex = bit / 8;
    return byteIndex < pu.controls.size() &&
           (pu.controls[byteIndex] & (1u << (bit % 8))) != 0;
}

int getControl(libusb_device_handle* handle, const ProcessingUnit& pu,
               uint8_t request, uint8_t selector, std::vector<uint8_t>& data) {
    const uint16_t wValue = static_cast<uint16_t>(selector) << 8;
    const uint16_t wIndex =
            (static_cast<uint16_t>(pu.unitId) << 8) | pu.interfaceNumber;
    return libusb_control_transfer(
            handle, UVC_REQ_IN, request, wValue, wIndex,
            data.data(), static_cast<uint16_t>(data.size()), CONTROL_TIMEOUT_MS);
}

bool readExact(libusb_device_handle* handle, const ProcessingUnit& pu,
               uint8_t request, uint8_t selector, std::vector<uint8_t>& data,
               std::string& error) {
    const int status = getControl(handle, pu, request, selector, data);
    if (status != static_cast<int>(data.size())) {
        error = usbError(status);
        return false;
    }
    return true;
}

bool writeExact(libusb_device_handle* handle, const ProcessingUnit& pu,
                uint8_t selector, std::vector<uint8_t>& data, std::string& error) {
    const uint16_t wValue = static_cast<uint16_t>(selector) << 8;
    const uint16_t wIndex =
            (static_cast<uint16_t>(pu.unitId) << 8) | pu.interfaceNumber;
    const int status = libusb_control_transfer(
            handle, UVC_REQ_OUT, UVC_SET_CUR, wValue, wIndex,
            data.data(), static_cast<uint16_t>(data.size()), CONTROL_TIMEOUT_MS);
    if (status != static_cast<int>(data.size())) {
        error = usbError(status);
        return false;
    }
    return true;
}

std::string interpretedValue(const std::vector<uint8_t>& data, bool signedValue) {
    if (data.empty() || data.size() > 8) return "n/a";
    uint64_t raw = 0;
    for (size_t i = 0; i < data.size(); ++i) {
        raw |= static_cast<uint64_t>(data[i]) << (8 * i);
    }
    if (!signedValue) return std::to_string(raw);

    const unsigned int bits = static_cast<unsigned int>(data.size() * 8);
    int64_t value;
    if (bits == 64) {
        value = static_cast<int64_t>(raw);
    } else {
        const uint64_t signBit = uint64_t{1} << (bits - 1);
        value = (raw & signBit)
                ? static_cast<int64_t>(raw | (~uint64_t{0} << bits))
                : static_cast<int64_t>(raw);
    }
    return std::to_string(value);
}

void appendControlReport(std::ostringstream& out, libusb_device_handle* handle,
                         const ProcessingUnit& pu, const ControlDefinition& control) {
    out << "\n" << control.name << "\n";
    const bool advertised = descriptorSupports(pu, control.selector);
    out << "Descriptor Supported : " << (advertised ? "YES" : "NO") << "\n";
    if (!advertised) {
        out << "Probe Policy : QUERY ANYWAY (descriptor may be incomplete)\n";
    }

    std::vector<uint8_t> info(1);
    std::string error;
    if (readExact(handle, pu, UVC_GET_INFO, control.selector, info, error)) {
        out << "GET_INFO : 0x" << std::uppercase << std::hex
            << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(info[0])
            << std::dec << " [GET=" << ((info[0] & 0x01) ? "YES" : "NO")
            << ", SET=" << ((info[0] & 0x02) ? "YES" : "NO")
            << ", DISABLED_BY_AUTO=" << ((info[0] & 0x04) ? "YES" : "NO")
            << ", AUTO_UPDATE=" << ((info[0] & 0x08) ? "YES" : "NO")
            << ", ASYNC=" << ((info[0] & 0x10) ? "YES" : "NO") << "]"
            << "  RAW: " << rawBytes(info) << "\n";
    } else {
        out << "GET_INFO : ERROR " << error << "\n";
    }

    std::vector<uint8_t> lengthBytes(2);
    error.clear();
    uint16_t length = 0;
    if (!readExact(handle, pu, UVC_GET_LEN, control.selector, lengthBytes, error)) {
        out << "GET_LEN  : ERROR " << error << "\n";
        length = 2;
        out << "Value Length : 2 (UVC PU known-control fallback)\n";
    } else {
        length = readLe16(lengthBytes.data());
        out << "GET_LEN  : " << length << "  RAW: " << rawBytes(lengthBytes) << "\n";
        if (length == 0) {
            length = 2;
            out << "Value Length : 2 (zero-length device-response fallback)\n";
        }
    }

    if (length > 64) {
        out << "Value queries skipped: invalid length\n";
        return;
    }

    struct Query { const char* name; uint8_t request; };
    constexpr std::array<Query, 5> queries{{
            {"MIN", UVC_GET_MIN}, {"MAX", UVC_GET_MAX},
            {"RES", UVC_GET_RES}, {"DEF", UVC_GET_DEF}, {"CUR", UVC_GET_CUR},
    }};
    for (const Query& query : queries) {
        std::vector<uint8_t> value(length);
        error.clear();
        out << std::left << std::setfill(' ') << std::setw(8) << query.name << ": ";
        if (readExact(handle, pu, query.request, control.selector, value, error)) {
            out << interpretedValue(value, control.signedValue)
                << "  RAW: " << rawBytes(value) << "\n";
        } else {
            out << "ERROR " << error << "\n";
        }
    }
}

std::string inspectUsb(int fd) {
    (void) fd;
    libusb_device_handle* handle = uvc_device::calibrationHandle();
    if (handle == nullptr) return "ERROR: USB session is not attached";

    std::ostringstream out;
    libusb_device* device = libusb_get_device(handle);
    libusb_device_descriptor descriptor{};
    const int status = device == nullptr ? LIBUSB_ERROR_NO_DEVICE
                                         : libusb_get_device_descriptor(device, &descriptor);
    if (status != LIBUSB_SUCCESS) {
        out << "ERROR: Device descriptor: " << usbError(status);
    } else {
        out << "PU RAW INSPECTOR\n\n";
        out << "VID : 0x" << std::uppercase << std::hex << std::setw(4)
            << std::setfill('0') << descriptor.idVendor << "\n";
        out << "PID : 0x" << std::setw(4) << descriptor.idProduct << std::dec << "\n";

        ProcessingUnit pu;
        std::string error;
        if (!findProcessingUnit(device, pu, error)) {
            out << "\nERROR: " << error;
        } else {
            out << "VC Interface : " << static_cast<unsigned int>(pu.interfaceNumber) << "\n";
            out << "PU Unit ID   : " << static_cast<unsigned int>(pu.unitId) << "\n";
            out << "bmControls   : " << rawBytes(pu.controls) << "\n";
            for (const ControlDefinition& control : CONTROLS) {
                appendControlReport(out, handle, pu, control);
            }
        }
    }

    return out.str();
}

const ControlDefinition* findControl(uint8_t selector) {
    for (const ControlDefinition& control : CONTROLS) {
        if (control.selector == selector) return &control;
    }
    return nullptr;
}

std::string setAndVerifyUsb(int fd, uint8_t selector, int64_t requested) {
    const ControlDefinition* control = findControl(selector);
    if (control == nullptr) return "ERROR: Unknown PU control selector";
    (void) fd;
    libusb_device_handle* handle = uvc_device::calibrationHandle();
    if (handle == nullptr) return "ERROR: USB session is not attached";

    std::ostringstream out;
    ProcessingUnit pu;
    std::string error;
    libusb_device* device = libusb_get_device(handle);
    if (!findProcessingUnit(device, pu, error)) {
        out << "ERROR: " << error;
    } else {
        uint16_t length = 2;
        std::vector<uint8_t> lengthBytes(2);
        if (readExact(handle, pu, UVC_GET_LEN, selector, lengthBytes, error)) {
            const uint16_t reportedLength = readLe16(lengthBytes.data());
            if (reportedLength > 0 && reportedLength <= 8) length = reportedLength;
        }

        const unsigned int bits = length * 8;
        const int64_t signedMinimum = bits == 64
                ? INT64_MIN : -(int64_t{1} << (bits - 1));
        const int64_t signedMaximum = bits == 64
                ? INT64_MAX : (int64_t{1} << (bits - 1)) - 1;
        const uint64_t unsignedMaximum = bits == 64
                ? UINT64_MAX : (uint64_t{1} << bits) - 1;
        const bool representable = control->signedValue
                ? requested >= signedMinimum && requested <= signedMaximum
                : requested >= 0 && static_cast<uint64_t>(requested) <= unsignedMaximum;

        out << "PU WRITE / READBACK\n\n";
        out << "Control   : " << control->name << "\n";
        out << "Selector  : 0x" << std::uppercase << std::hex
            << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(selector)
            << std::dec << "\n";
        out << "Descriptor Supported : "
            << (descriptorSupports(pu, selector) ? "YES" : "NO") << "\n";
        out << "Value Length : " << length << "\n";
        out << "Requested : " << requested << "\n";

        struct RangeQuery { const char* name; uint8_t request; };
        constexpr std::array<RangeQuery, 3> rangeQueries{{
                {"MIN", UVC_GET_MIN}, {"MAX", UVC_GET_MAX}, {"RES", UVC_GET_RES},
        }};
        for (const RangeQuery& query : rangeQueries) {
            std::vector<uint8_t> value(length);
            error.clear();
            out << std::left << std::setfill(' ') << std::setw(10) << query.name << ": ";
            if (readExact(handle, pu, query.request, selector, value, error)) {
                out << interpretedValue(value, control->signedValue)
                    << "  RAW: " << rawBytes(value) << "\n";
            } else {
                out << "ERROR " << error << "\n";
            }
        }

        if (!representable) {
            out << "RESULT : HOST_RANGE_REJECTED\n";
        } else {
            std::vector<uint8_t> before(length);
            error.clear();
            const bool haveBefore = readExact(
                    handle, pu, UVC_GET_CUR, selector, before, error);
            if (haveBefore) {
                out << "Before    : " << interpretedValue(before, control->signedValue)
                    << "  RAW: " << rawBytes(before) << "\n";
            } else {
                out << "Before    : ERROR " << error << "\n";
            }

            std::vector<uint8_t> request(length);
            const uint64_t encoded = static_cast<uint64_t>(requested);
            for (size_t i = 0; i < request.size(); ++i) {
                request[i] = static_cast<uint8_t>((encoded >> (8 * i)) & 0xffu);
            }
            out << "SET RAW   : " << rawBytes(request) << "\n";

            error.clear();
            if (!writeExact(handle, pu, selector, request, error)) {
                out << "SET_CUR   : REJECTED " << error << "\n";
                out << "RESULT    : SET_CUR_REJECTED\n";
            } else {
                out << "SET_CUR   : ACCEPTED\n";
                std::vector<uint8_t> after(length);
                error.clear();
                if (!readExact(handle, pu, UVC_GET_CUR, selector, after, error)) {
                    out << "Readback  : ERROR " << error << "\n";
                    out << "RESULT    : READBACK_FAILED\n";
                } else {
                    const std::string actual = interpretedValue(after, control->signedValue);
                    out << "Readback  : " << actual << "  RAW: " << rawBytes(after) << "\n";
                    if (after == request) {
                        out << "RESULT    : ACCEPTED_EXACT\n";
                    } else {
                        out << "RESULT    : ACCEPTED_QUANTIZED_OR_DIFFERENT\n";
                    }
                }
            }
            out << "\nImage Effect : VERIFY VISUALLY\n";
        }
    }

    return out.str();
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_hev_uvccapturecalibration_MainActivity_nativeInspectUsb(
        JNIEnv* env, jobject /* thiz */, jint fd) {
    const std::string report = inspectUsb(fd);
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_hev_uvccapturecalibration_MainActivity_nativeAttachUsb(
        JNIEnv* /* env */, jobject /* thiz */, jint fd) {
    capture_solver::clearLatestSolution();
    validation_engine::clearLatestResult();
    {
        std::lock_guard<std::mutex> lock(gPreviewMutex);
        gPreviewSequence = 0;
    }
    return uvc_device::openFromAndroidFd(fd) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_hev_uvccapturecalibration_MainActivity_nativeDetachUsb(
        JNIEnv* /* env */, jobject /* thiz */) {
    capture_solver::clearLatestSolution();
    validation_engine::clearLatestResult();
    {
        std::lock_guard<std::mutex> lock(gPreviewMutex);
        gPreviewSequence = 0;
    }
    uvc_device::close();
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_hev_uvccapturecalibration_MainActivity_nativeCopyPreviewArgb(
        JNIEnv* env, jobject /* thiz */, jint outputWidth, jint outputHeight) {
    std::lock_guard<std::mutex> lock(gPreviewMutex);
    if (outputWidth < 16 || outputHeight < 9 ||
        outputWidth > 1280 || outputHeight > 720) {
        return nullptr;
    }

    if (!gPreviewEnabled) {
        if (outputWidth != measurement_engine::PREVIEW_WIDTH ||
            outputHeight != measurement_engine::PREVIEW_HEIGHT) return nullptr;
        std::vector<uint32_t> snapshot;
        if (!measurement_engine::copyLatestMeasurementPreview(
                snapshot, gMeasurementPreviewSequence)) return nullptr;
        std::vector<jint> javaPixels(snapshot.begin(), snapshot.end());
        jintArray result = env->NewIntArray(outputWidth * outputHeight);
        if (result == nullptr) return nullptr;
        env->SetIntArrayRegion(
                result, 0, outputWidth * outputHeight,
                javaPixels.data());
        return result;
    }

    if (!uvc_mjpeg_decoder::isRunning()) return nullptr;

    std::vector<uint8_t> frame(1280u * 720u * 2u);
    uvc_mjpeg_decoder::DecodedFrameTiming timing{};
    if (!uvc_mjpeg_decoder::copyLatestFrame(
            frame.data(), frame.size(), gPreviewSequence, timing)) {
        return nullptr;
    }

    const uint8_t* yPlane = frame.data();
    const uint8_t* cbPlane = yPlane + timing.yBytes;
    const uint8_t* crPlane = cbPlane + timing.cbBytes;
    std::vector<jint> pixels(
            static_cast<size_t>(outputWidth) * static_cast<size_t>(outputHeight));
    for (int outputY = 0; outputY < outputHeight; ++outputY) {
        const int sourceY = outputY * timing.height / outputHeight;
        for (int outputX = 0; outputX < outputWidth; ++outputX) {
            const int sourceX = outputX * timing.width / outputWidth;
            const int y = yPlane[
                    static_cast<size_t>(sourceY) * timing.yStride + sourceX];
            const int cb = cbPlane[
                    static_cast<size_t>(sourceY) * timing.cbStride + sourceX / 2];
            const int cr = crPlane[
                    static_cast<size_t>(sourceY) * timing.crStride + sourceX / 2];
            const int c = std::max(0, y - 16);
            const int d = cb - 128;
            const int e = cr - 128;
            const int red = clampByte((298 * c + 409 * e + 128) >> 8);
            const int green = clampByte((298 * c - 100 * d - 208 * e + 128) >> 8);
            const int blue = clampByte((298 * c + 516 * d + 128) >> 8);
            pixels[static_cast<size_t>(outputY) * outputWidth + outputX] =
                    static_cast<jint>(0xFF000000u |
                    (static_cast<uint32_t>(red) << 16) |
                    (static_cast<uint32_t>(green) << 8) |
                    static_cast<uint32_t>(blue));
        }
    }

    jintArray result = env->NewIntArray(outputWidth * outputHeight);
    if (result == nullptr) return nullptr;
    env->SetIntArrayRegion(result, 0, outputWidth * outputHeight, pixels.data());
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_hev_uvccapturecalibration_MainActivity_nativeSetPreviewEnabled(
        JNIEnv* /* env */, jobject /* thiz */, jboolean enabled) {
    std::lock_guard<std::mutex> lock(gPreviewMutex);
    gPreviewEnabled = enabled == JNI_TRUE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_hev_uvccapturecalibration_MainActivity_nativeSetAndVerifyUsb(
        JNIEnv* env, jobject /* thiz */, jint fd, jint selector, jlong requested) {
    capture_solver::clearLatestSolution();
    validation_engine::clearLatestResult();
    const std::string report = setAndVerifyUsb(
            fd, static_cast<uint8_t>(selector), static_cast<int64_t>(requested));
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_hev_uvccapturecalibration_MainActivity_nativeMeasurePatches(
        JNIEnv* env, jobject thiz, jint fd, jint frameCount) {
    ScopedProgressCallback progress(env, thiz);
    calibration_progress::report(5, "Acquiring measurement frames");
    calibration_progress::setFrameRange(5, 95, "Acquiring measurement frames");
    (void) fd;
    const std::string report = measurement_engine::measureCurrentSession(frameCount);
    calibration_progress::report(100, "Measurement complete");
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_hev_uvccapturecalibration_MainActivity_nativeAutoCalibratePu(
        JNIEnv* env, jobject thiz, jint fd) {
    ScopedProgressCallback progress(env, thiz);
    calibration_progress::report(1, "Reading PU capabilities");
    capture_solver::clearLatestSolution();
    validation_engine::clearLatestResult();
    (void) fd;
    const std::string report = pu_auto_calibration::runCurrentSession();
    calibration_progress::report(100, "PU auto calibration complete");
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_hev_uvccapturecalibration_MainActivity_nativeSolveCaptureMatrix(
        JNIEnv* env, jobject thiz, jint fd) {
    ScopedProgressCallback progress(env, thiz);
    calibration_progress::report(10, "Acquiring solve frames");
    calibration_progress::setFrameRange(10, 85, "Acquiring solve frames");
    validation_engine::clearLatestResult();
    (void) fd;
    const std::string report = capture_solver::solveCurrentSession();
    calibration_progress::report(100, "Matrix solve complete");
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_hev_uvccapturecalibration_MainActivity_nativeValidateCalibration(
        JNIEnv* env, jobject thiz) {
    ScopedProgressCallback progress(env, thiz);
    calibration_progress::report(10, "Acquiring validation frames");
    calibration_progress::setFrameRange(10, 90, "Acquiring validation frames");
    const std::string report = validation_engine::validateCurrentSession();
    calibration_progress::report(100, "Validation complete");
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_hev_uvccapturecalibration_MainActivity_nativeBuildProfilePayload(
        JNIEnv* env, jobject /* thiz */) {
    const std::string payload = profile_engine::buildCurrentPayload();
    return env->NewStringUTF(payload.c_str());
}
