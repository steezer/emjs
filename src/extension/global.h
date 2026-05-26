#pragma once

#include "../engine.h"

namespace Emjs {

class EGlobal {
public:
    static void bind(JsEngine* js);
    static JsValue parseInt(JsEngine* js, JsValue* args, int nargs);
    static JsValue parseFloat(JsEngine* js, JsValue* args, int nargs);
};

}  // namespace Emjs
