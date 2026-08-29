#include "agent/patch_registry.h"

namespace runtime_agent {

PatchRegistry& PatchRegistry::instance()
{
    // Allocated once and never deleted. See the note in the header: the text
    // of this process refers to storage this owns, and a destructor at exit
    // would release a recovery lease that is deliberately still held.
    static PatchRegistry* const registry = new PatchRegistry();
    return *registry;
}

PatchManager& PatchRegistry::forEntry(void* const entry)
{
    std::unique_ptr<PatchManager>& manager = m_managers[entry];
    if (manager == nullptr) {
        manager = std::make_unique<PatchManager>(m_write);
    }
    return *manager;
}

bool PatchRegistry::knows(void* const entry) const
{
    return m_managers.find(entry) != m_managers.end();
}

std::vector<void*> PatchRegistry::entries() const
{
    std::vector<void*> known;
    known.reserve(m_managers.size());
    for (const auto& [entry, manager] : m_managers) {
        known.push_back(entry);
    }
    return known;
}

} // namespace runtime_agent
