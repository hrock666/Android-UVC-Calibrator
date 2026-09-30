#pragma once

#include <string>

namespace profile_engine {

// Returns a JSON object containing PU, correction, and validation data.
// Returns a string beginning with "ERROR:" when commit prerequisites fail.
std::string buildCurrentPayload();

}  // namespace profile_engine
