#include "calibration_progress.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace calibration_progress {
namespace {

std::mutex gCallbackMutex;
Callback gCallback;
int gFrameStart = 0;
int gFrameEnd = 100;
std::string gFrameMessage;

}  // namespace

void setCallback(Callback callback) {
    std::lock_guard<std::mutex> lock(gCallbackMutex);
    gCallback = std::move(callback);
    gFrameStart = 0;
    gFrameEnd = 100;
    gFrameMessage.clear();
}

void clearCallback() {
    std::lock_guard<std::mutex> lock(gCallbackMutex);
    gCallback = {};
}

void report(int percent, const std::string& message) {
    Callback callback;
    {
        std::lock_guard<std::mutex> lock(gCallbackMutex);
        callback = gCallback;
    }
    if (callback) callback(std::clamp(percent, 0, 100), message);
}

void setFrameRange(int startPercent, int endPercent, const std::string& message) {
    std::lock_guard<std::mutex> lock(gCallbackMutex);
    gFrameStart = std::clamp(startPercent, 0, 100);
    gFrameEnd = std::clamp(endPercent, gFrameStart, 100);
    gFrameMessage = message;
}

void reportFrame(int completed, int total) {
    int start = 0;
    int end = 100;
    std::string message;
    {
        std::lock_guard<std::mutex> lock(gCallbackMutex);
        start = gFrameStart;
        end = gFrameEnd;
        message = gFrameMessage;
    }
    const int safeTotal = std::max(1, total);
    const int safeCompleted = std::clamp(completed, 0, safeTotal);
    const int percent = start + (end - start) * safeCompleted / safeTotal;
    report(percent, message);
}

}  // namespace calibration_progress
