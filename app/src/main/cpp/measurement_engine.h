#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace measurement_engine {

constexpr int PREVIEW_WIDTH = 480;
constexpr int PREVIEW_HEIGHT = 270;

struct ChannelMeasurement {
    double mean = 0.0;
    double standardDeviation = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
};

struct PatchMeasurement {
    std::array<ChannelMeasurement, 3> rgb;
};

bool measureCurrentStream(
        int requestedFrames,
        int settlingFrames,
        std::vector<PatchMeasurement>& measurements,
        std::string& error);

bool copyLatestMeasurementPreview(
        std::vector<uint32_t>& pixels,
        uint64_t& inOutSequence);

// Opens the fixed 1280x720 MJPEG stream, discards initial frames, then measures
// the central 50% of each cell in a fixed 6x4 patch grid. No preview is created.
std::string measureFromAndroidFd(int fd, int requestedFrames);
std::string measureCurrentSession(int requestedFrames);

}  // namespace measurement_engine
