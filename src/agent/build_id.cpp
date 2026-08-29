#include "agent/build_id.h"

#if defined(__linux__)

#include <link.h>

#include <cstring>
#include <string>

namespace {

struct NoteSearch {
    std::string buildId;
};

QString toHex(const unsigned char* bytes, const std::size_t size)
{
    static constexpr char digits[] = "0123456789abcdef";
    QString hex;
    hex.reserve(static_cast<int>(size) * 2);
    for (std::size_t index = 0; index < size; ++index) {
        hex.append(QLatin1Char(digits[bytes[index] >> 4]));
        hex.append(QLatin1Char(digits[bytes[index] & 0x0F]));
    }
    return hex;
}

int visitObject(dl_phdr_info* info, std::size_t, void* data)
{
    auto* search = static_cast<NoteSearch*>(data);
    // dl_iterate_phdr reports the main program first, and that is the build this
    // process is. Everything after it is a shared library it loaded, which
    // includes the snippet modules being checked. Returning non-zero stops the
    // walk, and every path here does, so only the main program is ever seen.

    for (int index = 0; index < info->dlpi_phnum; ++index) {
        const ElfW(Phdr)& header = info->dlpi_phdr[index];
        if (header.p_type != PT_NOTE) {
            continue;
        }
        const auto* cursor = reinterpret_cast<const char*>(info->dlpi_addr + header.p_vaddr);
        const char* end = cursor + header.p_memsz;
        while (cursor + sizeof(ElfW(Nhdr)) <= end) {
            const auto* note = reinterpret_cast<const ElfW(Nhdr)*>(cursor);
            const char* name = cursor + sizeof(ElfW(Nhdr));
            const char* description = name + ((note->n_namesz + 3) & ~3u);
            if (description + note->n_descsz > end) {
                break;
            }
            if (note->n_type == NT_GNU_BUILD_ID
                && note->n_namesz == 4
                && std::memcmp(name, "GNU", 4) == 0) {
                search->buildId = std::string(description, note->n_descsz);
                return 1;
            }
            cursor = description + ((note->n_descsz + 3) & ~3u);
        }
    }
    return 1;
}

} // namespace

QString runtime_agent::hostBuildId()
{
    // Read once. The value cannot change while the process runs.
    static const QString cached = [] {
        NoteSearch search;
        ::dl_iterate_phdr(&visitObject, &search);
        if (search.buildId.empty()) {
            return QString();
        }
        return toHex(reinterpret_cast<const unsigned char*>(search.buildId.data()),
                     search.buildId.size());
    }();
    return cached;
}

#else

QString runtime_agent::hostBuildId()
{
    return QString();
}

#endif
