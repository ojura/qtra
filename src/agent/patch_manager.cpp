#include "agent/patch_manager.h"

#include <cassert>
#include <cstring>
#include <utility>

namespace runtime_agent {

const char* describe(const PatchState state) noexcept
{
    switch (state) {
    case PatchState::NoGateway:
        return "pristine";
    case PatchState::GatewayOriginal:
        return "original";
    case PatchState::GatewayReplacement:
        return "replacement";
    case PatchState::RecoveryRequired:
        return "recovery-required";
    }
    return "unknown";
}

PatchManager::~PatchManager()
{
    // Destroying the lease here would resume execution through an entry holding
    // a half-written gateway, so reaching this state is a bug in the shutdown
    // path and not something to paper over.
    assert(m_state != PatchState::RecoveryRequired
           && "roll back before destroying a manager that is holding recovery state");
}

bool PatchManager::installGateway(const PatchSite& site, Quiescer& quiescer, std::string& error)
{
    if (!siteAcceptsGateway(site, error)) {
        return false;
    }

    auto record = std::make_unique<GatewayRecord>();
    record->site = site;
    record->continuation = GatewayLayout::continuationAddress(site.patchAddress);

    // Published before the gateway exists, so the first instruction that reads
    // it already finds the original. An install that changes bytes and then
    // fails still runs the original function.
    record->slot.store(record->continuation, std::memory_order_release);
    record->selected = record->continuation;

    const std::vector<std::uint8_t> gateway = encodeGateway(&record->slot, site.availableBytes);
    if (gateway.empty()) {
        error = "the gateway could not be encoded for this site";
        return false;
    }

    std::unique_ptr<QuiescenceLease> lease = quiescer.acquire(error);
    if (lease == nullptr) {
        return false;
    }

    // Checked again with execution stopped, because everything above happened
    // while the target was still running.
    if (!siteAcceptsGateway(site, error)) {
        return false;
    }

    std::vector<std::uint8_t> original(site.availableBytes);
    std::memcpy(original.data(), site.patchAddress, site.availableBytes);

    const TextWriteResult write =
        writeText(site.patchAddress, gateway.data(), gateway.size(), m_protect);
    if (write.complete()) {
        m_record = std::move(record);
        m_original = std::move(original);
        m_state = PatchState::GatewayOriginal;
        return true;
    }

    error = write.error;
    if (!write.changedBytes()) {
        return false;
    }

    // The entry holds a gateway and the mapping could not be put back. The slot
    // names the continuation, so the original function is what runs, but the
    // bytes are not what they were and the caller is being told the install
    // failed. Keeping the lease is what stops execution reaching it.
    m_record = std::move(record);
    m_original = std::move(original);
    m_state = PatchState::RecoveryRequired;
    m_recoveryLease = std::move(lease);
    error += "; the gateway was already written, so the entry is not as it was and "
             "rollback is still required";
    return false;
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
    if (m_state == PatchState::RecoveryRequired) {
        error = "an earlier install left the entry rewritten and execution stopped; roll "
                "back before selecting anything else";
        return false;
    }
    if (!replacementIsReachable(site, replacement, error)) {
        return false;
    }

    if (m_state == PatchState::NoGateway) {
        if (!installGateway(site, quiescer, error)) {
            return false;
        }
    } else if (m_record->site.patchAddress != site.patchAddress) {
        error = "a gateway is installed at a different entry, and this manager holds one site";
        return false;
    }

    // The gateway exists, so selecting is a store. No lease: a thread reading
    // the slot gets the continuation or the replacement, and both are code it
    // may correctly run.
    m_record->slot.store(replacement, std::memory_order_release);
    m_record->selected = replacement;
    m_state = PatchState::GatewayReplacement;
    return true;
}

bool PatchManager::rollback(Quiescer& quiescer, std::string& error)
{
    error.clear();

    if (m_state == PatchState::RecoveryRequired) {
        // The one way back to an entry holding its own bytes. The lease that
        // stopped execution when the install failed is still held.
        const TextWriteResult write = writeText(m_record->site.patchAddress,
                                                m_original.data(),
                                                m_original.size(),
                                                m_protect);
        if (!write.changedBytes()) {
            error = write.error;
            return false;
        }

        m_state = PatchState::NoGateway;
        m_record.reset();
        m_original.clear();
        m_recoveryLease.reset();
        if (!write.complete()) {
            error = write.error;
            error += "; the entry was restored, so execution may reach it again, and the "
                     "mapping is left writable";
            return false;
        }
        return true;
    }

    if (m_state != PatchState::GatewayReplacement) {
        error = "no replacement is selected";
        return false;
    }

    // A store, like selecting one. The gateway stays where it is.
    (void)quiescer;
    m_record->slot.store(m_record->continuation, std::memory_order_release);
    m_record->selected = m_record->continuation;
    m_state = PatchState::GatewayOriginal;
    return true;
}

PatchStatus PatchManager::status() const
{
    PatchStatus status;
    status.state = m_state;
    if (m_record != nullptr) {
        status.site = m_record->site;
        status.slotAddress = &m_record->slot;
        status.replacement =
            m_state == PatchState::GatewayReplacement ? m_record->selected : nullptr;
    }
    return status;
}

} // namespace runtime_agent
