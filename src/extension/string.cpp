#include "string.h"
#include <string>

using namespace Emjs;

void EString::bind(JsEngine* js)
{
    auto obj = js->makeObject();
    js->set(obj, "fromCharCode", JsEngine::makeFunction(EString::fromCharCode));
    
    js->set(js->glob(), "String", obj);
}

JsValue EString::fromCharCode(JsEngine* js, JsValue* args, int nargs)
{
    std::string s;
    for (int i = 0; i < nargs; i++) {
        auto type = js->getType(args[i]);
        if(type==JsValueType::Number){
            int code=js->getNumber(args[i]);
            if (code <= 0x7F) {
                s += static_cast<char>(code);
            } else if (code <= 0x7FF) {
                s += static_cast<char>(0xC0 | (code >> 6));
                s += static_cast<char>(0x80 | (code & 0x3F));
            } else if (code <= 0xFFFF) {
                s += static_cast<char>(0xE0 | (code >> 12));
                s += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                s += static_cast<char>(0x80 | (code & 0x3F));
            } else {
                s += static_cast<char>(0xF0 | (code >> 18));
                s += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                s += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                s += static_cast<char>(0x80 | (code & 0x3F));
            }
        }
    }
    
    return js->makeString(s.c_str(), s.size());
}
