#include "agent/got_registry.h"

namespace runtime_agent {

GotRegistry& GotRegistry::instance()
{
    // Never destroyed. See the header: slots in loaded objects name what this
    // holds the originals for, and they outlive everything that asked.
    static GotRegistry* const registry = new GotRegistry();
    return *registry;
}

const GotRegistry::Generation* GotRegistry::Slot::newestLive() const noexcept
{
    for (auto it = generations.rbegin(); it != generations.rend(); ++it) {
        if (!it->released) {
            return &(*it);
        }
    }
    return nullptr;
}

bool GotRegistry::publish(Slot& slot, std::string& error)
{
    const Generation* live = slot.newestLive();
    void* const wanted = live != nullptr ? live->replacement : slot.original;

    const SlotWriteResult written = live != nullptr
        ? redirectGotSlot(slot.site, wanted, m_protect)
        : restoreGotSlot(slot.site, m_protect);

    if (written.outcome == SlotWriteOutcome::WrittenProtectionNotRestored) {
        // The slot names what it was asked to. What is wrong is a page that
        // should not be writable, which putting back needs no store, so this is
        // recorded and reported and not treated as the write having failed.
        slot.mappingLeftWritable = true;
        error = written.error;
        return true;
    }
    if (!written.complete()) {
        error = written.error;
        return false;
    }
    return true;
}

bool GotRegistry::bind(const GotSite& site,
                       void* const replacement,
                       const std::uint64_t owner,
                       const LandingPadRule rule,
                       GotBinding& binding,
                       std::string& error)
{
    binding = GotBinding{};
    error.clear();

    if (!site.valid()) {
        error = "the site was never resolved";
        return false;
    }
    if (replacement == nullptr) {
        error = "a replacement is required";
        return false;
    }
    if (rule == LandingPadRule::Required && !hasLandingPad(replacement)) {
        error = "the replacement does not begin with the marker an indirect branch is "
                "allowed to land on, and every way of reaching a slot's destination is an "
                "indirect branch. Build it with branch protection, or say the process is "
                "not checking";
        return false;
    }

    const std::lock_guard<std::mutex> held(m_mutex);

    std::unique_ptr<Slot>& slot = m_slots[site.slot];
    if (slot == nullptr) {
        slot = std::make_unique<Slot>();
        slot->site = site;
        // Captured once, from the first site anybody brought. Reading it again
        // later would read whatever is selected now.
        slot->original = site.resolved;
    }

    const Generation* displaced = slot->newestLive();
    binding.original = slot->original;
    binding.previous = displaced != nullptr ? displaced->replacement : slot->original;
    binding.id = m_nextBinding++;

    slot->generations.push_back(Generation{binding.id, owner, replacement, false});

    if (!publish(*slot, error)) {
        // Nothing was written, so this generation never took effect and is not
        // left behind for a later release to trip over.
        slot->generations.pop_back();
        binding = GotBinding{};
        return false;
    }
    return true;
}

bool GotRegistry::unbind(const std::uint64_t id, const std::uint64_t owner, std::string& error)
{
    error.clear();
    const std::lock_guard<std::mutex> held(m_mutex);

    for (auto& [address, slot] : m_slots) {
        for (Generation& generation : slot->generations) {
            if (generation.id != id) {
                continue;
            }
            if (generation.owner != owner) {
                error = "this binding belongs to another owner";
                return false;
            }
            if (generation.released) {
                error = "this binding was already released";
                return false;
            }
            generation.released = true;
            // Recomputed from what is still live, so releasing something that
            // was not selected leaves the slot alone, and releasing what was
            // selected falls back to the newest predecessor and only then to
            // the original.
            return publish(*slot, error);
        }
    }
    error = "no such binding";
    return false;
}

bool GotRegistry::statusOf(void** const slot, GotSlotStatus& into) const
{
    const std::lock_guard<std::mutex> held(m_mutex);
    const auto found = m_slots.find(slot);
    if (found == m_slots.end()) {
        return false;
    }
    const Slot& held_slot = *found->second;
    into = GotSlotStatus{};
    into.caller = held_slot.site.caller;
    into.symbol = held_slot.site.symbol;
    into.kind = held_slot.site.kind;
    into.original = held_slot.original;
    into.mappingLeftWritable = held_slot.mappingLeftWritable;
    if (const Generation* live = held_slot.newestLive(); live != nullptr) {
        into.selected = live->replacement;
        into.selectedBinding = live->id;
        into.selectedOwner = live->owner;
    } else {
        into.selected = held_slot.original;
    }
    for (const Generation& generation : held_slot.generations) {
        if (!generation.released) {
            ++into.liveBindings;
        }
    }
    return true;
}

std::vector<GotSlotStatus> GotRegistry::status() const
{
    std::vector<GotSlotStatus> all;
    const std::lock_guard<std::mutex> held(m_mutex);
    all.reserve(m_slots.size());
    for (const auto& [address, slot] : m_slots) {
        GotSlotStatus one;
        one.caller = slot->site.caller;
        one.symbol = slot->site.symbol;
        one.kind = slot->site.kind;
        one.original = slot->original;
        one.mappingLeftWritable = slot->mappingLeftWritable;
        if (const Generation* live = slot->newestLive(); live != nullptr) {
            one.selected = live->replacement;
            one.selectedBinding = live->id;
            one.selectedOwner = live->owner;
        } else {
            one.selected = slot->original;
        }
        for (const Generation& generation : slot->generations) {
            if (!generation.released) {
                ++one.liveBindings;
            }
        }
        all.push_back(std::move(one));
    }
    return all;
}

} // namespace runtime_agent
