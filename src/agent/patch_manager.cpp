#include "agent/patch_manager.h"

#include "agent/quiescence_providers.h"

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

    // The gateway is permanent and carries this record's slot address as an
    // immediate, so the storage has to outlive whatever installed it. Freeing it
    // here would leave every later call loading a destination out of released
    // memory, and the manager going away is not a reason for the entry to stop
    // working. So the record is released deliberately: it is unreachable
    // afterwards and that is the intent, since nothing may ever reclaim it.
    (void)m_record.release();
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

    // Recorded before acquiring, so a refusal reports what it was judged
    // against as well as a success.
    m_quiescedBy = quiescer.name();
    m_threadsAtInstall = observedThreadCount();

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

const PatchManager::Generation* PatchManager::newestLive() const noexcept
{
    for (auto it = m_generations.rbegin(); it != m_generations.rend(); ++it) {
        if (!it->released) {
            return &(*it);
        }
    }
    return nullptr;
}

void PatchManager::publishSelection() noexcept
{
    if (m_record == nullptr) {
        return;
    }
    const Generation* live = newestLive();
    void* target = live != nullptr ? live->replacement : m_record->continuation;
    m_record->slot.store(target, std::memory_order_release);
    m_record->selected = target;
    m_state = live != nullptr ? PatchState::GatewayReplacement : PatchState::GatewayOriginal;
}

void* PatchManager::continuation() const noexcept
{
    return m_record != nullptr ? m_record->continuation : nullptr;
}

bool PatchManager::bind(const PatchSite& site,
                        void* replacement,
                        const std::uint64_t owner,
                        Quiescer& quiescer,
                        PatchBinding& binding,
                        std::string& error)
{
    error.clear();
    binding = PatchBinding{};
    if (!site.valid() || replacement == nullptr) {
        error = "a resolved site and a replacement are both required";
        return false;
    }
    if (m_state == PatchState::RecoveryRequired) {
        error = "an earlier install left the entry rewritten and execution stopped; recover "
                "before binding anything else";
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
        error = "this manager owns one site and already has a gateway at a different "
                "entry; patching a second function needs a manager per site until sites "
                "are held in a map";
        return false;
    }

    // What this binding displaces, which is what a replacement chains to.
    const Generation* displaced = newestLive();
    binding.previous = displaced != nullptr ? displaced->replacement : m_record->continuation;
    binding.original = m_record->continuation;
    binding.id = m_nextBinding++;

    m_generations.push_back(Generation{binding.id, owner, replacement, false});
    publishSelection();
    return true;
}

bool PatchManager::unbind(const std::uint64_t id, const std::uint64_t owner, std::string& error)
{
    error.clear();
    for (Generation& generation : m_generations) {
        if (generation.id != id) {
            continue;
        }
        if (generation.owner != owner) {
            error = "this binding belongs to another module";
            return false;
        }
        if (generation.released) {
            error = "this binding was already released";
            return false;
        }
        generation.released = true;
        // Recomputed from what is still live, so releasing something that was
        // not selected leaves the slot alone and releasing the selected one
        // falls back to the newest predecessor rather than to the original.
        publishSelection();
        return true;
    }
    error = "no such binding";
    return false;
}

bool PatchManager::recover(std::string& error)
{
    error.clear();
    if (m_state != PatchState::RecoveryRequired) {
        error = "there is nothing to recover";
        return false;
    }

    // The one path that writes code outside installation. The lease that
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
    m_generations.clear();
    m_recoveryLease.reset();
    if (!write.complete()) {
        error = write.error;
        error += "; the entry was restored, so execution may reach it again, and the "
                 "mapping is left writable";
        return false;
    }
    return true;
}

PatchStatus PatchManager::status() const
{
    PatchStatus status;
    status.state = m_state;
    status.quiescedBy = m_quiescedBy;
    status.threadsAtInstall = m_threadsAtInstall;
    if (m_record != nullptr) {
        status.site = m_record->site;
        status.slotAddress = &m_record->slot;
        if (const Generation* live = newestLive(); live != nullptr) {
            status.replacement = live->replacement;
            status.selectedBinding = live->id;
            status.selectedOwner = live->owner;
        }
    }
    for (const Generation& generation : m_generations) {
        if (!generation.released) {
            ++status.liveBindings;
        }
    }
    return status;
}

} // namespace runtime_agent
