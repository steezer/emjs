#include "console.h"

#include "../internal.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <unordered_map>

namespace Emjs {
namespace {

using namespace internal;

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

std::unordered_map<std::string, TimePoint>& timers()
{
    static std::unordered_map<std::string, TimePoint> table;
    return table;
}

static double console_array_len(JsEngineState* st, JsValue arr)
{
    JsOffset off = prop_lookup(st, arr, "length", 6);
    if (off == 0) return 0;
    JsValue len = loadval(st, (JsOffset)(off + sizeof(JsOffset) * 2));
    return vtype(len) == T_NUM ? tod(len) : 0;
}

static JsValue console_array_at(JsEngineState* st, JsValue arr, double idx)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.17g", idx);
    JsOffset off = prop_lookup(st, arr, buf, std::strlen(buf));
    if (off == 0) return mkundef();
    return resolveprop(st, loadval(st, (JsOffset)(off + sizeof(JsOffset) * 2)));
}

void append_value(JsEngine* js, std::string& out, JsValue value)
{
    switch (JsEngine::getType(value)) {
        case JsValueType::Undefined:
        case JsValueType::Null:
            out += "undefined";
            break;
        case JsValueType::True:
        case JsValueType::False:
            out += js->getBool(value) ? "true" : "false";
            break;
        case JsValueType::Number: {
            double dv = JsEngine::getNumber(value);
            double iv = 0.0;
            char buf[32];
            const char* fmt = (std::modf(dv, &iv) == 0.0) ? "%.17g" : "%g";
            std::snprintf(buf, sizeof(buf), fmt, dv);
            out += buf;
            break;
        }
        case JsValueType::String: {
            std::size_t len = 0;
            char* s = js->getString(value, &len);
            if (s != nullptr) out.append(s, len);
            break;
        }
        case JsValueType::Error:
            out += js->str(value);
            break;
        case JsValueType::Private: {
            auto* st = reinterpret_cast<JsEngineState*>(js);
            if (vtype(value) == T_OBJ && is_array_obj(st, value)) {
                out += '[';
                const double len = console_array_len(st, value);
                for (double i = 0; i < len; i++) {
                    if (i > 0) out += ',';
                    append_value(js, out, console_array_at(st, value, i));
                }
                out += ']';
            } else {
                out += js->str(value);
            }
            break;
        }
    }
}

std::string format_args(JsEngine* js, JsValue* args, int nargs)
{
    std::string result;
    for (int i = 0; i < nargs; i++) {
        if (i > 0) result += ' ';
        append_value(js, result, args[i]);
    }
    return result;
}

void print_line(const std::string& line)
{
    if (!line.empty()) std::cout << line << std::endl;
}

std::string timer_label(JsEngine* js, JsValue* args, int nargs)
{
    if (nargs == 0) return "default";
    std::string label;
    append_value(js, label, args[0]);
    return label.empty() ? "default" : label;
}

double elapsed_ms(const TimePoint& start)
{
    const auto delta = Clock::now() - start;
    return std::chrono::duration<double, std::milli>(delta).count();
}

}  // namespace

void EConsole::bind(JsEngine* js)
{
    JsValue obj = js->makeObject();
    js->set(obj, "log", JsEngine::makeFunction(EConsole::log));
    js->set(obj, "time", JsEngine::makeFunction(EConsole::time));
    js->set(obj, "timeEnd", JsEngine::makeFunction(EConsole::timeEnd));
    js->set(obj, "timeLog", JsEngine::makeFunction(EConsole::timeLog));
    js->set(js->glob(), "console", obj);
}

JsValue EConsole::log(JsEngine* js, JsValue* args, int nargs)
{
    print_line(format_args(js, args, nargs));
    return js->makeUndefined();
}

JsValue EConsole::time(JsEngine* js, JsValue* args, int nargs)
{
    const std::string label = timer_label(js, args, nargs);
    timers()[label] = Clock::now();
    return js->makeUndefined();
}

JsValue EConsole::timeEnd(JsEngine* js, JsValue* args, int nargs)
{
    const std::string label = timer_label(js, args, nargs);
    auto& table = timers();
    const auto it = table.find(label);
    if (it == table.end()) {
        std::cout << "Timer '" << label << "' does not exist" << std::endl;
        return js->makeUndefined();
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s: %.3fms", label.c_str(), elapsed_ms(it->second));
    table.erase(it);
    print_line(buf);
    return js->makeUndefined();
}

JsValue EConsole::timeLog(JsEngine* js, JsValue* args, int nargs)
{
    if (nargs == 0) {
        std::cout << "Timer label is required" << std::endl;
        return js->makeUndefined();
    }
    const std::string label = timer_label(js, args, 1);
    auto& table = timers();
    const auto it = table.find(label);
    if (it == table.end()) {
        std::cout << "Timer '" << label << "' does not exist" << std::endl;
        return js->makeUndefined();
    }
    std::string line;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s: %.3fms", label.c_str(), elapsed_ms(it->second));
    line += buf;
    if (nargs > 1) {
        line += ' ';
        line += format_args(js, args + 1, nargs - 1);
    }
    print_line(line);
    return js->makeUndefined();
}

}  // namespace Emjs
