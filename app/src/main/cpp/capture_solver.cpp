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
#include <mutex>
#include <vector>

namespace capture_solver {
namespace {

constexpr int PATCH_COUNT = 24;
constexpr int FEATURE_COUNT = 4;
using Vector4 = std::array<double, FEATURE_COUNT>;
using Matrix4 = std::array<Vector4, FEATURE_COUNT>;
using Correction = std::array<Vector4, 3>;

std::mutex gSolutionMutex;
bool gHasSolution = false;
Solution gSolution;

void publishSolution(const Correction& correction, double condition,
                     double before, double after) {
    Solution solution;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            solution.matrix[row][column] = correction[row][column];
        }
        solution.offsetCode[row] = correction[row][3] * 255.0;
    }
    solution.conditionNumber = condition;
    solution.rmseBefore = before;
    solution.rmseAfter = after;
    std::lock_guard<std::mutex> lock(gSolutionMutex);
    gSolution = solution;
    gHasSolution = true;
}

constexpr std::array<std::array<double, 3>, PATCH_COUNT> EXPECTED{{
    {{0,0,0}}, {{26,26,26}}, {{64,64,64}}, {{128,128,128}},
    {{192,192,192}}, {{230,230,230}}, {{255,255,255}}, {{255,0,0}},
    {{0,255,0}}, {{0,0,255}}, {{0,255,255}}, {{255,0,255}},
    {{255,255,0}}, {{128,0,0}}, {{0,128,0}}, {{0,0,128}},
    {{0,128,128}}, {{128,0,128}}, {{128,128,0}}, {{64,0,0}},
    {{0,64,0}}, {{0,0,64}}, {{102,102,102}}, {{153,153,153}},
}};

bool solveLinear(Matrix4 matrix, Vector4 rhs, Vector4& solution) {
    for (int column = 0; column < FEATURE_COUNT; ++column) {
        int pivot = column;
        for (int row = column + 1; row < FEATURE_COUNT; ++row) {
            if (std::abs(matrix[row][column]) > std::abs(matrix[pivot][column])) {
                pivot = row;
            }
        }
        if (std::abs(matrix[pivot][column]) < 1e-12) return false;
        std::swap(matrix[column], matrix[pivot]);
        std::swap(rhs[column], rhs[pivot]);
        const double divisor = matrix[column][column];
        for (int c = column; c < FEATURE_COUNT; ++c) matrix[column][c] /= divisor;
        rhs[column] /= divisor;
        for (int row = 0; row < FEATURE_COUNT; ++row) {
            if (row == column) continue;
            const double factor = matrix[row][column];
            for (int c = column; c < FEATURE_COUNT; ++c) {
                matrix[row][c] -= factor * matrix[column][c];
            }
            rhs[row] -= factor * rhs[column];
        }
    }
    solution = rhs;
    return true;
}

std::array<double, FEATURE_COUNT> symmetricEigenvalues(Matrix4 matrix) {
    for (int iteration = 0; iteration < 80; ++iteration) {
        int p = 0;
        int q = 1;
        double largest = 0.0;
        for (int row = 0; row < FEATURE_COUNT; ++row) {
            for (int column = row + 1; column < FEATURE_COUNT; ++column) {
                if (std::abs(matrix[row][column]) > largest) {
                    largest = std::abs(matrix[row][column]);
                    p = row;
                    q = column;
                }
            }
        }
        if (largest < 1e-14) break;
        const double angle = 0.5 * std::atan2(
                2.0 * matrix[p][q], matrix[q][q] - matrix[p][p]);
        const double cosine = std::cos(angle);
        const double sine = std::sin(angle);
        for (int k = 0; k < FEATURE_COUNT; ++k) {
            if (k == p || k == q) continue;
            const double mkp = matrix[k][p];
            const double mkq = matrix[k][q];
            matrix[k][p] = matrix[p][k] = cosine * mkp - sine * mkq;
            matrix[k][q] = matrix[q][k] = sine * mkp + cosine * mkq;
        }
        const double app = matrix[p][p];
        const double aqq = matrix[q][q];
        const double apq = matrix[p][q];
        matrix[p][p] = cosine * cosine * app - 2 * sine * cosine * apq +
                sine * sine * aqq;
        matrix[q][q] = sine * sine * app + 2 * sine * cosine * apq +
                cosine * cosine * aqq;
        matrix[p][q] = matrix[q][p] = 0.0;
    }
    return {{matrix[0][0], matrix[1][1], matrix[2][2], matrix[3][3]}};
}

bool solve(const std::array<std::array<double, 3>, PATCH_COUNT>& measured,
           const std::array<std::array<double, 3>, PATCH_COUNT>& target,
           Correction& correction, double& conditionNumber) {
    Matrix4 normal{};
    std::array<Vector4, 3> rhs{};
    for (int patch = 0; patch < PATCH_COUNT; ++patch) {
        const Vector4 features{{
                measured[patch][0] / 255.0,
                measured[patch][1] / 255.0,
                measured[patch][2] / 255.0,
                1.0,
        }};
        for (int row = 0; row < FEATURE_COUNT; ++row) {
            for (int column = 0; column < FEATURE_COUNT; ++column) {
                normal[row][column] += features[row] * features[column];
            }
            for (int channel = 0; channel < 3; ++channel) {
                rhs[channel][row] += features[row] * target[patch][channel] / 255.0;
            }
        }
    }

    auto eigenvalues = symmetricEigenvalues(normal);
    const auto minimum = std::min_element(eigenvalues.begin(), eigenvalues.end());
    const auto maximum = std::max_element(eigenvalues.begin(), eigenvalues.end());
    if (*minimum <= 1e-12 || *maximum <= 0.0) {
        conditionNumber = std::numeric_limits<double>::infinity();
        return false;
    }
    conditionNumber = std::sqrt(*maximum / *minimum);
    if (!std::isfinite(conditionNumber) || conditionNumber > 1e6) return false;
    for (int channel = 0; channel < 3; ++channel) {
        if (!solveLinear(normal, rhs[channel], correction[channel])) return false;
    }
    return true;
}

std::array<double, 3> apply(const Correction& correction,
                            const std::array<double, 3>& input) {
    std::array<double, 3> output{};
    for (int row = 0; row < 3; ++row) {
        output[row] = correction[row][3] * 255.0;
        for (int column = 0; column < 3; ++column) {
            output[row] += correction[row][column] * input[column];
        }
    }
    return output;
}

double rmse(const std::array<std::array<double, 3>, PATCH_COUNT>& measured,
            const Correction* correction) {
    double squared = 0.0;
    for (int patch = 0; patch < PATCH_COUNT; ++patch) {
        const auto corrected = correction == nullptr
                ? measured[patch] : apply(*correction, measured[patch]);
        for (int channel = 0; channel < 3; ++channel) {
            const double error = corrected[channel] - EXPECTED[patch][channel];
            squared += error * error;
        }
    }
    return std::sqrt(squared / (PATCH_COUNT * 3));
}

bool syntheticSelfTest(double& maximumError) {
    const Correction known{{
            Vector4{{1.01, -0.02, 0.01, 0.012}},
            Vector4{{0.01, 0.98, 0.02, -0.008}},
            Vector4{{-0.01, 0.03, 0.99, 0.005}},
    }};
    std::array<std::array<double, 3>, PATCH_COUNT> target{};
    for (int patch = 0; patch < PATCH_COUNT; ++patch) {
        target[patch] = apply(known, EXPECTED[patch]);
    }
    Correction recovered{};
    double condition = 0.0;
    if (!solve(EXPECTED, target, recovered, condition)) return false;
    maximumError = 0.0;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            maximumError = std::max(
                    maximumError, std::abs(recovered[row][column] - known[row][column]));
        }
    }
    return maximumError < 1e-9;
}

}  // namespace

std::string solveFromAndroidFd(int fd) {
    std::ostringstream out;
    out << "PHASE 5 OFFSET + 3x3 MATRIX SOLVER\n\n";
    double syntheticError = 0.0;
    if (!syntheticSelfTest(syntheticError)) {
        out << "Synthetic self-test: FAILED (max coefficient error="
            << syntheticError << ")\nRESULT: SOLVER_SELF_TEST_FAILED";
        return out.str();
    }
    out << std::scientific << "Synthetic self-test: PASS (max error="
        << syntheticError << ")\n";

    if (!uvc_device::openFromAndroidFd(fd)) {
        out << "RESULT: STREAM_OPEN_FAILED";
        return out.str();
    }
    std::vector<measurement_engine::PatchMeasurement> patches;
    std::string error;
    const bool measured = measurement_engine::measureCurrentStream(16, 4, patches, error);
    uvc_device::close();
    if (!measured || patches.size() != PATCH_COUNT) {
        out << "RESULT: MEASUREMENT_FAILED " << error;
        return out.str();
    }

    std::array<std::array<double, 3>, PATCH_COUNT> measuredRgb{};
    for (int patch = 0; patch < PATCH_COUNT; ++patch) {
        for (int channel = 0; channel < 3; ++channel) {
            measuredRgb[patch][channel] = patches[patch].rgb[channel].mean;
        }
    }

    Correction correction{};
    double condition = 0.0;
    if (!solve(measuredRgb, EXPECTED, correction, condition)) {
        out << "Condition number: " << condition
            << "\nRESULT: SINGULAR_OR_UNSTABLE";
        return out.str();
    }

    out << std::fixed << std::setprecision(8);
    out << "Condition number: " << condition << "\n";
    out << "RMSE before: " << rmse(measuredRgb, nullptr) << " code values\n";
    out << "RMSE after : " << rmse(measuredRgb, &correction) << " code values\n\n";
    out << "Matrix:\n";
    for (int row = 0; row < 3; ++row) {
        out << "  [" << correction[row][0] << ", " << correction[row][1]
            << ", " << correction[row][2] << "]\n";
    }
    out << "Offset (code values):\n  ["
        << correction[0][3] * 255.0 << ", "
        << correction[1][3] * 255.0 << ", "
        << correction[2][3] * 255.0 << "]\n";
    out << "RESULT: SOLVE_OK\n";
    return out.str();
}

std::string solveCurrentSession() {
    clearLatestSolution();
    std::ostringstream out;
    out << "PHASE 5 OFFSET + 3x3 MATRIX SOLVER\n\nUSB session: REUSED\n";
    double syntheticError = 0.0;
    if (!syntheticSelfTest(syntheticError)) {
        out << "Synthetic self-test: FAILED\nRESULT: SOLVER_SELF_TEST_FAILED";
        return out.str();
    }
    out << std::scientific << "Synthetic self-test: PASS (max error="
        << syntheticError << ")\n";
    if (uvc_device::calibrationHandle() == nullptr) {
        out << "RESULT: USB_SESSION_NOT_ATTACHED";
        return out.str();
    }
    std::vector<measurement_engine::PatchMeasurement> patches;
    std::string error;
    if (!measurement_engine::measureCurrentStream(16, 4, patches, error) ||
        patches.size() != PATCH_COUNT) {
        out << "RESULT: MEASUREMENT_FAILED " << error;
        return out.str();
    }
    std::array<std::array<double, 3>, PATCH_COUNT> measuredRgb{};
    for (int patch = 0; patch < PATCH_COUNT; ++patch) {
        for (int channel = 0; channel < 3; ++channel) {
            measuredRgb[patch][channel] = patches[patch].rgb[channel].mean;
        }
    }
    Correction correction{};
    double condition = 0.0;
    if (!solve(measuredRgb, EXPECTED, correction, condition)) {
        out << "Condition number: " << condition << "\nRESULT: SINGULAR_OR_UNSTABLE";
        return out.str();
    }
    const double beforeRmse = rmse(measuredRgb, nullptr);
    const double afterRmse = rmse(measuredRgb, &correction);
    publishSolution(correction, condition, beforeRmse, afterRmse);
    out << std::fixed << std::setprecision(8);
    out << "Condition number: " << condition << "\n";
    out << "RMSE before: " << beforeRmse << " code values\n";
    out << "RMSE after : " << afterRmse << " code values\n\n";
    out << "Matrix:\n";
    for (int row = 0; row < 3; ++row) {
        out << "  [" << correction[row][0] << ", " << correction[row][1]
            << ", " << correction[row][2] << "]\n";
    }
    out << "Offset (code values):\n  [" << correction[0][3] * 255.0
        << ", " << correction[1][3] * 255.0 << ", "
        << correction[2][3] * 255.0 << "]\nRESULT: SOLVE_OK\n";
    return out.str();
}

bool latestSolution(Solution& solution) {
    std::lock_guard<std::mutex> lock(gSolutionMutex);
    if (!gHasSolution) return false;
    solution = gSolution;
    return true;
}

void clearLatestSolution() {
    std::lock_guard<std::mutex> lock(gSolutionMutex);
    gHasSolution = false;
    gSolution = {};
}

}  // namespace capture_solver
