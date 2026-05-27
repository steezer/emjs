#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "core.h"
#include "engine.h"
#include "internal.h"

#ifndef JS_EXPR_MAX
#define JS_EXPR_MAX 20
#endif

#ifndef JS_GC_THRESHOLD
#define JS_GC_THRESHOLD 0.25
#endif

namespace Emjs {

using namespace internal;

using JsOffset = std::uint32_t;

static constexpr std::uint8_t kFlagNoExec = 1U;
static constexpr std::uint8_t kFlagLoop = 2U;
static constexpr std::uint8_t kFlagCall = 4U;
static constexpr std::uint8_t kFlagBreak = 8U;
static constexpr std::uint8_t kFlagReturn = 16U;
static constexpr std::uint8_t kFlagInTry = 32U;
static constexpr std::uint8_t kFlagSwitch = 64U;
static constexpr std::uint8_t kFlagNative = 128U;

static constexpr JsOffset kPropConst = 0x40000000U;  // const binding marker (high bit)
static constexpr JsOffset kPropLinkMask = 0x3FFFFFFCU;  // strip type + const flag

// A JS memory stores diffenent entities: objects, properties, strings
// All entities are packed to the beginning of a buffer.
// The `brk` marks the end of the used memory:
//
//    | entity1 | entity2| .... |entityN|         unused memory        |
//    |---------|--------|------|-------|------------------------------|
//  js.mem                           js.brk                        js.size
//
//  Each entity is 4-byte aligned, therefore 2 LSB bits store entity type.
//  Object:   8 bytes: offset of the first property, offset of the upper obj
//  Property: 8 bytes + val: 4 byte next prop, 4 byte key offs, N byte value
//  String:   4xN bytes: 4 byte len << 2, 4byte-aligned 0-terminated data
//
// If C functions are imported, they use the upper part of memory as stack for
// passing params. Each argument is pushed to the top of the memory as JsValue,
// and js.size is decreased by sizeof(JsValue), i.e. 8 bytes. When function
// returns, js.size is restored back. So js.size is used as a stack pointer.

enum {
    TOK_ERR,
    TOK_EOF,
    TOK_IDENTIFIER,
    TOK_NUMBER,
    TOK_STRING,
    TOK_SEMICOLON,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    // Keyword tokens
    TOK_BREAK = 50,
    TOK_CASE,
    TOK_CATCH,
    TOK_CLASS,
    TOK_CONST,
    TOK_CONTINUE,
    TOK_DEFAULT,
    TOK_DELETE,
    TOK_DO,
    TOK_ELSE,
    TOK_FINALLY,
    TOK_FOR,
    TOK_FUNC,
    TOK_IF,
    TOK_IN,
    TOK_INSTANCEOF,
    TOK_LET,
    TOK_NEW,
    TOK_RETURN,
    TOK_SWITCH,
    TOK_THIS,
    TOK_THROW,
    TOK_TRY,
    TOK_VAR,
    TOK_VOID,
    TOK_WHILE,
    TOK_WITH,
    TOK_YIELD,
    TOK_UNDEF,
    TOK_NULL,
    TOK_TRUE,
    TOK_FALSE,
    // JS Operator tokens
    TOK_DOT = 100,
    TOK_CALL,
    TOK_POSTINC,
    TOK_POSTDEC,
    TOK_NOT,
    TOK_TILDA,  // 100
    TOK_TYPEOF,
    TOK_UPLUS,
    TOK_UMINUS,
    TOK_EXP,
    TOK_MUL,
    TOK_DIV,
    TOK_REM,  // 106
    TOK_PLUS,
    TOK_MINUS,
    TOK_SHL,
    TOK_SHR,
    TOK_ZSHR,
    TOK_LT,
    TOK_LE,
    TOK_GT,  // 113
    TOK_GE,
    TOK_EQ,
    TOK_NE,
    TOK_LOOSE_EQ,
    TOK_LOOSE_NE,
    TOK_AND,
    TOK_XOR,
    TOK_OR,
    TOK_LAND,
    TOK_LOR,  // 121
    TOK_COLON,
    TOK_Q,
    TOK_ASSIGN,
    TOK_PLUS_ASSIGN,
    TOK_MINUS_ASSIGN,
    TOK_MUL_ASSIGN,
    TOK_DIV_ASSIGN,
    TOK_REM_ASSIGN,
    TOK_SHL_ASSIGN,
    TOK_SHR_ASSIGN,
    TOK_ZSHR_ASSIGN,
    TOK_AND_ASSIGN,
    TOK_XOR_ASSIGN,
    TOK_OR_ASSIGN,
    TOK_COMMA,
    TOK_ARROW,
};

static const char* typestr(uint8_t t)
{
    const char* names[] = {"object",  "prop",     "string",  "undefined", "null", "number",
                           "boolean", "function", "coderef", "cfunc",     "err",  "nan"};
    return (t < sizeof(names) / sizeof(names[0])) ? names[t] : "??";
}

// Pack JS values into uin64_t, double nan, per IEEE 754
// 64bit "double": 1 bit sign, 11 bits exponent, 52 bits mantissa
//
// seeeeeee|eeeemmmm|mmmmmmmm|mmmmmmmm|mmmmmmmm|mmmmmmmm|mmmmmmmm|mmmmmmmm
// 11111111|11110000|00000000|00000000|00000000|00000000|00000000|00000000 inf
// 11111111|11111000|00000000|00000000|00000000|00000000|00000000|00000000 qnan
//
// 11111111|1111tttt|vvvvvvvv|vvvvvvvv|vvvvvvvv|vvvvvvvv|vvvvvvvv|vvvvvvvv
//  NaN marker |type|  48-bit placeholder for values: pointers, strings
//
static JsValue mkcoderef(JsValue off, JsOffset len)
{
    return mkval(T_CODEREF, (off & 0xffffffU) | ((JsValue)(len & 0xffffffU) << 24U));
}

static JsOffset coderefoff(JsValue v)
{
    return v & 0xffffffU;
}

static JsOffset codereflen(JsValue v)
{
    return (v >> 24U) & 0xffffffU;
}

static uint8_t unhex(uint8_t c)
{
    return (c >= '0' && c <= '9')   ? (uint8_t)(c - '0')
           : (c >= 'a' && c <= 'f') ? (uint8_t)(c - 'W')
           : (c >= 'A' && c <= 'F') ? (uint8_t)(c - '7')
                                    : 0;
}

static bool is_space(int c)
{
    return c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '\f' || c == '\v';
}

static bool is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static bool is_xdigit(int c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static double js_parse_number_literal(const char* buf, JsOffset len, JsOffset* consumed)
{
    if (len >= 2 && buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) {
        double val = 0;
        JsOffset i = 2;
        while (i < len && is_xdigit((unsigned char)buf[i])) {
            val = val * 16.0 + (double)unhex((uint8_t)buf[i]);
            i++;
        }
        *consumed = i;
        return val;
    }
    char* end = NULL;
    double val = strtod(buf, &end);
    *consumed = (JsOffset)(end - buf);
    return val;
}

static bool is_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_ident_begin(int c)
{
    return c == '_' || c == '$' || is_alpha(c);
}

static bool is_ident_continue(int c)
{
    return c == '_' || c == '$' || is_alpha(c) || is_digit(c);
}

static bool is_unary(uint8_t tok)
{
    return tok >= TOK_POSTINC && tok <= TOK_UMINUS;
}

static bool is_assign(uint8_t tok)
{
    return (tok >= TOK_ASSIGN && tok <= TOK_OR_ASSIGN);
}

static void saveoff(JsEngineState* st, JsOffset off, JsOffset val)
{
    memcpy(&st->memory[off], &val, sizeof(val));
}




static JsOffset vstrlen(JsEngineState* st, JsValue v)
{
    return offtolen(loadoff(st, (JsOffset)vdata(v)));
}



static JsValue upper(JsEngineState* st, JsValue scope)
{
    return mkval(T_OBJ, loadoff(st, (JsOffset)(vdata(scope) + sizeof(JsOffset))));
}

static inline JsOffset func_code_off(JsValue fn)
{
    return (JsOffset)(vdata(fn) & 0xffffffffUL);
}

static inline JsOffset func_scope_off(JsValue fn)
{
    return (JsOffset)(vdata(fn) >> 32);
}

static inline JsValue mkfunc(JsEngineState* st, JsValue str)
{
    JsOffset soff = (JsOffset)vdata(str);
    JsOffset scoff = (JsOffset)vdata(st->scope);
    return mkval(T_FUNC, ((JsValue)scoff << 32) | soff);
}

static JsOffset func_vstr(JsEngineState* st, JsValue fn, JsOffset* len)
{
    return vstr(st, mkval(T_STR, func_code_off(fn)), len);
}

static void js_fixup_func(JsEngineState* st, JsValue* vp, JsOffset start, JsOffset size)
{
    if (vp == NULL || vtype(*vp) != T_FUNC) return;
    JsOffset code = func_code_off(*vp);
    JsOffset scope = func_scope_off(*vp);
    if (code > start) code -= size;
    if (scope > start) scope -= size;
    *vp = mkval(T_FUNC, ((JsValue)scope << 32) | code);
}

// clang-format off
#define CHECKV(_v)        \
    do {                  \
        if (is_err(_v)) { \
            res = (_v);   \
            goto done;    \
        }                 \
    } while (0)
#define EXPECT(_tok, _e)                     \
    do {                                     \
        if (next(st) != _tok) {              \
            _e;                              \
            return mkerr(st, "parse error"); \
        };                                   \
        st->consumed = 1;                    \
    } while (0)
// clang-format on

// Forward declarations of the private functions
static size_t tostr(JsEngineState* st, JsValue value, char* buf, size_t len);
static JsValue js_expr(JsEngineState* st);
static JsValue js_stmt(JsEngineState* st);
static JsValue js_assignment(JsEngineState* st);
static JsValue do_op(JsEngineState* st, uint8_t op, JsValue l, JsValue r);
static bool js_loose_eq(JsEngineState* st, JsValue a, JsValue b);
static void setlwm(JsEngineState* st)
{
    JsOffset n = 0, css = 0;
    if (st->break_ < st->memSize) n = st->memSize - st->break_;
    if (st->lowWatermark > n) st->lowWatermark = n;
    if ((char*)st->cStackPtr > (char*)&n) css = (JsOffset)((char*)st->cStackPtr - (char*)&n);
    if (css > st->cStackSize) st->cStackSize = css;
}

// Copy src to dst, make no overflows, 0-terminate. Return bytes copied
static size_t cpy(char* dst, size_t dstlen, const char* src, size_t srclen)
{
    size_t i = 0;
    for (i = 0; i < dstlen && i < srclen && src[i] != 0; i++) dst[i] = src[i];
    if (dstlen > 0) dst[i < dstlen ? i : dstlen - 1] = '\0';
    return i;
}

// Stringify JS object
static size_t strobj(JsEngineState* st, JsValue obj, char* buf, size_t len)
{
    size_t n = cpy(buf, len, "{", 1);
    JsOffset next = loadoff(st, (JsOffset)vdata(obj)) & kPropLinkMask;  // First prop offset
    while (next < st->break_ && next != 0) {                  // Iterate over props
        JsOffset koff = loadoff(st, next + (JsOffset)sizeof(next));
        JsValue val = loadval(st, next + (JsOffset)(sizeof(next) + sizeof(koff)));
        // printf("PROP %u, koff %u\n", next & ~3, koff);
        n += cpy(buf + n, len - n, ",", n == 1 ? 0 : 1);
        n += tostr(st, mkval(T_STR, koff), buf + n, len - n);
        n += cpy(buf + n, len - n, ":", 1);
        n += tostr(st, val, buf + n, len - n);
        next = loadoff(st, next) & kPropLinkMask;  // Load next prop offset
    }
    return n + cpy(buf + n, len - n, "}", 1);
}

// Stringify numeric JS value
static size_t strnum(JsValue value, char* buf, size_t len)
{
    double dv = tod(value), iv;
    const char* fmt = modf(dv, &iv) == 0.0 ? "%.17g" : "%g";
    return (size_t)snprintf(buf, len, fmt, dv);
}

// Stringify string JS value
static size_t strstring(JsEngineState* st, JsValue value, char* buf, size_t len)
{
    JsOffset slen, off = vstr(st, value, &slen);
    size_t n = 0;
    n += cpy(buf + n, len - n, "\"", 1);
    n += cpy(buf + n, len - n, (char*)&st->memory[off], slen);
    n += cpy(buf + n, len - n, "\"", 1);
    return n;
}

// Stringify JS function
static size_t strfunc(JsEngineState* st, JsValue value, char* buf, size_t len)
{
    JsOffset sn, off = func_vstr(st, value, &sn);
    size_t n = cpy(buf, len, "function", 8);
    return n + cpy(buf + n, len - n, (char*)&st->memory[off], sn);
}

static JsOffset err_offset(JsEngineState* st)
{
    if (st->code == NULL || st->codeLen == 0) return 0;
    if (st->tokenOffset < st->codeLen) return st->tokenOffset;
    if (st->position > 0 && st->position <= st->codeLen) return st->position - 1;
    return st->codeLen > 0 ? st->codeLen - 1 : 0;
}

static void err_line_col(JsEngineState* st, JsOffset off, int* line, int* col)
{
    *line = 1;
    *col = 1;
    if (st->code == NULL || st->codeLen == 0) return;
    if (off >= st->codeLen) off = st->codeLen > 0 ? st->codeLen - 1 : 0;
    for (JsOffset i = 0; i < off; i++) {
        char c = st->code[i];
        if (c == '\n') {
            (*line)++;
            *col = 1;
        } else if (c == '\r') {
            (*line)++;
            *col = 1;
            if (i + 1 < st->codeLen && st->code[i + 1] == '\n') i++;
        } else {
            (*col)++;
        }
    }
}

// Stringify JS value into the given buffer
static size_t tostr(JsEngineState* st, JsValue value, char* buf, size_t len)
{
    switch (vtype(value)) {  // clang-format off
    case T_UNDEF: return cpy(buf, len, "undefined", 9);
    case T_NULL:  return cpy(buf, len, "null", 4);
    case T_BOOL:  return cpy(buf, len, vdata(value) & 1 ? "true" : "false", vdata(value) & 1 ? 4 : 5);
    case T_OBJ:   return strobj(st, value, buf, len);
    case T_STR:   return strstring(st, value, buf, len);
    case T_NUM:   return strnum(value, buf, len);
    case T_FUNC:  return strfunc(st, value, buf, len);
    case T_CFUNC: return (size_t) snprintf(buf, len, "\"c_func_0x%lx\"", (unsigned long) vdata(value));
    case T_PROP:  return (size_t) snprintf(buf, len, "PROP@%lu", (unsigned long) vdata(value));
    default:      return (size_t) snprintf(buf, len, "VTYPE%d", vtype(value));
  }  // clang-format on
}

static JsOffset js_alloc(JsEngineState* st, size_t size)
{
    JsOffset ofs = st->break_;
    size = align32((JsOffset)size);  // 4-byte align, (n + k - 1) / k * k
    if (st->break_ + size > st->memSize) return ~(JsOffset)0;
    st->break_ += (JsOffset)size;
    return ofs;
}

static JsValue mkentity(JsEngineState* st, JsOffset b, const void* buf, size_t len)
{
    JsOffset ofs = js_alloc(st, len + sizeof(b));
    if (ofs == (JsOffset)~0) return mkerr(st, "oom");
    memcpy(&st->memory[ofs], &b, sizeof(b));
    // Using memmove - in case we're stringifying data from the free JS mem
    if (buf != NULL) memmove(&st->memory[ofs + sizeof(b)], buf, len);
    if ((b & 3) == T_STR) st->memory[ofs + sizeof(b) + len - 1] = 0;  // 0-terminate
    // printf("MKE: %u @ %u type %d\n", st->break_ - ofs, ofs, b & 3);
    return mkval(b & 3, ofs);
}


static bool is_mem_entity(uint8_t t)
{
    return t == T_OBJ || t == T_PROP || t == T_STR || t == T_FUNC;
}

#define GCMASK ~(((JsOffset)~0) >> 1)  // Entity deletion marker

static void js_fixup_value(JsEngineState* st, JsValue* vp, JsOffset start, JsOffset size)
{
    if (vp == NULL) return;
    JsValue v = *vp;
    if (vtype(v) == T_FUNC) {
        js_fixup_func(st, vp, start, size);
        return;
    }
    if (is_mem_entity(vtype(v)) && (JsOffset)vdata(v) > start) {
        *vp = mkval(vtype(v), (unsigned long)(vdata(v) - size));
    }
}

static void js_fixup_offsets(JsEngineState* st, JsOffset start, JsOffset size)
{
    for (JsOffset n, v, off = 0; off < st->break_; off += n) {  // start from 0!
        v = loadoff(st, off);
        n = esize(v & ~GCMASK);
        if (v & GCMASK) continue;  // To be deleted, don't bother
        if ((v & 3) != T_OBJ && (v & 3) != T_PROP) continue;
        {
            JsOffset link = v & kPropLinkMask;
            if (link > start) saveoff(st, off, (link - size) | (v & ~kPropLinkMask));
        }
        if ((v & 3) == T_OBJ) {
            JsOffset u = loadoff(st, (JsOffset)(off + sizeof(JsOffset)));
            if (u > start) saveoff(st, (JsOffset)(off + sizeof(JsOffset)), u - size);
        }
        if ((v & 3) == T_PROP) {
            JsOffset koff = loadoff(st, (JsOffset)(off + sizeof(off)));
            if (koff > start) saveoff(st, (JsOffset)(off + sizeof(off)), koff - size);
            JsValue val = loadval(st, (JsOffset)(off + sizeof(off) + sizeof(off)));
            if (vtype(val) == T_FUNC) {
                js_fixup_func(st, &val, start, size);
                saveval(st, (JsOffset)(off + sizeof(off) + sizeof(off)), val);
            } else if (is_mem_entity(vtype(val)) && vdata(val) > start) {
                saveval(st, (JsOffset)(off + sizeof(off) + sizeof(off)),
                        mkval(vtype(val), (unsigned long)(vdata(val) - size)));
            }
        }
    }
    // Fixup st->scope
    JsOffset off = (JsOffset)vdata(st->scope);
    if (off > start) st->scope = mkval(T_OBJ, off - size);
    if (st->noGc >= start) st->noGc -= size;
    js_fixup_value(st, (JsValue*)st->cStackPtr, start, size);
    js_fixup_value(st, &st->callThis, start, size);
    if (st->code > (char*)st->memory && st->code - (char*)st->memory < st->memSize &&
        st->code - (char*)st->memory > start) {
        st->code -= size;
    }
    // printf("FIXEDOFF %u %u\n", start, size);
}

static void js_delete_marked_entities(JsEngineState* st)
{
    for (JsOffset n, v, off = 0; off < st->break_; off += n) {
        v = loadoff(st, off);
        n = esize(v & ~GCMASK);
        if (v & GCMASK) {
            js_fixup_offsets(st, off, n);
            memmove(&st->memory[off], &st->memory[off + n], st->break_ - off - n);
            st->break_ -= n;
            n = 0;
        }
    }
}

static void js_mark_all_entities_for_deletion(JsEngineState* st)
{
    for (JsOffset v, off = 0; off < st->break_; off += esize(v & ~GCMASK)) {
        v = loadoff(st, off);
        saveoff(st, off, v | GCMASK);
    }
}

static JsOffset js_unmark_entity(JsEngineState* st, JsOffset off)
{
    JsOffset v = loadoff(st, off);
    if (v & GCMASK) {
        saveoff(st, off, v & ~GCMASK);
        if ((v & 3) == T_OBJ) {
            JsOffset link = v & kPropLinkMask;
            if (link != 0) js_unmark_entity(st, link);
        }
        if ((v & 3) == T_PROP) {
            JsOffset link = v & kPropLinkMask;
            if (link != 0) js_unmark_entity(st, link);
            js_unmark_entity(st, loadoff(st, (JsOffset)(off + sizeof(off))));
            JsValue val = loadval(st, (JsOffset)(off + sizeof(off) + sizeof(off)));
            if (vtype(val) == T_FUNC) {
                js_unmark_entity(st, func_code_off(val));
                JsOffset sc = func_scope_off(val);
                if (sc != 0) js_unmark_entity(st, sc);
            } else if (is_mem_entity(vtype(val))) {
                js_unmark_entity(st, (JsOffset)vdata(val));
            }
        }
    }
    return v & ~(GCMASK | 3U);
}

static void js_unmark_used_entities(JsEngineState* st)
{
    js_unmark_entity(st, 0);
    for (JsValue scope = st->scope; vdata(scope) != 0;) {
        js_unmark_entity(st, (JsOffset)vdata(scope));
        scope = upper(st, scope);
    }
    if (st->noGc != 0 && st->noGc != (JsOffset)~0) js_unmark_entity(st, st->noGc);
}


// Skip whitespaces and comments
static JsOffset skiptonext(const char* code, JsOffset len, JsOffset n)
{
    // printf("SKIP: [%.*s]\n", len - n, &code[n]);
    while (n < len) {
        if (is_space(code[n])) {
            n++;
        } else if (n + 1 < len && code[n] == '/' && code[n + 1] == '/') {
            for (n += 2; n < len && code[n] != '\n';) n++;
        } else if (n + 3 < len && code[n] == '/' && code[n + 1] == '*') {
            for (n += 4; n < len && (code[n - 2] != '*' || code[n - 1] != '/');) n++;
        } else {
            break;
        }
    }
    return n;
}

static uint8_t parsekeyword(const char* buf, size_t len)
{
    struct KeywordToken {
        const char* text;
        uint8_t len;
        uint8_t token;
    };
    static const KeywordToken keywords[] = {
        {"break", 5, TOK_BREAK},         {"case", 4, TOK_CASE},
        {"catch", 5, TOK_CATCH},         {"class", 5, TOK_CLASS},
        {"const", 5, TOK_CONST},         {"continue", 8, TOK_CONTINUE},
        {"default", 7, TOK_DEFAULT},     {"do", 2, TOK_DO},
        {"else", 4, TOK_ELSE},           {"false", 5, TOK_FALSE},
        {"finally", 7, TOK_FINALLY},     {"for", 3, TOK_FOR},
        {"function", 8, TOK_FUNC},       {"if", 2, TOK_IF},
        {"in", 2, TOK_IN},               {"instanceof", 10, TOK_INSTANCEOF},
        {"let", 3, TOK_LET},             {"new", 3, TOK_NEW},
        {"null", 4, TOK_NULL},           {"return", 6, TOK_RETURN},
        {"switch", 6, TOK_SWITCH},       {"this", 4, TOK_THIS},
        {"throw", 5, TOK_THROW},         {"true", 4, TOK_TRUE},
        {"try", 3, TOK_TRY},             {"typeof", 6, TOK_TYPEOF},
        {"undefined", 9, TOK_UNDEF},     {"var", 3, TOK_VAR},
        {"void", 4, TOK_VOID},           {"while", 5, TOK_WHILE},
        {"with", 4, TOK_WITH},           {"yield", 5, TOK_YIELD},
    };
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        const KeywordToken& kw = keywords[i];
        if (kw.text[0] != buf[0]) continue;
        if (kw.len != len) continue;
        if (streq(kw.text, kw.len, buf, len)) return kw.token;
    }
    return TOK_IDENTIFIER;
}

static uint8_t parseident(const char* buf, JsOffset len, JsOffset* tlen)
{
    if (is_ident_begin(buf[0])) {
        while (*tlen < len && is_ident_continue(buf[*tlen])) {
            (*tlen)++;
        }
        return parsekeyword(buf, *tlen);
    }
    return TOK_ERR;
}

static uint8_t next(JsEngineState* st)
{
    if (st->consumed == 0) {
        return st->token;
    }
    
    st->consumed = 0;
    st->token = TOK_ERR;
    st->tokenOffset = st->position = skiptonext(st->code, st->codeLen, st->position);
    st->tokenLen = 0;
    const char* buf = st->code + st->tokenOffset;
    
    // clang-format off
    if (st->tokenOffset >= st->codeLen) {
        st->token = TOK_EOF; 
        return st->token; 
    }
    
#define TOK(T, LEN) { st->token = T; st->tokenLen = (LEN); break; }
#define LOOK(OFS, CH) st->tokenOffset + OFS < st->codeLen && buf[OFS] == CH

  switch (buf[0]) {
    case '?': TOK(TOK_Q, 1);
    case ':': TOK(TOK_COLON, 1);
    case '(': TOK(TOK_LPAREN, 1);
    case ')': TOK(TOK_RPAREN, 1);
    case '{': TOK(TOK_LBRACE, 1);
    case '}': TOK(TOK_RBRACE, 1);
    case '[': TOK(TOK_LBRACKET, 1);
    case ']': TOK(TOK_RBRACKET, 1);
    case ';': TOK(TOK_SEMICOLON, 1);
    case ',': TOK(TOK_COMMA, 1);
    case '!': if (LOOK(1, '=') && LOOK(2, '=')) TOK(TOK_NE, 3); if (LOOK(1, '=')) TOK(TOK_LOOSE_NE, 2); TOK(TOK_NOT, 1);
    case '.': TOK(TOK_DOT, 1);
    case '~': TOK(TOK_TILDA, 1);
    case '-': if (LOOK(1, '-')) TOK(TOK_POSTDEC, 2); if (LOOK(1, '=')) TOK(TOK_MINUS_ASSIGN, 2); TOK(TOK_MINUS, 1);
    case '+': if (LOOK(1, '+')) TOK(TOK_POSTINC, 2); if (LOOK(1, '=')) TOK(TOK_PLUS_ASSIGN, 2); TOK(TOK_PLUS, 1);
    case '*': if (LOOK(1, '*')) TOK(TOK_EXP, 2); if (LOOK(1, '=')) TOK(TOK_MUL_ASSIGN, 2); TOK(TOK_MUL, 1);
    case '/': if (LOOK(1, '=')) TOK(TOK_DIV_ASSIGN, 2); TOK(TOK_DIV, 1);
    case '%': if (LOOK(1, '=')) TOK(TOK_REM_ASSIGN, 2); TOK(TOK_REM, 1);
    case '&': if (LOOK(1, '&')) TOK(TOK_LAND, 2); if (LOOK(1, '=')) TOK(TOK_AND_ASSIGN, 2); TOK(TOK_AND, 1);
    case '|': if (LOOK(1, '|')) TOK(TOK_LOR, 2); if (LOOK(1, '=')) TOK(TOK_OR_ASSIGN, 2); TOK(TOK_OR, 1);
    case '=': if (LOOK(1, '>')) TOK(TOK_ARROW, 2); if (LOOK(1, '=') && LOOK(2, '=')) TOK(TOK_EQ, 3); if (LOOK(1, '=')) TOK(TOK_LOOSE_EQ, 2); TOK(TOK_ASSIGN, 1);
    case '<': if (LOOK(1, '<') && LOOK(2, '=')) TOK(TOK_SHL_ASSIGN, 3); if (LOOK(1, '<')) TOK(TOK_SHL, 2); if (LOOK(1, '=')) TOK(TOK_LE, 2); TOK(TOK_LT, 1);
    case '>': if (LOOK(1, '>') && LOOK(2, '>')) TOK(TOK_ZSHR, 3); if (LOOK(1, '>') && LOOK(2, '=')) TOK(TOK_SHR_ASSIGN, 3); if (LOOK(1, '>')) TOK(TOK_SHR, 2); if (LOOK(1, '=')) TOK(TOK_GE, 2); TOK(TOK_GT, 1);
    case '^': if (LOOK(1, '=')) TOK(TOK_XOR_ASSIGN, 2); TOK(TOK_XOR, 1);
    case '"': case '\'':
      st->tokenLen++;
      while (st->tokenOffset + st->tokenLen < st->codeLen && buf[st->tokenLen] != buf[0]) {
        uint8_t increment = 1;
        if (buf[st->tokenLen] == '\\') {
          if (st->tokenOffset + st->tokenLen + 2 > st->codeLen) break;
          increment = 2;
          if (buf[st->tokenLen + 1] == 'x') {
            if (st->tokenOffset + st->tokenLen + 4 > st->codeLen) break;
            increment = 4;
          }
        }
        st->tokenLen += increment;
      }
      if (buf[0] == buf[st->tokenLen]) st->token = TOK_STRING, st->tokenLen++;
      break;
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': {
      JsOffset consumed = 0;
      st->tokenValue = tov(js_parse_number_literal(buf, (JsOffset)(st->codeLen - st->tokenOffset), &consumed));
      TOK(TOK_NUMBER, consumed);
    }
    default: st->token = parseident(buf, st->codeLen - st->tokenOffset, &st->tokenLen); break;
  }  // clang-format on
    st->position = st->tokenOffset + st->tokenLen;
    // printf("NEXT: %d %d [%.*s]\n", st->token, st->position, (int) st->tokenLen,
    // buf);
    return st->token;
}

struct ParserTokenState {
    JsOffset position;
    JsOffset tokenOffset;
    JsOffset tokenLen;
    uint8_t token;
    uint8_t consumed;
};

static inline ParserTokenState parser_state_save(const JsEngineState* st)
{
    return ParserTokenState{st->position, st->tokenOffset, st->tokenLen, st->token, st->consumed};
}

static inline void parser_state_restore(JsEngineState* st, const ParserTokenState& snapshot)
{
    st->position = snapshot.position;
    st->tokenOffset = snapshot.tokenOffset;
    st->tokenLen = snapshot.tokenLen;
    st->token = snapshot.token;
    st->consumed = snapshot.consumed;
}

static inline uint8_t lookahead(JsEngineState* st)
{
    ParserTokenState snapshot = parser_state_save(st);
    st->consumed = 1;
    uint8_t tok = next(st);
    parser_state_restore(st, snapshot);
    return tok;
}

static bool linebreak_before_token(JsEngineState* st)
{
    if (st->tokenOffset == 0) return false;
    for (JsOffset i = st->tokenOffset; i > 0;) {
        char c = st->code[--i];
        if (c == '\n' || c == '\r') return true;
        if (!is_space(c)) return false;
    }
    return false;
}

static void mkscope(JsEngineState* st)
{
    assert((st->flags & kFlagNoExec) == 0);
    JsOffset prev = (JsOffset)vdata(st->scope);
    st->scope = mkobj(st, prev);
    // printf("ENTER SCOPE %u, prev %u\n", (JsOffset) vdata(st->scope), prev);
}

static void delscope(JsEngineState* st)
{
    st->scope = upper(st, st->scope);
    // printf("EXIT  SCOPE %u\n", (JsOffset) vdata(st->scope));
}

static JsOffset lkp(JsEngineState* st, JsValue obj, const char* buf, size_t len);

static void scope_copy_props(JsEngineState* st, JsValue src, JsValue dst)
{
    JsOffset off = loadoff(st, (JsOffset)vdata(src)) & kPropLinkMask;
    while (off != 0 && off < st->break_) {
        JsOffset koff = loadoff(st, (JsOffset)(off + sizeof(off)));
        JsOffset klen = (loadoff(st, koff) >> 2) - 1;
        const char* p = (char*)&st->memory[koff + sizeof(koff)];
        JsValue val = resolveprop(st, loadval(st, (JsOffset)(off + sizeof(off) + sizeof(off))));
        JsOffset pflags = loadoff(st, off) & kPropConst;
        setprop(st, dst, mkstr(st, p, klen), val, pflags);
        off = loadoff(st, off) & kPropLinkMask;
    }
}

static JsValue js_block(JsEngineState* st, bool create_scope)
{
    JsValue res = mkundef();
    if (create_scope) mkscope(st);  // Enter new scope
    st->consumed = 1;
    // JsOffset pos = st->position;
    while (next(st) != TOK_EOF && next(st) != TOK_RBRACE && !is_err(res)) {
        uint8_t t = st->token;
        res = js_stmt(st);
        if (is_err(res) && (st->flags & kFlagInTry)) break;
        if (!is_err(res) && t != TOK_LBRACE && t != TOK_IF && t != TOK_WHILE && t != TOK_DO &&
            t != TOK_SWITCH && t != TOK_TRY && t != TOK_FUNC &&
            st->consumed != 0 && st->token != TOK_SEMICOLON) {
            res = mkerr(st, "; expected");
            break;
        }
    }
    // printf("BLOCKEND %s\n", strValue(st, res));
    if (create_scope) delscope(st);  // Exit scope
    return res;
}

// Seach for property in a single object
static JsOffset lkp(JsEngineState* st, JsValue obj, const char* buf, size_t len)
{
    JsOffset off = loadoff(st, (JsOffset)vdata(obj)) & kPropLinkMask;  // Load first prop off
    // printf("LKP: %lu %u [%.*s]\n", vdata(obj), off, (int) len, buf);
    while (off < st->break_ && off != 0) {  // Iterate over props
        JsOffset koff = loadoff(st, (JsOffset)(off + sizeof(off)));
        JsOffset klen = (loadoff(st, koff) >> 2) - 1;
        const char* p = (char*)&st->memory[koff + sizeof(koff)];
        // printf("  %u %u[%.*s]\n", off, (int) klen, (int) klen, p);
        if (streq(buf, len, p, klen)) return off;  // Found !
        off = loadoff(st, off) & kPropLinkMask;              // Load next prop offset
    }
    return 0;  // Not found
}

// Lookup variable in the scope chain
static JsValue lookup(JsEngineState* st, const char* buf, size_t len)
{
    if (st->flags & kFlagNoExec) {
        return 0;
    }
    for (JsValue scope = st->scope;;) {
        JsOffset off = lkp(st, scope, buf, len);
        if (off != 0) return mkval(T_PROP, off);
        if (vdata(scope) == 0) break;
        scope = mkval(T_OBJ, loadoff(st, (JsOffset)(vdata(scope) + sizeof(JsOffset))));
    }
    return mkerr(st, "'%.*s' not found", (int)len, buf);
}

static JsValue assign(JsEngineState* st, JsValue lhs, JsValue val)
{
    if (vtype(lhs) == T_PROP && (loadoff(st, (JsOffset)vdata(lhs)) & kPropConst) != 0)
        return mkerr(st, "assignment to constant");
  saveval(st, (JsOffset)((vdata(lhs) & ~3U) + sizeof(JsOffset) * 2), val);
    if (vtype(lhs) == T_PROP) {
        JsOffset prop = (JsOffset)vdata(lhs);
        JsValue arr = internal::array_owner_of_prop(st, prop);
        if (vtype(arr) == T_OBJ) {
            JsOffset koff = loadoff(st, (JsOffset)(prop + sizeof(JsOffset)));
            JsOffset klen = (loadoff(st, koff) >> 2) - 1;
            const char* kp = (char*)&st->memory[koff + sizeof(koff)];
            char* end = NULL;
            double idx = strtod(kp, &end);
            if (end == kp + (ptrdiff_t)klen) internal::array_update_length_for_index(st, arr, idx);
        }
    }
    return lhs;
}

static JsValue do_assign_op(JsEngineState* st, uint8_t op, JsValue l, JsValue r)
{
    uint8_t m[] = {TOK_PLUS, TOK_MINUS, TOK_MUL, TOK_DIV, TOK_REM, TOK_SHL,
                   TOK_SHR,  TOK_ZSHR,  TOK_AND, TOK_XOR, TOK_OR};
    JsValue res = do_op(st, m[op - TOK_PLUS_ASSIGN], resolveprop(st, l), r);
    return assign(st, l, res);
}

static JsValue do_string_op(JsEngineState* st, uint8_t op, JsValue l, JsValue r);

static JsValue js_to_string_add(JsEngineState* st, JsValue v)
{
    return internal::js_to_string(st, v);
}

JsValue internal::js_to_string(JsEngineState* st, JsValue v)
{
    v = resolveprop(st, v);
    if (is_err(v)) return v;
    if (vtype(v) == T_STR) return v;
    if (vtype(v) == T_UNDEF || vtype(v) == T_NULL) return mkstr(st, "", 0);
    if (st->break_ + sizeof(JsOffset) >= st->memSize) return mkerr(st, "oom");
    char* buf = (char*)&st->memory[st->break_ + sizeof(JsOffset)];
    size_t avail = st->memSize - st->break_ - sizeof(JsOffset);
    size_t n = tostr(st, v, buf, avail);
    return mkstr(st, NULL, n);
}

static JsValue do_string_add(JsEngineState* st, JsValue l, JsValue r)
{
    JsValue ls = js_to_string_add(st, l);
    if (is_err(ls)) return ls;
    JsValue rs = js_to_string_add(st, r);
    if (is_err(rs)) return rs;
    return do_string_op(st, TOK_PLUS, ls, rs);
}

static JsValue do_string_op(JsEngineState* st, uint8_t op, JsValue l, JsValue r)
{
    JsOffset n1, off1 = vstr(st, l, &n1);
    JsOffset n2, off2 = vstr(st, r, &n2);
    if (op == TOK_PLUS) {
        JsValue res = mkstr(st, NULL, n1 + n2);
        // printf("STRPLUS %u %u %u %u [%.*s] [%.*s]\n", n1, off1, n2, off2, (int)
        // n1,
        //       &st->memory[off1], (int) n2, &st->memory[off2]);
        if (vtype(res) == T_STR) {
            JsOffset n, off = vstr(st, res, &n);
            memmove(&st->memory[off], &st->memory[off1], n1);
            memmove(&st->memory[off + n1], &st->memory[off2], n2);
        }
        return res;
    } else if (op == TOK_EQ) {
        bool eq = n1 == n2 && memcmp(&st->memory[off1], &st->memory[off2], n1) == 0;
        return mkval(T_BOOL, eq ? 1 : 0);
    } else if (op == TOK_NE) {
        bool eq = n1 == n2 && memcmp(&st->memory[off1], &st->memory[off2], n1) == 0;
        return mkval(T_BOOL, eq ? 0 : 1);
    } else if (op == TOK_LOOSE_EQ || op == TOK_LOOSE_NE) {
        bool eq = js_loose_eq(st, l, r);
        return mkval(T_BOOL, (op == TOK_LOOSE_EQ) ? (eq ? 1 : 0) : (eq ? 0 : 1));
    } else {
        return mkerr(st, "bad str op");
    }
}


static bool js_loose_eq(JsEngineState* st, JsValue a, JsValue b)
{
    a = resolveprop(st, a);
    b = resolveprop(st, b);
    uint8_t ta = vtype(a), tb = vtype(b);
    if (ta == tb) {
        if (ta == T_NUM) return tod(a) == tod(b);
        if (ta == T_STR) {
            JsOffset n1, off1 = vstr(st, a, &n1);
            JsOffset n2, off2 = vstr(st, b, &n2);
            return n1 == n2 && memcmp(&st->memory[off1], &st->memory[off2], n1) == 0;
        }
        if (ta == T_BOOL) return vdata(a) == vdata(b);
        if (ta == T_UNDEF || ta == T_NULL) return true;
        if (ta == T_OBJ) return vdata(a) == vdata(b);
    }
    if ((ta == T_NULL && tb == T_UNDEF) || (ta == T_UNDEF && tb == T_NULL)) return true;
    if (ta == T_BOOL) return js_loose_eq(st, tov(js_to_number(st, a)), b);
    if (tb == T_BOOL) return js_loose_eq(st, a, tov(js_to_number(st, b)));
    if ((ta == T_STR || ta == T_NUM) && (tb == T_STR || tb == T_NUM)) {
        double da = js_to_number(st, a), db = js_to_number(st, b);
        return da == db;
    }
    return false;
}

static JsValue js_array_literal(JsEngineState* st)
{
    uint8_t exe = !(st->flags & kFlagNoExec);
    JsValue arr = exe ? internal::array_create(st) : mkundef();
    if (is_err(arr)) return arr;
    st->consumed = 1;
    double idx = 0;
    for (bool comma = false; next(st) != TOK_EOF; comma = true) {
        if (!comma && next(st) == TOK_RBRACKET) break;
        JsValue val = js_expr(st);
        if (exe) {
            if (is_err(val)) return val;
            char buf[32];
            snprintf(buf, sizeof(buf), "%.17g", idx);
            JsValue r = setprop(st, arr, mkstr(st, buf, strlen(buf)), resolveprop(st, val));
            if (is_err(r)) return r;
            idx += 1.0;
        }
        if (next(st) == TOK_RBRACKET) break;
        EXPECT(TOK_COMMA, );
    }
    EXPECT(TOK_RBRACKET, );
    if (exe) internal::array_set_length(st, arr, idx);
    return arr;
}

static JsValue do_dot_op(JsEngineState* st, JsValue l, JsValue r)
{
    const char* ptr = (char*)&st->code[coderefoff(r)];
    if (vtype(r) != T_CODEREF) {
        return mkerr(st, "ident expected");
    }
    
    // Handle string methods
    if (vtype(l) == T_STR) {
        return string_dot_op(st, l, ptr, codereflen(r));
    }

    if (vtype(l) != T_OBJ) {
        return mkerr(st, "lookup in non-obj");
    }

    if (internal::is_array_obj(st, l)) {
        JsValue ar = array_dot_op(st, l, ptr, codereflen(r));
        if (vtype(ar) == T_CFUNC || vtype(ar) == T_NUM || is_err(ar)) return ar;
    }

    JsOffset off = lkp(st, l, ptr, codereflen(r));
    return off == 0 ? mkundef() : mkval(T_PROP, off);
}

static JsValue js_call_params(JsEngineState* st)
{
    JsOffset pos = st->position;
    uint8_t flags = st->flags;
    st->flags |= kFlagNoExec;
    st->consumed = 1;
    for (bool comma = false; next(st) != TOK_EOF; comma = true) {
        if (!comma && next(st) == TOK_RPAREN) break;
        JsValue arg = js_expr(st);
        if (is_err(arg)) return arg;
        if (next(st) == TOK_RPAREN) break;
        EXPECT(TOK_COMMA, st->flags = flags);
    }
    EXPECT(TOK_RPAREN, st->flags = flags);
    st->flags = flags;
    return mkcoderef(pos, st->position - pos - st->tokenLen);
}

static void reverse(JsValue* args, int nargs)
{
    for (int i = 0; i < nargs / 2; i++) {
        JsValue tmp = args[i];
        args[i] = args[nargs - i - 1], args[nargs - i - 1] = tmp;
    }
}

// Call native C function
static JsValue call_c(JsEngineState* st, JsEngine::NativeFunction fn)
{
    uint8_t flags = st->flags;
    JsValue recv = st->callThis;
    st->flags |= kFlagNative;
    int argc = 0;
    while (st->position < st->codeLen) {
        if (next(st) == TOK_RPAREN) break;
        JsValue arg = resolveprop(st, js_expr(st));
        st->callThis = recv;
        if (st->break_ + sizeof(arg) > st->memSize) return mkerr(st, "call oom");
        st->memSize -= (JsOffset)sizeof(arg);
        memcpy(&st->memory[st->memSize], &arg, sizeof(arg));
        argc++;
        // printf("  arg %d -> %s\n", argc, strValue(st, arg));
        if (next(st) == TOK_COMMA) st->consumed = 1;
    }
    reverse((JsValue*)&st->memory[st->memSize], argc);
    st->callThis = recv;
    JsValue res = fn(reinterpret_cast<JsEngine*>(st), (JsValue*)&st->memory[st->memSize], argc);
    setlwm(st);
    st->memSize += (JsOffset)sizeof(JsValue) * (JsOffset)argc;  // Restore stack
    st->flags = flags;
    return res;
}

static JsValue invoke_js_fn(JsEngineState* st, JsValue fnval, const char* fn, JsOffset fnlen, JsValue* args,
                            int argc)
{
    JsOffset fnpos = 1;
    JsValue saved_scope = st->scope;
    JsOffset parent = func_scope_off(fnval);
    if (parent == 0) parent = (JsOffset)vdata(saved_scope);
    st->scope = mkobj(st, parent);
    int argi = 0;
    while (fnpos < fnlen) {
        fnpos = skiptonext(fn, fnlen, fnpos);
        if (fnpos < fnlen && fn[fnpos] == ')') break;
        JsOffset identlen = 0;
        uint8_t tok = parseident(&fn[fnpos], fnlen - fnpos, &identlen);
        if (tok != TOK_IDENTIFIER) break;
        JsValue v = argi < argc ? args[argi++] : mkundef();
        setprop(st, st->scope, mkstr(st, &fn[fnpos], identlen), v);
        fnpos = skiptonext(fn, fnlen, fnpos + identlen);
        if (fnpos < fnlen && fn[fnpos] == ',') fnpos++;
    }
    if (fnpos < fnlen && fn[fnpos] == ')') fnpos++;
    fnpos = skiptonext(fn, fnlen, fnpos);
    if (fnpos < fnlen && fn[fnpos] == '{') fnpos++;
    size_t n = fnlen - fnpos - 1U;
    st->flags = kFlagCall;
    JsValue res = js_eval(reinterpret_cast<JsEngine*>(st), &fn[fnpos], n);
    if (!is_err(res) && !(st->flags & kFlagReturn)) res = mkundef();
    st->flags &= (uint8_t)~kFlagReturn;
    st->scope = saved_scope;
    return res;
}

// Call JS function. 'fn' looks like this: "(a,b) { return a + b; }"
static JsValue call_js(JsEngineState* st, JsValue fnval, const char* fn, JsOffset fnlen)
{
    int argc = 0;
    st->position = skiptonext(st->code, st->codeLen, st->position);
    while (st->position < st->codeLen && st->code[st->position] != ')') {
        st->consumed = 1;
        JsValue arg = js_expr(st);
        if (is_err(arg)) return arg;
        if (st->break_ + (JsOffset)sizeof(arg) > st->memSize) return mkerr(st, "call oom");
        st->memSize -= (JsOffset)sizeof(arg);
        memcpy(&st->memory[st->memSize], &arg, sizeof(arg));
        argc++;
        st->position = skiptonext(st->code, st->codeLen, st->position);
        if (st->position < st->codeLen && st->code[st->position] == ',') st->position++;
    }
    reverse((JsValue*)&st->memory[st->memSize], argc);
    JsValue res = invoke_js_fn(st, fnval, fn, fnlen, (JsValue*)&st->memory[st->memSize], argc);
    st->memSize += (JsOffset)sizeof(JsValue) * (JsOffset)argc;
    return res;
}

static JsValue do_call_op(JsEngineState* st, JsValue func, JsValue args)
{
    if (vtype(args) != T_CODEREF) return mkerr(st, "bad call");
    if (vtype(func) != T_FUNC && vtype(func) != T_CFUNC) return mkerr(st, "calling non-function");
    const char* code = st->code;  // Save current parser state
    JsOffset clen = st->codeLen,
             pos = st->position;                          // code, position and code length
    st->code = &st->code[coderefoff(args)];               // Point parser to args
    st->codeLen = codereflen(args);                       // Set args length
    st->position = skiptonext(st->code, st->codeLen, 0);  // Skip to 1st arg
    uint8_t tok = st->token, flags = st->flags;           // Save flags
    JsOffset nogc = st->noGc;
    JsValue res = mkundef();
    if (vtype(func) == T_FUNC) {
        JsOffset fnlen, fnoff = func_vstr(st, func, &fnlen);
        st->noGc = (JsOffset)(fnoff - sizeof(JsOffset));
        res = call_js(st, func, (const char*)(&st->memory[fnoff]), fnlen);
    } else {
        res = call_c(st, reinterpret_cast<JsEngine::NativeFunction>(vdata(func)));
    }
    st->code = code, st->codeLen = clen, st->position = pos;  // Restore parser
    st->flags = flags, st->token = tok, st->noGc = nogc;
    st->consumed = 1;
    return res;
}

// clang-format off
static JsValue do_op(JsEngineState *st, uint8_t op, JsValue lhs, JsValue rhs) 
{
  if (st->flags & kFlagNoExec) return 0;
  JsValue l = resolveprop(st, lhs), r = resolveprop(st, rhs);
  // printf("OP %d %d %d\n", op, vtype(lhs), vtype(r));
  setlwm(st);
  if (is_err(l)) return l;
  if (is_err(r)) return r;
  if (is_assign(op) && vtype(lhs) != T_PROP) return mkerr(st, "bad lhs");
  switch (op) {
    case TOK_TYPEOF:  return mkstr(st, typestr(vtype(r)), strlen(typestr(vtype(r))));
    case TOK_CALL:    return do_call_op(st, l, r);
    case TOK_ASSIGN:  return assign(st, lhs, r);
    case TOK_POSTINC: {
      if (vtype(lhs) != T_PROP) return mkerr(st, "bad lhs for ++");
      do_assign_op(st, TOK_PLUS_ASSIGN, lhs, tov(1)); return l;
    }
    case TOK_POSTDEC: {
      if (vtype(lhs) != T_PROP) return mkerr(st, "bad lhs for --");
      do_assign_op(st, TOK_MINUS_ASSIGN, lhs, tov(1)); return l;
    }
    case TOK_NOT:     if (vtype(r) == T_BOOL) return mkval(T_BOOL, !vdata(r)); break;
  }
  if (is_assign(op))    return do_assign_op(st, op, lhs, r);
  if (op == TOK_LOOSE_EQ || op == TOK_LOOSE_NE) {
    bool eq = js_loose_eq(st, l, r);
    return mkval(T_BOOL, (op == TOK_LOOSE_EQ) ? (eq ? 1 : 0) : (eq ? 0 : 1));
  }
  if (op == TOK_PLUS && (vtype(l) == T_STR || vtype(r) == T_STR)) return do_string_add(st, l, r);
  if (vtype(l) == T_STR && vtype(r) == T_STR) return do_string_op(st, op, l, r);
  if (is_unary(op) && vtype(r) != T_NUM) return mkerr(st, "type mismatch");
  if (!is_unary(op) && op != TOK_DOT && (vtype(l) != T_NUM || vtype(r) != T_NUM)) return mkerr(st, "type mismatch");
  double a = tod(l), b = tod(r);
  auto to_i32 = [](double d) -> int32_t {
    if (std::isnan(d) || !std::isfinite(d)) return 0;
    const double kTwo32 = 4294967296.0;
    const double kTwo31 = 2147483648.0;
    d = std::fmod(std::fmod(d, kTwo32) + kTwo32, kTwo32);
    return d >= kTwo31 ? (int32_t) (int64_t) (d - kTwo32) : (int32_t) (int64_t) d;
  };
  auto to_u32 = [](double d) -> uint32_t {
    if (std::isnan(d) || !std::isfinite(d)) return 0;
    const double kTwo32 = 4294967296.0;
    return (uint32_t) std::fmod(std::fmod(d, kTwo32) + kTwo32, kTwo32);
  };
  switch (op) {
    case TOK_EXP:     return tov(pow(a, b));
    case TOK_DIV:     return tod(r) == 0 ? mkerr(st, "div by zero") : tov(a / b);
    case TOK_REM:     return tov(a - b * ((double) (long) (a / b)));
    case TOK_MUL:     return tov(a * b);
    case TOK_PLUS:    return tov(a + b);
    case TOK_MINUS:   return tov(a - b);
    case TOK_XOR:     return tov((double) (to_i32(a) ^ to_i32(b)));
    case TOK_AND:     return tov((double) (to_i32(a) & to_i32(b)));
    case TOK_OR:      return tov((double) (to_i32(a) | to_i32(b)));
    case TOK_UMINUS:  return tov(-b);
    case TOK_UPLUS:   return r;
    case TOK_TILDA:   return tov((double) (~to_i32(b)));
    case TOK_NOT:     return mkval(T_BOOL, b == 0);
    case TOK_SHL:     return tov((double) (to_i32(a) << (to_u32(b) & 31U)));
    case TOK_SHR:     return tov((double) (to_i32(a) >> (to_u32(b) & 31U)));
    case TOK_ZSHR:    return tov((double) (to_u32(a) >> (to_u32(b) & 31U)));
    case TOK_DOT:     return do_dot_op(st, l, r);
    case TOK_EQ:      return mkval(T_BOOL, (long) a == (long) b);
    case TOK_NE:      return mkval(T_BOOL, (long) a != (long) b);
    case TOK_LT:      return mkval(T_BOOL, a < b);
    case TOK_LE:      return mkval(T_BOOL, a <= b);
    case TOK_GT:      return mkval(T_BOOL, a > b);
    case TOK_GE:      return mkval(T_BOOL, a >= b);
    default:          return mkerr(st, "unknown op %d", (int) op);  // LCOV_EXCL_LINE
  }
}  // clang-format on

static JsValue js_str_literal(JsEngineState* st)
{
    uint8_t* in = (uint8_t*)&st->code[st->tokenOffset];
    uint8_t* out = &st->memory[st->break_ + sizeof(JsOffset)];
    size_t n1 = 0, n2 = 0;
    // printf("STR %u %lu %lu\n", st->break_, st->tokenLen, st->codeLen);
    if (st->break_ + sizeof(JsOffset) + st->tokenLen > st->memSize) return mkerr(st, "oom");
    while (n2++ + 2 < st->tokenLen) {
        if (in[n2] == '\\') {
            if (in[n2 + 1] == in[0]) {
                out[n1++] = in[0];
            } else if (in[n2 + 1] == 'n') {
                out[n1++] = '\n';
            } else if (in[n2 + 1] == 't') {
                out[n1++] = '\t';
            } else if (in[n2 + 1] == 'r') {
                out[n1++] = '\r';
            } else if (in[n2 + 1] == 'x' && is_xdigit(in[n2 + 2]) && is_xdigit(in[n2 + 3])) {
                out[n1++] = (uint8_t)((unhex(in[n2 + 2]) << 4U) | unhex(in[n2 + 3]));
                n2 += 2;
            } else {
                return mkerr(st, "bad str literal");
            }
            n2++;
        } else {
            out[n1++] = ((uint8_t*)st->code)[st->tokenOffset + n2];
        }
    }
    return mkstr(st, NULL, n1);
}

static JsValue js_obj_literal(JsEngineState* st)
{
    uint8_t exe = !(st->flags & kFlagNoExec);
    // printf("OLIT1\n");
    JsValue obj = exe ? mkobj(st, 0) : mkundef();
    if (is_err(obj)) return obj;
    st->consumed = 1;
    while (next(st) != TOK_RBRACE) {
        JsValue key = 0;
        if (st->token == TOK_IDENTIFIER) {
            if (exe) key = mkstr(st, st->code + st->tokenOffset, st->tokenLen);
        } else if (st->token == TOK_STRING) {
            if (exe) key = js_str_literal(st);
        } else {
            return mkerr(st, "parse error");
        }
        st->consumed = 1;
        EXPECT(TOK_COLON, );
        JsValue val = js_expr(st);
        if (exe) {
            // printf("XXXX [%s] scope: %lu\n", strValue(st, val), vdata(st->scope));
            if (is_err(val)) return val;
            if (is_err(key)) return key;
            JsValue res = setprop(st, obj, key, resolveprop(st, val));
            if (is_err(res)) return res;
        }
        if (next(st) == TOK_RBRACE) break;
        EXPECT(TOK_COMMA, );
    }
    EXPECT(TOK_RBRACE, );
    return obj;
}

static JsValue mkfunc_parts(JsEngineState* st, const void* a, size_t na, const void* b, size_t nb)
{
    size_t n = na + nb;
    char stack[512];
    char* buf = n <= sizeof(stack) ? stack : (char*)malloc(n);
    if (buf == NULL) return mkerr(st, "oom");
    memcpy(buf, a, na);
    memcpy(buf + na, b, nb);
    JsValue str = mkstr(st, buf, n);
    if (buf != stack) free(buf);
    if (is_err(str)) return str;
    return mkfunc(st, str);
}

static bool src_peek_arrow(JsEngineState* st, JsOffset off)
{
    off = skiptonext(st->code, st->codeLen, off);
    return off + 1 < st->codeLen && st->code[off] == '=' && st->code[off + 1] == '>';
}

static JsOffset scan_arrow_params(JsEngineState* st, JsOffset inner, JsOffset* pend)
{
    JsOffset pos = skiptonext(st->code, st->codeLen, inner);
    if (pos < st->codeLen && st->code[pos] == ')') {
        pos++;
        if (!src_peek_arrow(st, pos)) return 0;
        *pend = pos;
        return pos;
    }
    while (pos < st->codeLen) {
        JsOffset idlen = 0;
        if (parseident(&st->code[pos], st->codeLen - pos, &idlen) != TOK_IDENTIFIER) return 0;
        pos = skiptonext(st->code, st->codeLen, pos + idlen);
        if (pos < st->codeLen && st->code[pos] == ')') {
            pos++;
            if (!src_peek_arrow(st, pos)) return 0;
            *pend = pos;
            return pos;
        }
        if (pos >= st->codeLen || st->code[pos] != ',') return 0;
        pos = skiptonext(st->code, st->codeLen, pos + 1);
    }
    return 0;
}

static JsOffset find_block_end(JsEngineState* st, JsOffset start)
{
    if (start >= st->codeLen || st->code[start] != '{') return 0;
    JsOffset depth = 0;
    for (JsOffset i = start; i < st->codeLen; i++) {
        if (st->code[i] == '{') depth++;
        else if (st->code[i] == '}') {
            depth--;
            if (depth == 0) return i + 1;
        }
    }
    return 0;
}

static JsOffset find_concise_body_end(JsEngineState* st, JsOffset start)
{
    JsOffset depth = 0;
    for (JsOffset i = start; i < st->codeLen; i++) {
        char c = st->code[i];
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') {
            if (depth > 0) depth--;
            else if (c == ')') return i;
        } else if (depth == 0 && (c == ',' || c == ';')) {
            return i;
        }
    }
    return st->codeLen;
}

static JsValue js_arrow_build(JsEngineState* st, const void* params, size_t plen, JsOffset arrow_at)
{
    if (arrow_at + 1 >= st->codeLen || st->code[arrow_at] != '=' || st->code[arrow_at + 1] != '>') {
        return mkerr(st, "=> expected");
    }
    JsOffset body_start = skiptonext(st->code, st->codeLen, arrow_at + 2);
    if (body_start >= st->codeLen) return mkerr(st, "arrow body expected");

    JsValue fn = mkundef();
    if (st->code[body_start] == '{') {
        JsOffset body_end = find_block_end(st, body_start);
        if (body_end == 0) return mkerr(st, "arrow block unclosed");
        fn = mkfunc_parts(st, params, plen, &st->code[body_start], (size_t)(body_end - body_start));
        st->position = body_end;
    } else {
        JsOffset body_end = find_concise_body_end(st, body_start);
        static const char prefix[] = " { return ";
        static const char suffix[] = "; }";
        size_t elen = (size_t)(body_end - body_start);
        size_t n = plen + sizeof(prefix) - 1 + elen + sizeof(suffix) - 1;
        char stack[512];
        char* buf = n <= sizeof(stack) ? stack : (char*)malloc(n);
        if (buf == NULL) return mkerr(st, "oom");
        size_t off = 0;
        memcpy(buf + off, params, plen);
        off += plen;
        memcpy(buf + off, prefix, sizeof(prefix) - 1);
        off += sizeof(prefix) - 1;
        memcpy(buf + off, &st->code[body_start], elen);
        off += elen;
        memcpy(buf + off, suffix, sizeof(suffix) - 1);
        JsValue str = mkstr(st, buf, n);
        if (buf != stack) free(buf);
        if (is_err(str)) return str;
        fn = mkfunc(st, str);
        st->position = body_end;
    }
    st->consumed = 1;
    return fn;
}

static JsValue js_arrow_from_source(JsEngineState* st, JsOffset pstart, JsOffset pend)
{
    JsOffset arrow_at = skiptonext(st->code, st->codeLen, pend);
    return js_arrow_build(st, &st->code[pstart], (size_t)(pend - pstart), arrow_at);
}

static JsValue js_arrow_single(JsEngineState* st, JsOffset idoff, JsOffset idlen)
{
    char paren[160];
    if (idlen + 2 >= sizeof(paren)) return mkerr(st, "parse error");
    paren[0] = '(';
    memcpy(paren + 1, &st->code[idoff], idlen);
    paren[1 + idlen] = ')';
    JsOffset arrow_at = skiptonext(st->code, st->codeLen, idoff + idlen);
    return js_arrow_build(st, paren, (size_t)(idlen + 2), arrow_at);
}

static JsValue js_arrow_paren(JsEngineState* st, JsOffset pstart, JsOffset pend)
{
    return js_arrow_from_source(st, pstart, pend);
}

static JsValue js_func_body(JsEngineState* st)
{
    uint8_t flags = st->flags;
    EXPECT(TOK_LPAREN, st->flags = flags);
    JsOffset pos = st->position - 1;
    for (bool comma = false; next(st) != TOK_EOF; comma = true) {
        if (!comma && next(st) == TOK_RPAREN) break;
        EXPECT(TOK_IDENTIFIER, st->flags = flags);
        if (next(st) == TOK_RPAREN) break;
        EXPECT(TOK_COMMA, st->flags = flags);
    }
    EXPECT(TOK_RPAREN, st->flags = flags);
    EXPECT(TOK_LBRACE, st->flags = flags);
    st->consumed = 0;
    st->flags |= kFlagNoExec;
    JsValue res = js_block(st, false);
    if (is_err(res)) {
        st->flags = flags;
        return res;
    }
    st->flags = flags;
    JsValue str = mkstr(st, &st->code[pos], st->position - pos);
    st->consumed = 1;
    return mkfunc(st, str);
}

static JsValue js_func_literal(JsEngineState* st)
{
    st->consumed = 1;
    return js_func_body(st);
}

static JsValue js_func_decl(JsEngineState* st)
{
    uint8_t exe = !(st->flags & kFlagNoExec);
    JsOffset noff = st->tokenOffset, nlen = st->tokenLen;
    char* name = (char*)&st->code[noff];
    st->consumed = 1;
    JsValue fn = js_func_body(st);
    if (is_err(fn)) return fn;
    if (exe) {
        if (lkp(st, st->scope, name, nlen) > 0)
            return mkerr(st, "'%.*s' already declared", (int)nlen, name);
        JsValue x = setprop(st, st->scope, mkstr(st, name, nlen), fn);
        if (is_err(x)) return x;
    }
    return fn;
}

#define RTL_BINOP(_f1, _f2, _cond)     \
    JsValue res = _f1(st);             \
    while (!is_err(res) && (_cond)) {  \
        uint8_t op = st->token;        \
        st->consumed = 1;              \
        JsValue rhs = _f2(st);         \
        if (is_err(rhs)) return rhs;   \
        res = do_op(st, op, res, rhs); \
    }                                  \
    return res;

#define LTR_BINOP(_f, _cond)           \
    JsValue res = _f(st);              \
    while (!is_err(res) && (_cond)) {  \
        uint8_t op = st->token;        \
        st->consumed = 1;              \
        JsValue rhs = _f(st);          \
        if (is_err(rhs)) return rhs;   \
        res = do_op(st, op, res, rhs); \
    }                                  \
    return res;

static JsValue js_literal(JsEngineState* st)
{
    next(st);
    setlwm(st);
    // printf("css : %u\n", st->cStackSize);
    if (st->maxCss > 0 && st->cStackSize > st->maxCss) return mkerr(st, "C stack");
    st->consumed = 1;
    switch (st->token) {  // clang-format off
    case TOK_ERR:         return mkerr(st, "parse error");
    case TOK_NUMBER:      return st->tokenValue;
    case TOK_STRING:      return js_str_literal(st);
    case TOK_LBRACE:      return js_obj_literal(st);
    case TOK_LBRACKET:    return js_array_literal(st);
    case TOK_FUNC:        return js_func_literal(st);
    case TOK_NULL:        return mknull();
    case TOK_UNDEF:       return mkundef();
    case TOK_TRUE:        return mktrue();
    case TOK_FALSE:       return mkfalse();
    case TOK_IDENTIFIER: {
        st->consumed = 0;
        if (lookahead(st) == TOK_ARROW) {
            return js_arrow_single(st, st->tokenOffset, st->tokenLen);
        }
        st->consumed = 1;
        return mkcoderef((JsOffset) st->tokenOffset, (JsOffset) st->tokenLen);
    }
    default:              return mkerr(st, "bad expr");
  }  // clang-format on
}

static JsValue js_group(JsEngineState* st)
{
    if (next(st) == TOK_LPAREN) {
        JsOffset pstart = st->tokenOffset;
        JsOffset inner = st->position;
        JsOffset pend = 0;
        if (scan_arrow_params(st, inner, &pend) != 0) {
            JsValue r = js_arrow_paren(st, pstart, pend);
            if (is_err(r)) return r;
            return r;
        }
        st->consumed = 1;
        JsValue v = js_expr(st);
        if (is_err(v)) return v;
        if (next(st) != TOK_RPAREN) return mkerr(st, ") expected");
        st->consumed = 1;
        return v;
    } else {
        return js_literal(st);
    }
}

static JsValue js_call_dot(JsEngineState* st)
{
    JsValue res = js_group(st);
    if (is_err(res)) return res;
    if (vtype(res) == T_CODEREF) {
        res = lookup(st, &st->code[coderefoff(res)], codereflen(res));
    }
    while (next(st) == TOK_LPAREN || next(st) == TOK_DOT || next(st) == TOK_LBRACKET) {
        if (st->token == TOK_DOT) {
            st->callThis = resolveprop(st, res);
            st->consumed = 1;
            res = do_op(st, TOK_DOT, res, js_group(st));
        } else if (st->token == TOK_LBRACKET) {
            st->consumed = 1;
            JsValue key = js_expr(st);
            if (is_err(key)) return key;
            if (next(st) != TOK_RBRACKET) return mkerr(st, "] expected");
            st->consumed = 1;
            if (st->flags & kFlagNoExec)
                res = mkundef();
            else
                res = internal::array_bracket_op(st, resolveprop(st, res), key);
        } else {
            JsValue recv = st->callThis;
            JsValue params = js_call_params(st);
            if (is_err(params)) return params;
            st->callThis = recv;
            res = do_op(st, TOK_CALL, res, params);
            st->callThis = recv;
        }
    }
    return res;
}

static JsValue js_postfix(JsEngineState* st)
{
    JsValue res = js_call_dot(st);
    if (is_err(res)) return res;
    next(st);
    if (st->token == TOK_POSTINC || st->token == TOK_POSTDEC) {
        st->consumed = 1;
        res = do_op(st, st->token, res, 0);
    }
    return res;
}

static JsValue js_unary(JsEngineState* st)
{
    if (next(st) == TOK_NOT || st->token == TOK_TILDA || st->token == TOK_TYPEOF ||
        st->token == TOK_MINUS || st->token == TOK_PLUS) {
        uint8_t t = st->token;
        if (t == TOK_MINUS) t = TOK_UMINUS;
        if (t == TOK_PLUS) t = TOK_UPLUS;
        st->consumed = 1;
        return do_op(st, t, mkundef(), js_unary(st));
    } else {
        return js_postfix(st);
    }
}

static JsValue js_exp_op(JsEngineState* st)
{
    LTR_BINOP(js_unary, (next(st) == TOK_EXP));
}

static JsValue js_mul_div_rem(JsEngineState* st)
{
    LTR_BINOP(js_exp_op, (next(st) == TOK_MUL || st->token == TOK_DIV || st->token == TOK_REM));
}

static JsValue js_plus_minus(JsEngineState* st)
{
    LTR_BINOP(js_mul_div_rem, (next(st) == TOK_PLUS || st->token == TOK_MINUS));
}

static JsValue js_shifts(JsEngineState* st)
{
    LTR_BINOP(js_plus_minus, (next(st) == TOK_SHR || next(st) == TOK_SHL || next(st) == TOK_ZSHR));
}

static JsValue js_comparison(JsEngineState* st)
{
    LTR_BINOP(js_shifts, (next(st) == TOK_LT || next(st) == TOK_LE || next(st) == TOK_GT ||
                          next(st) == TOK_GE));
}

static JsValue js_equality(JsEngineState* st)
{
    LTR_BINOP(js_comparison, (next(st) == TOK_EQ || st->token == TOK_NE ||
                              st->token == TOK_LOOSE_EQ || st->token == TOK_LOOSE_NE));
}

static JsValue js_bitwise_and(JsEngineState* st)
{
    LTR_BINOP(js_equality, (next(st) == TOK_AND));
}

static JsValue js_bitwise_xor(JsEngineState* st)
{
    LTR_BINOP(js_bitwise_and, (next(st) == TOK_XOR));
}

static JsValue js_bitwise_or(JsEngineState* st)
{
    LTR_BINOP(js_bitwise_xor, (next(st) == TOK_OR));
}

static JsValue js_logical_and(JsEngineState* st)
{
    JsValue res = js_bitwise_or(st);
    if (is_err(res)) return res;
    uint8_t flags = st->flags;
    while (next(st) == TOK_LAND) {
        st->consumed = 1;
        res = resolveprop(st, res);
        if (!truthy(st, res)) st->flags |= kFlagNoExec;  // false && ... shortcut
        if (st->flags & kFlagNoExec) {
            js_logical_and(st);
        } else {
            res = js_logical_and(st);
        }
    }
    st->flags = flags;
    return res;
}

static JsValue js_logical_or(JsEngineState* st)
{
    JsValue res = js_logical_and(st);
    if (is_err(res)) return res;
    uint8_t flags = st->flags;
    while (next(st) == TOK_LOR) {
        st->consumed = 1;
        res = resolveprop(st, res);
        if (truthy(st, res)) st->flags |= kFlagNoExec;  // true || ... shortcut
        if (st->flags & kFlagNoExec) {
            js_logical_or(st);
        } else {
            res = js_logical_or(st);
        }
    }
    st->flags = flags;
    return res;
}

static JsValue js_ternary(JsEngineState* st)
{
    JsValue res = js_logical_or(st);
    if (next(st) == TOK_Q) {
        uint8_t flags = st->flags;
        st->consumed = 1;
        if (truthy(st, resolveprop(st, res))) {
            res = js_ternary(st);
            st->flags |= kFlagNoExec;
            EXPECT(TOK_COLON, st->flags = flags);
            js_ternary(st);
            st->flags = flags;
        } else {
            st->flags |= kFlagNoExec;
            js_ternary(st);
            EXPECT(TOK_COLON, st->flags = flags);
            st->flags = flags;
            res = js_ternary(st);
        }
    }
    return res;
}

static JsValue js_assignment(JsEngineState* st)
{
    RTL_BINOP(
        js_ternary, js_assignment,
        (next(st) == TOK_ASSIGN || st->token == TOK_PLUS_ASSIGN || st->token == TOK_MINUS_ASSIGN ||
         st->token == TOK_MUL_ASSIGN || st->token == TOK_DIV_ASSIGN ||
         st->token == TOK_REM_ASSIGN || st->token == TOK_SHL_ASSIGN ||
         st->token == TOK_SHR_ASSIGN || st->token == TOK_ZSHR_ASSIGN ||
         st->token == TOK_AND_ASSIGN || st->token == TOK_XOR_ASSIGN || st->token == TOK_OR_ASSIGN));
}

static JsValue js_expr(JsEngineState* st)
{
    return js_assignment(st);
}

static JsValue js_bind(JsEngineState* st, bool is_const)
{
    uint8_t exe = !(st->flags & kFlagNoExec);
    st->consumed = 1;
    for (;;) {
        EXPECT(TOK_IDENTIFIER, );
        st->consumed = 0;
        JsOffset noff = st->tokenOffset, nlen = st->tokenLen;
        char* name = (char*)&st->code[noff];
        JsValue v = mkundef();
        st->consumed = 1;
        if (next(st) == TOK_ASSIGN) {
            st->consumed = 1;
            v = js_expr(st);
            if (is_err(v)) return v;  // Propagate error if any
        } else if (is_const) {
            return mkerr(st, "const needs init");
        }
        if (exe) {
            if (lkp(st, st->scope, name, nlen) > 0)
                return mkerr(st, "'%.*s' already declared", (int)nlen, name);
            JsOffset pflags = is_const ? kPropConst : 0;
            JsValue x = setprop(st, st->scope, mkstr(st, name, nlen), resolveprop(st, v), pflags);
            if (is_err(x)) return x;
        }
        if (next(st) == TOK_SEMICOLON || next(st) == TOK_EOF) break;  // Stop
        EXPECT(TOK_COMMA, );
    }
    return mkundef();
}

static JsValue js_block_or_stmt(JsEngineState* st)
{
    if (next(st) == TOK_LBRACE) return js_block(st, !(st->flags & kFlagNoExec));
    JsValue res = resolveprop(st, js_stmt(st));
    st->consumed = 0;  //
    return res;
}

static JsValue js_if(JsEngineState* st)
{
    st->consumed = 1;
    EXPECT(TOK_LPAREN, );
    JsValue res = mkundef(), cond = resolveprop(st, js_expr(st));
    EXPECT(TOK_RPAREN, );
    bool cond_true = truthy(st, cond), exe = !(st->flags & kFlagNoExec);
    // printf("IF COND: %s, true? %d\n", strValue(st, cond), cond_true);
    if (!cond_true) st->flags |= kFlagNoExec;
    JsValue blk = js_block_or_stmt(st);
    if (cond_true) res = blk;
    if (exe && !cond_true) st->flags &= (uint8_t)~kFlagNoExec;
    if (lookahead(st) == TOK_ELSE) {
        st->consumed = 1;
        next(st);
        st->consumed = 1;
        if (cond_true) st->flags |= kFlagNoExec;
        blk = js_block_or_stmt(st);
        if (!cond_true) res = blk;
        if (cond_true && exe) st->flags &= (uint8_t)~kFlagNoExec;
    }
    return res;
}

static inline bool expect(JsEngineState* st, uint8_t tok, JsValue* res)
{
    if (next(st) != tok) {
        *res = mkerr(st, "parse error");
        return false;
    } else {
        st->consumed = 1;
        return true;
    }
}

static inline bool is_err2(JsValue* v, JsValue* res)
{
    bool r = is_err(*v);
    if (r) *res = *v;
    return r;
}

static JsValue js_for(JsEngineState* st)
{
    enum : uint8_t { kForLexNone = 0, kForLexLet = 1, kForLexConst = 2 };
    uint8_t flags = st->flags, exe = !(flags & kFlagNoExec);
    uint8_t for_lex = kForLexNone;
    JsValue v, res = mkundef();
    JsOffset pos1 = 0, pos2 = 0, pos3 = 0, pos4 = 0;
    JsValue loopScope = mkundef();
    JsOffset loopScopeParent = 0;
    if (exe) mkscope(st);  // Enter new scope
    if (!expect(st, TOK_FOR, &res)) goto done;
    if (!expect(st, TOK_LPAREN, &res)) goto done;

    if (next(st) == TOK_SEMICOLON) {  // initialisation
    } else if (next(st) == TOK_LET) {
        for_lex = kForLexLet;
        v = js_bind(st, false);
        if (is_err2(&v, &res)) goto done;
    } else if (next(st) == TOK_CONST) {
        for_lex = kForLexConst;
        v = js_bind(st, true);
        if (is_err2(&v, &res)) goto done;
    } else {
        v = js_expr(st);
        if (is_err2(&v, &res)) goto done;
    }
    if (for_lex != kForLexNone && exe) {
        loopScope = st->scope;
        loopScopeParent = (JsOffset)vdata(upper(st, loopScope));
    }
    if (!expect(st, TOK_SEMICOLON, &res)) goto done;
    st->flags |= kFlagNoExec;
    pos1 = st->position;  // condition
    if (next(st) != TOK_SEMICOLON) {
        v = js_expr(st);
        if (is_err2(&v, &res)) goto done;
    }
    if (!expect(st, TOK_SEMICOLON, &res)) goto done;
    pos2 = st->position;  // final expr
    if (next(st) != TOK_RPAREN) {
        v = js_expr(st);
        if (is_err2(&v, &res)) goto done;
    }
    if (!expect(st, TOK_RPAREN, &res)) goto done;
    pos3 = st->position;  // body
    v = js_block_or_stmt(st);
    if (is_err2(&v, &res)) goto done;
    pos4 = st->position;  // end of body
    while (!(flags & kFlagNoExec)) {
        if (for_lex != kForLexNone && exe) {
            JsValue iterScope = mkobj(st, loopScopeParent);
            scope_copy_props(st, loopScope, iterScope);
            st->scope = iterScope;
        }
        st->flags = flags, st->position = pos1, st->consumed = 1;
        if (next(st) != TOK_SEMICOLON) {       // Is condition specified?
            v = resolveprop(st, js_expr(st));  // Yes. check condition
            if (is_err2(&v, &res)) goto loop_scope_done;
            if (!truthy(st, v)) {              // Exit the loop if condition is false
                if (for_lex != kForLexNone && exe) st->scope = loopScope;
                break;
            }
        }
        st->position = pos3, st->consumed = 1,
        st->flags |= kFlagLoop;             // Execute the
        v = js_block_or_stmt(st);           // loop body
        if (is_err2(&v, &res)) goto loop_scope_done;
        if (st->flags & kFlagBreak) {
            if (for_lex != kForLexNone && exe) st->scope = loopScope;
            break;
        }
        if (st->flags & kFlagReturn) {
            res = v;
            if (for_lex != kForLexNone && exe) st->scope = loopScope;
            break;
        }
        if (for_lex != kForLexNone && exe) st->scope = loopScope;
        st->flags = flags, st->position = pos2,
        st->consumed = 1;                      // Jump to final expr
        if (next(st) != TOK_RPAREN) {          // Is it specified?
            v = js_expr(st);                   // Yes. Execute it
            if (is_err2(&v, &res)) goto loop_scope_done;
        }
        if (for_lex != kForLexNone && exe) st->scope = loopScope;
    }
    if (!(st->flags & kFlagReturn)) {
        st->position = pos4;
        st->token = TOK_SEMICOLON;
        st->consumed = 0;
    }
    goto done;
loop_scope_done:
    if (for_lex != kForLexNone && exe) st->scope = loopScope;
done:
    if (exe) delscope(st);  // Exit scope
    if (st->flags & kFlagReturn) return res;
    st->flags = flags & (uint8_t)~kFlagBreak;  // Restore flags
    return res;
}

static void switch_skip_to_next_case(JsEngineState* st)
{
    while (next(st) != TOK_EOF) {
        if (st->token == TOK_CASE || st->token == TOK_DEFAULT || st->token == TOK_RBRACE) return;
        if (st->token == TOK_LBRACE) {
            st->consumed = 1;
            js_block(st, false);
            continue;
        }
        js_stmt(st);
    }
}

static JsOffset switch_find_match(JsEngineState* st, JsValue disc, JsOffset* defaultPos)
{
    JsOffset match = 0;
    *defaultPos = 0;
    uint8_t flags = st->flags;
    st->flags |= kFlagNoExec;
    st->consumed = 1;
    while (next(st) != TOK_RBRACE && next(st) != TOK_EOF) {
        if (st->token == TOK_CASE) {
            st->consumed = 1;
            JsValue cv = js_expr(st);
            if (next(st) != TOK_COLON) {
                st->flags = flags;
                return 0;
            }
            st->consumed = 1;
            if (match == 0 && js_loose_eq(st, disc, cv)) match = st->position;
            switch_skip_to_next_case(st);
        } else if (st->token == TOK_DEFAULT) {
            st->consumed = 1;
            if (next(st) != TOK_COLON) {
                st->flags = flags;
                return 0;
            }
            st->consumed = 1;
            if (*defaultPos == 0) *defaultPos = st->position;
            switch_skip_to_next_case(st);
        } else {
            switch_skip_to_next_case(st);
        }
    }
    st->flags = flags;
    return match;
}

static JsValue js_switch(JsEngineState* st)
{
    uint8_t flags = st->flags, exe = !(flags & kFlagNoExec);
    JsValue res = mkundef();
    st->consumed = 1;
    EXPECT(TOK_LPAREN, );
    JsValue disc = js_expr(st);
    if (is_err(disc)) return disc;
    disc = resolveprop(st, disc);
    EXPECT(TOK_RPAREN, );
    EXPECT(TOK_LBRACE, );
    JsOffset defaultPos = 0, endPos = 0, matchPos = 0;
    matchPos = switch_find_match(st, disc, &defaultPos);
    endPos = st->position;
    st->consumed = 1;
    if (exe && (matchPos || defaultPos)) {
        JsOffset start = matchPos ? matchPos : defaultPos;
        st->position = start;
        st->flags = flags | kFlagSwitch;
        st->consumed = 1;
        while (next(st) != TOK_RBRACE && next(st) != TOK_EOF && !(st->flags & kFlagBreak)) {
            if (st->token == TOK_CASE || st->token == TOK_DEFAULT) break;
            res = js_stmt(st);
            if (is_err(res)) break;
        }
    }
    st->position = endPos;
    st->token = TOK_SEMICOLON;
    st->consumed = 0;
    st->flags = flags & (uint8_t)~kFlagBreak;
    return res;
}

static JsValue js_while(JsEngineState* st)
{
    uint8_t flags = st->flags, exe = !(flags & kFlagNoExec);
    JsValue res = mkundef(), v;
    st->consumed = 1;
    EXPECT(TOK_LPAREN, );
    JsOffset posCond = st->position;
    v = js_expr(st);
    if (is_err(v)) return v;
    EXPECT(TOK_RPAREN, );
    JsOffset posBody = st->position;
    uint8_t saved = st->flags | kFlagNoExec;
    st->flags = saved;
    v = js_block_or_stmt(st);
    st->flags = flags;
    if (is_err(v)) return v;
    JsOffset posEnd = st->position;
    while (!(flags & kFlagNoExec)) {
        st->flags = flags;
        st->position = posCond;
        st->consumed = 1;
        v = resolveprop(st, js_expr(st));
        if (is_err(v)) return v;
        if (!truthy(st, v)) break;
        st->position = posBody;
        st->consumed = 1;
        st->flags |= kFlagLoop;
        v = js_block_or_stmt(st);
        if (is_err(v)) return v;
        if (next(st) == TOK_RBRACE) st->consumed = 1;
        if (st->flags & kFlagBreak) break;
        if (st->flags & kFlagReturn) {
            res = v;
            break;
        }
        res = v;
    }
    if (!(st->flags & kFlagReturn)) {
        st->position = posEnd;
        st->token = TOK_SEMICOLON;
        st->consumed = 0;
        st->flags = flags & (uint8_t)~kFlagBreak;
    }
    return res;
}

static JsValue js_do(JsEngineState* st)
{
    uint8_t flags = st->flags, exe = !(flags & kFlagNoExec);
    JsValue res = mkundef(), v;
    st->consumed = 1;
    next(st);
    JsOffset posBody = st->tokenOffset;
    st->consumed = 0;
    st->flags |= kFlagNoExec;
    v = js_block_or_stmt(st);
    st->flags = flags;
    if (is_err(v)) return v;
    if (next(st) == TOK_RBRACE) st->consumed = 1;
    EXPECT(TOK_WHILE, );
    EXPECT(TOK_LPAREN, );
    JsOffset posCond = st->position;
    v = js_expr(st);
    if (is_err(v)) return v;
    EXPECT(TOK_RPAREN, );
    JsOffset posEnd = st->position;
    if (exe) {
        for (;;) {
            st->position = posBody;
            st->consumed = 1;
            st->flags = flags | kFlagLoop;
            v = js_block_or_stmt(st);
            if (is_err(v)) return v;
            if (next(st) == TOK_RBRACE) st->consumed = 1;
            if (st->flags & kFlagBreak) break;
            if (st->flags & kFlagReturn) {
                res = v;
                break;
            }
            res = v;
            st->flags = flags;
            st->position = posCond;
            st->consumed = 1;
            v = resolveprop(st, js_expr(st));
            if (is_err(v)) return v;
            if (!truthy(st, v)) break;
        }
    }
    if (!(st->flags & kFlagReturn)) {
        st->position = posEnd;
        st->consumed = 1;
        next(st);
        st->consumed = 0;
        st->flags = flags & (uint8_t)~kFlagBreak;
    }
    return res;
}

static JsValue js_try(JsEngineState* st)
{
    uint8_t flags = st->flags;
    JsValue res = mkundef(), fin = mkundef();
    st->consumed = 1;
    EXPECT(TOK_LBRACE, );
    st->flags = flags | kFlagInTry;
    res = js_block(st, true);
    if (next(st) == TOK_RBRACE) st->consumed = 1;
    bool hadErr = is_err(res);
    JsValue err = res;
    if (hadErr) res = mkundef();
    st->flags = flags;
    if (hadErr && lookahead(st) == TOK_CATCH) {
        st->consumed = 1;
        next(st);
        st->consumed = 1;
        EXPECT(TOK_LPAREN, );
        EXPECT(TOK_IDENTIFIER, );
        char* ename = (char*)&st->code[st->tokenOffset];
        JsOffset elen = st->tokenLen;
        st->consumed = 1;
        EXPECT(TOK_RPAREN, );
        EXPECT(TOK_LBRACE, );
        mkscope(st);
        const char* msg = st->errMsg + 7;
        setprop(st, st->scope, mkstr(st, ename, elen), mkstr(st, msg, strlen(msg)));
        st->consumed = 1;
        res = js_block(st, false);
        if (next(st) == TOK_RBRACE) st->consumed = 1;
        delscope(st);
        if (is_err(res)) return res;
    } else if (hadErr) {
        return err;
    } else if (lookahead(st) == TOK_CATCH) {
        st->consumed = 1;
        next(st);
        st->consumed = 1;
        EXPECT(TOK_LPAREN, );
        EXPECT(TOK_IDENTIFIER, );
        st->consumed = 1;
        EXPECT(TOK_RPAREN, );
        EXPECT(TOK_LBRACE, );
        st->flags |= kFlagNoExec;
        js_block(st, false);
        if (next(st) == TOK_RBRACE) st->consumed = 1;
        st->flags = flags;
    }
    if (lookahead(st) == TOK_FINALLY) {
        st->consumed = 1;
        next(st);
        st->consumed = 1;
        EXPECT(TOK_LBRACE, );
        fin = js_block(st, true);
        if (next(st) == TOK_RBRACE) st->consumed = 1;
        if (is_err(fin)) return fin;
    }
    st->consumed = 1;
    next(st);
    st->consumed = 0;
    return res;
}

static JsValue js_throw(JsEngineState* st)
{
    st->consumed = 1;
    JsValue v = resolveprop(st, js_expr(st));
    size_t n = cpy(st->errMsg, sizeof(st->errMsg), "ERROR: ", 7);
    if (vtype(v) == T_STR) {
        JsOffset slen, off = vstr(st, v, &slen);
        snprintf(st->errMsg + n, sizeof(st->errMsg) - n, "%.*s", (int)slen,
                 (char*)&st->memory[off]);
    } else {
        const char* msg = strValue(st, v);
        snprintf(st->errMsg + n, sizeof(st->errMsg) - n, "%s", msg);
    }
    st->errMsg[sizeof(st->errMsg) - 1] = '\0';
    if (!(st->flags & kFlagInTry)) {
        st->position = st->codeLen;
        st->token = TOK_EOF;
    }
    st->consumed = 0;
    return mkval(T_ERR, 0);
}

static JsValue js_break(JsEngineState* st)
{
    if (st->flags & kFlagNoExec) {
    } else {
        if (!(st->flags & (kFlagLoop | kFlagSwitch))) return mkerr(st, "not in loop");
        st->flags |= kFlagBreak | kFlagNoExec;
    }
    st->consumed = 1;
    return mkundef();
}

static JsValue js_continue(JsEngineState* st)
{
    if (st->flags & kFlagNoExec) {
    } else {
        if (!(st->flags & kFlagLoop)) return mkerr(st, "not in loop");
        st->flags |= kFlagNoExec;
    }
    st->consumed = 1;
    return mkundef();
}

static JsValue js_return(JsEngineState* st)
{
    uint8_t exe = !(st->flags & kFlagNoExec);
    st->consumed = 1;
    if (exe && !(st->flags & kFlagCall)) return mkerr(st, "not in func");
    if (next(st) == TOK_SEMICOLON) {
        if (exe) {
            st->position = st->codeLen;
            st->flags |= kFlagReturn;
        }
        return mkundef();
    }
    JsValue res = resolveprop(st, js_expr(st));
    if (exe) {
        st->position = st->codeLen;  // Shift to the end - exit the code snippet
        st->flags |= kFlagReturn;    // Tell caller we've executed
    }
    return resolveprop(st, res);
}

static JsValue js_stmt_finish(JsEngineState* st)
{
    uint8_t t = next(st);
    if (t == TOK_SEMICOLON || t == TOK_EOF || t == TOK_RBRACE) {
        st->consumed = 1;
        return mkundef();
    }
    if (linebreak_before_token(st)) {
        st->consumed = 0;
        return mkundef();
    }
    return mkerr(st, "; expected");
}

static JsValue js_stmt(JsEngineState* st)
{
    JsValue res;
    // JsOffset pos = st->position - st->tokenLen;
    if (st->break_ > st->gcThreshold && !(st->flags & kFlagNative)) runGc(st);
    switch (next(st)) {  // clang-format off
    case TOK_CASE: case TOK_CATCH: case TOK_CLASS:
    case TOK_DEFAULT: case TOK_DELETE: case TOK_FINALLY:
    case TOK_IN: case TOK_INSTANCEOF: case TOK_NEW:
    case TOK_THIS: case TOK_VAR: case TOK_VOID:
    case TOK_WITH: case TOK_YIELD:
      res = mkerr(st, "'%.*s' not implemented", (int) st->tokenLen, st->code + st->tokenOffset);
      break;
    case TOK_CONTINUE:  res = js_continue(st); break;
    case TOK_BREAK:     res = js_break(st); break;
    case TOK_LET:       res = js_bind(st, false); break;
    case TOK_CONST:     res = js_bind(st, true); break;
    case TOK_FUNC: {
        st->consumed = 1;
        if (next(st) == TOK_IDENTIFIER) {
            ParserTokenState snapshot = parser_state_save(st);
            st->consumed = 1;
            uint8_t t = next(st);
            parser_state_restore(st, snapshot);
            st->consumed = 0;
            if (t == TOK_LPAREN)
                res = js_func_decl(st);
            else {
                st->consumed = 0;
                res = resolveprop(st, js_func_literal(st));
            }
        } else if (st->token == TOK_LPAREN) {
            res = resolveprop(st, js_func_body(st));
        } else {
            st->consumed = 0;
            res = resolveprop(st, js_func_literal(st));
        }
        break;
    }
    case TOK_IF:        res = js_if(st); break;
    case TOK_LBRACE:    res = js_block(st, !(st->flags & kFlagNoExec)); break;
    case TOK_FOR:       res = js_for(st); break;
    case TOK_WHILE:     res = js_while(st); break;
    case TOK_DO:        res = js_do(st); break;
    case TOK_SWITCH:    res = js_switch(st); break;
    case TOK_TRY:       res = js_try(st); break;
    case TOK_THROW:     res = js_throw(st); break;
    case TOK_RETURN:    res = js_return(st); break;
    default:            res = resolveprop(st, js_expr(st)); break;
  }
  //printf("STMT [%.*s] -> %s, tok %d, flags %d\n", (int) (st->position - pos), &st->code[pos], strValue(st, res), next(st), st->flags);
  {
    JsValue fin = js_stmt_finish(st);
    if (is_err(fin)) return fin;
  }
    // clang-format on
    return res;
}



// Return mem offset and length of the JS string
JsOffset internal::vstr(JsEngineState* st, JsValue value, JsOffset* len)
{
    JsOffset off = (JsOffset)vdata(value);
    if (len) *len = offtolen(loadoff(st, off));
    return (JsOffset)(off + sizeof(off));
}

void internal::fmt_err(JsEngineState* st, const char* fmt, va_list ap)
{
    size_t n = cpy(st->errMsg, sizeof(st->errMsg), "ERROR: ", 7);
    vsnprintf(st->errMsg + n, sizeof(st->errMsg) - n, fmt, ap);
    st->errMsg[sizeof(st->errMsg) - 1] = '\0';
    if (st->code != NULL && st->codeLen > 0) {
        int line = 1, col = 1;
        err_line_col(st, err_offset(st), &line, &col);
        n = strlen(st->errMsg);
        snprintf(st->errMsg + n, sizeof(st->errMsg) - n, " at line %d col %d", line, col);
        st->errMsg[sizeof(st->errMsg) - 1] = '\0';
    }
}

JsValue internal::mkerr(JsEngineState* st, const char* xx, ...)
{
    va_list ap;
    va_start(ap, xx);
    fmt_err(st, xx, ap);
    va_end(ap);
    if (!(st->flags & kFlagInTry)) {
        st->position = st->codeLen;
        st->token = TOK_EOF;
    }
    st->consumed = 0;
    return mkval(T_ERR, 0);
}

// Stringify JS value into a free JS memory block
const char* internal::strValue(JsEngineState* st, JsValue value)
{
    // Leave JsOffset placeholder between st->break_ and a stringify buffer,
    // in case if next step is convert it into a JS variable
    char* buf = (char*)&st->memory[st->break_ + sizeof(JsOffset)];
    size_t len, available = st->memSize - st->break_ - sizeof(JsOffset);
    if (is_err(value)) return st->errMsg;
    if (st->break_ + sizeof(JsOffset) >= st->memSize) return "";
    len = tostr(st, value, buf, available);
    mkstr(st, NULL, len);
    return buf;
}

bool internal::truthy(JsEngineState* st, JsValue v)
{
    uint8_t t = vtype(v);
    return (t == T_BOOL && vdata(v) != 0) || (t == T_NUM && tod(v) != 0.0) ||
           (t == T_OBJ || t == T_FUNC) || (t == T_STR && vstrlen(st, v) > 0);
}

JsValue internal::mkstr(JsEngineState* st, const void* ptr, size_t len)
{
    JsOffset n = (JsOffset)(len + 1);
    // printf("MKSTR %u %u\n", n, st->break_);
    return mkentity(st, (JsOffset)((n << 2) | T_STR), ptr, n);
}

JsValue internal::mkobj(JsEngineState* st, JsOffset parent)
{
    return mkentity(st, 0 | T_OBJ, &parent, sizeof(parent));
}

JsValue internal::setprop(JsEngineState* st, JsValue obj, JsValue k, JsValue v, JsOffset pflags)
{
    JsOffset koff = (JsOffset)vdata(k);         // Key offset
    JsOffset b, head = (JsOffset)vdata(obj);    // Property list head
    char buf[sizeof(koff) + sizeof(v)];         // Property memory layout
    memcpy(&b, &st->memory[head], sizeof(b));   // Load current 1st prop offset
    memcpy(buf, &koff, sizeof(koff));           // Initialize prop data: copy key
    memcpy(buf + sizeof(koff), &v, sizeof(v));  // Copy value
    JsOffset brk = st->break_ | T_OBJ;          // New prop offset
    // printf("PROP: %u -> %u\n", b, brk);
    JsValue res = mkentity(st, (b & kPropLinkMask) | T_PROP | pflags, buf, sizeof(buf));
    if (!is_err(res)) memcpy(&st->memory[head], &brk,
                             sizeof(brk));  // Repoint head to the new prop
    return res;
}

void internal::runGc(JsEngineState* st)
{
    // printf("================== GC %u\n", st->noGc);
    setlwm(st);
    if (st->noGc == (JsOffset)~0) return;  // ~0 is a special case: GC Is disabled
    js_mark_all_entities_for_deletion(st);
    js_unmark_used_entities(st);
    js_delete_marked_entities(st);
}

bool internal::streq(const char* buf, size_t len, const char* p, size_t n)
{
    return n == len && memcmp(buf, p, len) == 0;
}

JsValue internal::resolveprop(JsEngineState* st, JsValue v)
{
    if (vtype(v) != T_PROP) {
        return v;
    }
    return resolveprop(st, loadval(st, (JsOffset)(vdata(v) + sizeof(JsOffset) * 2)));
}

double internal::js_to_number(JsEngineState* st, JsValue v)
{
    v = resolveprop(st, v);
    uint8_t t = vtype(v);
    if (t == T_NUM) return tod(v);
    if (t == T_BOOL) return vdata(v) ? 1.0 : 0.0;
    if (t == T_NULL || t == T_UNDEF) return 0.0;
    if (t == T_STR) {
        JsOffset slen, off = vstr(st, v, &slen);
        char* end = nullptr;
        return strtod((char*)&st->memory[off], &end);
    }
    return NAN;
}

JsOffset internal::loadoff(JsEngineState* st, JsOffset off)
{
    JsOffset v = 0;
    assert(st->break_ <= st->memSize);
    memcpy(&v, &st->memory[off], sizeof(v));
    return v;
}
JsValue internal::loadval(JsEngineState* st, JsOffset off)
{
    JsValue v = 0;
    memcpy(&v, &st->memory[off], sizeof(v));
    return v;
}
JsValue internal::js_invoke(JsEngineState* st, JsValue func, JsValue thisArg, JsValue* args, int argc)
{
    func = resolveprop(st, func);
    JsValue recv = st->callThis;
    st->callThis = thisArg;
    JsValue res = mkundef();
    if (vtype(func) == T_CFUNC) {
        res = reinterpret_cast<JsEngine::NativeFunction>(vdata(func))(
            reinterpret_cast<JsEngine*>(st), args, argc);
    } else if (vtype(func) == T_FUNC) {
        JsOffset fnlen = 0, fnoff = func_vstr(st, func, &fnlen);
        JsOffset nogc = st->noGc;
        st->noGc = (JsOffset)(fnoff - sizeof(JsOffset));
        res = invoke_js_fn(st, func, (const char*)(&st->memory[fnoff]), fnlen, args, argc);
        st->noGc = nogc;
    } else {
        res = mkerr(st, "not a function");
    }
    st->callThis = recv;
    return res;
}

bool internal::js_same_value_zero(JsEngineState* st, JsValue a, JsValue b)
{
    a = resolveprop(st, a);
    b = resolveprop(st, b);
    uint8_t ta = vtype(a), tb = vtype(b);
    if (ta != tb) return false;
    if (ta == T_NUM) {
        double da = tod(a), db = tod(b);
        if (std::isnan(da) && std::isnan(db)) return true;
        return da == db;
    }
    if (ta == T_STR) {
        JsOffset n1 = 0, off1 = vstr(st, a, &n1);
        JsOffset n2 = 0, off2 = vstr(st, b, &n2);
        return n1 == n2 && std::memcmp(&st->memory[off1], &st->memory[off2], n1) == 0;
    }
    if (ta == T_BOOL) return vdata(a) == vdata(b);
    if (ta == T_UNDEF || ta == T_NULL) return true;
    if (ta == T_OBJ) return vdata(a) == vdata(b);
    return false;
}

JsValue internal::js_eval(JsEngine* st, const char* buf, size_t len)
{
    // printf("EVAL: [%.*s]\n", (int) len, buf);
    JsValue res = mkundef();
    if (len == (size_t)-1 || len == (size_t)~0U) len = strlen(buf);
    st->consumed = 1;
    st->token = TOK_ERR;
    st->code = buf;
    st->codeLen = (JsOffset)len;
    st->position = 0;
    st->cStackPtr = &res;
    while (next(st) != TOK_EOF && !is_err(res) && !(st->flags & kFlagReturn)) {
        res = js_stmt(st);
    }
    return res;
}


}  // namespace Emjs
