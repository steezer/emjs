# Emjs JavaScript Engine (Source-Aligned Guide)

Emjs is a lightweight JavaScript interpreter for embedded scenarios (C++17).  
This document is aligned with the current implementation under `src/`, and examples are written according to actual behavior.

- Version constant: `kVersion = "1.0.0"` in `src/core.h`
- Execution model: direct interpretation (no bytecode VM)
- Memory model: engine state + JS heap in a fixed buffer

---

## Table of Contents

- [Project Layout](#project-layout)
- [Build](#build)
- [CLI Tool (emjs)](#cli-tool-emjs)
- [Quick Start](#quick-start)
- [C++ API](#c-api)
- [Syntax and Semantics](#syntax-and-semantics)
- [Built-in Methods (Core)](#built-in-methods-core)
- [Extension Modules](#extension-modules)
- [Unsupported Features](#unsupported-features)
- [Errors and Debugging](#errors-and-debugging)
- [Build and Size Options](#build-and-size-options)
- [Source Mapping](#source-mapping)

---

## Project Layout

```text
src/
  core.h / core.cpp          # lexer, parser, interpreter, object model, GC
  engine.h / engine.cpp      # public C++ API
  internal.h / internal.cpp  # internal runtime + array/string core methods
  extension/
    console.*                # console.log/time/timeLog/timeEnd
    date.*                   # Date.now / Date.format
    global.*                 # parseInt / parseFloat
    json.*                   # JSON.parse / JSON.stringify
    math.*                   # Math constants + many math functions
    string.*                 # String.fromCharCode
tests/
  test_js.cpp                # script runner with default extension binding
```

---

## Build

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

Current CMake targets:

- `emjs` (CLI executable)
- `libemjs-core.a`
- `libemjs-exts.a`
- `test_js`

> Note: `tests/unit_test.cpp` exists in the repository, but `unit_test` is not enabled as a default CMake target.

---

## CLI Tool (emjs)

After build, the executable is available at `build/emjs`.

### Command usage

```bash
emjs <script.js>
emjs -h
emjs --help
emjs
```

Behavior:

- script path argument: run that JS file
- `-h` / `--help`: print help
- no argument: start interactive mode (stdin line-by-line)
- press `Ctrl+C` to exit interactive mode

### Examples

```bash
# run file
./build/emjs ./tests/script/types.js

# help
./build/emjs --help

# stdin / pipe mode
printf '%s\n' '1+2' 'let x=5; x+1;' | ./build/emjs
```

### Install and PATH setup

```bash
cmake -S . -B build
cmake --build build
cmake --install build --prefix /usr/local
```

Typical installed files:

- `/usr/local/bin/emjs`
- `/usr/local/lib/libemjs-core.a`
- `/usr/local/lib/libemjs-exts.a`
- `/usr/local/include/emjs/*.h`
- `/usr/local/include/emjs/extension/*.h`

If `emjs` is not found:

```bash
export PATH="/usr/local/bin:$PATH"
```

Add it to `~/.zshrc` or `~/.bashrc` to persist.

---

## Quick Start

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

  JsEngine::destroy(js);
  return 0;
}
```

---

## C++ API

Header: `src/engine.h`

### Create / destroy

```cpp
// external buffer
char mem[8192];
JsEngine* js = JsEngine::create(mem, sizeof(mem));
JsEngine::destroy(js);

// internally allocated
JsEngine* js2 = JsEngine::create(8192);
JsEngine::destroy(js2); // frees owned memory
```

### Evaluate JavaScript

```cpp
JsValue v = js->eval("1+2+3");   // length defaults to ~0U
const char* s = js->str(v);      // "6" or error string
```

### Call global JS function directly (faster than eval call string)

```cpp
js->eval("function add(a,b){return a+b;};");

// pointer + count form
JsValue argv[2] = {JsEngine::makeNumber(10), JsEngine::makeNumber(32)};
JsValue r1 = js->callGlobal("add", argv, 2); // 42

// initializer_list form
JsValue r2 = js->callGlobal("add", {
  JsEngine::makeNumber(1),
  JsEngine::makeNumber(2),
}); // 3
```

Signatures:

- `JsValue callGlobal(const char* functionName, const JsValue* args, int argCount);`
- `JsValue callGlobal(const char* functionName, std::initializer_list<JsValue> args = {});`

Error semantics:

- bad arguments: `bad call`
- function not found: `'name' not found`
- symbol exists but not callable: `'name' not callable`

### Inject native C++ function

```cpp
JsValue add(JsEngine* js, JsValue* args, int nargs) {
  if (!JsEngine::chkArgs(args, nargs, "dd")) return js->makeError("type mismatch");
  return JsEngine::makeNumber(
      JsEngine::getNumber(args[0]) + JsEngine::getNumber(args[1]));
}

js->set(js->glob(), "add", JsEngine::makeFunction(add));
```

`chkArgs` format:

- `d` number
- `b` boolean
- `s` string
- `j` any JS value

### Runtime controls

```cpp
js->setGcThreshold(1024);
js->setMaxCss(5000);
js->gc();

size_t total=0, lwm=0, css=0;
js->stats(&total, &lwm, &css);
```

---

## Syntax and Semantics

### Literals and values

```javascript
undefined; null; true; false;
123; 3.14; 0x10;
"abc"; 'xyz';
({a:1, b:2});
[1,2,3];
```

### Variables and assignment

- supported: `let`, `const`
- compound assignment: `+= -= *= /= %= <<= >>= >>>= &= ^= |=`
- postfix increments: `i++`, `i--`
- reassigning `const` raises `assignment to constant`

### Operators

- arithmetic: `+ - * / % **`
- bitwise: `~ & | ^ << >> >>>`
- compare: `< <= > >=`
- equality: `== != === !==`
- logical: `! && ||`
- ternary: `?:`
- `typeof`

### Control flow

- `if / else`
- `for`, `while`, `do...while`
- `switch`
- `break`, `continue`
- `try / catch / finally`
- `throw`
- `return` (inside function only)

### Functions

- function declarations and function expressions
- IIFE
- arrow functions:
  - `x => x * 2`
  - `(a,b) => a + b`
  - `(x) => { return x * 2; }`

### Statement termination

Semicolons are supported as expected.  
There is also newline-based statement finish logic (`js_stmt_finish`), but explicit semicolons are still recommended.

---

## Built-in Methods (Core)

From core runtime (`internal.cpp`), without extension modules.

### String instance methods

- `length`
- `charCodeAt(index)`
- `charAt(index)`
- `indexOf(substr[, from])`
- `substring(start[, end])`

### Array methods

- `length`
- `push`, `pop`, `shift`
- `slice`, `reverse`, `sort`, `splice`
- `forEach`, `map`, `every`, `some`, `find`, `findIndex`, `reduce`
- `indexOf`, `join`

Callback signatures:

- `forEach/map/every/some/find/findIndex`: `(elem, index, array)`
- `reduce`: `(acc, elem, index, array)`

---

## Extension Modules

Controlled by CMake options:

- `BUILD_CONSOLE`
- `BUILD_GLOBAL`
- `BUILD_STRING`
- `BUILD_DATE`
- `BUILD_JSON`
- `BUILD_MATH`

Runtime binding example:

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

### String (global object)

- `String.fromCharCode(...codes)`

### Date

- `Date.now()`
- `Date.format([timestampMs][, format])`

### Math

Constants:
`E, LN10, LN2, LOG10E, LOG2E, PI, SQRT1_2, SQRT2`

Functions:
`abs acos acosh asin asinh atan atan2 atanh cbrt ceil clz32 cos cosh exp expm1 f16round floor fround hypot imul log log10 log1p log2 max min pow random round sign sin sinh sqrt sumPrecise tan tanh trunc`

### JSON

- `JSON.parse(text)`
- `JSON.stringify(value)`

---

## Unsupported Features

The following are currently unsupported and can produce `not implemented` or parse/runtime errors:

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

Also unsupported: ES module system, Promise-based async APIs, full prototype-chain behavior, etc.

---

## Errors and Debugging

Common errors from runtime:

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

Most errors include `at line N col M`.

`JsEngine::dump()` is empty by default; enable with compile definition:

- `EMJS_ENABLE_DUMP=1`

---

## Build and Size Options

CMake option:

- `EMJS_OPTIMIZE_SIZE` (default `ON`)

Source macros:

- `JS_EXPR_MAX` (default `20`)
- `JS_GC_THRESHOLD` (default `0.25`)
- `EMJS_ENABLE_DUMP` (default `0`)

---

## Source Mapping

- Interpreter core: `src/core.cpp`
- Public API: `src/engine.h`, `src/engine.cpp`
- Array/String runtime internals: `src/internal.cpp`
- Extension bind example: `tests/test_js.cpp`
- Script examples: `tests/script/*.js`

When docs differ from behavior, source code and test output are the source of truth.

