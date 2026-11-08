#pragma once
#include <cstring>
#include "capture.h"

namespace ark::arkhelper::capture::operator_samples {

// Battle-local UIDs distinguish repeated characters and pooled objects reused by later rounds.
inline bool append(OperatorSample* samples, std::uint32_t& count, std::uint32_t capacity,
                   const char* id, float x, float y, bool screen_valid = true,
                   std::uint64_t instance_id = 0) {
    if (!samples || !id || !id[0] || std::strlen(id) >= sizeof(OperatorSample::actor_id)) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (instance_id && samples[i].instance_id == instance_id) return true;
        if (!instance_id && std::strcmp(samples[i].actor_id, id) == 0) { samples[i].flags = 0; return true; }
    }
    if (count >= capacity) return false;
    auto& sample = samples[count++];
    sample = {};
    sample.instance_id = instance_id;
    std::strcpy(sample.actor_id, id);
    sample.viewport_x = x; sample.viewport_y = y; sample.flags = screen_valid ? 1u : 0u;
    return true;
}
} // namespace ark::arkhelper::capture::operator_samples
