#include "agent/prologue_relocation.h"

#include <cerrno>
#include <cstring>
#include <limits>
#include <sstream>

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

namespace runtime_agent {
namespace {

// jmp rel32. Five bytes is the shortest jump reaching anywhere within two
// gigabytes, and taking five bytes of a prologue is possible where taking the
// thirteen an absolute jump needs is not.
constexpr std::size_t jumpBytes = 5;

// endbr64, which a function begins with on a build asking for landing pads.
constexpr std::uint8_t landingPad[]{0xF3U, 0x0FU, 0x1EU, 0xFAU};

// movabs r11, imm64; jmp r11. Reaches any address, which the jump back from the
// copy needs, since nothing bounds how far the copy sits from the body.
constexpr std::size_t absoluteJumpBytes = 13;

std::string hex(const std::uint8_t byte)
{
    static const char digits[] = "0123456789abcdef";
    std::string text = "0x";
    text += digits[byte >> 4U];
    text += digits[byte & 0x0FU];
    return text;
}

// What follows an opcode, so a length is computed and never guessed.
//
// Anything left unset is refused. A length this is not sure of would be a wrong
// length, and nothing downstream would notice: the sweep would walk into the
// middle of an instruction and every answer after that would be invented.
enum OperandFlags : unsigned {
    formUnknown = 0U,
    formKnown = 1U << 0U,
    formModRM = 1U << 1U,
    formImm8 = 1U << 2U,

    // Sixteen or thirty-two bits by the operand size, never sixty-four.
    formImmZ = 1U << 3U,

    // Sixteen, thirty-two or sixty-four, which only the register-immediate
    // moves reach.
    formImmV = 1U << 4U,

    formRel8 = 1U << 5U,
    formRelZ = 1U << 6U,

    // The unary group, where two of the eight operations carry an immediate and
    // the addressing byte says which one this is.
    formGroupUnary = 1U << 7U,

    // An absolute address inside the instruction, eight bytes wide in long mode.
    formMemoryOffset = 1U << 8U,
};

struct OpcodeTables {
    unsigned one[256]{};
    unsigned two[256]{};
};

// Built by filling ranges, because a literal of 256 entries is a thing nobody
// can check by reading.
constexpr OpcodeTables buildTables()
{
    OpcodeTables tables{};
    const auto fill = [](unsigned* table, unsigned from, unsigned to, unsigned flags) {
        for (unsigned index = from; index <= to; ++index) {
            table[index] = flags;
        }
    };

    // The arithmetic block repeats every eight opcodes: memory with register,
    // register with memory, twice over for byte and wide, then the accumulator
    // against an immediate.
    for (unsigned base = 0x00U; base <= 0x38U; base += 8U) {
        fill(tables.one, base, base + 3U, formKnown | formModRM);
        tables.one[base + 4U] = formKnown | formImm8;
        tables.one[base + 5U] = formKnown | formImmZ;
    }

    fill(tables.one, 0x50U, 0x5FU, formKnown);              // push and pop a register
    tables.one[0x63U] = formKnown | formModRM;              // movsxd
    tables.one[0x68U] = formKnown | formImmZ;               // push a wide immediate
    tables.one[0x69U] = formKnown | formModRM | formImmZ;   // imul, wide immediate
    tables.one[0x6AU] = formKnown | formImm8;               // push a byte
    tables.one[0x6BU] = formKnown | formModRM | formImm8;   // imul, byte immediate
    fill(tables.one, 0x70U, 0x7FU, formKnown | formRel8);   // the short conditional jumps
    tables.one[0x80U] = formKnown | formModRM | formImm8;   // the immediate group, byte
    tables.one[0x81U] = formKnown | formModRM | formImmZ;   // the immediate group, wide
    tables.one[0x83U] = formKnown | formModRM | formImm8;   // the immediate group, sign extended
    fill(tables.one, 0x84U, 0x8FU, formKnown | formModRM);  // test, xchg, mov, lea, pop
    fill(tables.one, 0x90U, 0x99U, formKnown);              // nop, xchg, sign extension
    // 9b is a wait, which stands alone in the stream but only ever appears in
    // front of a floating-point instruction this decoder does not read. Calling
    // it one byte would report a success and then refuse the next byte anyway,
    // so it refuses here where the message can say why.
    fill(tables.one, 0x9CU, 0x9FU, formKnown);              // the flag transfers
    fill(tables.one, 0xA0U, 0xA3U, formKnown | formMemoryOffset);
    fill(tables.one, 0xA4U, 0xA7U, formKnown);              // movs and cmps
    tables.one[0xA8U] = formKnown | formImm8;               // test the byte accumulator
    tables.one[0xA9U] = formKnown | formImmZ;               // test the wide accumulator
    fill(tables.one, 0xAAU, 0xAFU, formKnown);              // stos, lods, scas
    fill(tables.one, 0xB0U, 0xB7U, formKnown | formImm8);   // mov a byte immediate
    fill(tables.one, 0xB8U, 0xBFU, formKnown | formImmV);   // mov a wide immediate
    fill(tables.one, 0xC0U, 0xC1U, formKnown | formModRM | formImm8);   // shift by immediate
    tables.one[0xC3U] = formKnown;                          // ret
    tables.one[0xC6U] = formKnown | formModRM | formImm8;   // mov byte immediate
    tables.one[0xC7U] = formKnown | formModRM | formImmZ;   // mov wide immediate
    tables.one[0xC9U] = formKnown;                          // leave
    tables.one[0xCCU] = formKnown;                          // int3
    tables.one[0xCDU] = formKnown | formImm8;
    fill(tables.one, 0xD0U, 0xD3U, formKnown | formModRM);  // shift by one and by cl
    tables.one[0xE8U] = formKnown | formRelZ;               // call
    tables.one[0xE9U] = formKnown | formRelZ;               // jmp
    tables.one[0xEBU] = formKnown | formRel8;               // jmp, one byte
    fill(tables.one, 0xF4U, 0xF5U, formKnown);              // hlt and cmc
    fill(tables.one, 0xF6U, 0xF7U, formKnown | formModRM | formGroupUnary);
    fill(tables.one, 0xF8U, 0xFDU, formKnown);              // the flag operations
    fill(tables.one, 0xFEU, 0xFFU, formKnown | formModRM);  // inc, dec, the indirect group

    fill(tables.two, 0x00U, 0x03U, formKnown | formModRM);  // the descriptor groups
    fill(tables.two, 0x05U, 0x09U, formKnown);              // syscall, clts, sysret, invd
    tables.two[0x0BU] = formKnown;                          // ud2
    tables.two[0x0DU] = formKnown | formModRM;              // prefetch
    fill(tables.two, 0x10U, 0x23U, formKnown | formModRM);  // the vector moves, endbr64, nop
    fill(tables.two, 0x28U, 0x2FU, formKnown | formModRM);  // conversions and comparisons
    fill(tables.two, 0x30U, 0x35U, formKnown);              // the model-specific registers
    tables.two[0x37U] = formKnown;                          // getsec
    fill(tables.two, 0x40U, 0x4FU, formKnown | formModRM);  // the conditional moves
    fill(tables.two, 0x50U, 0x6FU, formKnown | formModRM);  // the vector arithmetic block
    fill(tables.two, 0x70U, 0x73U, formKnown | formModRM | formImm8);   // the shuffles
    fill(tables.two, 0x74U, 0x76U, formKnown | formModRM);  // the vector equality tests
    tables.two[0x77U] = formKnown;                          // emms
    fill(tables.two, 0x7CU, 0x7FU, formKnown | formModRM);  // haddps and the wide moves
    fill(tables.two, 0x80U, 0x8FU, formKnown | formRelZ);   // the long conditional jumps
    fill(tables.two, 0x90U, 0x9FU, formKnown | formModRM);  // the conditional sets
    fill(tables.two, 0xA0U, 0xA2U, formKnown);              // push fs, pop fs, cpuid
    tables.two[0xA3U] = formKnown | formModRM;              // bt
    tables.two[0xA4U] = formKnown | formModRM | formImm8;   // shld by an immediate
    tables.two[0xA5U] = formKnown | formModRM;              // shld by cl
    fill(tables.two, 0xA8U, 0xAAU, formKnown);              // push gs, pop gs, rsm
    tables.two[0xABU] = formKnown | formModRM;              // bts
    tables.two[0xACU] = formKnown | formModRM | formImm8;   // shrd by an immediate
    fill(tables.two, 0xADU, 0xB9U, formKnown | formModRM);  // shrd, fences, imul, movzx, popcnt
    tables.two[0xBAU] = formKnown | formModRM | formImm8;   // the bit-test group
    fill(tables.two, 0xBBU, 0xC1U, formKnown | formModRM);  // btc, bsf, bsr, movsx, xadd
    tables.two[0xC2U] = formKnown | formModRM | formImm8;   // the vector compares
    tables.two[0xC3U] = formKnown | formModRM;              // movnti
    fill(tables.two, 0xC4U, 0xC6U, formKnown | formModRM | formImm8);   // pinsrw, pextrw, shufps
    tables.two[0xC7U] = formKnown | formModRM;              // the compare-exchange group
    fill(tables.two, 0xC8U, 0xCFU, formKnown);              // bswap
    fill(tables.two, 0xD0U, 0xFEU, formKnown | formModRM);  // the rest of the vector block

    return tables;
}

constexpr OpcodeTables tables = buildTables();

} // namespace

bool decodeInstruction(const std::uint8_t* const at,
                       DecodedInstruction& decoded,
                       std::string& error)
{
    decoded = DecodedInstruction{};
    if (at == nullptr) {
        error = "an address is required";
        return false;
    }

    std::size_t offset = 0;
    bool operandSize16 = false;
    bool rexW = false;

    // Legacy prefixes. The address-size prefix is refused because it changes
    // what the addressing bytes mean, and nothing a compiler emits here needs
    // it.
    for (;; ++offset) {
        const std::uint8_t byte = at[offset];
        if (byte == 0x66U) {
            operandSize16 = true;
            continue;
        }
        if (byte == 0xF0U || byte == 0xF2U || byte == 0xF3U || byte == 0x2EU
            || byte == 0x36U || byte == 0x3EU || byte == 0x26U || byte == 0x64U
            || byte == 0x65U) {
            continue;
        }
        if (byte == 0x67U) {
            error = "an address-size prefix changes what the addressing bytes mean, and "
                    "this decoder does not read that form";
            return false;
        }
        break;
    }

    // The vector prefixes, refused by name so the message says what was found
    // instead of calling it an unknown opcode.
    if (at[offset] == 0xC4U || at[offset] == 0xC5U) {
        error = "a vector prefix at " + hex(at[offset])
            + " introduces the extended encoding, which this decoder does not read. A "
              "build using those instructions cannot be swept";
        return false;
    }

    // REX, which sits immediately before the opcode.
    if ((at[offset] & 0xF0U) == 0x40U) {
        rexW = (at[offset] & 0x08U) != 0U;
        ++offset;
    }

    unsigned form = formUnknown;
    bool impliedImm8 = false;
    std::uint8_t primaryOpcode = 0;

    if (at[offset] == 0x0FU) {
        ++offset;
        const std::uint8_t second = at[offset];
        if (second == 0x38U || second == 0x3AU) {
            // The three-byte maps. Everything in the first takes an addressing
            // byte, everything in the second takes one and a byte of immediate.
            impliedImm8 = second == 0x3AU;
            ++offset;
            form = formKnown | formModRM;
        } else {
            form = tables.two[second];
            if (form == formUnknown) {
                error = "the two-byte opcode 0f " + hex(second)
                    + " is not one this decoder reads, so its length is unknown and "
                      "guessing it would be silent";
                return false;
            }
        }
    } else {
        primaryOpcode = at[offset];
        form = tables.one[primaryOpcode];
        if (form == formUnknown) {
            error = "the opcode " + hex(primaryOpcode)
                + " is not one this decoder reads, so its length is unknown and guessing "
                  "it would be silent";
            return false;
        }
    }
    ++offset;

    std::uint8_t modrmReg = 0;
    if ((form & formModRM) != 0U) {
        const std::uint8_t modrm = at[offset];
        ++offset;
        const std::uint8_t mod = (modrm >> 6U) & 0x03U;
        modrmReg = (modrm >> 3U) & 0x07U;
        const std::uint8_t rm = modrm & 0x07U;

        if (mod != 0x03U) {
            bool ripRelative = false;
            unsigned displacement = 0;

            if (rm == 0x04U) {
                // A scale-index-base byte describes the address instead.
                const std::uint8_t sib = at[offset];
                ++offset;
                const std::uint8_t base = sib & 0x07U;
                if (mod == 0x00U && base == 0x05U) {
                    displacement = 4;
                } else if (mod == 0x01U) {
                    displacement = 1;
                } else if (mod == 0x02U) {
                    displacement = 4;
                }
            } else if (mod == 0x00U && rm == 0x05U) {
                // The one form naming an address as a distance from the end of
                // the instruction.
                ripRelative = true;
                displacement = 4;
            } else if (mod == 0x01U) {
                displacement = 1;
            } else if (mod == 0x02U) {
                displacement = 4;
            }

            if (ripRelative) {
                decoded.ripRelative = true;
                decoded.displacementOffset = offset;
            }
            offset += displacement;
        }
    }

    if ((form & (formRel8 | formRelZ)) != 0U) {
        const std::size_t displacementAt = offset;
        // A near branch in long mode always carries four bytes of displacement.
        // The operand-size prefix does not shrink it, and compilers emit that
        // prefix here purely as padding to align what follows, so reading two
        // bytes because it is present walks into the middle of the branch.
        const std::size_t width = (form & formRel8) != 0U ? 1U : 4U;
        offset += width;

        std::int64_t delta = 0;
        if (width == 1U) {
            delta = static_cast<std::int8_t>(at[displacementAt]);
        } else {
            std::int32_t wide = 0;
            std::memcpy(&wide, at + displacementAt, sizeof(wide));
            delta = wide;
        }
        decoded.relativeBranch = true;
        decoded.branchTarget = at + offset + delta;
    }

    std::size_t immediate = 0;
    if ((form & formGroupUnary) != 0U) {
        // Of the eight operations sharing this opcode only test carries an
        // immediate, and the addressing byte's middle field says which one this
        // is. The byte-wide member takes a byte whatever the operand size says.
        if (modrmReg <= 1U) {
            immediate = primaryOpcode == 0xF6U ? 1U : (operandSize16 ? 2U : 4U);
        }
    } else if ((form & formImm8) != 0U) {
        immediate = 1;
    } else if ((form & formImmZ) != 0U) {
        immediate = operandSize16 ? 2U : 4U;
    } else if ((form & formImmV) != 0U) {
        immediate = operandSize16 ? 2U : (rexW ? 8U : 4U);
    } else if ((form & formMemoryOffset) != 0U) {
        immediate = 8;
    }
    if (impliedImm8) {
        immediate += 1;
    }
    offset += immediate;

    decoded.length = offset;
    return true;
}

namespace {

// The function's extent, which the sweep needs and nothing else provides.
bool extentOf(void* const function, const std::uint8_t*& entry, std::size_t& bytes,
              std::string& error)
{
    ::Dl_info info{};
    void* symbolPointer = nullptr;
    if (::dladdr1(function, &info, &symbolPointer, RTLD_DL_SYMENT) == 0
        || symbolPointer == nullptr) {
        error = "the loader cannot name the function at this address, so there is no way to "
                "learn where its body ends. Only a symbol in the dynamic table can be swept, "
                "so a static function, or one in a binary linked without exported symbols, "
                "is refused";
        return false;
    }

    const auto* symbol = static_cast<const ElfW(Sym)*>(symbolPointer);
    if (symbol->st_size == 0) {
        error = std::string("the symbol '")
            + (info.dli_sname != nullptr ? info.dli_sname : "?")
            + "' records no size, so where its body ends is unknown and it cannot be swept "
              "for branches into the bytes that would be taken";
        return false;
    }

    entry = static_cast<const std::uint8_t*>(info.dli_saddr);
    bytes = static_cast<std::size_t>(symbol->st_size);
    return true;
}

// Somewhere to put the copy, close enough that a moved operand naming its
// address as a distance can still reach what it named.
//
// The correction is a signed thirty-two bit distance, so the copy has to sit
// within two gigabytes of the original. Asking for an address near the function
// is what makes that true. Falling back to anywhere still installs, as long as
// nothing taken was position-dependent.
void* allocateNear(const std::uint8_t* const target, const std::size_t bytes)
{
    const auto pageSize = static_cast<std::uintptr_t>(::sysconf(_SC_PAGESIZE));
    const std::size_t rounded =
        static_cast<std::size_t>((bytes + pageSize - 1U) / pageSize * pageSize);
    const auto base = reinterpret_cast<std::uintptr_t>(target) & ~(pageSize - 1U);

    // Outward from the function in both directions, staying well inside the
    // reach of a thirty-two bit displacement.
    constexpr std::uintptr_t limit = 1ULL << 30U;
    for (std::uintptr_t step = pageSize; step < limit; step *= 2U) {
        const std::uintptr_t candidates[]{base + step, base - step};
        for (const std::uintptr_t candidate : candidates) {
            if (candidate < pageSize) {
                continue;
            }
            void* const hint = reinterpret_cast<void*>(candidate);
            void* const mapping = ::mmap(hint, rounded, PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                                         -1, 0);
            if (mapping != MAP_FAILED) {
                return mapping;
            }
        }
    }

    void* const anywhere = ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return anywhere == MAP_FAILED ? nullptr : anywhere;
}

void writeAbsoluteJump(std::uint8_t* const at, const void* const destination)
{
    // movabs r11, <destination>. r11 is caller-saved under the System V AMD64
    // ABI, so nothing arriving here is holding anything in it.
    at[0] = 0x49U;
    at[1] = 0xBBU;
    const auto value = reinterpret_cast<std::uintptr_t>(destination);
    std::memcpy(at + 2, &value, sizeof(value));
    // jmp r11
    at[10] = 0x41U;
    at[11] = 0xFFU;
    at[12] = 0xE3U;
}

} // namespace

bool planPrologueRelocation(void* const function, ProloguePlan& plan, std::string& error)
{
    plan = ProloguePlan{};
    error.clear();
    if (function == nullptr) {
        error = "a function address is required";
        return false;
    }

    const std::uint8_t* entry = nullptr;
    std::size_t functionBytes = 0;
    if (!extentOf(function, entry, functionBytes, error)) {
        return false;
    }

    // A landing pad stays where it is. Writing over it would leave every
    // indirect call to this function faulting on a machine enforcing them.
    const std::uint8_t* patchAt = entry;
    bool keepsLandingPad = false;
    if (functionBytes >= sizeof(landingPad)
        && std::memcmp(entry, landingPad, sizeof(landingPad)) == 0) {
        patchAt = entry + sizeof(landingPad);
        keepsLandingPad = true;
    }

    // Whole instructions, until there is room for the jump.
    std::size_t taken = 0;
    while (taken < jumpBytes) {
        const std::size_t reached = static_cast<std::size_t>(patchAt - entry) + taken;
        if (reached >= functionBytes) {
            error = "this function's body ends before enough whole instructions have been "
                    "read to hold a jump, so there is nothing here to take";
            return false;
        }

        DecodedInstruction decoded;
        std::string why;
        if (!decodeInstruction(patchAt + taken, decoded, why)) {
            error = "the instruction at offset " + std::to_string(taken)
                + " of this function's opening bytes cannot be read: " + why
                + ". A prologue using forms this decoder does not know cannot be moved; the "
                  "entry patcher or a call through the table may still reach this function";
            return false;
        }
        if (decoded.relativeBranch) {
            error = "the instruction at offset " + std::to_string(taken)
                + " of this function's opening bytes branches somewhere named as a distance "
                  "from itself, so moving it would change where it goes. This decoder does "
                  "not rewrite branches, so the site is refused";
            return false;
        }
        if (decoded.ripRelative) {
            plan.adjustedRipRelative = true;
        }
        taken += decoded.length;
    }

    // The whole body, for anything jumping back into what would be taken.
    const std::uint8_t* const takenBegin = patchAt;
    const std::uint8_t* const takenEnd = patchAt + taken;

    std::size_t offset = 0;
    while (offset < functionBytes) {
        DecodedInstruction decoded;
        std::string why;
        if (!decodeInstruction(entry + offset, decoded, why)) {
            error = "sweeping this function for branches stopped at offset "
                + std::to_string(offset) + ": " + why
                + ". Without a complete sweep there is no way to know whether something "
                  "later jumps into the bytes that would be overwritten, and writing "
                  "without knowing is the outcome worth avoiding";
            plan = ProloguePlan{};
            return false;
        }

        if (decoded.relativeBranch && decoded.branchTarget > takenBegin
            && decoded.branchTarget < takenEnd) {
            std::ostringstream message;
            message << "the instruction at offset " << offset << " branches to offset "
                    << static_cast<std::size_t>(decoded.branchTarget - entry)
                    << ", which is inside the " << taken
                    << " bytes a jump would be written over. That branch would land in the "
                       "middle of the jump and run whatever the remaining bytes happen to "
                       "be. Nothing can be written here; the entry patcher or a call through "
                       "the table may still reach this function";
            error = message.str();
            plan = ProloguePlan{};
            return false;
        }

        offset += decoded.length;
    }

    plan.entry = const_cast<std::uint8_t*>(entry);
    plan.functionBytes = functionBytes;
    plan.patchAddress = const_cast<std::uint8_t*>(patchAt);
    plan.takenBytes = taken;
    plan.keepsLandingPad = keepsLandingPad;
    return true;
}

bool installRelocatedPrologue(const ProloguePlan& plan,
                              void* const replacement,
                              Quiescer& quiescer,
                              TextWriter& writer,
                              RelocatedPrologue& installed,
                              std::string& error)
{
    installed = RelocatedPrologue{};
    error.clear();

    if (!plan.valid()) {
        error = "a planned site is required";
        return false;
    }
    if (replacement == nullptr) {
        error = "a replacement is required";
        return false;
    }

    auto* const patchAt = static_cast<std::uint8_t*>(plan.patchAddress);

    // The jump names its destination as a distance, so the replacement has to
    // be within reach of it.
    const auto from = reinterpret_cast<std::intptr_t>(patchAt + jumpBytes);
    const auto to = reinterpret_cast<std::intptr_t>(replacement);
    const std::intptr_t delta = to - from;
    if (delta > std::numeric_limits<std::int32_t>::max()
        || delta < std::numeric_limits<std::int32_t>::min()) {
        error = "the replacement sits further from this entry than a five-byte jump reaches. "
                "A longer jump would need more of the prologue than is being taken";
        return false;
    }

    // The copy, its landing pad, and the jump back.
    const std::size_t trampolineBytes =
        sizeof(landingPad) + plan.takenBytes + absoluteJumpBytes;
    void* const trampoline = allocateNear(patchAt, trampolineBytes);
    if (trampoline == nullptr) {
        error = "no memory could be mapped for the copy of this prologue";
        return false;
    }

    auto* const copy = static_cast<std::uint8_t*>(trampoline);
    // Reached through a pointer, which is an indirect branch, so it begins with
    // a landing pad whatever the target did.
    std::memcpy(copy, landingPad, sizeof(landingPad));
    std::memcpy(copy + sizeof(landingPad), patchAt, plan.takenBytes);

    // Every operand naming an address as a distance from where it sat has to
    // name the same address from where it now sits.
    std::size_t offset = 0;
    while (offset < plan.takenBytes) {
        DecodedInstruction decoded;
        std::string why;
        if (!decodeInstruction(patchAt + offset, decoded, why)) {
            (void)::munmap(trampoline, trampolineBytes);
            error = "the prologue changed between planning and installing: " + why;
            return false;
        }
        if (decoded.ripRelative) {
            const std::uint8_t* const originalAt = patchAt + offset;
            std::uint8_t* const copiedAt = copy + sizeof(landingPad) + offset;
            std::int32_t displacement = 0;
            std::memcpy(&displacement, originalAt + decoded.displacementOffset,
                        sizeof(displacement));

            const auto originalEnd =
                reinterpret_cast<std::intptr_t>(originalAt + decoded.length);
            const auto copiedEnd = reinterpret_cast<std::intptr_t>(copiedAt + decoded.length);
            const std::intptr_t corrected =
                static_cast<std::intptr_t>(displacement) + originalEnd - copiedEnd;
            if (corrected > std::numeric_limits<std::int32_t>::max()
                || corrected < std::numeric_limits<std::int32_t>::min()) {
                (void)::munmap(trampoline, trampolineBytes);
                error = "the copy landed too far from the original for an operand naming its "
                        "address as a distance to still reach what it named";
                return false;
            }
            const auto narrowed = static_cast<std::int32_t>(corrected);
            std::memcpy(copiedAt + decoded.displacementOffset, &narrowed, sizeof(narrowed));
        }
        offset += decoded.length;
    }

    writeAbsoluteJump(copy + sizeof(landingPad) + plan.takenBytes, patchAt + plan.takenBytes);

    if (::mprotect(trampoline, trampolineBytes, PROT_READ | PROT_EXEC) != 0) {
        const std::string reason = std::strerror(errno);
        (void)::munmap(trampoline, trampolineBytes);
        error = "the copy could not be made executable: " + reason;
        return false;
    }
    __builtin___clear_cache(static_cast<char*>(trampoline),
                            static_cast<char*>(trampoline) + trampolineBytes);

    // Several bytes change at once, so a thread standing inside them would run
    // the join of what was there and what is arriving. That range is exactly
    // what a policy able to account for threads has to be asked about, and it
    // is the same range nothing may jump into, which is the question the sweep
    // answers because no policy can.
    std::unique_ptr<QuiescenceLease> lease =
        quiescer.acquire(WriteRegion{plan.patchAddress, plan.takenBytes}, error);
    if (lease == nullptr) {
        (void)::munmap(trampoline, trampolineBytes);
        return false;
    }

    std::vector<std::uint8_t> written(plan.takenBytes, 0x90U);
    written[0] = 0xE9U;
    const auto narrowed = static_cast<std::int32_t>(delta);
    std::memcpy(written.data() + 1, &narrowed, sizeof(narrowed));

    installed.savedBytes.assign(patchAt, patchAt + plan.takenBytes);

    const TextWriteResult result = writer.write(patchAt, written.data(), written.size());
    if (!result.changedBytes()) {
        (void)::munmap(trampoline, trampolineBytes);
        installed = RelocatedPrologue{};
        error = result.error;
        return false;
    }

    installed.plan = plan;
    installed.trampoline = trampoline;
    installed.trampolineBytes = trampolineBytes;
    installed.original = copy;

    if (!result.complete()) {
        // The bytes changed and the mapping could not be put back. The entry
        // reaches the replacement as asked, and something that should be
        // read-only is not, so the caller is told both.
        error = result.error;
        return false;
    }
    return true;
}

bool restoreRelocatedPrologue(const RelocatedPrologue& installed,
                              Quiescer& quiescer,
                              TextWriter& writer,
                              std::string& error)
{
    error.clear();
    if (installed.savedBytes.empty() || !installed.plan.valid()) {
        error = "there is nothing recorded to put back";
        return false;
    }

    // The same bytes going back, so the same question about them.
    std::unique_ptr<QuiescenceLease> lease = quiescer.acquire(
        WriteRegion{installed.plan.patchAddress, installed.plan.takenBytes}, error);
    if (lease == nullptr) {
        return false;
    }

    const TextWriteResult result =
        writer.write(installed.plan.patchAddress, installed.savedBytes.data(),
                     installed.savedBytes.size());
    if (!result.complete()) {
        error = result.error;
        return false;
    }

    // The copy stays mapped. A replacement was handed its address and may still
    // hold it, and a thread may be executing inside it now.
    return true;
}

} // namespace runtime_agent
