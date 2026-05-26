#pragma once

#include "../engine.h"

namespace Emjs {

class EConsole {
public:
    static void bind(JsEngine* js);
    static JsValue log(JsEngine* js, JsValue* args, int nargs);
    static JsValue time(JsEngine* js, JsValue* args, int nargs);
    static JsValue timeEnd(JsEngine* js, JsValue* args, int nargs);
    static JsValue timeLog(JsEngine* js, JsValue* args, int nargs);
};

}  // namespace Emjs
