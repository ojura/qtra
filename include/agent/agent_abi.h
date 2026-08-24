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

    // Byte stash.
    //
    // The host keeps opaque byte strings under caller-chosen keys and never
    // interprets them. Its purpose is that a module which overwrites host state
    // can put the original somewhere outside its own memory, so that code
    // written later — including a module written after this one misbehaved —
    // can restore it. A module's own private copy is unreachable to everything
    // else, which is what makes an undo written after the fact impossible.
    //
    // The namespace is flat and deliberately so: scoping keys to a module would
    // shut out exactly the later repair module the stash exists to serve. The
    // convention is "<module-name>/<what>". Entries outlive the module that
    // wrote them and are only removed by an explicit drop, because deciding
    // that a restore actually worked needs an observation the module cannot
    // make. All three copy synchronously and may be called from any thread,
    // including from outside an invocation.

    // Returns 1 if the key was already present, 0 if it was new, and a negative
    // value on failure; -1 specifically means the key exists and overwrite was
    // 0. Overwriting is opt-in because the dangerous collision is a later
    // generation saving corrupt data over its predecessor's good copy.
    std::int32_t (*stash_put)(void* agent_context,
                              const char* key_utf8,
                              const void* bytes,
                              std::int64_t size,
                              std::int32_t overwrite);

    // Returns the total size of the entry, copying min(size, capacity) bytes.
    // A capacity of 0 queries the size without copying. Returns -1 when the key
    // is absent, which an empty entry (0) is distinct from.
    std::int64_t (*stash_get)(void* agent_context,
                              const char* key_utf8,
                              void* buffer,
                              std::int64_t capacity);

    // Returns 1 if an entry was removed, 0 if the key was absent.
    std::int32_t (*stash_drop)(void* agent_context, const char* key_utf8);

    // Writes a JSON array of {key, size, monotonicNs, moduleId} as UTF-8 and
    // returns its total length, following the same convention as stash_get: a
    // capacity of 0 queries the length. The host stamps each entry, so
    // provenance does not depend on the depositor reporting it honestly.
    std::int64_t (*stash_list)(void* agent_context, char* buffer, std::int64_t capacity);
};

using RuntimeAgentSnippetRunV1 = void (*)(const RuntimeAgentHostV1* host);

struct RuntimeAgentSnippetV1 {
    std::uint32_t abi_version;
    std::uint32_t struct_size;
    const char* name_utf8;
    RuntimeAgentSnippetRunV1 run;

    // Optional: undo whatever run() installed. It receives a host the same way
    // run() does and reports through complete_json/fail, so a failed release is
    // an error a program can act on rather than a payload nobody checked.
    //
    // A null release means this module declares it has nothing to undo. That is
    // a real answer, not a placeholder: a one-shot probe genuinely installs
    // nothing. It does not mean the module is safe — nothing can distinguish
    // "nothing to release" from "the author forgot", so the defences at install
    // time still matter.
    //
    // Release must not report completion before its effects are applied. A
    // caller sequencing a handover — release the old generation, then install
    // the new one — relies on completion meaning done; a release that defers
    // its real work and completes early hands the next install the state it was
    // supposed to have cleaned up.
    //
    // Release should be safe to call when nothing is installed, and leaves the
    // module resident and runnable again afterwards.
    RuntimeAgentSnippetRunV1 release;
};

// Descriptors compiled against the older struct end after run(). The host must
// check struct_size before reading release, and treats anything smaller than
// this as declaring no release.
inline constexpr std::uint32_t RUNTIME_AGENT_SNIPPET_V1_WITH_RELEASE =
    static_cast<std::uint32_t>(offsetof(RuntimeAgentSnippetV1, release)
                               + sizeof(RuntimeAgentSnippetRunV1));

using RuntimeAgentSnippetInitV1 = const RuntimeAgentSnippetV1* (*)();

// Every runtime-compiled snippet exports this exact symbol.
RUNTIME_AGENT_EXPORT const RuntimeAgentSnippetV1* runtime_agent_snippet_init_v1();

} // extern "C"
