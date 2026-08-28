#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace runtime_agent {

// What a write into mapped text did, which is a different question from whether
// it succeeded. Writing runs as three steps: make the mapping writable, copy,
// put the permissions back. A failure in the third step leaves bytes changed,
// so a caller told only "false" would carry on as though the process still ran
// the code it did before.
enum class TextWriteOutcome {
    NotWritten,
    Written,
    WrittenProtectionNotRestored,
};

struct TextWriteResult {
    TextWriteOutcome outcome = TextWriteOutcome::NotWritten;
    std::string error;

    [[nodiscard]] bool complete() const noexcept
    {
        return outcome == TextWriteOutcome::Written;
    }

    // Whether the bytes at the address may differ from what was there before.
    [[nodiscard]] bool changedBytes() const noexcept
    {
        return outcome != TextWriteOutcome::NotWritten;
    }
};

// The permission call, as a parameter so a test can fail the one after the copy.
// A real mprotect will not fail on demand, and that is the path worth testing,
// because it is the only one that leaves the process changed and the caller
// told nothing worked.
using ProtectFunction = int (*)(void* address, std::size_t length, int protection);

// Copies bytes over mapped executable text. Knows nothing about what the bytes
// mean, so gateway installation and entry redirection share it unchanged.
[[nodiscard]] TextWriteResult writeText(void* address,
                                        const std::uint8_t* bytes,
                                        std::size_t size,
                                        ProtectFunction protect = nullptr);

// Inactive means the target holds its own instructions. RecoveryRequired means
// the bytes were changed and the mapping could not be put back, so what the
// target now runs is not what either side intended and nothing may resume on
// the assumption that it is.
enum class PatchState {
    Inactive,
    Active,
    RecoveryRequired,
};

// Deliberately small and sharp-edged. This demo implementation is x86-64/Linux
// only and expects a GCC patchable_function_entry NOP region at the target
// address, optionally following a four-byte Intel CET ENDBR64 landing pad.
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

    // Tests substitute the permission call to reach the path where the copy
    // has happened and putting the mapping back has not.
    void setProtectFunction(const ProtectFunction protect) noexcept { m_protect = protect; }

    [[nodiscard]] PatchState state() const noexcept { return m_state; }

    // True while the target may hold anything other than its own instructions,
    // which includes the state where a write completed and the mapping could
    // not be restored, since the redirection is live either way.
    [[nodiscard]] bool active() const noexcept { return m_state != PatchState::Inactive; }
    [[nodiscard]] void* target() const noexcept { return m_target; }
    [[nodiscard]] void* patchAddress() const noexcept { return m_patchAddress; }
    [[nodiscard]] void* replacement() const noexcept { return m_replacement; }
    [[nodiscard]] std::size_t reservedBytes() const noexcept { return m_original.size(); }

private:
    PatchState m_state = PatchState::Inactive;
    ProtectFunction m_protect = nullptr;
    void* m_target = nullptr;
    void* m_patchAddress = nullptr;
    void* m_replacement = nullptr;
    std::vector<std::uint8_t> m_original;
};

} // namespace runtime_agent
