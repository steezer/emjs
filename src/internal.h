#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>

#include "core.h"
#include "engine.h"

namespace Emjs {
namespace internal {

enum {
    T_OBJ,  T_PROP, T_STR,     T_UNDEF, T_NULL, T_NUM,  
    T_BOOL, T_FUNC, T_CODEREF, T_CFUNC, T_ERR
};

inline JsValue tov(double d)
{
    union {
        double d;
        JsValue v;
    } u = {d};
    return u.v;
}

inline double tod(JsValue v)
{
    union {
        JsValue v;
        double d;
    } u = {v};
    return u.d;
}

inline JsValue mkval(std::uint8_t type, std::uint64_t data)
{
    return ((JsValue)0x7ff0U << 48U) | ((JsValue)(type) << 48) | (data & 0xffffffffffffUL);
}

inline bool is_nan(JsValue v)
{
    return (v >> 52U) == 0x7ffU;
}

inline std::uint8_t vtype(JsValue v)
{
    return is_nan(v) ? ((v >> 48U) & 15U) : (std::uint8_t)T_NUM;
}

inline std::size_t vdata(JsValue v)
{
    return (std::size_t)(v & ~((JsValue)0x7fffUL << 48U));
}

inline bool is_err(JsValue v)
{
    return vtype(v) == T_ERR;
}

inline JsOffset offtolen(JsOffset off)
{
    return (off >> 2) - 1;
}

inline JsOffset align32(JsOffset v)
{
    return ((v + 3) >> 2) << 2;
}

inline JsOffset esize(JsOffset w)
{
    switch (w & 3U) {
        case T_OBJ:
            return (JsOffset)(sizeof(JsOffset) + sizeof(JsOffset));
        case T_PROP:
            return (JsOffset)(sizeof(JsOffset) + sizeof(JsOffset) + sizeof(JsValue));
        case T_STR:
            return (JsOffset)(sizeof(JsOffset) + align32(w >> 2U));
        default:
            return (JsOffset)~0U;
    }
}

inline JsValue mkundef()
{
    return mkval(T_UNDEF, 0);
}

inline JsValue mknull()
{
    return mkval(T_NULL, 0);
}

inline JsValue mktrue()
{
    return mkval(T_BOOL, 1);
}

inline JsValue mkfalse()
{
    return mkval(T_BOOL, 0);
}

inline JsValue mknum(double value)
{
    return tov(value);
}

inline JsValue mkfun(JsEngine::NativeFunction fn)
{
    return mkval(T_CFUNC, (std::size_t)(void*)fn);
}

JsOffset loadoff(JsEngineState* st, JsOffset off);
JsValue loadval(JsEngineState* st, JsOffset off);
JsValue mkobj(JsEngineState* st, JsOffset parent);
JsValue setprop(JsEngineState* st, JsValue obj, JsValue k, JsValue v, JsOffset pflags = 0);
void fmt_err(JsEngineState* st, const char* fmt, std::va_list ap);
const char* strValue(JsEngineState* st, JsValue value);
bool truthy(JsEngineState* st, JsValue v);
void runGc(JsEngineState* st);
JsValue js_eval(JsEngine* st, const char* buf, std::size_t len);
JsValue mkerr(JsEngineState* st, const char* fmt, ...);
JsValue resolveprop(JsEngineState* st, JsValue v);
double js_to_number(JsEngineState* st, JsValue v);
JsOffset vstr(JsEngineState* st, JsValue value, JsOffset* len);
JsValue mkstr(JsEngineState* st, const void* ptr, std::size_t len);
bool streq(const char* buf, std::size_t len, const char* p, std::size_t n);
void saveval(JsEngineState* st, JsOffset off, JsValue val);
JsOffset prop_lookup(JsEngineState* st, JsValue obj, const char* key, std::size_t key_len);

JsValue array_create(JsEngineState* st);
bool is_array_obj(JsEngineState* st, JsValue obj);
void array_set_length(JsEngineState* st, JsValue arr, double len);
JsValue array_owner_of_prop(JsEngineState* st, JsOffset prop_off);
void array_update_length_for_index(JsEngineState* st, JsValue arr, double idx);
JsValue array_key_from_value(JsEngineState* st, JsValue keyval);
JsValue array_bracket_op(JsEngineState* st, JsValue obj, JsValue keyval);
JsValue js_invoke(JsEngineState* st, JsValue func, JsValue thisArg, JsValue* args, int argc);
bool js_same_value_zero(JsEngineState* st, JsValue a, JsValue b);
JsValue js_to_string(JsEngineState* st, JsValue v);

}  // namespace internal

JsValue string_dot_op(JsEngineState* st, JsValue str, const char* prop, std::size_t prop_len);
JsValue array_dot_op(JsEngineState* st, JsValue arr, const char* prop, std::size_t prop_len);

}  // namespace Emjs
