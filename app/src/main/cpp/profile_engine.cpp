#include "profile_engine.h"

#include "capture_solver.h"
#include "uvc_device.h"
#include "validation_engine.h"

#include <libusb.h>

#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace profile_engine {
namespace {

constexpr uint8_t CS_INTERFACE = 0x24;
constexpr uint8_t VC_PROCESSING_UNIT = 0x05;
constexpr uint8_t GET_CUR = 0x81;
constexpr uint8_t REQ_IN = LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS |
        LIBUSB_RECIPIENT_INTERFACE;

struct PuLocation { uint8_t interfaceNumber = 0; uint8_t unitId = 0; };
struct PuControl { const char* jsonName; uint8_t selector; bool signedValue; };
constexpr std::array<PuControl, 4> CONTROLS{{
        {"brightness", 0x02, true}, {"contrast", 0x03, false},
        {"saturation", 0x07, false}, {"hue", 0x06, true},
}};

bool findPu(libusb_device* device, PuLocation& pu) {
    libusb_config_descriptor* config = nullptr;
    if (libusb_get_active_config_descriptor(device, &config) != LIBUSB_SUCCESS) return false;
    bool found = false;
    for (uint8_t i = 0; i < config->bNumInterfaces && !found; ++i) {
        const libusb_interface& iface = config->interface[i];
        for (int a = 0; a < iface.num_altsetting && !found; ++a) {
            const auto& alt = iface.altsetting[a];
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

bool readCurrent(libusb_device_handle* handle, const PuLocation& pu,
                 const PuControl& control, int& value) {
    uint8_t data[2]{};
    const int status = libusb_control_transfer(
            handle, REQ_IN, GET_CUR, static_cast<uint16_t>(control.selector) << 8,
            (static_cast<uint16_t>(pu.unitId) << 8) | pu.interfaceNumber,
            data, 2, 1000);
    if (status != 2) return false;
    const uint16_t raw = static_cast<uint16_t>(data[0]) |
            (static_cast<uint16_t>(data[1]) << 8);
    value = control.signedValue ? static_cast<int16_t>(raw) : raw;
    return true;
}

}  // namespace

std::string buildCurrentPayload() {
    capture_solver::Solution solution;
    validation_engine::Result validation;
    if (!capture_solver::latestSolution(solution)) {
        return "ERROR: No solved calibration is available";
    }
    if (!validation_engine::latestResult(validation)) {
        return "ERROR: Calibration has not been validated";
    }
    if (validation.status != "CALIBRATION_VALID" &&
        validation.status != "CALIBRATION_POOR_FIT") {
        return "ERROR: Validation result is " + validation.status;
    }
    libusb_device_handle* handle = uvc_device::calibrationHandle();
    PuLocation pu;
    if (handle == nullptr || !findPu(libusb_get_device(handle), pu)) {
        return "ERROR: USB session or Processing Unit is unavailable";
    }
    std::array<int, 4> puValues{};
    for (size_t i = 0; i < CONTROLS.size(); ++i) {
        if (!readCurrent(handle, pu, CONTROLS[i], puValues[i])) {
            return std::string("ERROR: GET_CUR failed for ") + CONTROLS[i].jsonName;
        }
    }

    std::ostringstream out;
    out << std::fixed << std::setprecision(10);
    out << "{\"pu\":{";
    for (size_t i = 0; i < CONTROLS.size(); ++i) {
        if (i != 0) out << ',';
        out << '\"' << CONTROLS[i].jsonName << "\":" << puValues[i];
    }
    out << "},\"correction\":{\"matrix\":[";
    for (int row = 0; row < 3; ++row) {
        if (row != 0) out << ',';
        out << '[' << solution.matrix[row][0] << ',' << solution.matrix[row][1]
            << ',' << solution.matrix[row][2] << ']';
    }
    out << "],\"offsetCode\":[" << solution.offsetCode[0] << ','
        << solution.offsetCode[1] << ',' << solution.offsetCode[2] << "]},";
    out << "\"validation\":{\"result\":\"" << validation.status << "\",";
    out << "\"maeBefore\":" << validation.maeBefore << ',';
    out << "\"maeAfter\":" << validation.maeAfter << ',';
    out << "\"rmseBefore\":" << validation.rmseBefore << ',';
    out << "\"rmseAfter\":" << validation.rmseAfter << ',';
    out << "\"maximumChannelError\":" << validation.maximumChannelError << ',';
    out << "\"neutralAxisRmse\":" << validation.neutralAxisRmse << ',';
    out << "\"primaryRmse\":" << validation.primaryRmse << ',';
    out << "\"secondaryRmse\":" << validation.secondaryRmse << ',';
    out << "\"blackOffsetMax\":" << validation.blackOffsetMax << ',';
    out << "\"whiteErrorMax\":" << validation.whiteErrorMax << ',';
    out << "\"clippingDetected\":"
        << (validation.clippingDetected ? "true" : "false") << ',';
    out << "\"matrixCondition\":" << validation.matrixCondition << "}}";
    return out.str();
}

}  // namespace profile_engine
