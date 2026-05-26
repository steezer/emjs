#include "global.h"

#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace Emjs {
namespace {

bool is_space_char(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool value_to_cstr(JsEngine* js, JsValue v, std::string& out)
{
    switch (JsEngine::getType(v)) {
        case JsValueType::String: {
            std::size_t len = 0;
            char* s = js->getString(v, &len);
            if (s == nullptr) return false;
            out.assign(s, len);
            return true;
        }
        case JsValueType::Number: {
            char buf[64];
            double dv = JsEngine::getNumber(v);
            double iv = 0.0;
            const char* fmt = (std::modf(dv, &iv) == 0.0) ? "%.17g" : "%g";
            std::snprintf(buf, sizeof(buf), fmt, dv);
            out = buf;
            return true;
        }
        case JsValueType::True:
            out = "true";
            return true;
        case JsValueType::False:
            out = "false";
            return true;
        default:
            return false;
    }
}

const char* skip_ws(const char* p)
{
    while (*p != '\0' && is_space_char(*p)) p++;
    return p;
}

int digit_value(char c, int radix)
{
    int d = -1;
    if (c >= '0' && c <= '9')
        d = c - '0';
    else if (c >= 'a' && c <= 'z')
        d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z')
        d = c - 'A' + 10;
    return (d >= 0 && d < radix) ? d : -1;
}

}  // namespace

void EGlobal::bind(JsEngine* js)
{
    js->set(js->glob(), "parseInt", JsEngine::makeFunction(EGlobal::parseInt));
    js->set(js->glob(), "parseFloat", JsEngine::makeFunction(EGlobal::parseFloat));
}

JsValue EGlobal::parseFloat(JsEngine* js, JsValue* args, int nargs)
{
    if (nargs < 1) return JsEngine::makeNumber(std::numeric_limits<double>::quiet_NaN());

    std::string text;
    if (!value_to_cstr(js, args[0], text)) {
        return JsEngine::makeNumber(std::numeric_limits<double>::quiet_NaN());
    }

    const char* p = skip_ws(text.c_str());
    char* end = nullptr;
    double v = std::strtod(p, &end);
    if (end == p) return JsEngine::makeNumber(std::numeric_limits<double>::quiet_NaN());
    return JsEngine::makeNumber(v);
}

JsValue EGlobal::parseInt(JsEngine* js, JsValue* args, int nargs)
{
    if (nargs < 1) return JsEngine::makeNumber(std::numeric_limits<double>::quiet_NaN());

    std::string text;
    if (!value_to_cstr(js, args[0], text)) {
        return JsEngine::makeNumber(std::numeric_limits<double>::quiet_NaN());
    }

    const char* p = skip_ws(text.c_str());
    int sign = 1;
    if (*p == '+') {
        p++;
    } else if (*p == '-') {
        sign = -1;
        p++;
    }

    int radix = 10;
    if (nargs >= 2 && JsEngine::getType(args[1]) == JsValueType::Number) {
        double rv = JsEngine::getNumber(args[1]);
        if (std::isnan(rv) || !std::isfinite(rv)) {
            return JsEngine::makeNumber(std::numeric_limits<double>::quiet_NaN());
        }
        radix = static_cast<int>(rv);
    }

    if (radix == 0 || radix == 10) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            p += 2;
            radix = 16;
        } else if (radix == 0) {
            radix = 10;
        }
    } else if (radix == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    if (radix < 2 || radix > 36) {
        return JsEngine::makeNumber(std::numeric_limits<double>::quiet_NaN());
    }

    double val = 0.0;
    bool any = false;
    while (*p != '\0') {
        int d = digit_value(*p, radix);
        if (d < 0) break;
        any = true;
        val = val * static_cast<double>(radix) + static_cast<double>(d);
        p++;
    }

    if (!any) return JsEngine::makeNumber(std::numeric_limits<double>::quiet_NaN());
    return JsEngine::makeNumber(static_cast<double>(sign) * val);
}

}  // namespace Emjs
