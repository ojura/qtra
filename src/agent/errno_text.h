#pragma once

// Rendering an errno value as text.
//
// Four files under src/agent report failed system calls, and each said what it
// wanted differently: three appended std::strerror alone, one appended the
// number as well. The sentence around it belongs to the caller, which knows
// what it was doing and can say so precisely. What does not is how the value
// itself becomes readable, so that is here.
//
// Keeping the number matters when the text is generic. "Invalid argument" from
// an mprotect on its own says little; "Invalid argument (errno=22)" can be
// looked up and matched against strace output.

#include <cerrno>
#include <cstring>
#include <string>

namespace runtime_agent {

// Takes the value rather than reading errno.
//
// Reading the global one here would read it wherever the message happens to be
// built, which is not always where the call failed: anything in between, a
// destructor or another system call in an argument, may have set it. Callers
// that saved it at the failure pass what they saved.
//
// std::strerror is not required to be thread-safe, and this agent does format
// errors from more than one thread. That is unchanged from before, and this is
// now the one place a switch to strerror_r would have to happen.
[[nodiscard]] inline std::string errnoText(const int number)
{
    return std::string(std::strerror(number)) + " (errno=" + std::to_string(number) + ')';
}

} // namespace runtime_agent
