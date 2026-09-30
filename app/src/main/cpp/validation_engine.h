#pragma once

#include <string>

namespace validation_engine {

struct Result {
    std::string status;
    double maeBefore = 0.0;
    double maeAfter = 0.0;
    double rmseBefore = 0.0;
    double rmseAfter = 0.0;
    double maximumChannelError = 0.0;
    double neutralAxisRmse = 0.0;
    double primaryRmse = 0.0;
    double secondaryRmse = 0.0;
    double blackOffsetMax = 0.0;
    double whiteErrorMax = 0.0;
    bool clippingDetected = false;
    double matrixCondition = 0.0;
};

std::string validateCurrentSession();
bool latestResult(Result& result);
void clearLatestResult();

}  // namespace validation_engine
