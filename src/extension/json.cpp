#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../internal.h"

namespace Emjs {
namespace {

using namespace internal;

constexpr JsOffset kPropLinkMask = 0x3FFFFFFCU;

JsEngineState* state(JsEngine* js)
{
    return reinterpret_cast<JsEngineState*>(js);
}

JsOffset lkp(JsEngineState* st, JsValue obj, const char* key)
{
    JsOffset head = loadoff(st, (JsOffset)vdata(obj)) & kPropLinkMask;
    const size_t len = strlen(key);
    for (JsOffset off = head; off != 0 && off < st->break_;) {
        JsOffset koff = loadoff(st, (JsOffset)(off + sizeof(JsOffset)));
        JsOffset klen = (loadoff(st, koff) >> 2) - 1;
        const char* p = (char*)&st->memory[koff + sizeof(koff)];
        if (streq(key, len, p, klen)) return off;
        off = loadoff(st, off) & kPropLinkMask;
    }
    return 0;
}

JsValue prop_val(JsEngineState* st, JsOffset prop_off)
{
    return loadval(st, (JsOffset)(prop_off + sizeof(JsOffset) * 2));
}

JsValue set_key(JsEngineState* st, JsValue obj, const char* key, JsValue val)
{
    return setprop(st, obj, mkstr(st, key, strlen(key)), val);
}

bool is_array(JsEngineState* st, JsValue obj)
{
    if (vtype(obj) != T_OBJ) return false;
    JsOffset off = lkp(st, obj, "length");
    return off != 0 && vtype(prop_val(st, off)) == T_NUM;
}

double array_len(JsEngineState* st, JsValue arr)
{
    JsOffset off = lkp(st, arr, "length");
    return off == 0 ? 0.0 : tod(prop_val(st, off));
}

JsValue array_get(JsEngineState* st, JsValue arr, double idx)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.17g", idx);
    JsOffset off = lkp(st, arr, buf);
    return off == 0 ? mkundef() : prop_val(st, off);
}

JsValue array_push(JsEngineState* st, JsValue arr, JsValue item)
{
    double len = array_len(st, arr);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.17g", len);
    JsValue r = setprop(st, arr, mkstr(st, buf, strlen(buf)), item);
    if (is_err(r)) return r;
    return set_key(st, arr, "length", tov(len + 1.0));
}

void append_utf8(std::string& out, uint32_t cp)
{
    if (cp <= 0x7FU) {
        out += static_cast<char>(cp);
    } else if (cp <= 0x7FFU) {
        out += static_cast<char>(0xC0U | (cp >> 6));
        out += static_cast<char>(0x80U | (cp & 0x3FU));
    } else if (cp <= 0xFFFFU) {
        out += static_cast<char>(0xE0U | (cp >> 12));
        out += static_cast<char>(0x80U | ((cp >> 6) & 0x3FU));
        out += static_cast<char>(0x80U | (cp & 0x3FU));
    } else {
        out += static_cast<char>(0xF0U | (cp >> 18));
        out += static_cast<char>(0x80U | ((cp >> 12) & 0x3FU));
        out += static_cast<char>(0x80U | ((cp >> 6) & 0x3FU));
        out += static_cast<char>(0x80U | (cp & 0x3FU));
    }
}

struct Parser {
    JsEngine* js = nullptr;
    JsEngineState* st = nullptr;
    const char* src = nullptr;
    size_t len = 0;
    size_t pos = 0;

    JsValue err(const char* msg) { return js->makeError("%s", msg); }

    char peek() const { return pos < len ? src[pos] : '\0'; }

    char get()
    {
        if (pos >= len) return '\0';
        return src[pos++];
    }

    void skip_ws()
    {
        while (pos < len) {
            char c = src[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                pos++;
            } else {
                break;
            }
        }
    }

    bool match(const char* s)
    {
        size_t i = 0;
        for (; s[i] != '\0'; i++) {
            if (pos + i >= len || src[pos + i] != s[i]) return false;
        }
        pos += i;
        return true;
    }

    JsValue parse_string()
    {
        if (get() != '"') return err("JSON parse error");
        std::string out;
        for (;;) {
            char c = get();
            if (c == '\0') return err("JSON parse error");
            if (c == '"') return mkstr(st, out.data(), out.size());
            if (c == '\\') {
                c = get();
                if (c == '\0') return err("JSON parse error");
                switch (c) {
                    case '"':
                    case '\\':
                    case '/':
                        out += c;
                        break;
                    case 'b':
                        out += '\b';
                        break;
                    case 'f':
                        out += '\f';
                        break;
                    case 'n':
                        out += '\n';
                        break;
                    case 'r':
                        out += '\r';
                        break;
                    case 't':
                        out += '\t';
                        break;
                    case 'u': {
                        uint32_t cp = 0;
                        for (int i = 0; i < 4; i++) {
                            char h = get();
                            if (h == '\0') return err("JSON parse error");
                            cp <<= 4;
                            if (h >= '0' && h <= '9')
                                cp |= (uint32_t)(h - '0');
                            else if (h >= 'a' && h <= 'f')
                                cp |= (uint32_t)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F')
                                cp |= (uint32_t)(h - 'A' + 10);
                            else
                                return err("JSON parse error");
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default:
                        return err("JSON parse error");
                }
            } else {
                out += c;
            }
        }
        return err("JSON parse error");
    }

    JsValue parse_number()
    {
        size_t start = pos;
        if (peek() == '-') get();
        if (peek() == '0') {
            get();
        } else {
            if (peek() < '1' || peek() > '9') return err("JSON parse error");
            while (peek() >= '0' && peek() <= '9') get();
        }
        if (peek() == '.') {
            get();
            if (peek() < '0' || peek() > '9') return err("JSON parse error");
            while (peek() >= '0' && peek() <= '9') get();
        }
        if (peek() == 'e' || peek() == 'E') {
            get();
            if (peek() == '+' || peek() == '-') get();
            if (peek() < '0' || peek() > '9') return err("JSON parse error");
            while (peek() >= '0' && peek() <= '9') get();
        }
        char* end = nullptr;
        double v = strtod(src + start, &end);
        if (end != src + pos) return err("JSON parse error");
        return tov(v);
    }

    JsValue parse_array()
    {
        if (get() != '[') return err("JSON parse error");
        JsValue arr = mkobj(st, 0);
        if (is_err(arr)) return arr;
        JsValue lr = set_key(st, arr, "length", tov(0));
        if (is_err(lr)) return lr;
        skip_ws();
        if (peek() == ']') {
            get();
            return arr;
        }
        for (;;) {
            skip_ws();
            JsValue item = parse_value();
            if (is_err(item)) return item;
            JsValue pr = array_push(st, arr, item);
            if (is_err(pr)) return pr;
            skip_ws();
            if (peek() == ']') {
                get();
                return arr;
            }
            if (get() != ',') return err("JSON parse error");
        }
    }

    JsValue parse_object()
    {
        if (get() != '{') return err("JSON parse error");
        JsValue obj = mkobj(st, 0);
        if (is_err(obj)) return obj;
        skip_ws();
        if (peek() == '}') {
            get();
            return obj;
        }
        for (;;) {
            skip_ws();
            if (peek() != '"') return err("JSON parse error");
            JsValue keyv = parse_string();
            if (is_err(keyv)) return keyv;
            skip_ws();
            if (get() != ':') return err("JSON parse error");
            skip_ws();
            JsValue val = parse_value();
            if (is_err(val)) return val;
            JsValue sr = setprop(st, obj, keyv, val);
            if (is_err(sr)) return sr;
            skip_ws();
            if (peek() == '}') {
                get();
                return obj;
            }
            if (get() != ',') return err("JSON parse error");
        }
    }

    JsValue parse_value()
    {
        skip_ws();
        char c = peek();
        if (c == '"') return parse_string();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == 't' && match("true")) return mktrue();
        if (c == 'f' && match("false")) return mkfalse();
        if (c == 'n' && match("null")) return mknull();
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        return err("JSON parse error");
    }
};

struct Stringifier {
    JsEngine* js = nullptr;
    JsEngineState* st = nullptr;
    std::string out;

    void append_raw(const char* s, size_t n) { out.append(s, n); }
    void append_cstr(const char* s) { out += s; }

    void escape_string(JsValue v)
    {
        JsOffset slen = 0;
        JsOffset off = vstr(st, v, &slen);
        append_cstr("\"");
        for (JsOffset i = 0; i < slen; i++) {
            unsigned char c = st->memory[off + i];
            switch (c) {
                case '"':
                    append_cstr("\\\"");
                    break;
                case '\\':
                    append_cstr("\\\\");
                    break;
                case '\b':
                    append_cstr("\\b");
                    break;
                case '\f':
                    append_cstr("\\f");
                    break;
                case '\n':
                    append_cstr("\\n");
                    break;
                case '\r':
                    append_cstr("\\r");
                    break;
                case '\t':
                    append_cstr("\\t");
                    break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        append_cstr(buf);
                    } else {
                        out += static_cast<char>(c);
                    }
                    break;
            }
        }
        append_cstr("\"");
    }

    void append_number(JsValue v)
    {
        double dv = tod(v);
        if (std::isnan(dv)) {
            append_cstr("null");
            return;
        }
        if (std::isinf(dv)) {
            append_cstr("null");
            return;
        }
        char buf[64];
        double iv = 0.0;
        const char* fmt = (std::modf(dv, &iv) == 0.0) ? "%.17g" : "%g";
        snprintf(buf, sizeof(buf), fmt, dv);
        append_cstr(buf);
    }

    bool json_omit(JsValue v)
    {
        uint8_t t = vtype(v);
        return t == T_UNDEF || t == T_FUNC || t == T_CFUNC || t == T_CODEREF || t == T_PROP || t == T_ERR;
    }

    JsValue json_atom(JsValue v)
    {
        if (json_omit(v)) return mknull();
        switch (vtype(v)) {
            case T_NULL:
                append_cstr("null");
                break;
            case T_BOOL:
                append_cstr(vdata(v) & 1 ? "true" : "false");
                break;
            case T_NUM:
                append_number(v);
                break;
            case T_STR:
                escape_string(v);
                break;
            case T_OBJ:
                return stringify_value(v);
            default:
                append_cstr("null");
                break;
        }
        return mkundef();
    }

    JsValue stringify_array(JsValue arr)
    {
        append_cstr("[");
        double len = array_len(st, arr);
        bool first = true;
        for (double i = 0; i < len; i++) {
            if (!first) append_cstr(",");
            first = false;
            JsValue item = array_get(st, arr, i);
            if (json_omit(item)) {
                append_cstr("null");
            } else {
                JsValue r = stringify_value(item);
                if (is_err(r)) return r;
            }
        }
        append_cstr("]");
        return mkundef();
    }

    JsValue stringify_object(JsValue obj)
    {
        append_cstr("{");
        bool first = true;
        JsOffset next = loadoff(st, (JsOffset)vdata(obj)) & kPropLinkMask;
        while (next != 0 && next < st->break_) {
            JsOffset koff = loadoff(st, (JsOffset)(next + sizeof(JsOffset)));
            JsOffset klen = (loadoff(st, koff) >> 2) - 1;
            const char* kp = (char*)&st->memory[koff + sizeof(koff)];
            if (streq("length", 6, kp, klen)) {
                next = loadoff(st, next) & kPropLinkMask;
                continue;
            }
            JsValue val = loadval(st, (JsOffset)(next + sizeof(JsOffset) * 2));
            if (json_omit(val)) {
                next = loadoff(st, next) & kPropLinkMask;
                continue;
            }
            if (!first) append_cstr(",");
            first = false;
            escape_string(mkval(T_STR, koff));
            append_cstr(":");
            JsValue r = stringify_value(val);
            if (is_err(r)) return r;
            next = loadoff(st, next) & kPropLinkMask;
        }
        append_cstr("}");
        return mkundef();
    }

    JsValue stringify_value(JsValue v)
    {
        v = resolveprop(st, v);
        if (is_err(v)) return v;
        if (vtype(v) == T_OBJ) {
            if (is_array(st, v)) return stringify_array(v);
            return stringify_object(v);
        }
        return json_atom(v);
    }
};

}  // namespace

void EJson::bind(JsEngine* js)
{
    JsValue obj = js->makeObject();
    js->set(obj, "parse", JsEngine::makeFunction(EJson::parse));
    js->set(obj, "stringify", JsEngine::makeFunction(EJson::stringify));
    js->set(js->glob(), "JSON", obj);
}

JsValue EJson::parse(JsEngine* js, JsValue* args, int nargs)
{
    if (!JsEngine::chkArgs(args, nargs, "s")) return js->makeError("type mismatch");
    size_t len = 0;
    char* text = js->getString(args[0], &len);
    if (text == nullptr) return js->makeError("type mismatch");

    Parser p;
    p.js = js;
    p.st = state(js);
    p.src = text;
    p.len = len;
    p.pos = 0;
    JsValue v = p.parse_value();
    if (is_err(v)) return v;
    p.skip_ws();
    if (p.pos != p.len) return js->makeError("JSON parse error");
    return v;
}

JsValue EJson::stringify(JsEngine* js, JsValue* args, int nargs)
{
    if (nargs < 1) return js->makeError("type mismatch");
    if (!JsEngine::chkArgs(args, 1, "j")) return js->makeError("type mismatch");

    Stringifier s;
    s.js = js;
    s.st = state(js);
    JsValue v = resolveprop(s.st, args[0]);
    if (is_err(v)) return v;

    if (s.json_omit(v)) return js->makeUndefined();

    JsValue r = s.stringify_value(v);
    if (is_err(r)) return r;
    return js->makeString(s.out.data(), s.out.size());
}

}  // namespace Emjs
