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

// Bumped whenever either struct below grows or changes shape. The loader
// rejects any module that does not match exactly, so a module built against an
// older layout is refused at load rather than reading fields the host never
// wrote. This is a prototype under active development: there is no
// compatibility path and none is wanted. The alternative, a module silently
// reading past the end of the struct it was given, is worse than a
// refused load and much harder to notice.
inline constexpr std::uint32_t RUNTIME_AGENT_ABI_V1 = 0x0002'0000u;

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

    // The fields of this struct fall into two lifetimes, and the split is the
    // rule to apply to any field added later rather than a fact about these two
    // in particular.
    //
    // Process lifetime: agent_context and every function pointer below. The
    // callbacks are the host's own functions, and agent_context identifies the
    // calling module for as long as it is loaded, which is forever, since
    // modules are deliberately never unloaded.
    //
    // Invocation lifetime: invocation_context, and only that. It belongs to the
    // call in progress and is meaningless once run() or release() returns.
    //
    // A snippet that installs a callback outliving the invocation, such as a
    // draw hook or a menu handler, should keep a by-value copy of this struct
    // with invocation_context set to nullptr, and use that afterwards. Copying
    // the struct is sanctioned precisely because everything except that one
    // field is process-lifetime. Rebuilding a partial struct by hand instead is
    // a mistake worth naming: it leaves callbacks null while struct_size claims
    // they are present, which turns a missing field into a crash inside the
    // host rather than a refused load.
    //
    // Anything invocation-scoped added here in future must be cleared by that
    // same copy step, and the classification above is the rule for deciding
    // which of the two a new field is.
    void* agent_context;
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
    // written later, including a module written after this one misbehaved, can
    // restore it. A module's own private copy is unreachable to everything
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
    // A null release means this module declares it has nothing to undo, and is
    // the only way to declare that: a descriptor built against an older layout
    // is refused at load rather than read as releaseless. It is a real answer,
    // not a placeholder: a one-shot probe genuinely installs nothing.
    //
    // It does not mean the module is safe. Nothing can distinguish "nothing to
    // release" from "the author forgot", so the defences at install time still
    // matter.
    //
    // Release must not report completion before its effects are applied. A
    // caller sequencing a handover, releasing the old generation and then
    // installing the new one, relies on completion meaning done; a release that defers
    // its real work and completes early hands the next install the state it was
    // supposed to have cleaned up.
    //
    // Release should be safe to call when nothing is installed, and leaves the
    // module resident and runnable again afterwards.
    RuntimeAgentSnippetRunV1 release;
};

using RuntimeAgentSnippetInitV1 = const RuntimeAgentSnippetV1* (*)();

// Every runtime-compiled snippet exports this exact symbol.
RUNTIME_AGENT_EXPORT const RuntimeAgentSnippetV1* runtime_agent_snippet_init_v1();

} // extern "C"
