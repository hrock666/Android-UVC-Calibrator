#include "validation_engine.h"

#include "capture_solver.h"
#include "measurement_engine.h"
#include "uvc_device.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>
#include <mutex>

namespace validation_engine {
namespace {

std::mutex gResultMutex;
bool gHasResult = false;
Result gResult;

constexpr int PATCH_COUNT = 24;
constexpr std::array<std::array<double, 3>, PATCH_COUNT> EXPECTED{{
    {{0,0,0}}, {{26,26,26}}, {{64,64,64}}, {{128,128,128}},
    {{192,192,192}}, {{230,230,230}}, {{255,255,255}}, {{255,0,0}},
    {{0,255,0}}, {{0,0,255}}, {{0,255,255}}, {{255,0,255}},
    {{255,255,0}}, {{128,0,0}}, {{0,128,0}}, {{0,0,128}},
    {{0,128,128}}, {{128,0,128}}, {{128,128,0}}, {{64,0,0}},
    {{0,64,0}}, {{0,0,64}}, {{102,102,102}}, {{153,153,153}},
}};
constexpr std::array<int, 9> NEUTRALS{{0,1,2,3,4,5,6,22,23}};
constexpr std::array<int, 9> PRIMARIES{{7,8,9,13,14,15,19,20,21}};
constexpr std::array<int, 6> SECONDARIES{{10,11,12,16,17,18}};

std::array<double, 3> corrected(
        const capture_solver::Solution& solution,
        const measurement_engine::PatchMeasurement& patch) {
    std::array<double, 3> output{};
    for (int row = 0; row < 3; ++row) {
        output[row] = solution.offsetCode[row];
        for (int column = 0; column < 3; ++column) {
            output[row] += solution.matrix[row][column] * patch.rgb[column].mean;
        }
    }
    return output;
}

double groupRmse(const std::array<std::array<double, 3>, PATCH_COUNT>& values,
                 const int* indices, size_t count) {
    double squared = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const int patch = indices[i];
        for (int channel = 0; channel < 3; ++channel) {
            const double error = values[patch][channel] - EXPECTED[patch][channel];
            squared += error * error;
        }
    }
    return std::sqrt(squared / static_cast<double>(count * 3));
}

}  // namespace

std::string validateCurrentSession() {
    clearLatestResult();
    std::ostringstream out;
    out << "PHASE 6 VALIDATION\n\nUSB session: REUSED\n";
    capture_solver::Solution solution;
    if (!capture_solver::latestSolution(solution)) {
        out << "RESULT: NO_SOLVED_CALIBRATION\nRun Phase 5 Solve first.";
        return out.str();
    }
    if (uvc_device::calibrationHandle() == nullptr) {
        out << "RESULT: USB_SESSION_NOT_ATTACHED";
        return out.str();
    }

    std::vector<measurement_engine::PatchMeasurement> measured;
    std::string error;
    if (!measurement_engine::measureCurrentStream(16, 4, measured, error) ||
        measured.size() != PATCH_COUNT) {
        out << "RESULT: VALIDATION_MEASUREMENT_FAILED " << error;
        return out.str();
    }

    std::array<std::array<double, 3>, PATCH_COUNT> before{};
    std::array<std::array<double, 3>, PATCH_COUNT> after{};
    double absoluteSumBefore = 0.0;
    double absoluteSumAfter = 0.0;
    double squaredBefore = 0.0;
    double squaredAfter = 0.0;
    double maximumAfter = 0.0;
    bool clipped = false;
    for (int patch = 0; patch < PATCH_COUNT; ++patch) {
        for (int channel = 0; channel < 3; ++channel) {
            before[patch][channel] = measured[patch].rgb[channel].mean;
        }
        after[patch] = corrected(solution, measured[patch]);
        for (int channel = 0; channel < 3; ++channel) {
            const double errorBefore = before[patch][channel] - EXPECTED[patch][channel];
            const double errorAfter = after[patch][channel] - EXPECTED[patch][channel];
            absoluteSumBefore += std::abs(errorBefore);
            absoluteSumAfter += std::abs(errorAfter);
            squaredBefore += errorBefore * errorBefore;
            squaredAfter += errorAfter * errorAfter;
            maximumAfter = std::max(maximumAfter, std::abs(errorAfter));
            const double expected = EXPECTED[patch][channel];
            const double observed = before[patch][channel];
            if (expected > 16.0 && expected < 239.0 &&
                (observed <= 1.0 || observed >= 254.0)) clipped = true;
        }
    }

    constexpr double SAMPLE_COUNT = PATCH_COUNT * 3.0;
    const double maeBefore = absoluteSumBefore / SAMPLE_COUNT;
    const double maeAfter = absoluteSumAfter / SAMPLE_COUNT;
    const double rmseBefore = std::sqrt(squaredBefore / SAMPLE_COUNT);
    const double rmseAfter = std::sqrt(squaredAfter / SAMPLE_COUNT);
    const double neutralError = groupRmse(after, NEUTRALS.data(), NEUTRALS.size());
    const double primaryError = groupRmse(after, PRIMARIES.data(), PRIMARIES.size());
    const double secondaryError = groupRmse(after, SECONDARIES.data(), SECONDARIES.size());
    double blackOffset = 0.0;
    double whiteError = 0.0;
    for (int channel = 0; channel < 3; ++channel) {
        blackOffset = std::max(blackOffset, std::abs(after[0][channel]));
        whiteError = std::max(whiteError, std::abs(after[6][channel] - 255.0));
    }

    std::string status;
    if (solution.conditionNumber > 1e4 || !std::isfinite(solution.conditionNumber)) {
        status = "CALIBRATION_UNSTABLE";
    } else if (clipped) {
        status = "CALIBRATION_CLIPPED";
    } else if (rmseAfter >= rmseBefore || maeAfter > 3.0 || maximumAfter > 12.0 ||
               neutralError > 4.0 || blackOffset > 6.0 || whiteError > 10.0) {
        status = "CALIBRATION_POOR_FIT";
    } else {
        status = "CALIBRATION_VALID";
    }

    Result result;
    result.status = status;
    result.maeBefore = maeBefore;
    result.maeAfter = maeAfter;
    result.rmseBefore = rmseBefore;
    result.rmseAfter = rmseAfter;
    result.maximumChannelError = maximumAfter;
    result.neutralAxisRmse = neutralError;
    result.primaryRmse = primaryError;
    result.secondaryRmse = secondaryError;
    result.blackOffsetMax = blackOffset;
    result.whiteErrorMax = whiteError;
    result.clippingDetected = clipped;
    result.matrixCondition = solution.conditionNumber;
    {
        std::lock_guard<std::mutex> lock(gResultMutex);
        gResult = result;
        gHasResult = true;
    }

    out << std::fixed << std::setprecision(4);
    out << "MAE before          : " << maeBefore << "\n";
    out << "MAE after           : " << maeAfter << "\n";
    out << "RMSE before         : " << rmseBefore << "\n";
    out << "RMSE after          : " << rmseAfter << "\n";
    out << "Maximum channel err : " << maximumAfter << "\n";
    out << "Neutral axis RMSE   : " << neutralError << "\n";
    out << "Primary RMSE        : " << primaryError << "\n";
    out << "Secondary RMSE      : " << secondaryError << "\n";
    out << "Black offset max    : " << blackOffset << "\n";
    out << "White error max     : " << whiteError << "\n";
    out << "Clipping detected   : " << (clipped ? "YES" : "NO") << "\n";
    out << "Matrix condition    : " << solution.conditionNumber << "\n\n";
    out << "Provisional thresholds:\n"
        << "  MAE<=3, Max<=12, Neutral<=4, Black<=6, White<=10, Cond<=10000\n";
    out << "RESULT: " << status << "\n";
    return out.str();
}

bool latestResult(Result& result) {
    std::lock_guard<std::mutex> lock(gResultMutex);
    if (!gHasResult) return false;
    result = gResult;
    return true;
}

void clearLatestResult() {
    std::lock_guard<std::mutex> lock(gResultMutex);
    gHasResult = false;
    gResult = {};
}

}  // namespace validation_engine
