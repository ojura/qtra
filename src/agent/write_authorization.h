#pragma once

// Why writing into a live function's entry was allowed, recorded with the write
// that used it.
//
// This is what was true at the moment of the write, kept so that whoever
// finishes an install that could not finish can say what the write was made
// under. It is history and nothing else. Nothing here is re-read to decide a
// later question, and the layer that holds it never looks inside: deciding
// whether a write may happen is a policy, and a patch manager that owned one
// would be answering a question it has no evidence for.
//
// Custody is not permission. Holding this says an authorization existed, which
// is a different claim from the entry being safe to write at this instant. What
// makes recovery safe is the lease the failed install is still holding, plus a
// check made at the time, which today is the calling thread and later becomes
// an accounting of every thread.

#include <string>

namespace runtime_agent {

// What made the write safe. These are separate claims and a record that said
// only "authorized" would not be worth keeping.
enum class WriteAuthorizationBasis {
    // Nothing that could reach the target was running: the process was patched
    // before its threads started, or the only thread that reaches it is the one
    // doing the writing and it is between requests.
    AlreadyQuiescent,

    // The build recorded which threads reach the target, and the claim was
    // strong enough to stand in for stopping them at a request boundary.
    RequestBoundary,
};

[[nodiscard]] const char* describe(WriteAuthorizationBasis basis) noexcept;

// An aggregate so a test can state one directly. What makes a real one mean
// anything is where it is built: the layer that evaluated the evidence makes
// it, and only after that evidence allowed the write.
struct LiveTextWriteAuthorization {
    WriteAuthorizationBasis basis = WriteAuthorizationBasis::AlreadyQuiescent;

    // The quiescence policy that was used, by name.
    std::string provider;

    // What was being patched and in which binary, so a record found later can
    // be attributed without asking anything that may have changed since.
    std::string target;
    std::string buildId;

    // What the basis said, in a form worth reporting.
    std::string detail;
};

} // namespace runtime_agent
