cmake_minimum_required (VERSION 3.15)
project (emjs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED true)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE MinSizeRel)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_CONSOLE "Whether build console extension" ON)
option(BUILD_GLOBAL "Whether build global extension" ON)
option(BUILD_STRING "Whether build string extension" ON)
option(BUILD_DATE "Whether build date extension" ON)
option(BUILD_JSON "Whether build json extension" ON)
option(BUILD_MATH "Whether build math extension" ON)
option(EMJS_OPTIMIZE_SIZE "Enable aggressive size optimization flags" ON)
option(EMJS_ENGINE_DUMP "Enable dump for engine" OFF)
option(EMJS_TESTS "Enable build tests" ON)

set(EJS_EXTENSION_FILES )

if(BUILD_CONSOLE)
    list(APPEND EJS_EXTENSION_FILES src/extension/console.cpp )
endif()
if(BUILD_GLOBAL)
    list(APPEND EJS_EXTENSION_FILES src/extension/global.cpp )
endif()
if(BUILD_STRING)
    list(APPEND EJS_EXTENSION_FILES src/extension/string.cpp )
endif()
if(BUILD_DATE)
    list(APPEND EJS_EXTENSION_FILES src/extension/date.cpp )
endif()
if(BUILD_JSON)
    list(APPEND EJS_EXTENSION_FILES src/extension/json.cpp )
endif()
if(BUILD_MATH)
    list(APPEND EJS_EXTENSION_FILES src/extension/math.cpp )
endif()

aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/src EJS_SOURCE_FILES)
add_library(emjs-core STATIC ${EJS_SOURCE_FILES})
add_library(emjs-exts STATIC ${EJS_EXTENSION_FILES})

if(EMJS_OPTIMIZE_SIZE)
    set(EMJS_SIZE_FLAGS
        -Os
        -fno-exceptions
        -fno-rtti
        -fno-unwind-tables
        -fno-asynchronous-unwind-tables
        -fomit-frame-pointer
    )
    target_compile_options(emjs-core PRIVATE ${EMJS_SIZE_FLAGS})
    target_compile_options(emjs-exts PRIVATE ${EMJS_SIZE_FLAGS})
endif()

add_executable(emjs app.cpp)
target_link_libraries(emjs emjs-core emjs-exts)

if(BUILD_CONSOLE)
    target_compile_definitions(emjs PRIVATE EMJS_BUILD_CONSOLE)
endif()
if(BUILD_STRING)
    target_compile_definitions(emjs PRIVATE EMJS_BUILD_STRING)
endif()
if(BUILD_GLOBAL)
    target_compile_definitions(emjs PRIVATE EMJS_BUILD_GLOBAL)
endif()
if(BUILD_DATE)
    target_compile_definitions(emjs PRIVATE EMJS_BUILD_DATE)
endif()
if(BUILD_MATH)
    target_compile_definitions(emjs PRIVATE EMJS_BUILD_MATH)
endif()
if(BUILD_JSON)
    target_compile_definitions(emjs PRIVATE EMJS_BUILD_JSON)
endif()

if(EMJS_OPTIMIZE_SIZE)
    target_compile_options(emjs PRIVATE ${EMJS_SIZE_FLAGS})
endif()

if(EMJS_OPTIMIZE_SIZE)
    if(APPLE)
        target_link_options(emjs PRIVATE -Wl,-dead_strip)
    else()
        target_link_options(emjs PRIVATE -Wl,--gc-sections)
    endif()
endif()

if(EMJS_ENGINE_DUMP)
    target_compile_definitions(emjs-core PRIVATE EMJS_ENABLE_DUMP)
endif()


if(EMJS_TESTS)
    add_executable(test_js tests/test_js.cpp)
    target_link_libraries(test_js emjs-core emjs-exts)

    if(EMJS_OPTIMIZE_SIZE)
        if(APPLE)
            target_link_options(test_js PRIVATE -Wl,-dead_strip)
        else()
            target_link_options(test_js PRIVATE -Wl,--gc-sections)
        endif()
    endif()
endif()

install(TARGETS emjs emjs-core emjs-exts
    RUNTIME DESTINATION bin
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY src/
    DESTINATION include/emjs
    FILES_MATCHING PATTERN "*.h"
)
