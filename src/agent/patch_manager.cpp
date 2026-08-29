#include "agent/patch_manager.h"

#include <cassert>
#include <utility>

namespace runtime_agent {

PatchManager::~PatchManager()
{
    // Destroying the lease here would resume execution through an entry that
    // still holds a half-written redirect, so reaching this state is a bug in
    // the shutdown path and not something to paper over.
    assert(m_entry.state() != PatchState::RecoveryRequired
           && "roll back before destroying a manager that is holding recovery state");
}

bool PatchManager::activate(const PatchSite& site,
                            void* replacement,
                            Quiescer& quiescer,
                            std::string& error)
{
    error.clear();
    if (!site.valid() || replacement == nullptr) {
        error = "a resolved site and a replacement are both required";
        return false;
    }
    if (m_recoveryLease != nullptr) {
        error = "an earlier activation left the entry rewritten and execution stopped; "
                "roll back before activating anything else";
        return false;
    }

    std::unique_ptr<QuiescenceLease> lease = quiescer.acquire(error);
    if (lease == nullptr) {
        return false;
    }

    std::string nativeError;
    if (m_entry.apply(site, replacement, nativeError)) {
        m_site = site;
        m_replacement = replacement;
        return true;
    }

    error = nativeError;
    if (m_entry.state() == PatchState::RecoveryRequired) {
        // The entry holds a redirect the caller is being told did not install.
        // Keeping the lease is what stops execution reaching it.
        m_site = site;
        m_replacement = replacement;
        m_recoveryLease = std::move(lease);
        return false;
    }

    m_site.reset();
    m_replacement = nullptr;
    return false;
}

bool PatchManager::rollback(Quiescer& quiescer, std::string& error)
{
    error.clear();
    if (!m_entry.active()) {
        error = "no patch is active";
        return false;
    }

    // Recovery already holds one. Acquiring a second would ask a provider to
    // stop what it has not been told is running.
    std::unique_ptr<QuiescenceLease> acquired;
    if (m_recoveryLease == nullptr) {
        acquired = quiescer.acquire(error);
        if (acquired == nullptr) {
            return false;
        }
    }

    std::string nativeError;
    const bool restored = m_entry.rollback(nativeError);
    if (m_entry.state() == PatchState::Inactive) {
        // The original bytes are back, so execution may reach the entry again
        // whatever else the write reported.
        m_site.reset();
        m_replacement = nullptr;
        m_recoveryLease.reset();
        if (!restored) {
            error = nativeError;
            return false;
        }
        return true;
    }

    // Nothing changed, so the entry is as it was and stays that way.
    error = nativeError;
    return false;
}

PatchStatus PatchManager::status() const
{
    PatchStatus status;
    status.state = m_entry.state();
    status.site = m_site;
    status.replacement = m_replacement;
    return status;
}

} // namespace runtime_agent
