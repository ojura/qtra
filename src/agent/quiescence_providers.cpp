#include "agent/quiescence_providers.h"

#include <string>

#if defined(__linux__)
#  include <dirent.h>
#endif

namespace runtime_agent {

std::optional<std::size_t> observedThreadCount()
{
#if defined(__linux__)
    DIR* directory = ::opendir("/proc/self/task");
    if (directory == nullptr) {
        return std::nullopt;
    }
    std::size_t count = 0;
    while (const dirent* entry = ::readdir(directory)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") {
            ++count;
        }
    }
    ::closedir(directory);
    return count;
#else
    return std::nullopt;
#endif
}

std::unique_ptr<QuiescenceLease> AlreadyQuiescent::acquire(const WriteRegion&, std::string& error)
{
    error.clear();
    return std::make_unique<QuiescenceLease>();
}

std::unique_ptr<QuiescenceLease> SingleThreadQuiescer::acquire(const WriteRegion&,
                                                               std::string& error)
{
    const std::optional<std::size_t> threads = observedThreadCount();
    if (!threads.has_value()) {
        error = "this process's threads could not be enumerated, so nothing can be said "
                "about what might be running the target. A caller that knows the phase "
                "is quiet can say so with a different policy";
        return nullptr;
    }
    if (*threads != 1) {
        error = "this process has " + std::to_string(*threads)
              + " threads, so the caller is not the only one that could be inside the "
                "target; install before they start, or supply a policy that accounts "
                "for them";
        return nullptr;
    }
    error.clear();
    return std::make_unique<QuiescenceLease>();
}

std::unique_ptr<QuiescenceLease> RefusingQuiescer::acquire(const WriteRegion&, std::string& error)
{
    const std::optional<std::size_t> threads = observedThreadCount();
    error = "no policy here can account for what might be running the target";
    if (threads.has_value()) {
        error += ", and this process has " + std::to_string(*threads) + " threads";
    }
    error += ". Writing the entry would change several bytes of code another thread may "
             "be executing, so nothing was written";
    return nullptr;
}

} // namespace runtime_agent
