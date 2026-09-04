#pragma once

#include "capture_e2e_support.hpp"

namespace fcs_test::detail {

// Drives the controller-owned resource handshake used by the black-box
// capture fixtures. It stays internal to the split support implementation.
bool ServiceForcedHostResources(FcsIpcV1* ipc);

} // namespace fcs_test::detail
