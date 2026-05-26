#pragma once

#include "../engine.h"

namespace Emjs {

class EString{
    public:
        static void bind(JsEngine* js);
        static JsValue fromCharCode(JsEngine* js, JsValue* args, int nargs);
}; 
    
}