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
    }
    return "unknown";
}

PatchManager::~PatchManager()
{
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

    // Everything between here and releasing is the write and what it needs,
    // and none of it allocates. The site is checked again because everything
    // above happened while the target was still running, and it is checked with
    // the form that answers without saying why: a parked thread may hold the
    // allocator's lock, so composing a sentence here would be waiting for a
    // thread that is waiting for this one.
    const bool stillAcceptsGateway = siteAcceptsGateway(site);
    const TextWriteResult write = stillAcceptsGateway
        ? m_write->write(site.patchAddress, gateway.data(), gateway.size())
        : TextWriteResult{};

    lease.reset();

    if (!stillAcceptsGateway) {
        // Asked again now that everything is running, for the reason.
        (void)siteAcceptsGateway(site, error);
        return false;
    }

    // From here the bytes have changed, whatever the outcome, so what admitted
    // that write is what describes the entry from now on.
    if (write.complete()) {
        m_installedUnder = admission;
        m_record = std::move(record);
        m_state = PatchState::GatewayOriginal;
        return true;
    }

    error = write.error;
    if (!write.changedBytes()) {
        return false;
    }

    // The whole gateway was copied and the mapping could not be made executable
    // again. There is no outcome here that copies part of an instruction
    // stream, so the entry holds a complete, working gateway: its slot names
    // the continuation, so the original function runs, and choosing a
    // replacement afterwards is a store like any other.
    //
    // So this is an installed gateway with one thing wrong with it, and what is
    // wrong is a page that should not be writable. Calling it a state needing
    // the entry's bytes put back would say the bytes are wrong, which they are
    // not.
    m_record = std::move(record);
    m_installedUnder = admission;
    m_mappingLeftWritable = true;
    m_state = PatchState::GatewayOriginal;
    error += "; the gateway is installed and runs the original, and the page it is on is "
             "still writable, which repairing needs no code write and no stop";
    return true;
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

PatchStatus PatchManager::status() const
{
    PatchStatus status;
    status.state = m_state;
    status.installedUnder = m_installedUnder;
    status.threadsAtInstall = m_threadsAtInstall;
    status.mappingLeftWritable = m_mappingLeftWritable;
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
