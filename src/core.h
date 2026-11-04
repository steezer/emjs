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
inline constexpr int kJsMaxRoots = 64;

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
    bool ownsBuffer;
    std::size_t bufferSize;
};

}  // namespace Emjs
