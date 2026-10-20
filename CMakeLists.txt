cmake_minimum_required(VERSION 3.20)
project(marina CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

# ---------------------------------------------------------------------------
# libmarina_runtime — static library linked into every compiled marina binary
# (exports the stable C ABI called by the generated LLVM IR).
# ---------------------------------------------------------------------------
file(GLOB MARINA_RUNTIME_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/runtime/src/*.cpp)
add_library(marina_runtime STATIC ${MARINA_RUNTIME_SOURCES})
target_include_directories(marina_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/runtime/include)
target_link_libraries(marina_runtime PUBLIC SQLite::SQLite3 Threads::Threads)
target_compile_options(marina_runtime PRIVATE -O2)
# the runtime uses POSIX sockets / unix domain sockets / etc.
find_library(LIBRT rt)

# ---------------------------------------------------------------------------
# libmarina — the compiler library (lexer, parser, checker, codegen, pipeline)
# ---------------------------------------------------------------------------
add_library(marina STATIC
  compiler/src/lexer.cpp
  compiler/src/parser.cpp
  compiler/src/checker.cpp
  compiler/src/codegen.cpp
  compiler/src/pipeline.cpp
)
target_include_directories(marina PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/compiler/include)
target_compile_options(marina PRIVATE)

# ---------------------------------------------------------------------------
# wave — package manager & build tool
# ---------------------------------------------------------------------------
add_executable(wave wave/src/main.cpp)
target_link_libraries(wave PRIVATE marina)
target_compile_options(wave PRIVATE)

# ---------------------------------------------------------------------------
# marina-lsp — language server (JSON-RPC over stdio)
# ---------------------------------------------------------------------------
add_executable(marina-lsp compiler/lsp/main.cpp)
target_link_libraries(marina-lsp PRIVATE marina)
target_compile_options(marina-lsp PRIVATE)

# ---------------------------------------------------------------------------
# debug tools
# ---------------------------------------------------------------------------
add_executable(tokdump compiler/tools/tokdump.cpp)
target_link_libraries(tokdump PRIVATE marina)

add_executable(astdump compiler/tools/astdump.cpp)
target_link_libraries(astdump PRIVATE marina)

# the compiler embeds the workspace root at compile time (used to locate the
# runtime library, mirroring the old CARGO_MANIFEST_DIR behaviour)
target_compile_definitions(marina PRIVATE
  MARINA_WORKSPACE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
  MARINA_RUNTIME_LIB="$<TARGET_FILE:marina_runtime>"
)
