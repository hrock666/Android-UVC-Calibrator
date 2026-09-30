#include "measurement_engine.h"

#include "calibration_progress.h"
#include "uvc_device.h"
#include "uvc_mjpeg_decoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace measurement_engine {
namespace {

constexpr int PATCH_COLUMNS = 6;
constexpr int PATCH_ROWS = 4;
constexpr int PATCH_COUNT = PATCH_COLUMNS * PATCH_ROWS;
constexpr int SETTLING_FRAMES = 4;
constexpr int FRAME_TIMEOUT_MS = 2000;
std::mutex gPreviewMutex;
std::vector<uint32_t> gPreviewPixels;
uint64_t gPreviewSequence = 0;

struct ChannelStats {
    double sum = 0.0;
    double squareSum = 0.0;
    uint64_t samples = 0;
    double minimum = std::numeric_limits<double>::max();
    double maximum = std::numeric_limits<double>::lowest();

    void add(double value) {
        sum += value;
        squareSum += value * value;
        ++samples;
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    }

    double mean() const { return samples == 0 ? 0.0 : sum / samples; }
    double deviation() const {
        if (samples == 0) return 0.0;
        return std::sqrt(std::max(0.0, squareSum / samples - mean() * mean()));
    }
};

struct PatchStats {
    std::array<ChannelStats, 3> rgb;
};

double clampCode(double value) {
    return std::max(0.0, std::min(255.0, value));
}

void addPixel(PatchStats& stats, uint8_t y, uint8_t cb, uint8_t cr) {
    // Match the Field Monitor transport contract: decoded planar MJPEG values
    // are nominal BT.601 limited-range codes (Y 16..235, Cb/Cr 16..240).
    const double luminance = (static_cast<double>(y) - 16.0) / 219.0;
    const double chromaB = (static_cast<double>(cb) - 128.0) / 224.0;
    const double chromaR = (static_cast<double>(cr) - 128.0) / 224.0;
    stats.rgb[0].add(clampCode((luminance + 1.402 * chromaR) * 255.0));
    stats.rgb[1].add(clampCode(
            (luminance - 0.344136 * chromaB - 0.714136 * chromaR) * 255.0));
    stats.rgb[2].add(clampCode((luminance + 1.772 * chromaB) * 255.0));
}

bool acquireFrame(std::vector<uint8_t>& frame, uint64_t& sequence,
                  uvc_mjpeg_decoder::DecodedFrameTiming& timing) {
    uint64_t readySequence = sequence;
    if (!uvc_mjpeg_decoder::waitForDecodedFrame(
            sequence, readySequence, FRAME_TIMEOUT_MS)) {
        return false;
    }
    return uvc_mjpeg_decoder::copyLatestFrame(
            frame.data(), frame.size(), sequence, timing);
}

void publishMeasurementPreview(
        const std::vector<uint8_t>& frame,
        const uvc_mjpeg_decoder::DecodedFrameTiming& timing) {
    if (timing.width <= 0 || timing.height <= 0 ||
        timing.yBytes + timing.cbBytes + timing.crBytes > frame.size()) return;
    const uint8_t* yPlane = frame.data();
    const uint8_t* cbPlane = yPlane + timing.yBytes;
    const uint8_t* crPlane = cbPlane + timing.cbBytes;
    std::vector<uint32_t> pixels(
            static_cast<size_t>(PREVIEW_WIDTH) * PREVIEW_HEIGHT);
    for (int outputY = 0; outputY < PREVIEW_HEIGHT; ++outputY) {
        const int sourceY = outputY * timing.height / PREVIEW_HEIGHT;
        for (int outputX = 0; outputX < PREVIEW_WIDTH; ++outputX) {
            const int sourceX = outputX * timing.width / PREVIEW_WIDTH;
            const int y = yPlane[static_cast<size_t>(sourceY) * timing.yStride + sourceX];
            const int cb = cbPlane[
                    static_cast<size_t>(sourceY) * timing.cbStride + sourceX / 2];
            const int cr = crPlane[
                    static_cast<size_t>(sourceY) * timing.crStride + sourceX / 2];
            const int c = std::max(0, y - 16);
            const int d = cb - 128;
            const int e = cr - 128;
            const int red = std::clamp((298 * c + 409 * e + 128) >> 8, 0, 255);
            const int green = std::clamp(
                    (298 * c - 100 * d - 208 * e + 128) >> 8, 0, 255);
            const int blue = std::clamp((298 * c + 516 * d + 128) >> 8, 0, 255);
            pixels[static_cast<size_t>(outputY) * PREVIEW_WIDTH + outputX] =
                    0xFF000000u | (static_cast<uint32_t>(red) << 16) |
                    (static_cast<uint32_t>(green) << 8) | static_cast<uint32_t>(blue);
        }
    }
    std::lock_guard<std::mutex> lock(gPreviewMutex);
    gPreviewPixels = std::move(pixels);
    ++gPreviewSequence;
}

}  // namespace

bool measureCurrentStream(
        int requestedFrames,
        int settlingFrames,
        std::vector<PatchMeasurement>& measurements,
        std::string& error) {
    const int frameCount = std::max(1, std::min(32, requestedFrames));
    const int discardCount = std::max(0, std::min(30, settlingFrames));
    std::array<PatchStats, PATCH_COUNT> patches{};
    uint64_t sequence = 0;
    int acceptedFrames = 0;
    std::vector<uint8_t> frame(1280u * 720u * 2u);
    uvc_mjpeg_decoder::DecodedFrameTiming lastTiming{};

    for (int frameIndex = 0; frameIndex < discardCount + frameCount; ++frameIndex) {
        uvc_mjpeg_decoder::DecodedFrameTiming timing{};
        if (!acquireFrame(frame, sequence, timing)) {
            error = "Decoded frame timeout";
            return false;
        }
        calibration_progress::reportFrame(
                frameIndex + 1, discardCount + frameCount);
        if (frameIndex < discardCount) continue;
        if (timing.width <= 0 || timing.height <= 0 ||
            timing.yBytes + timing.cbBytes + timing.crBytes > frame.size()) {
            error = "Invalid decoded frame layout";
            return false;
        }

        const uint8_t* yPlane = frame.data();
        const uint8_t* cbPlane = yPlane + timing.yBytes;
        const uint8_t* crPlane = cbPlane + timing.cbBytes;
        for (int row = 0; row < PATCH_ROWS; ++row) {
            const int cellTop = row * timing.height / PATCH_ROWS;
            const int cellBottom = (row + 1) * timing.height / PATCH_ROWS;
            const int roiTop = cellTop + (cellBottom - cellTop) / 4;
            const int roiBottom = cellBottom - (cellBottom - cellTop) / 4;
            for (int column = 0; column < PATCH_COLUMNS; ++column) {
                const int cellLeft = column * timing.width / PATCH_COLUMNS;
                const int cellRight = (column + 1) * timing.width / PATCH_COLUMNS;
                const int roiLeft = cellLeft + (cellRight - cellLeft) / 4;
                const int roiRight = cellRight - (cellRight - cellLeft) / 4;
                PatchStats& patch = patches[row * PATCH_COLUMNS + column];
                for (int y = roiTop; y < roiBottom; ++y) {
                    for (int x = roiLeft; x < roiRight; ++x) {
                        addPixel(
                                patch,
                                yPlane[static_cast<size_t>(y) * timing.yStride + x],
                                cbPlane[static_cast<size_t>(y) * timing.cbStride + x / 2],
                                crPlane[static_cast<size_t>(y) * timing.crStride + x / 2]);
                    }
                }
            }
        }
        ++acceptedFrames;
        lastTiming = timing;
    }

    if (acceptedFrames != frameCount) {
        error = "Incomplete frame accumulation";
        return false;
    }
    publishMeasurementPreview(frame, lastTiming);
    measurements.assign(PATCH_COUNT, {});
    for (int patch = 0; patch < PATCH_COUNT; ++patch) {
        for (int channel = 0; channel < 3; ++channel) {
            const ChannelStats& source = patches[patch].rgb[channel];
            measurements[patch].rgb[channel] = {
                    source.mean(), source.deviation(), source.minimum, source.maximum};
        }
    }
    return true;
}

bool copyLatestMeasurementPreview(
        std::vector<uint32_t>& pixels,
        uint64_t& inOutSequence) {
    std::lock_guard<std::mutex> lock(gPreviewMutex);
    if (gPreviewSequence <= inOutSequence ||
        gPreviewPixels.size() != static_cast<size_t>(PREVIEW_WIDTH) * PREVIEW_HEIGHT) {
        return false;
    }
    pixels = gPreviewPixels;
    inOutSequence = gPreviewSequence;
    return true;
}

std::string measureFromAndroidFd(int fd, int requestedFrames) {
    const int frameCount = std::max(1, std::min(32, requestedFrames));
    std::ostringstream out;
    out << "PHASE 3 MEASUREMENT\n\n";
    out << "Geometry : fixed 6 x 4\n";
    out << "ROI      : central 50% per cell\n";
    out << "Frames   : " << frameCount << " (+ " << SETTLING_FRAMES
        << " settling)\n";
    out << "RGB      : BT.601 limited-range YCbCr -> encoded RGB\n\n";

    if (!uvc_device::openFromAndroidFd(fd)) {
        out << "RESULT : STREAM_OPEN_FAILED";
        return out.str();
    }

    std::vector<PatchMeasurement> measurements;
    std::string error;
    const bool measured = measureCurrentStream(
            frameCount, SETTLING_FRAMES, measurements, error);

    uvc_device::close();
    out << "Captured : " << (measured ? frameCount : 0) << " / " << frameCount << "\n";
    if (!measured) {
        out << "RESULT   : INCOMPLETE (" << error << ")\n";
        return out.str();
    }

    out << "RESULT   : OK\n\n";
    out << std::fixed << std::setprecision(2);
    for (int index = 0; index < PATCH_COUNT; ++index) {
        out << "P" << std::setw(2) << std::setfill('0') << index + 1
            << std::setfill(' ') << "  ";
        static constexpr char CHANNEL_NAMES[] = {'R', 'G', 'B'};
        for (int channel = 0; channel < 3; ++channel) {
            const ChannelMeasurement& stats = measurements[index].rgb[channel];
            out << CHANNEL_NAMES[channel] << "=" << stats.mean
                << " sd=" << stats.standardDeviation
                << " [" << stats.minimum << "," << stats.maximum << "]";
            if (channel != 2) out << "  ";
        }
        out << "\n";
    }
    return out.str();
}

std::string measureCurrentSession(int requestedFrames) {
    const int frameCount = std::max(1, std::min(32, requestedFrames));
    std::ostringstream out;
    out << "PHASE 3 MEASUREMENT\n\n";
    out << "USB session : REUSED\n";
    out << "Geometry : fixed 6 x 4\n";
    out << "ROI      : central 50% per cell\n";
    out << "Frames   : " << frameCount << " (+ " << SETTLING_FRAMES
        << " settling)\n";
    out << "RGB      : BT.601 limited-range YCbCr -> encoded RGB\n\n";
    if (uvc_device::calibrationHandle() == nullptr) {
        out << "RESULT : USB_SESSION_NOT_ATTACHED";
        return out.str();
    }
    std::vector<PatchMeasurement> measurements;
    std::string error;
    const bool measured = measureCurrentStream(
            frameCount, SETTLING_FRAMES, measurements, error);
    out << "Captured : " << (measured ? frameCount : 0) << " / " << frameCount << "\n";
    if (!measured) {
        out << "RESULT   : INCOMPLETE (" << error << ")\n";
        return out.str();
    }
    out << "RESULT   : OK\n\n" << std::fixed << std::setprecision(2);
    for (int index = 0; index < PATCH_COUNT; ++index) {
        out << "P" << std::setw(2) << std::setfill('0') << index + 1
            << std::setfill(' ') << "  ";
        static constexpr char NAMES[] = {'R', 'G', 'B'};
        for (int channel = 0; channel < 3; ++channel) {
            const ChannelMeasurement& stats = measurements[index].rgb[channel];
            out << NAMES[channel] << "=" << stats.mean << " sd="
                << stats.standardDeviation << " [" << stats.minimum << ","
                << stats.maximum << "]";
            if (channel != 2) out << "  ";
        }
        out << "\n";
    }
    return out.str();
}

}  // namespace measurement_engine
