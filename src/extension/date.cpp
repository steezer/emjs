#include "date.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace Emjs {
namespace {

constexpr const char kDefaultFormat[] = "%Y-%m-%d %H:%i:%s";

std::string translate_format(const char* fmt)
{
    std::string out;
    if (fmt == nullptr) return out;
    for (size_t i = 0; fmt[i] != '\0'; i++) {
        if (fmt[i] == '%' && fmt[i + 1] != '\0') {
            char n = fmt[i + 1];
            if (n == '%') {
                out += "%%";
                i++;
            } else if (n == 'i') {
                out += "%M";
                i++;
            } else if (n == 's') {
                out += "%S";
                i++;
            } else {
                out += '%';
                out += n;
                i++;
            }
        } else {
            out += fmt[i];
        }
    }
    return out;
}

int64_t current_timestamp_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool read_format_arg(JsEngine* js, JsValue v, std::string& out)
{
    if (JsEngine::getType(v) != JsValueType::String) return false;
    std::size_t len = 0;
    char* s = js->getString(v, &len);
    if (s == nullptr) return false;
    out.assign(s, len);
    return true;
}

void local_tm_from_ms(int64_t timestamp_ms, struct tm* out)
{
    time_t sec = static_cast<time_t>(timestamp_ms / 1000);
#if defined(_WIN32)
    localtime_s(out, &sec);
#else
    localtime_r(&sec, out);
#endif
}

}  // namespace

void EDate::bind(JsEngine* js)
{
    JsValue obj = js->makeObject();
    js->set(obj, "now", JsEngine::makeFunction(EDate::now));
    js->set(obj, "format", JsEngine::makeFunction(EDate::format));
    js->set(js->glob(), "Date", obj);
}

JsValue EDate::now(JsEngine* js, JsValue* args, int nargs)
{
    (void)args;
    (void)nargs;
    return JsEngine::makeNumber(static_cast<double>(current_timestamp_ms()));
}

JsValue EDate::format(JsEngine* js, JsValue* args, int nargs)
{
    if (nargs > 2) return js->makeError("bad call");

    int64_t timestamp_ms = current_timestamp_ms();
    std::string fmt_storage;
    const char* fmt_input = kDefaultFormat;

    if (nargs == 0) {
        // current time + default format
    } else if (nargs == 1) {
        if (JsEngine::getType(args[0]) == JsValueType::String) {
            if (!read_format_arg(js, args[0], fmt_storage)) return js->makeError("type mismatch");
            fmt_input = fmt_storage.c_str();
        } else if (JsEngine::getType(args[0]) == JsValueType::Number) {
            timestamp_ms = static_cast<int64_t>(JsEngine::getNumber(args[0]));
        } else {
            return js->makeError("type mismatch");
        }
    } else {
        if (!JsEngine::chkArgs(args, nargs, "ds")) return js->makeError("type mismatch");
        timestamp_ms = static_cast<int64_t>(JsEngine::getNumber(args[0]));
        if (!read_format_arg(js, args[1], fmt_storage)) return js->makeError("type mismatch");
        fmt_input = fmt_storage.c_str();
    }

    const std::string strftime_fmt = translate_format(fmt_input);

    struct tm tm_buf {};
    local_tm_from_ms(timestamp_ms, &tm_buf);

    char buf[256];
    std::memset(buf, 0, sizeof(buf));
    if (std::strftime(buf, sizeof(buf), strftime_fmt.c_str(), &tm_buf) == 0) {
        return js->makeError("format failed");
    }
    return js->makeString(buf, std::strlen(buf));
}

}  // namespace Emjs
