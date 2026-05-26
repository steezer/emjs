#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "internal.h"

namespace Emjs {
namespace internal {

static JsValue string_charCodeAt(JsEngineState* st, JsValue str, JsValue* args, int argc)
{
    if (vtype(str) != T_STR) return mkerr(st, "not string");
    if (argc < 1) return mkerr(st, "bad call");
    JsValue idxv = resolveprop(st, args[0]);
    if (is_err(idxv)) return idxv;
    double idx = js_to_number(st, idxv);
    JsOffset slen, off = vstr(st, str, &slen);
    if (idx < 0 || idx >= (double)slen) return tov(NAN);
    return tov((double)(unsigned char)st->memory[off + (JsOffset)idx]);
}

static JsValue string_charAt(JsEngineState* st, JsValue str, JsValue* args, int argc)
{
    if (vtype(str) != T_STR) return mkerr(st, "not string");
    if (argc < 1) return mkerr(st, "bad call");
    JsValue idxv = resolveprop(st, args[0]);
    if (is_err(idxv)) return idxv;
    double idx = js_to_number(st, idxv);
    JsOffset slen, off = vstr(st, str, &slen);
    if (idx < 0 || idx >= (double)slen) return mkstr(st, "", 0);
    return mkstr(st, &st->memory[off + (JsOffset)idx], 1);
}

static JsOffset string_clamp_index(double idx, JsOffset slen)
{
    if (std::isnan(idx) || idx < 0) return 0;
    if (idx >= (double)slen) return slen;
    return (JsOffset)idx;
}

static JsValue string_substring(JsEngineState* st, JsValue str, JsValue* args, int argc)
{
    if (vtype(str) != T_STR) return mkerr(st, "not string");
    if (argc < 1) return mkerr(st, "bad call");

    JsOffset slen, off = vstr(st, str, &slen);

    JsValue startv = resolveprop(st, args[0]);
    if (is_err(startv)) return startv;
    double start = js_to_number(st, startv);

    double end = (double)slen;
    if (argc >= 2) {
        JsValue endv = resolveprop(st, args[1]);
        if (is_err(endv)) return endv;
        end = js_to_number(st, endv);
    }

    JsOffset istart = string_clamp_index(start, slen);
    JsOffset iend = string_clamp_index(end, slen);
    if (istart > iend) {
        JsOffset tmp = istart;
        istart = iend;
        iend = tmp;
    }
    if (istart >= slen || iend <= istart) return mkstr(st, "", 0);
    return mkstr(st, &st->memory[off + istart], iend - istart);
}

static JsValue string_indexOf(JsEngineState* st, JsValue str, JsValue* args, int argc)
{
    if (vtype(str) != T_STR) return mkerr(st, "not string");
    if (argc < 1) return mkerr(st, "bad call");
    JsValue needlev = resolveprop(st, args[0]);
    if (is_err(needlev)) return needlev;
    if (vtype(needlev) != T_STR) return mkerr(st, "type mismatch");

    double from = 0;
    if (argc >= 2) {
        JsValue fromv = resolveprop(st, args[1]);
        if (is_err(fromv)) return fromv;
        from = js_to_number(st, fromv);
    }

    JsOffset hay_len, hay_off = vstr(st, str, &hay_len);
    JsOffset needle_len, needle_off = vstr(st, needlev, &needle_len);

    if (needle_len == 0) {
        JsOffset pos = from < 0 ? 0 : (JsOffset)from;
        if (pos > hay_len) pos = hay_len;
        return tov((double)pos);
    }

    if (from < 0) from = 0;
    if (from >= (double)hay_len) return tov(-1);

    const char* hay = (const char*)&st->memory[hay_off];
    const char* needle = (const char*)&st->memory[needle_off];
    for (JsOffset i = (JsOffset)from; i + needle_len <= hay_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) return tov((double)i);
    }
    return tov(-1);
}

static inline JsEngineState* engine_state(JsEngine* js)
{
    return reinterpret_cast<JsEngineState*>(js);
}

static JsValue call_this_with_args(
    JsEngine* js,
    JsValue (*fn)(JsEngineState*, JsValue, JsValue*, int),
    JsValue* args,
    int n)
{
    JsEngineState* st = engine_state(js);
    return fn(st, js->callThis, args, n);
}

static JsValue call_this_no_args(JsEngine* js, JsValue (*fn)(JsEngineState*, JsValue))
{
    JsEngineState* st = engine_state(js);
    return fn(st, js->callThis);
}

static JsValue string_charCodeAt_fn(JsEngine* js, JsValue* args, int n)
{
    return call_this_with_args(js, string_charCodeAt, args, n);
}

static JsValue string_charAt_fn(JsEngine* js, JsValue* args, int n)
{
    return call_this_with_args(js, string_charAt, args, n);
}

static JsValue string_indexOf_fn(JsEngine* js, JsValue* args, int n)
{
    return call_this_with_args(js, string_indexOf, args, n);
}

static JsValue string_substring_fn(JsEngine* js, JsValue* args, int n)
{
    return call_this_with_args(js, string_substring, args, n);
}

constexpr JsOffset kPropLinkMask = 0x3FFFFFFCU;
constexpr JsOffset kGcMask = ~(((JsOffset)~0) >> 1);

void saveval(JsEngineState* st, JsOffset off, JsValue val)
{
    std::memcpy(&st->memory[off], &val, sizeof(val));
}

JsOffset prop_lookup(JsEngineState* st, JsValue obj, const char* key, std::size_t key_len)
{
    JsOffset off = loadoff(st, (JsOffset)vdata(obj)) & kPropLinkMask;
    while (off < st->break_ && off != 0) {
        JsOffset koff = loadoff(st, (JsOffset)(off + sizeof(JsOffset)));
        JsOffset klen = (loadoff(st, koff) >> 2) - 1;
        const char* p = (char*)&st->memory[koff + sizeof(koff)];
        if (streq(key, key_len, p, klen)) return off;
        off = loadoff(st, off) & kPropLinkMask;
    }
    return 0;
}

static double array_get_length(JsEngineState* st, JsValue arr)
{
    JsOffset off = prop_lookup(st, arr, "length", 6);
    if (off == 0) return 0;
    JsValue len = loadval(st, (JsOffset)(off + sizeof(JsOffset) * 2));
    return vtype(len) == T_NUM ? tod(len) : 0;
}

JsValue array_create(JsEngineState* st)
{
    JsValue arr = mkobj(st, 0);
    if (is_err(arr)) return arr;
    JsValue r = setprop(st, arr, mkstr(st, "length", 6), tov(0));
    if (is_err(r)) return r;
    return arr;
}

bool is_array_obj(JsEngineState* st, JsValue obj)
{
    if (vtype(obj) != T_OBJ) return false;
    JsOffset off = prop_lookup(st, obj, "length", 6);
    if (off == 0) return false;
    JsValue len = loadval(st, (JsOffset)(off + sizeof(JsOffset) * 2));
    return vtype(len) == T_NUM;
}

void array_set_length(JsEngineState* st, JsValue arr, double len)
{
    JsOffset off = prop_lookup(st, arr, "length", 6);
    if (off != 0) {
        saveval(st, (JsOffset)(off + sizeof(JsOffset) * 2), tov(len));
    } else {
        setprop(st, arr, mkstr(st, "length", 6), tov(len));
    }
}

JsValue array_owner_of_prop(JsEngineState* st, JsOffset prop_off)
{
    for (JsOffset n, v, off = 0; off < st->break_; off += n) {
        v = loadoff(st, off);
        n = esize(v & ~kGcMask);
        if ((v & 3) != T_OBJ) continue;
        JsValue obj = mkval(T_OBJ, off);
        if (!is_array_obj(st, obj)) continue;
        for (JsOffset p = loadoff(st, off) & kPropLinkMask; p != 0 && p < st->break_;) {
            if (p == prop_off) return obj;
            p = loadoff(st, p) & kPropLinkMask;
        }
    }
    return mkundef();
}

void array_update_length_for_index(JsEngineState* st, JsValue arr, double idx)
{
    if (idx < 0) return;
    double len = array_get_length(st, arr);
    if (idx + 1.0 > len) array_set_length(st, arr, idx + 1.0);
}

JsValue array_key_from_value(JsEngineState* st, JsValue keyval)
{
    keyval = resolveprop(st, keyval);
    uint8_t t = vtype(keyval);
    if (t == T_STR) return keyval;
    if (t == T_NUM) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.17g", tod(keyval));
        return mkstr(st, buf, std::strlen(buf));
    }
    return mkerr(st, "bad index");
}

JsValue array_bracket_op(JsEngineState* st, JsValue obj, JsValue keyval)
{
    JsValue key = array_key_from_value(st, keyval);
    if (is_err(key)) return key;
    if (vtype(obj) != T_OBJ) return mkerr(st, "lookup in non-obj");
    JsOffset koff = (JsOffset)vdata(key);
    JsOffset klen = (loadoff(st, koff) >> 2) - 1;
    const char* ptr = (char*)&st->memory[koff + sizeof(koff)];
    JsOffset off = prop_lookup(st, obj, ptr, klen);
    if (off == 0) {
        JsValue res = setprop(st, obj, key, mkundef());
        if (is_err(res)) return res;
        off = prop_lookup(st, obj, ptr, klen);
    }
    return mkval(T_PROP, off);
}

static bool array_require(JsEngineState* st, JsValue arr, double* len, JsValue* err)
{
    if (!is_array_obj(st, arr)) {
        if (err != nullptr) *err = mkerr(st, "not array");
        return false;
    }
    if (len != nullptr) *len = array_get_length(st, arr);
    return true;
}

static JsValue resolve_optional_arg(JsEngineState* st, JsValue* args, int argc, int idx)
{
    if (idx >= argc) return mkundef();
    return resolveprop(st, args[idx]);
}

static JsValue array_resolve_callback(
    JsEngineState* st,
    JsValue* args,
    int argc,
    JsValue* thisArg,
    const char* bad_call_msg)
{
    if (argc < 1) return mkerr(st, bad_call_msg);
    JsValue fn = resolveprop(st, args[0]);
    if (is_err(fn)) return fn;
    if (vtype(fn) != T_FUNC && vtype(fn) != T_CFUNC) return mkerr(st, "not a function");
    JsValue recv = resolve_optional_arg(st, args, argc, 1);
    if (is_err(recv)) return recv;
    if (thisArg != nullptr) *thisArg = recv;
    return fn;
}

static void array_index_key(char* buf, std::size_t bufsz, double idx);

static JsValue array_push(JsEngineState* st, JsValue arr, JsValue* args, int argc)
{
    JsValue err = mkundef();
    double len = 0;
    if (!array_require(st, arr, &len, &err)) return err;
    for (int i = 0; i < argc; i++) {
        char buf[32];
        array_index_key(buf, sizeof(buf), len);
        JsValue key = mkstr(st, buf, std::strlen(buf));
        JsValue v = setprop(st, arr, key, resolveprop(st, args[i]));
        if (is_err(v)) return v;
        len += 1.0;
    }
    array_set_length(st, arr, len);
    return tov(len);
}

static JsValue array_pop(JsEngineState* st, JsValue arr)
{
    JsValue err = mkundef();
    double len = 0;
    if (!array_require(st, arr, &len, &err)) return err;
    if (len <= 0) return mkundef();
    len -= 1.0;
    char buf[32];
    array_index_key(buf, sizeof(buf), len);
    JsOffset off = prop_lookup(st, arr, buf, std::strlen(buf));
    JsValue res = mkundef();
    if (off != 0) res = resolveprop(st, loadval(st, (JsOffset)(off + sizeof(JsOffset) * 2)));
    array_set_length(st, arr, len);
    return res;
}

static void array_index_key(char* buf, std::size_t bufsz, double idx)
{
    std::snprintf(buf, bufsz, "%.17g", idx);
}

static JsValue array_get_elem(JsEngineState* st, JsValue arr, double idx)
{
    char buf[32];
    array_index_key(buf, sizeof(buf), idx);
    JsOffset off = prop_lookup(st, arr, buf, std::strlen(buf));
    if (off == 0) return mkundef();
    return resolveprop(st, loadval(st, (JsOffset)(off + sizeof(JsOffset) * 2)));
}

static JsValue array_set_elem(JsEngineState* st, JsValue arr, double idx, JsValue val)
{
    char buf[32];
    array_index_key(buf, sizeof(buf), idx);
    JsValue key = mkstr(st, buf, std::strlen(buf));
    JsValue r = setprop(st, arr, key, resolveprop(st, val));
    if (is_err(r)) return r;
    array_update_length_for_index(st, arr, idx);
    return mkundef();
}

static double array_relative_index(double len, double idx)
{
    if (idx < 0) idx = len + idx;
    if (idx < 0) idx = 0;
    if (idx > len) idx = len;
    return idx;
}

static JsValue array_cb(JsEngineState* st, JsValue arr, JsValue fn, JsValue thisArg, double idx,
                        JsValue elem)
{
    JsValue args[3] = {elem, tov(idx), arr};
    return js_invoke(st, fn, thisArg, args, 3);
}

static JsValue array_shift(JsEngineState* st, JsValue arr)
{
    JsValue err = mkundef();
    double len = 0;
    if (!array_require(st, arr, &len, &err)) return err;
    if (len <= 0) return mkundef();
    JsValue res = array_get_elem(st, arr, 0);
    for (double i = 1; i < len; i++) {
        JsValue r = array_set_elem(st, arr, i - 1.0, array_get_elem(st, arr, i));
        if (is_err(r)) return r;
    }
    array_set_length(st, arr, len - 1.0);
    return res;
}

static JsValue array_slice(JsEngineState* st, JsValue arr, JsValue* args, int argc)
{
    JsValue err = mkundef();
    double len = 0;
    if (!array_require(st, arr, &len, &err)) return err;
    double start = 0, end = len;
    if (argc >= 1) {
        JsValue sv = resolveprop(st, args[0]);
        if (is_err(sv)) return sv;
        start = array_relative_index(len, js_to_number(st, sv));
    }
    if (argc >= 2) {
        JsValue ev = resolveprop(st, args[1]);
        if (is_err(ev)) return ev;
        end = array_relative_index(len, js_to_number(st, ev));
    }
    if (start > end) start = end;
    JsValue out = array_create(st);
    if (is_err(out)) return out;
    double j = 0;
    for (double i = start; i < end; i++, j++) {
        JsValue r = array_set_elem(st, out, j, array_get_elem(st, arr, i));
        if (is_err(r)) return r;
    }
    array_set_length(st, out, j);
    return out;
}

static JsValue array_reverse(JsEngineState* st, JsValue arr)
{
    JsValue err = mkundef();
    double len = 0;
    if (!array_require(st, arr, &len, &err)) return err;
    for (double lo = 0, hi = len - 1.0; lo < hi; lo++, hi--) {
        JsValue a = array_get_elem(st, arr, lo);
        JsValue b = array_get_elem(st, arr, hi);
        JsValue r1 = array_set_elem(st, arr, lo, b);
        if (is_err(r1)) return r1;
        JsValue r2 = array_set_elem(st, arr, hi, a);
        if (is_err(r2)) return r2;
    }
    return arr;
}

static int array_default_compare(JsEngineState* st, JsValue a, JsValue b)
{
    JsValue sa = js_to_string(st, a);
    if (is_err(sa)) return 0;
    JsValue sb = js_to_string(st, b);
    if (is_err(sb)) return 0;
    JsOffset n1 = 0, off1 = vstr(st, sa, &n1);
    JsOffset n2 = 0, off2 = vstr(st, sb, &n2);
    const char* p1 = (const char*)&st->memory[off1];
    const char* p2 = (const char*)&st->memory[off2];
    std::size_t i = 0;
    while (i < n1 && i < n2) {
        if (p1[i] != p2[i]) return p1[i] < p2[i] ? -1 : 1;
        i++;
    }
    if (n1 < n2) return -1;
    if (n1 > n2) return 1;
    return 0;
}

static JsValue array_sort(JsEngineState* st, JsValue arr, JsValue* args, int argc)
{
    JsValue err = mkundef();
    double len = 0;
    if (!array_require(st, arr, &len, &err)) return err;
    if (len <= 1) return arr;

    JsValue cmpfn = mkundef();
    if (argc >= 1) {
        cmpfn = resolveprop(st, args[0]);
        if (is_err(cmpfn)) return cmpfn;
        if (vtype(cmpfn) != T_UNDEF && vtype(cmpfn) != T_FUNC && vtype(cmpfn) != T_CFUNC) {
            return mkerr(st, "bad compare");
        }
    }

    JsOffset n = (JsOffset)len;
    JsValue* buf = (JsValue*)std::malloc(sizeof(JsValue) * n);
    if (buf == nullptr) return mkerr(st, "oom");
    for (JsOffset i = 0; i < n; i++) buf[i] = array_get_elem(st, arr, (double)i);

    for (JsOffset i = 0; i < n; i++) {
        for (JsOffset j = i + 1; j < n; j++) {
            int c = 0;
            if (vtype(cmpfn) == T_UNDEF) {
                c = array_default_compare(st, buf[i], buf[j]);
            } else {
                JsValue cargs[2] = {buf[i], buf[j]};
                JsValue cr = js_invoke(st, cmpfn, mkundef(), cargs, 2);
                if (is_err(cr)) {
                    std::free(buf);
                    return cr;
                }
                c = (int)js_to_number(st, cr);
                if (std::isnan((double)c)) c = 0;
            }
            if (c > 0) {
                JsValue tmp = buf[i];
                buf[i] = buf[j];
                buf[j] = tmp;
            }
        }
    }

    for (JsOffset i = 0; i < n; i++) {
        JsValue r = array_set_elem(st, arr, (double)i, buf[i]);
        if (is_err(r)) {
            std::free(buf);
            return r;
        }
    }
    std::free(buf);
    return arr;
}

static JsValue array_splice(JsEngineState* st, JsValue arr, JsValue* args, int argc)
{
    JsValue err = mkundef();
    double len = 0;
    if (!array_require(st, arr, &len, &err)) return err;
    if (argc < 1) return mkerr(st, "bad call");

    JsValue sv = resolveprop(st, args[0]);
    if (is_err(sv)) return sv;
    double start = js_to_number(st, sv);
    if (start < 0) {
        start = len + start;
        if (start < 0) start = 0;
    } else if (start > len) {
        start = len;
    }

    double del = len - start;
    if (argc >= 2) {
        JsValue dv = resolveprop(st, args[1]);
        if (is_err(dv)) return dv;
        del = js_to_number(st, dv);
        if (del < 0) del = 0;
        if (start + del > len) del = len - start;
    }

    int insertc = argc > 2 ? argc - 2 : 0;
    JsValue removed = array_create(st);
    if (is_err(removed)) return removed;
    for (double i = 0; i < del; i++) {
        JsValue r = array_set_elem(st, removed, i, array_get_elem(st, arr, start + i));
        if (is_err(r)) return r;
    }
    array_set_length(st, removed, del);

    JsOffset tail = (JsOffset)(len - start - del);
    JsValue* tailbuf = nullptr;
    if (tail > 0) {
        tailbuf = (JsValue*)std::malloc(sizeof(JsValue) * tail);
        if (tailbuf == nullptr) return mkerr(st, "oom");
        for (JsOffset i = 0; i < tail; i++) {
            tailbuf[i] = array_get_elem(st, arr, start + del + (double)i);
        }
    }

    for (int i = 0; i < insertc; i++) {
        JsValue r = array_set_elem(st, arr, start + (double)i, resolveprop(st, args[i + 2]));
        if (is_err(r)) {
            if (tailbuf) std::free(tailbuf);
            return r;
        }
    }
    for (JsOffset i = 0; i < tail; i++) {
        JsValue r = array_set_elem(st, arr, start + (double)insertc + (double)i, tailbuf[i]);
        if (is_err(r)) {
            if (tailbuf) std::free(tailbuf);
            return r;
        }
    }
    if (tailbuf) std::free(tailbuf);
    array_set_length(st, arr, len - del + (double)insertc);
    return removed;
}

static JsValue array_forEach(JsEngineState* st, JsValue arr, JsValue* args, int argc)
{
    JsValue err = mkundef();
    double len = 0;
    if (!array_require(st, arr, &len, &err)) return err;
    JsValue thisArg = mkundef();
    JsValue fn = array_resolve_callback(st, args, argc, &thisArg, "bad call");
    if (is_err(fn)) return fn;
    for (double i = 0; i < len; i++) {
        JsValue r = array_cb(st, arr, fn, thisArg, i, array_get_elem(st, arr, i));
        if (is_err(r)) return r;
    }
    return mkundef();
}

static JsValue array_map(JsEngineState* st, JsValue arr, JsValue* args, int argc)
{
    JsValue err = mkundef();
    double len = 0;
    if (!array_require(st, arr, &len, &err)) return err;
    JsValue thisArg = mkundef();
    JsValue fn = array_resolve_callback(st, args, argc, &thisArg, "bad call");
    if (is_err(fn)) return fn;
    JsValue out = array_create(st);
    if (is_err(out)) return out;
    for (double i = 0; i < len; i++) {
        JsValue r = array_cb(st, arr, fn, thisArg, i, array_get_elem(st, arr, i));
        if (is_err(r)) return r;
        JsValue s = array_set_elem(st, out, i, r);
        if (is_err(s)) return s;
    }
    array_set_length(st, out, len);
    return out;
}

static JsValue array_every(JsEngineState* st, JsValue arr, JsValue* args, int argc)
{
    JsValue err = mkundef();
    double len = 0;
    if (!array_require(st, arr, &len, &err)) return err;
    JsValue thisArg = mkundef();
    JsValue fn = array_resolve_callback(st, args, argc, &thisArg, "bad call");
    if (is_err(fn)) return fn;
    for (double i = 0; i < len; i++) {
        JsValue r = array_cb(st, arr, fn, thisArg, i, array_get_elem(st, arr, i));
        if (is_err(r)) return r;
        if (!truthy(st, r)) return mkfalse();
    }
    return mktrue();
}

static JsValue array_some(JsEngineState* st, JsValue arr, JsValue* args, int argc)
{
    JsValue err = mkundef();
    double len = 0;
    if (!array_require(st, arr, &len, &err)) return err;
    JsValue thisArg = mkundef();
    JsValue fn = array_resolve_callback(st, args, argc, &thisArg, "bad call");
    if (is_err(fn)) return fn;
    for (double i = 0; i < len; i++) {
        JsValue r = array_cb(st, arr, fn, thisArg, i, array_get_elem(st, arr, i));
        if (is_err(r)) return r;
        if (truthy(st, r)) return mktrue();
    }
    return mkfalse();
}

static JsValue array_find(JsEngineState* st, JsValue arr, JsValue* args, int argc)
{
    JsValue err = mkundef();
    double len = 0;
    if (!array_require(st, arr, &len, &err)) return err;
    JsValue thisArg = mkundef();
    JsValue fn = array_resolve_callback(st, args, argc, &thisArg, "bad call");
    if (is_err(fn)) return fn;
    for (double i = 0; i < len; i++) {
        JsValue elem = array_get_elem(st, arr, i);
        JsValue r = array_cb(st, arr, fn, thisArg, i, elem);
        if (is_err(r)) return r;
        if (truthy(st, r)) return elem;
    }
    return mkundef();
}

static JsValue array_findIndex(JsEngineState* st, JsValue arr, JsValue* args, int argc)
{
    JsValue err = mkundef();
    double len = 0;
    if (!array_require(st, arr, &len, &err)) return err;
    JsValue thisArg = mkundef();
    JsValue fn = array_resolve_callback(st, args, argc, &thisArg, "bad call");
    if (is_err(fn)) return fn;
    for (double i = 0; i < len; i++) {
        JsValue r = array_cb(st, arr, fn, thisArg, i, array_get_elem(st, arr, i));
        if (is_err(r)) return r;
        if (truthy(st, r)) return tov(i);
    }
    return tov(-1.0);
}

static JsValue array_reduce(JsEngineState* st, JsValue arr, JsValue* args, int argc)
{
    JsValue err = mkundef();
    double len = 0;
    if (!array_require(st, arr, &len, &err)) return err;
    JsValue fn = array_resolve_callback(st, args, argc, nullptr, "bad call");
    if (is_err(fn)) return fn;
    double start = 0;
    JsValue acc;
    if (argc >= 2) {
        acc = resolveprop(st, args[1]);
        if (is_err(acc)) return acc;
    } else {
        if (len == 0) return mkerr(st, "reduce empty");
        acc = array_get_elem(st, arr, 0);
        start = 1.0;
    }

    for (double i = start; i < len; i++) {
        JsValue elem = array_get_elem(st, arr, i);
        JsValue rargs[4] = {acc, elem, tov(i), arr};
        acc = js_invoke(st, fn, mkundef(), rargs, 4);
        if (is_err(acc)) return acc;
    }
    return acc;
}

static JsValue array_indexOf(JsEngineState* st, JsValue arr, JsValue* args, int argc)
{
    JsValue err = mkundef();
    double len = 0;
    if (!array_require(st, arr, &len, &err)) return err;
    if (argc < 1) return mkerr(st, "bad call");
    JsValue needle = resolveprop(st, args[0]);
    if (is_err(needle)) return needle;

    double from = 0;
    if (argc >= 2) {
        JsValue fv = resolveprop(st, args[1]);
        if (is_err(fv)) return fv;
        from = js_to_number(st, fv);
    }

    if (from < 0) {
        from = len + from;
        if (from < 0) from = 0;
    }
    if (from >= len) return tov(-1.0);

    for (double i = from; i < len; i++) {
        if (js_same_value_zero(st, array_get_elem(st, arr, i), needle)) return tov(i);
    }
    return tov(-1.0);
}

static JsValue array_join(JsEngineState* st, JsValue arr, JsValue* args, int argc)
{
    JsValue err = mkundef();
    double len = 0;
    if (!array_require(st, arr, &len, &err)) return err;
    const char* sep = ",";
    std::size_t seplen = 1;
    if (argc >= 1) {
        JsValue sv = resolveprop(st, args[0]);
        if (is_err(sv)) return sv;
        if (vtype(sv) != T_UNDEF && vtype(sv) != T_NULL) {
            sv = js_to_string(st, sv);
            if (is_err(sv)) return sv;
            JsOffset slen = 0, soff = vstr(st, sv, &slen);
            sep = (const char*)&st->memory[soff];
            seplen = slen;
        }
    }

    std::size_t cap = 64;
    char* out = (char*)std::malloc(cap);
    if (out == nullptr) return mkerr(st, "oom");
    std::size_t n = 0;

    for (double i = 0; i < len; i++) {
        if (i > 0) {
            if (n + seplen + 1 > cap) {
                cap = (cap + seplen) * 2;
                char* p = (char*)std::realloc(out, cap);
                if (p == nullptr) {
                    std::free(out);
                    return mkerr(st, "oom");
                }
                out = p;
            }
            std::memcpy(out + n, sep, seplen);
            n += seplen;
        }
        JsValue sv = js_to_string(st, array_get_elem(st, arr, i));
        if (is_err(sv)) {
            std::free(out);
            return sv;
        }
        JsOffset plen = 0, poff = vstr(st, sv, &plen);
        const char* part = (const char*)&st->memory[poff];
        if (n + plen + 1 > cap) {
            cap = (cap + plen) * 2;
            char* p = (char*)std::realloc(out, cap);
            if (p == nullptr) {
                std::free(out);
                return mkerr(st, "oom");
            }
            out = p;
        }
        std::memcpy(out + n, part, plen);
        n += plen;
    }
    JsValue res = mkstr(st, out, n);
    std::free(out);
    return res;
}

static JsValue array_push_fn(JsEngine* js, JsValue* args, int n)
{
    return call_this_with_args(js, array_push, args, n);
}

static JsValue array_pop_fn(JsEngine* js, JsValue* args, int n)
{
    (void)args;
    (void)n;
    return call_this_no_args(js, array_pop);
}

static JsValue array_shift_fn(JsEngine* js, JsValue* args, int n)
{
    (void)args;
    (void)n;
    return call_this_no_args(js, array_shift);
}

static JsValue array_slice_fn(JsEngine* js, JsValue* args, int n)
{
    return call_this_with_args(js, array_slice, args, n);
}

static JsValue array_reverse_fn(JsEngine* js, JsValue* args, int n)
{
    (void)args;
    (void)n;
    return call_this_no_args(js, array_reverse);
}

static JsValue array_sort_fn(JsEngine* js, JsValue* args, int n)
{
    return call_this_with_args(js, array_sort, args, n);
}

static JsValue array_splice_fn(JsEngine* js, JsValue* args, int n)
{
    return call_this_with_args(js, array_splice, args, n);
}

static JsValue array_forEach_fn(JsEngine* js, JsValue* args, int n)
{
    return call_this_with_args(js, array_forEach, args, n);
}

static JsValue array_map_fn(JsEngine* js, JsValue* args, int n)
{
    return call_this_with_args(js, array_map, args, n);
}

static JsValue array_every_fn(JsEngine* js, JsValue* args, int n)
{
    return call_this_with_args(js, array_every, args, n);
}

static JsValue array_some_fn(JsEngine* js, JsValue* args, int n)
{
    return call_this_with_args(js, array_some, args, n);
}

static JsValue array_find_fn(JsEngine* js, JsValue* args, int n)
{
    return call_this_with_args(js, array_find, args, n);
}

static JsValue array_findIndex_fn(JsEngine* js, JsValue* args, int n)
{
    return call_this_with_args(js, array_findIndex, args, n);
}

static JsValue array_reduce_fn(JsEngine* js, JsValue* args, int n)
{
    return call_this_with_args(js, array_reduce, args, n);
}

static JsValue array_indexOf_fn(JsEngine* js, JsValue* args, int n)
{
    return call_this_with_args(js, array_indexOf, args, n);
}

static JsValue array_join_fn(JsEngine* js, JsValue* args, int n)
{
    return call_this_with_args(js, array_join, args, n);
}

struct NativeMethod {
    const char* name;
    std::size_t len;
    JsEngine::NativeFunction fn;
};

static JsValue lookup_native_method(
    const NativeMethod* methods,
    std::size_t count,
    const char* prop,
    std::size_t prop_len)
{
    for (std::size_t i = 0; i < count; i++) {
        if (streq(prop, prop_len, methods[i].name, methods[i].len)) {
            return mkval(T_CFUNC, (std::size_t)(void*)methods[i].fn);
        }
    }
    return mkundef();
}

}  // namespace internal

JsValue array_dot_op(JsEngineState* st, JsValue arr, const char* prop, std::size_t prop_len)
{
    using namespace internal;
    static const NativeMethod kArrayMethods[] = {
        {"push", 4, array_push_fn},         {"pop", 3, array_pop_fn},
        {"shift", 5, array_shift_fn},       {"slice", 5, array_slice_fn},
        {"reverse", 7, array_reverse_fn},   {"sort", 4, array_sort_fn},
        {"splice", 6, array_splice_fn},     {"forEach", 7, array_forEach_fn},
        {"map", 3, array_map_fn},           {"every", 5, array_every_fn},
        {"some", 4, array_some_fn},         {"find", 4, array_find_fn},
        {"findIndex", 9, array_findIndex_fn},
        {"reduce", 6, array_reduce_fn},     {"indexOf", 7, array_indexOf_fn},
        {"join", 4, array_join_fn},
    };
    if (!is_array_obj(st, arr)) return mkundef();
    if (streq(prop, prop_len, "length", 6)) return tov(array_get_length(st, arr));
    return lookup_native_method(kArrayMethods, sizeof(kArrayMethods) / sizeof(kArrayMethods[0]), prop, prop_len);
}

JsValue string_dot_op(JsEngineState* st, JsValue str, const char* prop, std::size_t prop_len)
{
    using namespace internal;
    static const NativeMethod kStringMethods[] = {
        {"charCodeAt", 10, string_charCodeAt_fn},
        {"charAt", 6, string_charAt_fn},
        {"indexOf", 7, string_indexOf_fn},
        {"substring", 9, string_substring_fn},
    };
    if (streq(prop, prop_len, "length", 6)) {
        return tov(offtolen(loadoff(st, (JsOffset)vdata(str))));
    }
    JsValue method =
        lookup_native_method(kStringMethods, sizeof(kStringMethods) / sizeof(kStringMethods[0]), prop, prop_len);
    if (vtype(method) != T_UNDEF) return method;
    return mkerr(st, "lookup in non-obj");
}

}// namespace Emjs



