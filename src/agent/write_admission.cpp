#include "agent/write_admission.h"

namespace runtime_agent {

const char* describe(const WriteAdmissionBasis basis) noexcept
{
    switch (basis) {
    case WriteAdmissionBasis::AlreadyQuiescent:
        return "already-quiescent";
    case WriteAdmissionBasis::RequestBoundary:
        return "request-boundary";
    }
    return "unknown";
}

bool attributable(const LiveTextWriteAdmission& admission, std::string& error)
{
    if (admission.provider().empty()) {
        error = "an admission has to name what provided it";
        return false;
    }
    if (admission.detail().empty()) {
        error = "an admission has to say what its basis said";
        return false;
    }
    if (admission.target().empty()) {
        error = "an admission has to name what was written";
        return false;
    }
    if (admission.basis() == WriteAdmissionBasis::RequestBoundary
        && admission.buildId().empty()) {
        error = "a request-boundary admission is a claim one build made about one function, "
                "so it has to name the build";
        return false;
    }
    return true;
}

} // namespace runtime_agent
