#include "math.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>

namespace Emjs {
namespace {

double to_num(JsValue v)
{
    return JsEngine::getNumber(v);
}

JsValue unary(JsEngine* js, JsValue* args, int nargs, const char* spec, double (*fn)(double))
{
    if (!JsEngine::chkArgs(args, nargs, spec)) return js->makeError("type mismatch");
    return JsEngine::makeNumber(fn(to_num(args[0])));
}

JsValue binary(JsEngine* js, JsValue* args, int nargs, const char* spec, double (*fn)(double, double))
{
    if (!JsEngine::chkArgs(args, nargs, spec)) return js->makeError("type mismatch");
    return JsEngine::makeNumber(fn(to_num(args[0]), to_num(args[1])));
}

double js_round(double x)
{
    if (std::isnan(x) || std::isinf(x)) return x;
    if (x >= 0.0) return std::floor(x + 0.5);
    return std::ceil(x - 0.5);
}

double js_sign(double x)
{
    if (std::isnan(x)) return std::numeric_limits<double>::quiet_NaN();
    if (x == 0.0) return std::copysign(0.0, x);
    return x > 0.0 ? 1.0 : -1.0;
}

uint32_t to_u32(double x)
{
    if (std::isnan(x) || !std::isfinite(x)) return 0;
    const double kTwo32 = 4294967296.0;
    x = std::fmod(std::fmod(x, kTwo32) + kTwo32, kTwo32);
    return static_cast<uint32_t>(x);
}

int32_t to_i32(double x)
{
    const double kTwo32 = 4294967296.0;
    const double kTwo31 = 2147483648.0;
    if (std::isnan(x) || !std::isfinite(x)) return 0;
    x = std::fmod(std::fmod(x, kTwo32) + kTwo32, kTwo32);
    return x >= kTwo31 ? static_cast<int32_t>(static_cast<int64_t>(x - kTwo32))
                       : static_cast<int32_t>(static_cast<int64_t>(x));
}

uint16_t double_to_float16(double value)
{
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint64_t sign = (bits >> 63) & 1;
    int64_t exp = static_cast<int64_t>((bits >> 52) & 0x7ff);
    uint64_t mant = bits & 0xfffffffffffffULL;

    if (exp == 0x7ff) {
        uint16_t h = static_cast<uint16_t>((sign << 15) | 0x7c00);
        if (mant != 0) h |= 0x0200;
        return h;
    }

    if (exp == 0 && mant == 0) return static_cast<uint16_t>(sign << 15);

    int64_t half_exp = exp - 1023 + 15;
    if (half_exp >= 31) return static_cast<uint16_t>((sign << 15) | 0x7c00);
    if (half_exp <= -10) return static_cast<uint16_t>(sign << 15);

    uint64_t half_mant = mant >> 42;
    uint64_t remainder = mant & ((1ULL << 42) - 1);
    const uint64_t round_bit = 1ULL << 41;

    if (half_exp <= 0) {
        half_mant |= 1ULL << 52;
        int shift = 1 - half_exp;
        remainder = (mant | (1ULL << 52)) & ((1ULL << shift) - 1);
        half_mant >>= shift;
        half_exp = 0;
    } else {
        if (remainder > round_bit || (remainder == round_bit && (half_mant & 1))) {
            half_mant++;
            if (half_mant == 0x400) {
                half_mant = 0;
                half_exp++;
                if (half_exp >= 31) return static_cast<uint16_t>((sign << 15) | 0x7c00);
            }
        }
    }

    return static_cast<uint16_t>((sign << 15) | (static_cast<uint16_t>(half_exp) << 10) |
                                 static_cast<uint16_t>(half_mant & 0x3ff));
}

double float16_to_double(uint16_t h)
{
    const uint16_t sign = (h >> 15) & 1;
    const uint16_t exp = (h >> 10) & 0x1f;
    uint64_t mant = h & 0x3ff;

    uint64_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = static_cast<uint64_t>(sign) << 63;
        } else {
            int e = -14;
            while ((mant & 0x400) == 0) {
                mant <<= 1;
                e--;
            }
            mant &= 0x3ff;
            const int64_t exp64 = e + 1023;
            bits = (static_cast<uint64_t>(sign) << 63) | (static_cast<uint64_t>(exp64) << 52) |
                   (mant << 42);
        }
    } else if (exp == 0x1f) {
        bits = (static_cast<uint64_t>(sign) << 63) | (0x7ffULL << 52) | (mant << 42);
    } else {
        const int64_t exp64 = static_cast<int64_t>(exp) - 15 + 1023;
        bits = (static_cast<uint64_t>(sign) << 63) | (static_cast<uint64_t>(exp64) << 52) |
               (mant << 42);
    }

    double out = 0;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

double math_f16round(double x)
{
    if (std::isnan(x)) return x;
    if (!std::isfinite(x)) return x;
    return float16_to_double(double_to_float16(x));
}

std::mt19937_64& rng()
{
    static std::mt19937_64 gen{std::random_device{}()};
    return gen;
}

#define MATH_UNARY(name, impl) \
    JsValue name(JsEngine* js, JsValue* args, int nargs) { return unary(js, args, nargs, "d", impl); }

#define MATH_BINARY(name, impl) \
    JsValue name(JsEngine* js, JsValue* args, int nargs) { return binary(js, args, nargs, "dd", impl); }

MATH_UNARY(abs, std::fabs)
MATH_UNARY(acos, std::acos)
MATH_UNARY(acosh, std::acosh)
MATH_UNARY(asin, std::asin)
MATH_UNARY(asinh, std::asinh)
MATH_UNARY(atan, std::atan)
MATH_BINARY(atan2, std::atan2)
MATH_UNARY(atanh, std::atanh)
MATH_UNARY(cbrt, std::cbrt)
MATH_UNARY(ceil, std::ceil)
MATH_UNARY(cos, std::cos)
MATH_UNARY(cosh, std::cosh)
MATH_UNARY(exp, std::exp)
MATH_UNARY(expm1, std::expm1)
MATH_UNARY(floor, std::floor)
MATH_UNARY(log, std::log)
MATH_UNARY(log10, std::log10)
MATH_UNARY(log1p, std::log1p)
MATH_UNARY(log2, std::log2)
MATH_BINARY(pow, std::pow)
MATH_UNARY(round, js_round)
MATH_UNARY(sin, std::sin)
MATH_UNARY(sinh, std::sinh)
MATH_UNARY(sqrt, std::sqrt)
MATH_UNARY(tan, std::tan)
MATH_UNARY(tanh, std::tanh)
MATH_UNARY(trunc, std::trunc)

JsValue sign(JsEngine* js, JsValue* args, int nargs)
{
    return unary(js, args, nargs, "d", js_sign);
}

JsValue fround(JsEngine* js, JsValue* args, int nargs)
{
    if (!JsEngine::chkArgs(args, nargs, "d")) return js->makeError("type mismatch");
    float f = static_cast<float>(to_num(args[0]));
    return JsEngine::makeNumber(static_cast<double>(f));
}

JsValue f16round(JsEngine* js, JsValue* args, int nargs)
{
    return unary(js, args, nargs, "d", math_f16round);
}

JsValue clz32(JsEngine* js, JsValue* args, int nargs)
{
    if (!JsEngine::chkArgs(args, nargs, "d")) return js->makeError("type mismatch");
    const uint32_t x = to_u32(to_num(args[0]));
    if (x == 0) return JsEngine::makeNumber(32);
#if defined(__GNUC__) || defined(__clang__)
    return JsEngine::makeNumber(static_cast<double>(__builtin_clz(x)));
#else
    int n = 0;
    uint32_t v = x;
    if ((v & 0xffff0000U) == 0) {
        n += 16;
        v <<= 16;
    }
    if ((v & 0xff000000U) == 0) {
        n += 8;
        v <<= 8;
    }
    if ((v & 0xf0000000U) == 0) {
        n += 4;
        v <<= 4;
    }
    if ((v & 0xc0000000U) == 0) {
        n += 2;
        v <<= 2;
    }
    if ((v & 0x80000000U) == 0) n += 1;
    return JsEngine::makeNumber(static_cast<double>(n));
#endif
}

JsValue imul(JsEngine* js, JsValue* args, int nargs)
{
    if (!JsEngine::chkArgs(args, nargs, "dd")) return js->makeError("type mismatch");
    const int32_t a = to_i32(to_num(args[0]));
    const int32_t b = to_i32(to_num(args[1]));
    return JsEngine::makeNumber(static_cast<double>(static_cast<int32_t>(a * b)));
}

JsValue hypot(JsEngine* js, JsValue* args, int nargs)
{
    if (nargs < 2) return js->makeError("type mismatch");
    double acc = 0.0;
    for (int i = 0; i < nargs; i++) {
        if (JsEngine::getType(args[i]) != JsValueType::Number) return js->makeError("type mismatch");
        const double v = to_num(args[i]);
        acc = std::hypot(acc, v);
    }
    return JsEngine::makeNumber(acc);
}

JsValue random(JsEngine* js, JsValue* args, int nargs)
{
    (void)js;
    (void)args;
    if (nargs != 0) return js->makeError("type mismatch");
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return JsEngine::makeNumber(dist(rng()));
}

JsValue max(JsEngine* js, JsValue* args, int nargs)
{
    if (nargs == 0) return JsEngine::makeNumber(-std::numeric_limits<double>::infinity());
    double m = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < nargs; i++) {
        if (JsEngine::getType(args[i]) != JsValueType::Number) return js->makeError("type mismatch");
        const double v = to_num(args[i]);
        if (std::isnan(v)) return JsEngine::makeNumber(v);
        if (v > m || std::isnan(m)) m = v;
    }
    return JsEngine::makeNumber(m);
}

JsValue min(JsEngine* js, JsValue* args, int nargs)
{
    if (nargs == 0) return JsEngine::makeNumber(std::numeric_limits<double>::infinity());
    double m = std::numeric_limits<double>::infinity();
    for (int i = 0; i < nargs; i++) {
        if (JsEngine::getType(args[i]) != JsValueType::Number) return js->makeError("type mismatch");
        const double v = to_num(args[i]);
        if (std::isnan(v)) return JsEngine::makeNumber(v);
        if (v < m || std::isnan(m)) m = v;
    }
    return JsEngine::makeNumber(m);
}

JsValue sumPrecise(JsEngine* js, JsValue* args, int nargs)
{
    double sum = 0.0;
    double c = 0.0;
    for (int i = 0; i < nargs; i++) {
        if (JsEngine::getType(args[i]) != JsValueType::Number) return js->makeError("type mismatch");
        const double v = to_num(args[i]);
        const double y = v - c;
        const double t = sum + y;
        c = (t - sum) - y;
        sum = t;
    }
    return JsEngine::makeNumber(sum);
}

void bind_fn(JsEngine* js, JsValue obj, const char* name, JsEngine::NativeFunction fn)
{
    js->set(obj, name, JsEngine::makeFunction(fn));
}

void bind_const(JsEngine* js, JsValue obj, const char* name, double value)
{
    js->set(obj, name, JsEngine::makeNumber(value));
}

}  // namespace

void EMath::bind(JsEngine* js)
{
    JsValue obj = js->makeObject();

    bind_const(js, obj, "E", M_E);
    bind_const(js, obj, "LN10", M_LN10);
    bind_const(js, obj, "LN2", M_LN2);
    bind_const(js, obj, "LOG10E", M_LOG10E);
    bind_const(js, obj, "LOG2E", M_LOG2E);
    bind_const(js, obj, "PI", M_PI);
    bind_const(js, obj, "SQRT1_2", M_SQRT1_2);
    bind_const(js, obj, "SQRT2", M_SQRT2);

    bind_fn(js, obj, "abs", abs);
    bind_fn(js, obj, "acos", acos);
    bind_fn(js, obj, "acosh", acosh);
    bind_fn(js, obj, "asin", asin);
    bind_fn(js, obj, "asinh", asinh);
    bind_fn(js, obj, "atan", atan);
    bind_fn(js, obj, "atan2", atan2);
    bind_fn(js, obj, "atanh", atanh);
    bind_fn(js, obj, "cbrt", cbrt);
    bind_fn(js, obj, "ceil", ceil);
    bind_fn(js, obj, "clz32", clz32);
    bind_fn(js, obj, "cos", cos);
    bind_fn(js, obj, "cosh", cosh);
    bind_fn(js, obj, "exp", exp);
    bind_fn(js, obj, "expm1", expm1);
    bind_fn(js, obj, "f16round", f16round);
    bind_fn(js, obj, "floor", floor);
    bind_fn(js, obj, "fround", fround);
    bind_fn(js, obj, "hypot", hypot);
    bind_fn(js, obj, "imul", imul);
    bind_fn(js, obj, "log", log);
    bind_fn(js, obj, "log10", log10);
    bind_fn(js, obj, "log1p", log1p);
    bind_fn(js, obj, "log2", log2);
    bind_fn(js, obj, "max", max);
    bind_fn(js, obj, "min", min);
    bind_fn(js, obj, "pow", pow);
    bind_fn(js, obj, "random", random);
    bind_fn(js, obj, "round", round);
    bind_fn(js, obj, "sign", sign);
    bind_fn(js, obj, "sin", sin);
    bind_fn(js, obj, "sinh", sinh);
    bind_fn(js, obj, "sqrt", sqrt);
    bind_fn(js, obj, "sumPrecise", sumPrecise);
    bind_fn(js, obj, "tan", tan);
    bind_fn(js, obj, "tanh", tanh);
    bind_fn(js, obj, "trunc", trunc);

    js->set(js->glob(), "Math", obj);
}

}  // namespace Emjs
