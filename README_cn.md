# Emjs JavaScript 引擎（源码对齐版说明）

Emjs 是一个面向嵌入式场景的微型 JavaScript 解释器（C++17）。  
本 README 根据当前 `src/` 实现更新，示例与支持范围以源码为准。

- 版本常量：`src/core.h` 中 `kVersion = "1.0.0"`
- 运行模型：直接解释执行（无字节码）
- 内存模型：在固定缓冲区内维护引擎状态 + JS 堆

---

## 目录

- [项目结构](#项目结构)
- [构建](#构建)
- [CLI 工具（emjs）](#cli-工具emjs)
- [快速开始](#快速开始)
- [C++ API](#c-api)
- [语法与语义支持](#语法与语义支持)
- [内置对象与方法（核心）](#内置对象与方法核心)
- [扩展模块（extension）](#扩展模块extension)
- [明确不支持项](#明确不支持项)
- [错误与调试](#错误与调试)
- [源码对应关系](#源码对应关系)

---

## 项目结构

```text
src/
  core.h / core.cpp          # 词法、语法、解释执行、对象模型、GC
  engine.h / engine.cpp      # 对外 C++ API（创建/销毁/eval/值工厂）
  internal.h / internal.cpp  # 内部运行时与数组/字符串核心方法
  extension/
    console.*                # console.log/time/timeLog/timeEnd
    date.*                   # Date.now / Date.format
    global.*                 # parseInt / parseFloat
    json.*                   # JSON.parse / JSON.stringify
    math.*                   # Math 常量 + 多数数学函数
    string.*                 # String.fromCharCode
tests/
  test_js.cpp                # 运行脚本文件并绑定常用扩展
```

---

## 构建

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

当前 CMake 目标（`CMakeLists.txt`）：

- `emjs`（命令行工具）
- `libemjs-core.a`
- `libemjs-exts.a`
- `test_js`

> 说明：`tests/unit_test.cpp` 在仓库中存在，但当前 CMake 默认未生成 `unit_test` 可执行目标。

---

## CLI 工具（emjs）

构建完成后会生成可执行文件 `build/emjs`，提供类似 `node` 的基础脚本执行能力。

### 命令用法

```bash
emjs <script.js>
emjs -h
emjs --help
emjs
```

行为说明：

- 传入脚本路径：执行该 JS 文件
- `-h` / `--help`：显示帮助
- 无参数：进入交互模式（从 stdin 逐行读取并执行）
- 交互模式按 `Ctrl+C` 退出

### 使用示例

```bash
# 1) 运行脚本文件
./build/emjs ./tests/script/types.js

# 2) 查看帮助
./build/emjs --help

# 3) 管道输入（等价于 stdin 交互输入）
printf '%s\n' '1+2' 'let x=5; x+1;' | ./build/emjs
```

### 安装与 PATH 配置

安装（包含 `emjs`、静态库、头文件）：

```bash
cmake -S . -B build
cmake --build build
cmake --install build --prefix /usr/local
```

安装后典型布局：

- `/usr/local/bin/emjs`
- `/usr/local/lib/libemjs-core.a`
- `/usr/local/lib/libemjs-exts.a`
- `/usr/local/include/emjs/*.h`
- `/usr/local/include/emjs/extension/*.h`

若终端找不到 `emjs`，加入 PATH（macOS/Linux）：

```bash
export PATH="/usr/local/bin:$PATH"
```

可写入 `~/.zshrc` 或 `~/.bashrc` 后重新打开终端生效。

---

## 快速开始

```cpp
#include "src/engine.h"
#include <cstdio>

using namespace Emjs;

int main() {
  char mem[4096] = {0};
  JsEngine* js = JsEngine::create(mem, sizeof(mem));
  if (!js) return 1;

  JsValue r = js->eval("let x=3; let y=4; x*y + 1;");
  std::printf("%s\n", js->str(r)); // 13

  JsEngine::destroy(js); // 调用方提供内存时不会 free(mem)
  return 0;
}
```

---

## C++ API

头文件：`src/engine.h`

### 创建/销毁

```cpp
// 方式1：使用外部缓冲区
char mem[8192];
JsEngine* js = JsEngine::create(mem, sizeof(mem));
JsEngine::destroy(js);

// 方式2：引擎内部 malloc
JsEngine* js2 = JsEngine::create(8192);
JsEngine::destroy(js2); // 会释放整块内存
```

### 执行

```cpp
JsValue v = js->eval("1+2+3");      // length 默认 ~0U，按 C 字符串读取
const char* s = js->str(v);         // "6" 或错误文本
```

### 直接调用全局函数（高于 eval 的调用效率）

当脚本中已定义全局函数时，推荐使用 `callGlobal`，避免每次拼接并解析 `eval("fn(...)")`：

```cpp
// 先定义全局函数
js->eval("function add(a,b){return a+b;};");

// 方式1：数组参数
JsValue argv[2] = {JsEngine::makeNumber(10), JsEngine::makeNumber(32)};
JsValue r1 = js->callGlobal("add", argv, 2);   // 42

// 方式2：initializer_list（更简洁）
JsValue r2 = js->callGlobal("add", {
    JsEngine::makeNumber(1),
    JsEngine::makeNumber(2)
});                                             // 3
```

接口定义（`src/engine.h`）：

- `JsValue callGlobal(const char* functionName, const JsValue* args, int argCount);`
- `JsValue callGlobal(const char* functionName, std::initializer_list<JsValue> args = {});`

行为说明：

- 函数不存在：返回 Error（`'xxx' not found`）
- 目标非函数：返回 Error（`'xxx' not callable`）
- 参数非法（如 `argCount > 0 && args == nullptr`）：返回 Error（`bad call`）

### 注入原生函数

```cpp
JsValue add(JsEngine* js, JsValue* args, int nargs) {
  if (!JsEngine::chkArgs(args, nargs, "dd")) {
    return js->makeError("type mismatch");
  }
  return JsEngine::makeNumber(
      JsEngine::getNumber(args[0]) + JsEngine::getNumber(args[1]));
}

js->set(js->glob(), "add", JsEngine::makeFunction(add));
js->eval("add(10, 32);"); // 42
```

`chkArgs` 规格：

- `d` number
- `b` boolean
- `s` string
- `j` 任意值

### 运行时控制

```cpp
js->setGcThreshold(1024); // 字节阈值
js->setMaxCss(5000);      // C 栈保护阈值
js->gc();

size_t total=0, lwm=0, css=0;
js->stats(&total, &lwm, &css);
```

---

## 语法与语义支持

以下是当前 `src/core.cpp + src/internal.cpp` 的实际能力。

### 基础类型与字面量

```javascript
undefined; null; true; false;
123; 3.14; 0x10;
"abc"; 'xyz';
({a:1, b:2});
[1, 2, 3];
```

### 变量与赋值

- 支持：`let`、`const`
- 支持复合赋值：`+= -= *= /= %= <<= >>= >>>= &= ^= |=`
- 支持后缀自增减：`i++`、`i--`
- `const` 重新赋值会报错：`assignment to constant`

```javascript
let a=1; a+=2; a;        // 3
const c=5;
```

### 表达式与运算符

- 算术：`+ - * / % **`
- 位运算：`~ & | ^ << >> >>>`
- 比较：`< <= > >=`
- 相等：`== != === !==`
- 逻辑：`! && ||`
- 三元：`cond ? a : b`
- `typeof`

```javascript
typeof 1;          // "number"
1 == true;         // true
1 === true;        // false
(-1) >>> 1;        // 2147483647
```

### 控制流

- `if / else`
- `for`
- `while`
- `do...while`
- `switch`
- `break / continue`
- `try / catch / finally`
- `throw`
- `return`（仅函数内）

### 函数

- 支持函数声明/函数表达式
- 支持 IIFE
- 支持箭头函数（本轮已实现）
  - 单参数：`x => x * 2`
  - 括号参数：`(a, b) => a + b`
  - 块体：`(x) => { return x * 2; }`

```javascript
function add(a,b){ return a+b; }
let inc = x => x + 1;
let sum = (a,b) => (a+b);
```

### 分号规则

支持显式分号；同时存在换行终止逻辑（`js_stmt_finish`），并非“绝对强制每行都写分号”。  
建议生产脚本仍显式写 `;`，避免歧义。

---

## 内置对象与方法（核心）

这部分不依赖 extension 模块，来自 `internal.cpp` / `core.cpp`。

### String 实例方法

- `length`
- `charCodeAt(index)`
- `charAt(index)`
- `indexOf(substr[, from])`
- `substring(start[, end])`

```javascript
let s = "Abcc";
s.length;          // 4
s.charCodeAt(0);   // 65
s.charAt(1);       // "b"
s.indexOf("cc");   // 2
s.substring(1,3);  // "bc"
```

### Array 方法

- `length`
- `push`, `pop`, `shift`
- `slice`, `reverse`, `sort`, `splice`
- `forEach`, `map`, `every`, `some`, `find`, `findIndex`, `reduce`
- `indexOf`, `join`

```javascript
let a = [2,1,3];
a.sort();
a.join("-");                       // "1-2-3"
a.map(x => x * 2);                 // [2,4,6]
a.reduce((acc,x)=>acc+x, 0);       // 6
a.findIndex(x => x === 2);         // 1
```

回调签名（与源码一致）：

- `forEach/map/every/some/find/findIndex`：`(elem, index, array)`
- `reduce`：`(acc, elem, index, array)`

---

## 扩展模块（extension）

扩展默认是否编译由 CMake 选项控制：

- `BUILD_CONSOLE`
- `BUILD_GLOBAL`
- `BUILD_STRING`
- `BUILD_DATE`
- `BUILD_JSON`
- `BUILD_MATH`

在运行时需要显式 `bind` 到引擎：

```cpp
#include "src/extension/console.h"
#include "src/extension/global.h"
#include "src/extension/string.h"
#include "src/extension/date.h"
#include "src/extension/math.h"
#include "src/extension/json.h"

EConsole::bind(js);
EGlobal::bind(js);
EString::bind(js);
EDate::bind(js);
EMath::bind(js);
EJson::bind(js);
```

### console

- `console.log(...args)`
- `console.time(label?)`
- `console.timeLog(label, ...args)`
- `console.timeEnd(label?)`

### global

- `parseInt(str[, radix])`
- `parseFloat(str)`

### String（全局对象）

- `String.fromCharCode(...codes)`

### Date

- `Date.now()`
- `Date.format([timestampMs][, format])`

格式默认：`"%Y-%m-%d %H:%i:%s"`（`%i` 映射分钟，`%s` 映射秒）

### Math

常量：`E, LN10, LN2, LOG10E, LOG2E, PI, SQRT1_2, SQRT2`

函数（已绑定）：  
`abs acos acosh asin asinh atan atan2 atanh cbrt ceil clz32 cos cosh exp expm1 f16round floor fround hypot imul log log10 log1p log2 max min pow random round sign sin sinh sqrt sumPrecise tan tanh trunc`

### JSON

- `JSON.parse(text)`
- `JSON.stringify(value)`

```javascript
let o = JSON.parse("{\"a\":1,\"b\":[2,3]}");
JSON.stringify(o); // {"a":1,"b":[2,3]}
```

---

## 明确不支持项

以下关键字/能力在当前源码中会报 `not implemented` 或解析错误：

- `var`
- `class`
- `new`
- `this`
- `delete`
- `with`
- `yield`
- `void`
- `in`
- `instanceof`

另外，不支持 ES6 模块、Promise、原型链/`prototype` 机制等完整标准库行为。

---

## 错误与调试

常见错误文本（来自源码）：

- `parse error`
- `; expected`
- `type mismatch`
- `not a function`
- `not array`
- `reduce empty`
- `assignment to constant`
- `oom`
- `C stack`
- `'xxx' not found`
- `'xxx' not implemented`

多数错误会带位置后缀：`at line N col M`。

`JsEngine::dump()` 在默认配置下是空实现；如需调试输出，编译时定义：

- `EMJS_ENABLE_DUMP=1`

---

## 编译与体积选项

`CMakeLists.txt` 已提供：

- `EMJS_OPTIMIZE_SIZE`（默认 `ON`）  
  启用 `-Oz` 及若干体积优化选项。

源码宏：

- `JS_EXPR_MAX`（默认 `20`）
- `JS_GC_THRESHOLD`（默认 `0.25`）
- `EMJS_ENABLE_DUMP`（默认 `0`）

---

## 源码对应关系

- 解释器主流程：`src/core.cpp`
- 对外 API：`src/engine.h` / `src/engine.cpp`
- 数组/字符串核心方法：`src/internal.cpp`
- 扩展绑定示例：`tests/test_js.cpp`
- 脚本样例：`tests/script/*.js`

如文档与行为不一致，以源码与测试输出为准。

# Emjs JavaScript 引擎使用指南

Emjs 是一个面向嵌入式系统的微型 JavaScript 引擎。本仓库为 **C++17** 封装版本：直接解释 JS 源码（无字节码），在调用方提供的固定内存缓冲区中运行，适合 MCU、固件、工具链内嵌脚本等场景。

引擎**不包含标准库**（无 `console`、`Math`、`JSON` 等）。设备能力与 IO 需由 C++ 侧实现，并通过 `JsEngine::set()` 注入，由 JS 编排逻辑。

当前版本：`1.0.0`（见 `src/core.h` 中 `kVersion`）

---

## 目录

- [项目结构](#项目结构)
- [构建与测试](#构建与测试)
- [快速开始](#快速开始)
- [C++ 嵌入 API](#c-嵌入-api)
- [支持的 JavaScript 语法](#支持的-javascript-语法)
- [不支持的特性](#不支持的特性)
- [语义与限制](#语义与限制)
- [完整示例](#完整示例)
- [编译选项](#编译选项)

---

## 项目结构

```
src/
  core.h           # 基础类型：JsValue、JsEngineState、JsValueType
  engine.h    # JsEngine 公开 API（应用代码应 include 此文件）
  engine.cpp  # JsEngine 实现：创建/销毁、eval、值工厂、GC 控制
  core.cpp         # 解析器、解释器、GC、对象模型
  string.cpp  # 字符串方法：length、charCodeAt、charAt
  internal.h  # 引擎内部共享接口（扩展引擎时使用）
```

对外只需：

```cpp
#include "src/engine.h"  // 已包含 core.h
```

`core.h` **不会**自动引入 `JsEngine`，请勿仅依赖 `core.h` 使用引擎 API。

---

## 构建与测试

```bash
mkdir build && cd build
cmake ..
make -j4
```

| 目标 | 说明 |
|------|------|
| `libejs.a` | 静态库（`core.cpp` + `engine.cpp` + `string.cpp`） |
| `test_js` | 运行外部 JS 文件，示例：`./test_js ../tests/script/md5.js` |
| `unit_test` | 语法与行为单元测试（覆盖最全） |

链接示例：

```bash
c++ -std=c++17 -I../src your_app.cpp -L. -lejs -lm -o your_app
```

---

## 快速开始

```cpp
#include "src/engine.h"
#include <cstdio>

using namespace Emjs;

int main() {
  char mem[512];
  JsEngine* js = JsEngine::create(mem, sizeof(mem));
  if (!js) return 1;

  JsValue v = js->eval("1 + 2 * 3");
  printf("result: %s\n", js->str(v));  // 7

  JsEngine::destroy(js);
  return 0;
}
```

**注意：** `eval()` 的返回值以及 `str()` 返回的 C 字符串，在下一次 `eval()` 之前有效（每次顶层 `eval` 可能触发 GC，复用内部缓冲区）。

---

## C++ 嵌入 API

### 创建与销毁

**方式一：调用方提供缓冲区（推荐嵌入式）**

```cpp
char mem[4096];
JsEngine* js = JsEngine::create(mem, sizeof(mem));
// destroy 不会 free(mem)，仅析构引擎状态
JsEngine::destroy(js);
```

**方式二：引擎内部分配（`malloc`）**

```cpp
JsEngine* js = JsEngine::create(4096);  // 整块 buffer 由引擎持有
JsEngine::destroy(js);                  // 释放全部内存
```

内存布局：

```
| JsEngine 状态 (~100B) | JS 堆（对象/字符串/属性） | 空闲 |
```

缓冲区过小（约 < `sizeof(JsEngine) + 对象头`）时 `create` 返回 `nullptr`。可用 `stats()` 观察峰值用量。

### 执行 JavaScript

```cpp
JsValue result = js->eval("let x = 10; x + 5;", ~0U);  // length=~0U 表示以 '\0' 结尾
const char* text = js->str(result);  // "15"
```

### 直接调用脚本中的全局函数（推荐）

`callGlobal` 会直接查找并调用全局函数，省去 `eval("fn(...)")` 的词法/语法解析开销：

```cpp
js->eval("function mul(a,b){ return a*b; };");

// 指针 + 参数个数
JsValue args[2] = {
  JsEngine::makeNumber(6),
  JsEngine::makeNumber(7),
};
JsValue out1 = js->callGlobal("mul", args, 2);   // 42

// initializer_list 便捷重载
JsValue out2 = js->callGlobal("mul", {
  JsEngine::makeNumber(3),
  JsEngine::makeNumber(5),
});                                               // 15
```

签名：

```cpp
JsValue callGlobal(const char* functionName, const JsValue* args, int argCount);
JsValue callGlobal(const char* functionName, std::initializer_list<JsValue> args = {});
```

错误语义：

- 函数名为空/参数非法：`bad call`
- 函数不存在：`'name' not found`
- 找到但不是函数：`'name' not callable`

### 注入 C++ 函数

```cpp
JsValue myAdd(JsEngine* js, JsValue* args, int nargs) {
  if (!JsEngine::chkArgs(args, nargs, "dd"))
    return js->makeError("需要两个数字参数");
  double a = JsEngine::getNumber(args[0]);
  double b = JsEngine::getNumber(args[1]);
  return JsEngine::makeNumber(a + b);
}

js->set(js->glob(), "add", JsEngine::makeFunction(myAdd));
js->eval("add(3, 4);");  // 7
```

`chkArgs` 规格字符：

| 字符 | 含义 |
|------|------|
| `b` | 布尔 |
| `d` | 数字 |
| `s` | 字符串 |
| `j` | 任意 JS 值 |

### 构造与读取 JS 值

| 方法 | 说明 |
|------|------|
| `makeUndefined()` / `makeNull()` / `makeTrue()` / `makeFalse()` | 基本类型 |
| `makeNumber(double)` | 数字 |
| `makeString(data, len)` | 字符串（二进制字节序列） |
| `makeObject()` | 空对象 |
| `makeFunction(NativeFunction)` | C++ 回调 |
| `makeError(fmt, ...)` | 错误值 |
| `getType(value)` | `JsValueType` 枚举 |
| `getNumber(value)` / `getBool(value)` | 提取数值/布尔 |
| `getString(value, &len)` | 提取字符串指针与长度 |
| `isTruthy(value)` | 真值判断 |
| `set(obj, key, value)` | 设置对象属性 |
| `glob()` | 全局对象 |

### 内存、GC 与调试

```cpp
js->setGcThreshold(threshold);  // 手动设置 GC 阈值（字节）
js->setMaxCss(5000);            // 限制 C 调用栈深度，防递归过深
js->gc();                       // 手动触发 GC
js->stats(&total, &lwm, &css);  // total=堆大小, lwm=低水位(近似剩余), css=C栈峰值
// js->dump();                  // 需编译时定义 JS_DUMP
```

默认 GC 阈值：`memSize × 0.25`（`JS_GC_THRESHOLD`）。阈值越小 GC 越频繁，大脚本/复杂对象图更安全。

---

## 支持的 JavaScript 语法

### 1. 字面量与类型

```javascript
undefined;
null;
true;
false;
42;
1.23;
0x64;              // 十六进制 → 100
0xEFCDAB89;        // 32 位范围内精确
'hello';
"world";
({});
({a: 1, b: 2});
```

### 2. 变量声明

```javascript
let a = 1, b = 2;
const PI = 3.14;
const obj = {x: 1};
```

- 支持 **`let`** 与 **`const`**
- `const` 绑定不可重新赋值；`const obj = {}` 仍可修改 `obj.x`
- 同一作用域不可重复声明
- **不支持** `var`
- 使用前必须声明，否则 `'x' not found`

### 3. 语句与分号

每条语句以 **`;` 结尾**（块内同样要求）：

```javascript
let a = 1;
let b = 2;
a + b;
```

多语句顺序执行，返回最后一条语句的值：`1; 2; 3;` → `3`。

### 4. 注释

```javascript
// 单行
/* 块注释 */
let x = 1 + /* inline */ 2;
```

### 5. 算术与位运算

| 运算符 | 说明 |
|--------|------|
| `+ - * / %` | 算术（`+` 两侧均为字符串时为连接） |
| `**` | 幂运算 |
| `~` `<<` `>>` `>>>` | 位运算（**32 位 JavaScript 语义**） |
| `& \| ^` | 按位与/或/异或 |
| 一元 `+` `-` | 正负号 |

```javascript
3 + 4 * 2;           // 11
2 ** 3;              // 8
100 << 3;            // 800
(-1) >>> 1;          // 2147483647
6 & 3;               // 2
'a' + 'b';           // "ab"
```

数字运算要求操作数为数字类型，否则 `type mismatch`（字符串拼接除外）。

### 6. 比较与逻辑

| 运算符 | 说明 |
|--------|------|
| `===` `!==` | 严格相等（同类型） |
| `==` `!=` | 宽松相等（数字/布尔/字符串/null/undefined 可互相转换比较） |
| `< <= > >=` | 数字比较 |
| `&&` `\|\|` `!` | 短路逻辑 |

```javascript
1 === 1;             // true
1 == true;           // true
null == undefined;   // true
1 && 2;              // 2
```

### 7. 三元、赋值、自增

```javascript
let max = a > b ? a : b;
a += 5;
a <<= 2;
i++;                 // 仅后缀 ++/--
```

### 8. 字符串

字符串为**二进制字节序列**（非 Unicode 码点串）：

```javascript
'hello'.length;           // 5
'Київ'.length;            // 8（UTF-8 按字节计）
'abc'.charCodeAt(0);      // 97
'abc'.charAt(1);          // "b"
'a' + 'b';                // "ab"
```

转义：`\n` `\t` `\r` `\'` `\"` `\xHH`

索引越界时：`charCodeAt` → `NaN`，`charAt` → `""`。

### 9. 对象与属性访问

```javascript
let obj = {name: 'dev', value: 42, nested: {x: 1}};
obj.name;
obj.nested.x;
obj['name'];         // 支持 [] 下标访问
obj.value = 100;
```

对象可挂载方法：

```javascript
let calc = {
  double: function(x) { return x * 2; }
};
calc.double(5);      // 10
```

### 10. 数组

数组为实现带 `length` 的对象：

```javascript
let a = [1, 2, 3];
a[1];                // 2
a[1] = 9;
a.length;            // 3
a.push(4);
a.pop();
```

对数字下标赋值会自动扩展 `length`（稀疏数组、`arr[i]=0` 初始化等场景可用）。

### 11. 函数

```javascript
function add(a, b) {
  return a + b;
}

let inc = function(x) {
  return x + 1;
};

(function() {
  return 42;
})();                // 42
```

- 支持函数声明与函数表达式
- 支持 `return`（仅函数体内）
- 参数按 **JavaScript 规则**：所有实参在绑定形参之前于调用方作用域求值（支持 `FF(d, a, b, c, ...)` 这类形参与实参交叉命名的写法）
- 内层函数可通过作用域链访问外层已声明变量
- **不支持**箭头函数 `=>`
- 函数体必须用 `{ ... }` 包裹

### 12. `typeof`

```javascript
typeof 1;            // "number"
typeof 'hi';         // "string"
typeof true;         // "boolean"
typeof null;         // "null"
typeof undefined;    // "undefined"
typeof {};           // "object"
typeof add;          // "function"
```

### 13. 控制流

**`if` / `else if` / `else`**

```javascript
if (x > 0) {
  y = 1;
} else if (x < 0) {
  y = -1;
} else {
  y = 0;
}
```

**`for` / `while` / `do...while`**，支持 `break` / `continue`

```javascript
for (let i = 0; i < 10; i++) {
  if (i === 5) break;
}

let n = 0;
while (n < 3) { n++; }

do { n++; } while (n < 3);
```

**`switch`**（`case` 使用 `==` 匹配）

```javascript
let x = 0;
switch (2) {
  case 1: x = 1; break;
  case 2: x = 2; break;
  default: x = 9;
}
```

**`try` / `catch` / `finally` / `throw`**

```javascript
try {
  risky();
} catch (e) {
  // e 为错误消息字符串
} finally {
  cleanup();
}

throw 'something went wrong';
```

### 14. 块作用域

```javascript
let a = 5;
{
  a = 6;           // 可修改外层 let
  let x = 2;       // 块内局部
}
// x 不可访问

{
  let b = 6;
}
// b 不可访问
```

`for (let i = 0; ...)` 中 `i` 仅在循环块内可见。

### 15. 真值表

| 值 | 真/假 |
|----|-------|
| `true` | 真 |
| `false` | 假 |
| 非 0 数字 | 真 |
| `0` | 假 |
| 非空字符串 | 真 |
| `''` | 假 |
| 对象、函数 | 真 |
| `null`、`undefined` | 假 |

---

## 不支持的特性

以下会报 `'xxx' not implemented`、解析错误或 `type mismatch`：

| 类别 | 不支持项 |
|------|----------|
| 声明 | `var` |
| 函数 | 箭头函数 `=>` |
| 面向对象 | `class`、`new`、`this`、`prototype` |
| 运算符/关键字 | `delete`、`with`、`in`、`instanceof`、`void`、`yield` |
| 标准库 | `Date`、`RegExp`、`JSON`、`Math`、`console`、`Promise` 等 |
| 字符串 | 除 `length` / `charCodeAt` / `charAt` 外的方法 |
| 数组 | 除 `push` / `pop` / `length` / 下标访问外的数组方法 |

未实现的语法关键字在词法/解析阶段即拒绝（如 `class`、`var`、`with`）。

---

## 语义与限制

### 必须写分号

```javascript
let a = 1;   // 正确
let a = 1    // 错误: ; expected
```

### 字符串非 Unicode

`.length`、`charCodeAt`、下标相关逻辑均按**字节**处理。多字节 UTF-8 字符占多个字节。

### 无标准库

IO、定时器、硬件等须 C++ 实现并注入，例如：

```cpp
js->set(js->glob(), "print", JsEngine::makeFunction(nativePrint));
js->set(js->glob(), "delay", JsEngine::makeFunction(nativeDelay));
```

`test_js` 中演示了注入 `console.log` 的写法（见 `tests/test_js.cpp`）。

### 内存与 GC

- 堆空间不足时：`ERROR: oom`
- 每次顶层 `eval` 执行前可能触发 mark-and-sweep GC
- 典型占用：对象头 8B，每个属性约 16B，字符串 4 + 内容（4 字节对齐）
- 大脚本（如 `tests/script/md5.js`、`tree.js`）建议预留足够堆空间，必要时调低 `setGcThreshold`

### 错误格式

出错时 `getType(result) == JsValueType::Error`，`str(result)` 形如：

```
ERROR: 'x' not found
ERROR: type mismatch
ERROR: ; expected
ERROR: parse error
ERROR: oom
ERROR: C stack
ERROR: assignment to constant
```

常带 ` at line N col M` 后缀。

### 性能

直接解释执行，适合配置/胶水逻辑，不适合高性能计算。参考量级（100 次空循环 `for (let i = 0; i < 100; i++) true;`）：

| 平台 | 耗时（约） |
|------|------------|
| Arduino Uno 16MHz | ~97 ms |
| SAMD21 48MHz | ~16 ms |
| RP2040 133MHz | ~5 ms |
| ESP32 240MHz | ~2 ms |

---

## 完整示例

### 示例 1：表达式求值

```cpp
#include "src/engine.h"
#include <cstdio>

using namespace Emjs;

int main() {
  char mem[512];
  JsEngine* js = JsEngine::create(mem, sizeof(mem));

  JsValue v = js->eval(
      "let base = 10;"
      "let rate = 0.05;"
      "base * (1 + rate);");
  printf("%s\n", js->str(v));  // 10.5

  JsEngine::destroy(js);
  return 0;
}
```

### 示例 2：C++ 与 JS 互调

```cpp
#include "src/engine.h"
#include <cstdio>

using namespace Emjs;

JsValue readAdc(JsEngine* js, JsValue* args, int nargs) {
  if (!JsEngine::chkArgs(args, nargs, "d"))
    return js->makeError("need channel");
  int ch = (int)JsEngine::getNumber(args[0]);
  return JsEngine::makeNumber(512.0 + ch * 10);
}

JsValue setGpio(JsEngine* js, JsValue* args, int nargs) {
  if (!JsEngine::chkArgs(args, nargs, "dd"))
    return js->makeError("need pin and value");
  int pin = (int)JsEngine::getNumber(args[0]);
  int val = (int)JsEngine::getNumber(args[1]);
  printf("[HW] pin %d = %d\n", pin, val);
  return JsEngine::makeUndefined();
}

int main() {
  char mem[2048];
  JsEngine* js = JsEngine::create(mem, sizeof(mem));

  js->set(js->glob(), "readAdc", JsEngine::makeFunction(readAdc));
  js->set(js->glob(), "setGpio", JsEngine::makeFunction(setGpio));

  js->eval(
      "let process = function(ch, th) {"
      "  let raw = readAdc(ch);"
      "  setGpio(13, raw > th ? 1 : 0);"
      "  return raw;"
      "};"
      "process(0, 500);");

  printf("result: %s\n", js->str(js->eval("process(0, 500);")));
  JsEngine::destroy(js);
  return 0;
}
```

### 示例 3：运行脚本文件（test_js）

```bash
./test_js ../tests/script/test1.js
./test_js ../tests/script/md5.js    # 需 ~600KB 堆，验证位运算/字符串/数组
./test_js ../tests/script/tree.js   # GC 压力测试
```

### 示例 4：纯 JS 状态机

```javascript
let state = 'idle';
let count = 0;

let tick = function() {
  if (state === 'idle') {
    state = 'running';
    count = 0;
  } else if (state === 'running') {
    count++;
    if (count >= 5) state = 'done';
  }
  return state;
};
```

### 示例 5：定时回调（C++ 稍后 eval）

```cpp
// 注册 set_timer(name)，在定时器中断里：js->eval("f(7);");
js->eval("let v = 0, f = function(x) { v += x; };");
js->eval("set_timer('f');");
// ... 定时器触发后 ...
js->eval("v;");  // 7
```

---

## 运算符优先级（从高到低）

1. 后缀 `++` `--`、`.`、`()`、`[]`
2. 一元 `!` `~` `typeof` `+` `-`
3. `**`（右结合）
4. `*` `/` `%`
5. `+` `-`
6. `<<` `>>` `>>>`
7. `<` `<=` `>` `>=`
8. `==` `!=` `===` `!==`
9. `&`
10. `^`
11. `|`
12. `&&`
13. `||`
14. 三元 `? :`
15. 赋值 `=` `+=` `-=` …（右结合）

---

## 编译选项

在编译 `emjs` 库前定义（如 CMake `target_compile_definitions`）：

| 宏 | 默认值 | 说明 |
|----|--------|------|
| `JS_EXPR_MAX` | 20 | 表达式求值栈深度 |
| `JS_GC_THRESHOLD` | 0.25 | GC 触发比例（`break_ > memSize × 阈值`） |
| `JS_DUMP` | 未定义 | 启用 `JsEngine::dump()` 堆调试输出 |

---

## 参考

| 路径 | 说明 |
|------|------|
| `src/engine.h` | 应用侧主入口 |
| `src/core.h` | 基础类型 |
| `tests/unit_test.cpp` | 语法与行为测试（最全） |
| `tests/test_js.cpp` | 加载 JS 文件 + 注入 `console.log` |
| `tests/script/` | 示例脚本（`md5.js`、`tree.js` 等） |
