#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#  define RUNTIME_AGENT_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#  define RUNTIME_AGENT_EXPORT __attribute__((visibility("default")))
#else
#  define RUNTIME_AGENT_EXPORT
#endif

extern "C" {

inline constexpr std::uint32_t RUNTIME_AGENT_ABI_V1 = 0x0001'0000u;

enum RuntimeAgentLogLevelV1 : std::int32_t {
    RUNTIME_AGENT_LOG_DEBUG = 0,
    RUNTIME_AGENT_LOG_INFO = 1,
    RUNTIME_AGENT_LOG_WARNING = 2,
    RUNTIME_AGENT_LOG_ERROR = 3,
};

// This is intentionally a small, stable C ABI. A snippet may include all of the
// application's C++ headers, but its entry point and host callbacks remain easy
// to validate and version.
struct RuntimeAgentHostV1 {
    std::uint32_t abi_version;
    std::uint32_t struct_size;

    // agent_context remains valid for the lifetime of the application. It may be
    // copied into callbacks installed by a snippet because loaded modules are not
    // unloaded by the demo.
    void* agent_context;

    // invocation_context is only valid while the snippet invocation is active.
    // Do not retain it unless a future ABI adds retain/release callbacks.
    void* invocation_context;

    void (*log)(void* agent_context, std::int32_t level, const char* message_utf8);
    void (*emit_event_json)(void* agent_context,
                            const char* event_name_utf8,
                            const char* object_json_utf8);

    // Returns the actual QObject pointer as void*. The caller is responsible for
    // casting it to the right type and obeying (or deliberately violating) Qt's
    // thread-affinity and lifetime rules.
    void* (*find_qobject)(void* agent_context, const char* object_name_utf8);

    // Uses dlsym(RTLD_DEFAULT, ...). This only sees dynamically visible symbols;
    // the external build oracle can provide addresses for other symbols.
    void* (*find_symbol)(void* agent_context, const char* symbol_name_utf8);

    // Valid until the invocation returns.
    const char* (*request_json)(void* invocation_context);

    // Both callbacks copy their strings synchronously.
    void (*complete_json)(void* invocation_context, const char* result_json_utf8);
    void (*fail)(void* invocation_context, const char* error_utf8);

    std::uint64_t (*monotonic_time_ns)(void* agent_context);
};

using RuntimeAgentSnippetRunV1 = void (*)(const RuntimeAgentHostV1* host);

struct RuntimeAgentSnippetV1 {
    std::uint32_t abi_version;
    std::uint32_t struct_size;
    const char* name_utf8;
    RuntimeAgentSnippetRunV1 run;
};

using RuntimeAgentSnippetInitV1 = const RuntimeAgentSnippetV1* (*)();

// Every runtime-compiled snippet exports this exact symbol.
RUNTIME_AGENT_EXPORT const RuntimeAgentSnippetV1* runtime_agent_snippet_init_v1();

} // extern "C"
