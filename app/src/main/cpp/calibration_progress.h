#pragma once

#include <functional>
#include <string>

namespace calibration_progress {

using Callback = std::function<void(int, const std::string&)>;

void setCallback(Callback callback);
void clearCallback();
void report(int percent, const std::string& message);
void setFrameRange(int startPercent, int endPercent, const std::string& message);
void reportFrame(int completed, int total);

}  // namespace calibration_progress
