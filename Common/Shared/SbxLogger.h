#pragma once
#include <stdint.h>

namespace vbs { namespace log {
/** Hash the basename only: build roots never affect event identity.
 * FNV-1a is an identifier, not a security primitive; collisions are possible.
 */
constexpr uint32_t FileId(const char* path) noexcept {
    const char* base = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    uint32_t hash = 2166136261u;
    for (; *base; ++base) hash = (hash ^ static_cast<unsigned char>(*base)) * 16777619u;
    return hash;
}
using Sink = void (*)(uint32_t event, uint32_t line);
/** Sink must be nonthrowing, resident, allocation-free and bounded in root mode.
 * No global sink mutation or hidden locking. Assertion failure does not abort:
 * caller must observe the returned bool and take its defined failure path.
 */
inline void Emit(Sink sink, uint32_t event, uint32_t line) noexcept {
    if (sink) sink(event, line);
}
inline bool Assert(bool condition, Sink sink, uint32_t event, uint32_t line) noexcept {
    if (!condition) Emit(sink, event, line);
    return condition;
}
} }
// Enum initializer REQUIRES constant evaluation; never pass __FILE__ at runtime.
#define SBX_LOG(sink) do { \
    enum : uint32_t { sbx_file_event = ::vbs::log::FileId(__FILE__) }; \
    ::vbs::log::Emit((sink), sbx_file_event, static_cast<uint32_t>(__LINE__)); \
} while (0)
#define SBX_ASSERT(condition, sink) ([&]() noexcept -> bool { \
    enum : uint32_t { sbx_file_event = ::vbs::log::FileId(__FILE__) }; \
    return ::vbs::log::Assert(static_cast<bool>(condition), (sink), \
                            sbx_file_event, static_cast<uint32_t>(__LINE__)); \
}())
