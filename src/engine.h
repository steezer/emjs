#pragma once

#include <initializer_list>

#include "core.h"

namespace Emjs {

class JsEngine : public JsEngineState
{
public:
    using NativeFunction = JsValue (*)(JsEngine*, JsValue*, int);

    // Create in a caller-owned buffer (not freed on destroy).
    static JsEngine* create(void* buffer, std::size_t length);
    // Create with malloc; entire block is freed by destroy().
    static JsEngine* create(std::size_t length);
    static void destroy(JsEngine* engine);

    ~JsEngine();

    JsValue eval(const char* code, std::size_t length = ~0U);
    // Call a global JS function directly (faster than building "fn(...)" via eval).
    JsValue callGlobal(const char* functionName, const JsValue* args, int argCount);
    // Convenience overload for inline argument lists.
    JsValue callGlobal(const char* functionName, std::initializer_list<JsValue> args = {});
    JsValue glob() const;
    const char* str(JsValue value);
    static bool chkArgs(JsValue* args, int argCount, const char* spec);
    bool isTruthy(JsValue value);
    void setMaxCss(std::size_t max);
    void setGcThreshold(std::size_t threshold);
    void gc();
    void stats(std::size_t* total, std::size_t* lowWatermark, std::size_t* cStackSize) const;
    void dump() const;

    static JsValue makeUndefined();
    static JsValue makeNull();
    static JsValue makeTrue();
    static JsValue makeFalse();
    JsValue makeString(const void* data, std::size_t length);
    static JsValue makeNumber(double value);
    JsValue makeError(const char* format, ...);
    static JsValue makeFunction(NativeFunction function);
    JsValue makeObject();

    void set(JsValue object, const char* key, JsValue value);

    static JsValueType getType(JsValue value);
    static double getNumber(JsValue value);
    static int getBool(JsValue value);
    char* getString(JsValue value, std::size_t* length);
};

}  // namespace Emjs
