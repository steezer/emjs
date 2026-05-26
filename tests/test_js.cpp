#include <stdio.h>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <string>
#include <stcpp/basic.hpp>
#include <stcpp/fs.hpp>
#include "../src/engine.h"
#include "../src/extension/console.h"
#include "../src/extension/date.h"
#include "../src/extension/math.h"
#include "../src/extension/string.h"
#include "../src/extension/json.h"
#include "../src/extension/global.h"

using namespace Emjs;

// C function that adds two numbers. Will be called from JS
JsValue sum(JsEngine* js, JsValue* args, int nargs)
{
    double a = JsEngine::getNumber(args[0]);  // Fetch 1st arg
    double b = JsEngine::getNumber(args[1]);  // Fetch 2nd arg
    return JsEngine::makeNumber(a + b);
}

int runTest(void* buffer, std::size_t length, const char* code)
{
    // Heap-allocated engine; destroy() frees all memory via free().
    JsEngine* js = JsEngine::create(buffer, length);
    if (js == nullptr) {
        std::cerr << "JsEngine::create failed\n";
        return 1;
    }

    js->set(js->glob(), "sum", JsEngine::makeFunction(sum));
    
    EConsole::bind(js);
    EString::bind(js);
    EGlobal::bind(js);
    EDate::bind(js);
    
    EMath::bind(js);
    EJson::bind(js);
    
    auto evalOk = [&](const char* code) {
        JsValue r = js->eval(code);
        if (JsEngine::getType(r) == JsValueType::Error) {
            std::cerr << js->str(r) << std::endl;
            return false;
        }
        return true;
    };
    
    auto startTime=Stcpp::getTime(true);
    evalOk(code);
    std::cout << "Use time: " << (Stcpp::getTime(true)-startTime)/1000.0 << "ms"<< std::endl;
    
    size_t total, lwm, css;
    js->stats(&total, &lwm, &css);
    // total - lwm ≈ 峰值已用量
    printf("Used: %zu bytes, free: ~%zu bytes, total: %zu bytes\n", total - lwm, lwm, total);

    JsEngine::destroy(js);
    
    return 0;
}

int main(int argc, char const* argv[])
{
    std::string buf(600000, 0);
    
    //std::cout << buf.size() << std::endl;
    
    // size_t length=1024*1024;
    // void* buffer = std::malloc(length);
    // if (buffer == nullptr) return -1;
    
    for (int i=1; i< argc; i++) {
        if(Stcpp::Fs::isFile(argv[i])){
            std::cout << "[" <<argv[i] << "]:"<< std::endl;
            auto code=Stcpp::Fs::getFileContent(argv[i]);
            runTest(buf.data(), buf.size(), code.c_str());
        }
    }
    return 0;
}
