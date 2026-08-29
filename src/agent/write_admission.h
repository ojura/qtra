#pragma once

// What admitted a write into a live function's entry, kept with the write that
// used it.
//
// Admission and not authorization, because the difference is the whole point:
// this says a write was admitted, in the past, on stated grounds. It does not
// say anything may be written now.
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
#include <utility>

namespace runtime_agent {

// What made the write safe. These are separate claims and a record that said
// only "authorized" would not be worth keeping.
enum class WriteAdmissionBasis {
    // Nothing that could reach the target was running: the process was patched
    // before its threads started, or the only thread that reaches it is the one
    // doing the writing and it is between requests.
    AlreadyQuiescent,

    // The build recorded which threads reach the target, and the claim was
    // strong enough to stand in for stopping them at a request boundary.
    RequestBoundary,
};

[[nodiscard]] const char* describe(WriteAdmissionBasis basis) noexcept;

// Every field is required, so there is no partly-stated one and no default
// that could stand in for evidence nobody supplied. A test builds one directly;
// what makes a real one mean anything is where it is built, which is the layer
// that read the evidence, after that evidence allowed the write.
//
// There is no default basis, because both of them are substantive claims.
// Already-quiescent says nothing that could reach the target was running, which
// is as strong a statement as the other one and not a way of saying nothing is
// known. A zero value meaning either would be evidence made out of nothing.
//
// Copyable so it can be reported, and readable only, because nothing may adjust
// what a write was made under after the write has happened.
class LiveTextWriteAdmission {
public:
    LiveTextWriteAdmission(const WriteAdmissionBasis basis,
                           std::string provider,
                           std::string target,
                           std::string detail,
                           std::string buildId = {})
        : m_basis(basis)
        , m_provider(std::move(provider))
        , m_target(std::move(target))
        , m_detail(std::move(detail))
        , m_buildId(std::move(buildId))
    {
    }

    [[nodiscard]] WriteAdmissionBasis basis() const noexcept { return m_basis; }

    // The quiescence policy that was used, by name.
    [[nodiscard]] const std::string& provider() const noexcept { return m_provider; }

    // What was patched and in which binary, so a record found later can be
    // attributed without asking anything that may have changed since. The build
    // is empty where the basis never came from a build's decision.
    [[nodiscard]] const std::string& target() const noexcept { return m_target; }
    [[nodiscard]] const std::string& buildId() const noexcept { return m_buildId; }

    // What the basis said, in a form worth reporting.
    [[nodiscard]] const std::string& detail() const noexcept { return m_detail; }

private:
    WriteAdmissionBasis m_basis;
    std::string m_provider;
    std::string m_target;
    std::string m_detail;
    std::string m_buildId;
};

// Whether this says enough to attribute a write to it.
//
// Beside the fields it governs, so the rule and the data stay together and
// whatever installs a gateway has one thing to ask. It checks that the record
// is complete and never what the record claims: whether the grounds were good
// is a question for whoever read the evidence, and a patch manager weighing it
// again would be owning a policy it has nothing to decide with.
//
// Required of every basis: what provided it, and what it said. A record naming
// neither says a write was admitted by nobody, over nothing.
//
// RequestBoundary additionally requires the target and the build, because it is
// a claim a particular build made about a particular function, and it means
// nothing without knowing which. AlreadyQuiescent needs the target but no
// build: no manifest was involved, so there is none to name.
[[nodiscard]] bool attributable(const LiveTextWriteAdmission& admission, std::string& error);

} // namespace runtime_agent
