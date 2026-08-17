#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace runtime_agent {

// Deliberately small and sharp-edged. This demo implementation is x86-64/Linux
// only and expects a GCC patchable_function_entry region at the target address.
class EntryHotpatch final {
public:
    EntryHotpatch() = default;
    EntryHotpatch(const EntryHotpatch&) = delete;
    EntryHotpatch& operator=(const EntryHotpatch&) = delete;
    ~EntryHotpatch();

    [[nodiscard]] bool apply(void* target,
                             void* replacement,
                             std::size_t reserved_bytes,
                             std::string& error);
    [[nodiscard]] bool rollback(std::string& error);

    [[nodiscard]] bool active() const noexcept { return m_target != nullptr; }
    [[nodiscard]] void* target() const noexcept { return m_target; }
    [[nodiscard]] void* replacement() const noexcept { return m_replacement; }
    [[nodiscard]] std::size_t reservedBytes() const noexcept { return m_original.size(); }

private:
    [[nodiscard]] bool writeBytes(const std::uint8_t* bytes,
                                  std::size_t size,
                                  std::string& error);

    void* m_target = nullptr;
    void* m_replacement = nullptr;
    std::vector<std::uint8_t> m_original;
};

} // namespace runtime_agent
