#pragma once

#include <QString>

namespace runtime_agent {

// The GNU build id of the running executable, as lowercase hex.
//
// This identifies one build of the host. A module compiled against a different
// one computed its offsets into application types from different source, so the
// loader compares this with what the module reports and refuses a mismatch.
//
// Empty when the executable carries no build id note, which leaves the loader
// with nothing to compare and every module treated as unstamped.
[[nodiscard]] QString hostBuildId();

} // namespace runtime_agent
