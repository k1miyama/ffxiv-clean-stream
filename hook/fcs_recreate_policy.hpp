#pragma once

#include <stdint.h>

namespace fcs::hook {

struct ExternalRecreateDecision {
    bool consume;
    bool releaseLiveGraph;
};

inline constexpr ExternalRecreateDecision DecideExternalRecreate(
    bool requestPending, bool hasLiveGraph, int64_t nowQpc,
    int64_t recreateNotBeforeQpc) {
    const bool consume = requestPending &&
        (!hasLiveGraph || nowQpc >= recreateNotBeforeQpc);
    return ExternalRecreateDecision{consume, consume && hasLiveGraph};
}

inline constexpr bool BypassResourceRetryForColdRecreate(
    bool requestPending, bool hasLiveGraph) {
    return requestPending && !hasLiveGraph;
}

} // namespace fcs::hook
