#pragma once

#include <array>
#include <string>

namespace capture_solver {

struct Solution {
    std::array<std::array<double, 3>, 3> matrix{};
    std::array<double, 3> offsetCode{};
    double conditionNumber = 0.0;
    double rmseBefore = 0.0;
    double rmseAfter = 0.0;
};

std::string solveFromAndroidFd(int fd);
std::string solveCurrentSession();
bool latestSolution(Solution& solution);
void clearLatestSolution();

}  // namespace capture_solver
