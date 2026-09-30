#include "pu_auto_calibration.h"

#include "calibration_progress.h"
#include "measurement_engine.h"
#include "uvc_device.h"

#include <libusb.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace pu_auto_calibration {
namespace {

constexpr unsigned int TIMEOUT_MS = 1000;
constexpr uint8_t CS_INTERFACE = 0x24;
constexpr uint8_t VC_PROCESSING_UNIT = 0x05;
constexpr uint8_t SET_CUR = 0x01;
constexpr uint8_t GET_CUR = 0x81;
constexpr uint8_t GET_MIN = 0x82;
constexpr uint8_t GET_MAX = 0x83;
constexpr uint8_t GET_RES = 0x84;
constexpr uint8_t GET_DEF = 0x87;
constexpr uint8_t REQ_IN = LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS |
        LIBUSB_RECIPIENT_INTERFACE;
constexpr uint8_t REQ_OUT = LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS |
        LIBUSB_RECIPIENT_INTERFACE;
constexpr int SEARCH_FRAMES = 3;
constexpr int SETTLING_FRAMES = 3;
constexpr int TOTAL_SEARCH_EVALUATIONS = 2 * 4 * 3 * 3;

struct PuLocation { uint8_t interfaceNumber = 0; uint8_t unitId = 0; };
struct Control {
    const char* name;
    uint8_t selector;
    bool signedValue;
    int value = 0;
    int minimum = 0;
    int maximum = 0;
    int resolution = 1;
    int defaultValue = 0;
};

constexpr std::array<std::array<double, 3>, 24> EXPECTED{{
    {{0,0,0}}, {{26,26,26}}, {{64,64,64}}, {{128,128,128}},
    {{192,192,192}}, {{230,230,230}}, {{255,255,255}}, {{255,0,0}},
    {{0,255,0}}, {{0,0,255}}, {{0,255,255}}, {{255,0,255}},
    {{255,255,0}}, {{128,0,0}}, {{0,128,0}}, {{0,0,128}},
    {{0,128,128}}, {{128,0,128}}, {{128,128,0}}, {{64,0,0}},
    {{0,64,0}}, {{0,0,64}}, {{102,102,102}}, {{153,153,153}},
}};

bool findPu(libusb_device* device, PuLocation& pu) {
    libusb_config_descriptor* config = nullptr;
    if (libusb_get_active_config_descriptor(device, &config) != LIBUSB_SUCCESS) return false;
    bool found = false;
    for (uint8_t i = 0; i < config->bNumInterfaces && !found; ++i) {
        const libusb_interface& iface = config->interface[i];
        for (int a = 0; a < iface.num_altsetting && !found; ++a) {
            const libusb_interface_descriptor& alt = iface.altsetting[a];
            if (alt.bAlternateSetting != 0 || alt.bInterfaceClass != LIBUSB_CLASS_VIDEO ||
                alt.bInterfaceSubClass != 1) continue;
            const uint8_t* p = alt.extra;
            int remaining = alt.extra_length;
            while (p && remaining >= 3) {
                const uint8_t length = p[0];
                if (length < 3 || length > remaining) break;
                if (p[1] == CS_INTERFACE && p[2] == VC_PROCESSING_UNIT && length >= 5) {
                    pu = {alt.bInterfaceNumber, p[3]};
                    found = true;
                    break;
                }
                p += length;
                remaining -= length;
            }
        }
    }
    libusb_free_config_descriptor(config);
    return found;
}

int transfer(libusb_device_handle* handle, const PuLocation& pu, uint8_t request,
             uint8_t selector, uint8_t* data, uint16_t length, bool input) {
    return libusb_control_transfer(handle, input ? REQ_IN : REQ_OUT, request,
            static_cast<uint16_t>(selector) << 8,
            (static_cast<uint16_t>(pu.unitId) << 8) | pu.interfaceNumber,
            data, length, TIMEOUT_MS);
}

bool read16(libusb_device_handle* handle, const PuLocation& pu, uint8_t request,
            const Control& control, int& value) {
    uint8_t data[2]{};
    if (transfer(handle, pu, request, control.selector, data, 2, true) != 2) return false;
    const uint16_t raw = static_cast<uint16_t>(data[0]) |
            (static_cast<uint16_t>(data[1]) << 8);
    value = control.signedValue ? static_cast<int16_t>(raw) : raw;
    return true;
}

bool write16(libusb_device_handle* handle, const PuLocation& pu,
             const Control& control, int value) {
    const uint16_t raw = static_cast<uint16_t>(value);
    uint8_t data[2]{static_cast<uint8_t>(raw), static_cast<uint8_t>(raw >> 8)};
    return transfer(handle, pu, SET_CUR, control.selector, data, 2, false) == 2;
}

double luma(const measurement_engine::PatchMeasurement& patch) {
    return 0.299 * patch.rgb[0].mean + 0.587 * patch.rgb[1].mean +
           0.114 * patch.rgb[2].mean;
}

std::array<double, 2> chroma(double r, double g, double b) {
    const double y = 0.299 * r + 0.587 * g + 0.114 * b;
    return {{(b - y) / 1.772, (r - y) / 1.402}};
}

double metric(const std::vector<measurement_engine::PatchMeasurement>& patches,
              uint8_t selector) {
    double sum = 0.0;
    int count = 0;
    if (selector == 0x02) {
        for (int index : {0, 1, 2}) {
            const double error = luma(patches[index]) - EXPECTED[index][0];
            sum += error * error;
            ++count;
        }
    } else if (selector == 0x03) {
        for (int index : {0, 1, 2, 3, 4, 5, 6, 22, 23}) {
            const double error = luma(patches[index]) - EXPECTED[index][0];
            sum += error * error;
            ++count;
        }
    } else {
        for (int index = 7; index <= 12; ++index) {
            const auto expected = chroma(
                    EXPECTED[index][0], EXPECTED[index][1], EXPECTED[index][2]);
            const auto measured = chroma(
                    patches[index].rgb[0].mean,
                    patches[index].rgb[1].mean,
                    patches[index].rgb[2].mean);
            if (selector == 0x07) {
                const double error = std::hypot(measured[0], measured[1]) -
                        std::hypot(expected[0], expected[1]);
                sum += error * error;
            } else {
                double angle = std::atan2(measured[1], measured[0]) -
                        std::atan2(expected[1], expected[0]);
                angle = std::atan2(std::sin(angle), std::cos(angle));
                sum += angle * angle;
            }
            ++count;
        }
    }
    return std::sqrt(sum / std::max(1, count));
}

int alignValue(int value, const Control& control) {
    const int resolution = std::max(1, control.resolution);
    value = std::max(control.minimum, std::min(control.maximum, value));
    const int steps = static_cast<int>(std::llround(
            static_cast<double>(value - control.minimum) / resolution));
    return std::max(control.minimum,
            std::min(control.maximum, control.minimum + steps * resolution));
}

bool evaluate(libusb_device_handle* handle, const PuLocation& pu, Control& control,
              int candidate, double& score, std::string& error) {
    candidate = alignValue(candidate, control);
    if (!write16(handle, pu, control, candidate)) {
        error = std::string("SET_CUR failed for ") + control.name;
        return false;
    }
    int readback = 0;
    if (!read16(handle, pu, GET_CUR, control, readback)) {
        error = std::string("GET_CUR failed for ") + control.name;
        return false;
    }
    control.value = readback;
    std::vector<measurement_engine::PatchMeasurement> patches;
    if (!measurement_engine::measureCurrentStream(
            SEARCH_FRAMES, SETTLING_FRAMES, patches, error)) return false;
    score = metric(patches, control.selector);
    return true;
}

bool optimize(libusb_device_handle* handle, const PuLocation& pu, Control& control,
              int pass, int& completedEvaluations,
              std::ostringstream& out, std::string& error) {
    const int span = std::max(control.resolution, control.maximum - control.minimum);
    const std::array<int, 3> steps{{
            std::max(control.resolution, span / 8),
            std::max(control.resolution, span / 32),
            std::max(1, control.resolution),
    }};
    int center = control.value;
    double bestScore = std::numeric_limits<double>::infinity();
    int evaluations = 0;
    for (int rawStep : steps) {
        const int step = std::max(control.resolution,
                alignValue(control.minimum + rawStep, control) - control.minimum);
        int bestValue = center;
        double levelBest = std::numeric_limits<double>::infinity();
        std::array<int, 3> candidates{{center - step, center, center + step}};
        for (int candidate : candidates) {
            candidate = alignValue(candidate, control);
            std::ostringstream progress;
            progress << "PU pass " << pass << "/2 - " << control.name
                     << " - candidate " << evaluations + 1 << "/9";
            const int rangeStart =
                    5 + completedEvaluations * 90 / TOTAL_SEARCH_EVALUATIONS;
            const int rangeEnd =
                    5 + (completedEvaluations + 1) * 90 / TOTAL_SEARCH_EVALUATIONS;
            calibration_progress::setFrameRange(
                    rangeStart, rangeEnd, progress.str());
            double score = 0.0;
            if (!evaluate(handle, pu, control, candidate, score, error)) return false;
            ++evaluations;
            ++completedEvaluations;
            calibration_progress::report(
                    5 + completedEvaluations * 90 / TOTAL_SEARCH_EVALUATIONS,
                    progress.str());
            if (score < levelBest) {
                levelBest = score;
                bestValue = control.value;
            }
        }
        center = bestValue;
        bestScore = levelBest;
        if (!write16(handle, pu, control, center)) {
            error = std::string("Failed to restore best ") + control.name;
            return false;
        }
        control.value = center;
    }
    out << control.name << " = " << control.value << "  score="
        << std::fixed << std::setprecision(4) << bestScore
        << "  evaluations=" << evaluations << "\n";
    return true;
}

}  // namespace

std::string runFromAndroidFd(int fd) {
    std::ostringstream out;
    out << "PHASE 4 PU AUTO CALIBRATION\n\n";
    out << "Order: Brightness -> Contrast -> Saturation -> Hue (2 passes)\n";
    out << "Search measurement: 3 settling + 3 measured frames\n\n";
    if (!uvc_device::openFromAndroidFd(fd)) {
        out << "RESULT: STREAM_OPEN_FAILED";
        return out.str();
    }
    libusb_device_handle* handle = uvc_device::calibrationHandle();
    PuLocation pu;
    std::string error;
    if (handle == nullptr || !findPu(libusb_get_device(handle), pu)) {
        uvc_device::close();
        out << "RESULT: PU_DISCOVERY_FAILED";
        return out.str();
    }

    std::array<Control, 4> controls{{
            {"Brightness", 0x02, true}, {"Contrast", 0x03, false},
            {"Saturation", 0x07, false}, {"Hue", 0x06, true},
    }};
    std::array<int, 4> originalValues{};
    size_t controlIndex = 0;
    for (Control& control : controls) {
        if (!read16(handle, pu, GET_MIN, control, control.minimum) ||
            !read16(handle, pu, GET_MAX, control, control.maximum) ||
            !read16(handle, pu, GET_RES, control, control.resolution) ||
            !read16(handle, pu, GET_DEF, control, control.defaultValue)) {
            uvc_device::close();
            out << "RESULT: RANGE_READ_FAILED " << control.name;
            return out.str();
        }
        if (!read16(handle, pu, GET_CUR, control, originalValues[controlIndex])) {
            uvc_device::close();
            out << "RESULT: CURRENT_READ_FAILED " << control.name;
            return out.str();
        }
        control.resolution = std::max(1, control.resolution);
        control.value = alignValue(control.defaultValue, control);
        ++controlIndex;
    }
    for (size_t index = 0; index < controls.size(); ++index) {
        if (!write16(handle, pu, controls[index], controls[index].value)) {
            for (size_t rollback = 0; rollback < index; ++rollback) {
                write16(handle, pu, controls[rollback], originalValues[rollback]);
            }
            uvc_device::close();
            out << "RESULT: DEFAULT_SET_FAILED " << controls[index].name;
            return out.str();
        }
    }

    bool success = true;
    int completedEvaluations = 0;
    for (int pass = 1; pass <= 2 && success; ++pass) {
        out << "PASS " << pass << "\n";
        for (Control& control : controls) {
            if (!optimize(handle, pu, control, pass, completedEvaluations, out, error)) {
                success = false;
                break;
            }
        }
        out << "\n";
    }

    if (success) {
        calibration_progress::report(97, "Verifying final PU values");
        calibration_progress::setFrameRange(97, 99, "Verifying final PU values");
        std::vector<measurement_engine::PatchMeasurement> finalMeasurement;
        success = measurement_engine::measureCurrentStream(
                8, SETTLING_FRAMES, finalMeasurement, error);
    }
    if (!success) {
        for (size_t index = 0; index < controls.size(); ++index) {
            write16(handle, pu, controls[index], originalValues[index]);
        }
        out << "Rollback: original PU values restored\n";
    }
    uvc_device::close();
    out << (success ? "RESULT: AUTO_CAL_OK\n" : "RESULT: AUTO_CAL_FAILED\n");
    if (!success) out << "ERROR: " << error << "\n";
    out << "Image effect must be verified visually.\n";
    return out.str();
}

std::string runCurrentSession() {
    std::ostringstream out;
    out << "PHASE 4 PU AUTO CALIBRATION\n\n";
    out << "USB session: REUSED\n";
    out << "Order: Brightness -> Contrast -> Saturation -> Hue (2 passes)\n";
    out << "Search measurement: 3 settling + 3 measured frames\n\n";
    libusb_device_handle* handle = uvc_device::calibrationHandle();
    PuLocation pu;
    std::string error;
    if (handle == nullptr || !findPu(libusb_get_device(handle), pu)) {
        out << "RESULT: USB_SESSION_OR_PU_UNAVAILABLE";
        return out.str();
    }

    std::array<Control, 4> controls{{
            {"Brightness", 0x02, true}, {"Contrast", 0x03, false},
            {"Saturation", 0x07, false}, {"Hue", 0x06, true},
    }};
    std::array<int, 4> originalValues{};
    for (size_t index = 0; index < controls.size(); ++index) {
        Control& control = controls[index];
        if (!read16(handle, pu, GET_MIN, control, control.minimum) ||
            !read16(handle, pu, GET_MAX, control, control.maximum) ||
            !read16(handle, pu, GET_RES, control, control.resolution) ||
            !read16(handle, pu, GET_DEF, control, control.defaultValue) ||
            !read16(handle, pu, GET_CUR, control, originalValues[index])) {
            out << "RESULT: CONTROL_READ_FAILED " << control.name;
            return out.str();
        }
        control.resolution = std::max(1, control.resolution);
        control.value = alignValue(control.defaultValue, control);
    }
    for (size_t index = 0; index < controls.size(); ++index) {
        if (!write16(handle, pu, controls[index], controls[index].value)) {
            for (size_t rollback = 0; rollback < index; ++rollback) {
                write16(handle, pu, controls[rollback], originalValues[rollback]);
            }
            out << "RESULT: DEFAULT_SET_FAILED " << controls[index].name;
            return out.str();
        }
    }

    bool success = true;
    int completedEvaluations = 0;
    for (int pass = 1; pass <= 2 && success; ++pass) {
        out << "PASS " << pass << "\n";
        for (Control& control : controls) {
            if (!optimize(handle, pu, control, pass, completedEvaluations, out, error)) {
                success = false;
                break;
            }
        }
        out << "\n";
    }
    if (success) {
        calibration_progress::report(97, "Verifying final PU values");
        calibration_progress::setFrameRange(97, 99, "Verifying final PU values");
        std::vector<measurement_engine::PatchMeasurement> finalMeasurement;
        success = measurement_engine::measureCurrentStream(
                8, SETTLING_FRAMES, finalMeasurement, error);
    }
    if (!success) {
        for (size_t index = 0; index < controls.size(); ++index) {
            write16(handle, pu, controls[index], originalValues[index]);
        }
        out << "Rollback: original PU values restored\n";
    }
    out << (success ? "RESULT: AUTO_CAL_OK\n" : "RESULT: AUTO_CAL_FAILED\n");
    if (!success) out << "ERROR: " << error << "\n";
    return out.str();
}

}  // namespace pu_auto_calibration
