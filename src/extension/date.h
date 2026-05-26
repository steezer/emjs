#pragma once

#include "../engine.h"

namespace Emjs {

class EDate {
public:
    static void bind(JsEngine* js);
    static JsValue now(JsEngine* js, JsValue* args, int nargs);
    static JsValue format(JsEngine* js, JsValue* args, int nargs);
};

}  // namespace Emjs
