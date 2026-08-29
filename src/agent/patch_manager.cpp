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
    // Destroying a manager that is still in recovery loses the saved bytes and
    // drops the lease, so nothing can recover afterwards and whatever the lease
    // was holding is let go. This embedding does not do that: it recovers first,
    // and the assert says so to whoever changes that.
    //
    // Leaking the lease instead would be worse. Today's lease restores nothing,
    // so letting it go costs nothing, but a lease from a policy that actually
    // stopped threads would leave them stopped for the life of the process with
    // nothing able to release them.
    //
    // The structural answer is for the site registry to outlive any one manager
    // and hold the recovery state, so recovery stays reachable no matter who is
    // destroyed. That is part of splitting the registry out.
    //
    // What produces this state today is a gateway that was copied completely and
    // whose mapping could not be made executable again. A half-written
    // instruction stream cannot occur here, because the copy does not half-fail.
    assert(m_state != PatchState::RecoveryRequired
           && "recover before destroying a manager that is holding recovery state");
    // Nothing here reclaims what the record holds, and nothing needs to: the
    // registry owns managers for the life of the process, so this runs only in
    // a test or at exit.

    // The gateway is permanent and carries this record's slot address as an
    // immediate, so the storage has to outlive whatever installed it. Freeing it
    // here would leave every later call loading a destination out of released
    // memory, and the manager going away is not a reason for the entry to stop
    // working. So the record is released deliberately: it is unreachable
    // afterwards and that is the intent, since nothing may ever reclaim it.
    (void)m_record.release();
}

bool PatchManager::installGateway(const PatchSite& site,
                                  const LiveTextWriteAdmission& admission,
                                  Quiescer& quiescer,
                                  std::string& error)
{
    error.clear();
    if (!site.valid()) {
        error = "a resolved site is required to install a gateway";
        return false;
    }
    if (m_state == PatchState::RecoveryRequired) {
        error = "an earlier install left the entry rewritten and execution stopped; recover "
                "before writing anything else";
        return false;
    }
    if (m_state != PatchState::NoGateway) {
        error = "this entry already has a gateway; selecting what it reaches is a store and "
                "needs no further write";
        return false;
    }
    // Before anything is written, because a record that cannot be attributed is
    // worth nothing to whoever finds it later, and by then the bytes have
    // changed.
    if (!attributable(admission, error)) {
        return false;
    }
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

    const std::vector<std::uint8_t> gateway = encodeGateway(&record->slot, site.availableBytes);
    if (gateway.empty()) {
        error = "the gateway could not be encoded for this site";
        return false;
    }

    // Recorded before acquiring, so a refusal reports what it was judged
    // against as well as a success.
    m_threadsAtInstall = observedThreadCount();

    // The bytes about to change, so a policy able to account for threads can
    // check that none of them is standing inside them.
    std::unique_ptr<QuiescenceLease> lease =
        quiescer.acquire(WriteRegion{site.patchAddress, gateway.size()}, error);
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
        m_write->write(site.patchAddress, gateway.data(), gateway.size());
    // From here the bytes have changed, whatever the outcome, so what admitted
    // that write is what describes the entry from now on.
    m_installedUnder = admission;

    if (write.complete()) {
        m_record = std::move(record);
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
    // failed.
    //
    // Keeping the lease is what stops execution reaching it, and only a policy
    // whose lease costs the rest of the process nothing can do that. One that
    // parked every thread cannot be held across a return into ordinary code:
    // the next allocation or log line can wait on a lock a parked thread holds.
    if (!quiescer.leaseMaySurviveTheWrite()) {
        // Threads go first, and nothing else happens until they have. They are
        // parked wherever they were, holding whatever they held, so appending
        // to a string or allocating a record here can wait on a lock only a
        // parked thread can release.
        lease.reset();
        error += "; the entry is rewritten and this policy's lease cannot be held while "
                 "anything else runs, so execution was let go with the entry in that state. "
                 "It runs the original through the gateway, and rollback is still required";
        m_record = std::move(record);
        m_recovery = std::make_unique<Recovery>(std::move(original), nullptr, admission,
                                                m_write);
        m_state = PatchState::RecoveryRequired;
        return false;
    }

    m_record = std::move(record);
    m_recovery = std::make_unique<Recovery>(std::move(original), std::move(lease), admission,
                                            m_write);
    m_state = PatchState::RecoveryRequired;
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
    m_state = live != nullptr ? PatchState::GatewayReplacement : PatchState::GatewayOriginal;
}

void* PatchManager::continuation() const noexcept
{
    return m_record != nullptr ? m_record->continuation : nullptr;
}

bool PatchManager::bind(void* replacement,
                        const std::uint64_t owner,
                        PatchBinding& binding,
                        std::string& error)
{
    error.clear();
    binding = PatchBinding{};
    if (replacement == nullptr) {
        error = "a replacement is required";
        return false;
    }
    if (m_state == PatchState::RecoveryRequired) {
        error = "an earlier install left the entry rewritten and execution stopped; recover "
                "before binding anything else";
        return false;
    }
    if (m_state == PatchState::NoGateway) {
        error = "this entry has no gateway, so there is nothing to select through; install "
                "one first";
        return false;
    }

    // Against the site the gateway was installed at, which is the one the jump
    // will actually be taken from.
    if (!replacementIsReachable(m_record->site, replacement, error)) {
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
        // falls back to the newest predecessor, not to the original.
        publishSelection();
        return true;
    }
    error = "no such binding";
    return false;
}

bool PatchManager::recover(Quiescer& quiescer, std::string& error)
{
    error.clear();
    if (m_state != PatchState::RecoveryRequired) {
        error = "there is nothing to recover";
        return false;
    }

    // Its own stop. The install's lease may still be held, in which case this
    // asks for a second one over the same bytes and a policy that stopped
    // nothing gives it freely; or it was let go because it could not be held,
    // in which case threads have been running and nothing about the earlier
    // stop says anything about now.
    std::unique_ptr<QuiescenceLease> lease = quiescer.acquire(
        WriteRegion{m_record->site.patchAddress, m_recovery->original.size()}, error);
    if (lease == nullptr) {
        return false;
    }

    // The one path that writes code outside installation, and it writes with
    // the writer the failed install used, held by the record. The lease that
    // install acquired is in there too and is still held.
    const TextWriteResult write = m_recovery->write->write(m_record->site.patchAddress,
                                                           m_recovery->original.data(),
                                                           m_recovery->original.size());
    if (!write.changedBytes()) {
        error = write.error;
        return false;
    }

    // The entry's own bytes are back, so nothing is standing over a half
    // state and the threads can go. They go before anything here frees
    // storage: a parked thread may hold the allocator's lock, and destroying
    // the record and the saved bytes is where that would be waited on.
    lease.reset();

    m_state = PatchState::NoGateway;
    m_record.reset();
    m_generations.clear();
    m_recovery.reset();
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
    status.installedUnder = m_installedUnder;
    status.threadsAtInstall = m_threadsAtInstall;
    if (m_recovery != nullptr) {
        status.recoveryAdmission = m_recovery->admission;
    }
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
