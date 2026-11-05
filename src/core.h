#pragma once

#include <cstddef>
#include <cstdint>

namespace Emjs {

inline constexpr const char kVersion[] = "1.1.0";

using JsValue = std::uint64_t;
using JsOffset = std::uint32_t;

enum class JsValueType : int { Undefined = 0, Null, True, False, String, Number, Error, Private };

// Root slots keep JS values that live in C++ locals across nested statement
// execution visible to the moving GC. Pointers to those locals are registered
// so the collector can both trace and relocate them.
#ifndef JS_MAX_ROOTS
#define JS_MAX_ROOTS 16
#endif
inline constexpr int kJsMaxRoots = JS_MAX_ROOTS;

// Small identifier intern table. Every binding and parameter name gets a key
// string; without interning the same name is re-allocated on every function
// call. Slots hold string-entity offsets and are treated as GC roots, so the
// table is bounded by kJsInternSlots strings. Must be a power of two.
#ifndef JS_INTERN_SLOTS
#define JS_INTERN_SLOTS 32
#endif
inline constexpr int kJsInternSlots = JS_INTERN_SLOTS;

// Direct-mapped cache of string literals / function bodies keyed by source
// position, so re-executing the same literal or closure does not re-allocate
// (or re-parse) it. Invalidated on GC and at the outermost eval. Power of two.
#ifndef JS_CODE_CACHE_SLOTS
#define JS_CODE_CACHE_SLOTS 8
#endif
inline constexpr int kJsCodeCacheSlots = JS_CODE_CACHE_SLOTS;

// Cached source-scan decisions (does a block declare anything? does a loop
// body contain a closure?) keyed by source position. Power of two.
#ifndef JS_DECISION_CACHE_SLOTS
#define JS_DECISION_CACHE_SLOTS 16
#endif
inline constexpr int kJsDecisionCacheSlots = JS_DECISION_CACHE_SLOTS;

// Elide scope objects for blocks that declare nothing and for `for(let ...)`
// loops with no closure capture (P1 frame-reuse optimization).
#ifndef JS_SCOPE_ELISION
#define JS_SCOPE_ELISION 1
#endif

// Bind function parameters into fixed slots of a call frame instead of
// allocating a key string + property per parameter (P1 slotization).
#ifndef JS_PARAM_SLOTS
#define JS_PARAM_SLOTS 1
#endif

// Compile straight-line function bodies (no control flow) to a small bytecode
// and cache it per function; unsupported bodies fall back to the interpreter.
#ifndef JS_BYTECODE
#define JS_BYTECODE 1
#endif

// Per-function slot metadata cache, keyed by code-string entity offset.
#ifndef JS_META_CACHE_SLOTS
#define JS_META_CACHE_SLOTS 32
#endif
inline constexpr int kJsMetaCacheSlots = JS_META_CACHE_SLOTS;

struct JsDecisionCacheEntry {
    const char* code;
    JsOffset pos;
    std::uint8_t kind;
    std::uint8_t value;
};

struct JsMetaCacheEntry {
    JsOffset code;
    JsOffset meta;
    JsOffset bc;  // 0 = not compiled, ~0U = not compilable, else bytecode entity
};

struct JsEngineState {
    JsOffset cStackSize;
    JsOffset lowWatermark;
    const char* code;
    char errMsg[128];
    std::uint8_t token;
    std::uint8_t consumed;
    std::uint8_t flags;
    JsOffset codeLen;
    JsOffset position;
    JsOffset tokenOffset;
    JsOffset tokenLen;
    JsOffset noGc;
    JsValue tokenValue;
    JsValue scope;
    JsValue callThis;
    std::uint8_t* memory;
    JsOffset memSize;
    JsOffset break_;
    JsOffset gcThreshold;
    JsOffset maxCss;
    void* cStackPtr;
    JsValue* roots[kJsMaxRoots];
    std::uint8_t rootCount;
    std::uint8_t rootOverflow;  // GC suppressed while roots cannot be tracked
    JsOffset intern[kJsInternSlots];
    const char* codeCachePtr[kJsCodeCacheSlots];
    JsOffset codeCacheStr[kJsCodeCacheSlots];
    JsOffset codeCachePos[kJsCodeCacheSlots];
    JsOffset codeCacheEnd[kJsCodeCacheSlots];
    JsDecisionCacheEntry decisions[kJsDecisionCacheSlots];
    JsMetaCacheEntry metaCache[kJsMetaCacheSlots];
    bool ownsBuffer;
    std::size_t bufferSize;
};

}  // namespace Emjs
