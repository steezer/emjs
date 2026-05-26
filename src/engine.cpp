#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "engine.h"
#include "internal.h"

#ifndef JS_GC_THRESHOLD
#define JS_GC_THRESHOLD 0.25
#endif

#ifndef EMJS_ENABLE_DUMP
#define EMJS_ENABLE_DUMP 0
#endif

namespace Emjs {
using namespace internal;

static constexpr std::uint8_t kTokEOF = 1;

static JsValue lookupGlobalCallable(JsEngineState* st, const char* functionName)
{
    if (functionName == nullptr || functionName[0] == '\0') return mkerr(st, "bad call");
    JsValue global = mkval(T_OBJ, 0);
    JsOffset off = prop_lookup(st, global, functionName, std::strlen(functionName));
    if (off == 0) return mkerr(st, "'%s' not found", functionName);
    JsValue fn = resolveprop(st, loadval(st, (JsOffset)(off + sizeof(JsOffset) * 2)));
    if (is_err(fn)) return fn;
    uint8_t t = vtype(fn);
    if (t != T_FUNC && t != T_CFUNC) return mkerr(st, "'%s' not callable", functionName);
    return fn;
}

static JsEngine* createState(void* buffer, size_t length, bool ownsBuffer)
{
    if (length < sizeof(JsEngine) + esize(T_OBJ)) return nullptr;
    std::memset(buffer, 0, length);
    auto* st = new (buffer) JsEngine();
    st->memory = reinterpret_cast<uint8_t*>(st + 1);
    st->memSize = static_cast<JsOffset>(length - sizeof(JsEngine));
    st->scope = mkobj(st, 0);
    st->memSize = st->memSize / 8U * 8U;
    st->lowWatermark = st->memSize;
    st->gcThreshold = (JsOffset)(static_cast<double>(st->memSize) * JS_GC_THRESHOLD);
    st->ownsBuffer = ownsBuffer;
    st->bufferSize = length;
    return st;
}

static void js_dump(JsEngineState* st)
{
#if EMJS_ENABLE_DUMP
    JsOffset off = 0, v;
    printf("JS size %u, brk %u, lwm %u, css %u, nogc %u\n", st->memSize, st->break_,
           st->lowWatermark, (unsigned)st->cStackSize, st->noGc);
    while (off < st->break_) {
        memcpy(&v, &st->memory[off], sizeof(v));
        printf(" %5u: ", off);
        if ((v & 3U) == T_OBJ) {
            printf("OBJ %u %u\n", v & ~3U, loadoff(st, (JsOffset)(off + sizeof(off))));
        } else if ((v & 3U) == T_PROP) {
            JsOffset koff = loadoff(st, (JsOffset)(off + sizeof(v)));
            JsValue val = loadval(st, (JsOffset)(off + sizeof(v) + sizeof(v)));
            printf("PROP next %u, koff %u vtype %d vdata %lu\n", v & ~3U, koff, vtype(val),
                   (unsigned long)vdata(val));
        } else if ((v & 3) == T_STR) {
            JsOffset len = offtolen(v);
            printf("STR %u [%.*s]\n", len, (int)len, st->memory + off + sizeof(v));
        } else {
            printf("???\n");
            break;
        }
        off += esize(v);
    }
#else
    (void)st;
#endif
}

JsValue JsEngine::makeError(const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    JsEngineState* st = reinterpret_cast<JsEngineState*>(this);
    fmt_err(st, format, ap);
    va_end(ap);
    st->position = st->codeLen;
    st->token = kTokEOF;
    st->consumed = 0;
    return mkval(T_ERR, 0);
}

JsEngine* JsEngine::create(void* buffer, std::size_t length)
{
    return createState(buffer, length, false);
}

JsEngine* JsEngine::create(std::size_t length)
{
    if (length < sizeof(JsEngine) + esize(T_OBJ)) return nullptr;
    void* buffer = std::malloc(length);
    if (buffer == nullptr) return nullptr;
    JsEngine* engine = createState(buffer, length, true);
    if (engine == nullptr) std::free(buffer);
    return engine;
}

JsEngine::~JsEngine()
{
    if (memory != nullptr && memSize > 0) {
        std::memset(memory, 0, memSize);
    }
    break_ = 0;
    scope = mkundef();
    lowWatermark = 0;
    cStackSize = 0;
    gcThreshold = 0;
    memSize = 0;
    memory = nullptr;
    code = nullptr;
    codeLen = 0;
    position = 0;
    tokenOffset = 0;
    tokenLen = 0;
    noGc = 0;
    tokenValue = 0;
    errMsg[0] = '\0';
    token = 0;
    consumed = 0;
    flags = 0;
    maxCss = 0;
    cStackPtr = nullptr;
}

void JsEngine::destroy(JsEngine* engine)
{
    if (engine == nullptr) return;
    const bool owned = engine->ownsBuffer;
    void* const buffer = engine;
    const std::size_t size = engine->bufferSize;
    engine->~JsEngine();
    if (owned) {
        std::memset(buffer, 0, size);
        std::free(buffer);
    }
}

JsValue JsEngine::eval(const char* code, std::size_t length)
{
    return js_eval(this, code, length);
}

JsValue JsEngine::callGlobal(const char* functionName, const JsValue* args, int argCount)
{
    if (argCount < 0) return makeError("bad call");
    if (argCount > 0 && args == nullptr) return makeError("bad call");
    auto* st = reinterpret_cast<JsEngineState*>(this);
    JsValue fn = lookupGlobalCallable(st, functionName);
    if (is_err(fn)) return fn;
    return js_invoke(st, fn, mkundef(), const_cast<JsValue*>(args), argCount);
}

JsValue JsEngine::callGlobal(const char* functionName, std::initializer_list<JsValue> args)
{
    return callGlobal(functionName, args.begin(), (int)args.size());
}

JsValue JsEngine::glob() const
{
    return mkval(T_OBJ, 0);
}

const char* JsEngine::str(JsValue value)
{
    return strValue(reinterpret_cast<JsEngineState*>(this), value);
}

bool JsEngine::chkArgs(JsValue* args, int argCount, const char* spec)
{
    int i = 0, ok = 1;
    for (; ok && i < argCount && spec[i]; i++) {
        uint8_t t = vtype(args[i]), c = (uint8_t)spec[i];
        ok = (c == 'b' && t == T_BOOL) || (c == 'd' && t == T_NUM) || (c == 's' && t == T_STR) ||
             (c == 'j');
    }
    if (spec[i] != '\0' || i != argCount) ok = 0;
    return ok != 0;
}

bool JsEngine::isTruthy(JsValue value)
{
    return truthy(reinterpret_cast<JsEngine*>(this), value);
}

void JsEngine::setMaxCss(std::size_t max)
{
    reinterpret_cast<JsEngine*>(this)->maxCss = (JsOffset)max;
}

void JsEngine::setGcThreshold(std::size_t threshold)
{
    reinterpret_cast<JsEngine*>(this)->gcThreshold = (JsOffset)threshold;
}

void JsEngine::gc()
{
    runGc(this);
}

void JsEngine::stats(std::size_t* total, std::size_t* lowWatermark, std::size_t* cStackSize) const
{
    auto* st = const_cast<JsEngine*>(this);
    if (total) *total = st->memSize;
    if (lowWatermark) *lowWatermark = st->lowWatermark;
    if (cStackSize) *cStackSize = st->cStackSize;
}

JsValue JsEngine::makeUndefined()
{
    return mkundef();
}

JsValue JsEngine::makeNull()
{
    return mknull();
}

JsValue JsEngine::makeTrue()
{
    return mktrue();
}

JsValue JsEngine::makeFalse()
{
    return mkfalse();
}

JsValue JsEngine::makeNumber(double value)
{
    return mknum(value);
}

JsValue JsEngine::makeFunction(NativeFunction function)
{
    return mkfun(function);
}

JsValue JsEngine::makeString(const void* data, std::size_t length)
{
    return mkstr(reinterpret_cast<JsEngineState*>(this), data, length);
}

JsValue JsEngine::makeObject()
{
    return mkobj(reinterpret_cast<JsEngineState*>(this), 0);
}

void JsEngine::set(JsValue object, const char* key, JsValue value)
{
    auto* st = reinterpret_cast<JsEngineState*>(this);
    if (vtype(object) == T_OBJ) setprop(st, object, mkstr(st, key, std::strlen(key)), value);
}

JsValueType JsEngine::getType(JsValue value)
{
    switch (vtype(value)) {
        case T_UNDEF:
            return JsValueType::Undefined;
        case T_NULL:
            return JsValueType::Null;
        case T_BOOL:
            return vdata(value) == 0 ? JsValueType::False : JsValueType::True;
        case T_STR:
            return JsValueType::String;
        case T_NUM:
            return JsValueType::Number;
        case T_ERR:
            return JsValueType::Error;
        default:
            return JsValueType::Private;
    }
}

double JsEngine::getNumber(JsValue value)
{
    return tod(value);
}

int JsEngine::getBool(JsValue value)
{
    return (vdata(value) & 1U) ? 1 : 0;
}

char* JsEngine::getString(JsValue value, std::size_t* length)
{
    auto* st = reinterpret_cast<JsEngineState*>(this);
    if (vtype(value) != T_STR) return nullptr;
    JsOffset n = 0, off = vstr(st, value, &n);
    if (length != nullptr) *length = n;
    return (char*)&st->memory[off];
}

void JsEngine::dump() const
{
    js_dump(const_cast<JsEngine*>(this));
}

}  // namespace Emjs
