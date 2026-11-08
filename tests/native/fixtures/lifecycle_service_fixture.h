// Service contract shared by lifecycle_service_fixture.cpp and lifecycle_tests.cpp.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace lifecycle_fixture {

inline constexpr char kServiceName[] = "ark.tests.lifecycle";
inline constexpr char kFailedServiceName[] = "ark.tests.failed-init";
inline constexpr char kTransitiveServiceName[] = "ark.tests.transitive";
inline constexpr uint32_t kServiceAbi = 7;

struct LifecycleService {
    int (*value)();
    void (*increment)();
    int (*frames)();
    int (*consumer_shutdowns)();
    void (*consumer_stopped)();
};

} // namespace lifecycle_fixture
