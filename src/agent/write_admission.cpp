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

} // namespace runtime_agent
