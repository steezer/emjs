#pragma once

#include "../engine.h"

namespace Emjs {

class EJson {
public:
    static void bind(JsEngine* js);
    static JsValue parse(JsEngine* js, JsValue* args, int nargs);
    static JsValue stringify(JsEngine* js, JsValue* args, int nargs);
};

}  // namespace Emjs
