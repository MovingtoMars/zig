cmake_minimum_required(VERSION 2.8)

set(CMAKE_BUILD_TYPE "Debug" CACHE STRING
    "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)

project(zig CXX)
set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake ${CMAKE_MODULE_PATH})

set(ZIG_VERSION_MAJOR 0)
set(ZIG_VERSION_MINOR 0)
set(ZIG_VERSION_PATCH 0)
set(ZIG_VERSION "${ZIG_VERSION_MAJOR}.${ZIG_VERSION_MINOR}.${ZIG_VERSION_PATCH}")
message("Configuring zig version ${ZIG_VERSION}")

find_package(llvm)
include_directories(${LLVM_INCLUDE_DIRS})
link_directories(${LLVM_LIBDIRS})

find_package(clang)
include_directories(${CLANG_INCLUDE_DIRS})

include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}
)

set(ZIG_SOURCES
    "${CMAKE_SOURCE_DIR}/src/ast_render.cpp"
    "${CMAKE_SOURCE_DIR}/src/bignum.cpp"
    "${CMAKE_SOURCE_DIR}/src/tokenizer.cpp"
    "${CMAKE_SOURCE_DIR}/src/parser.cpp"
    "${CMAKE_SOURCE_DIR}/src/analyze.cpp"
    "${CMAKE_SOURCE_DIR}/src/codegen.cpp"
    "${CMAKE_SOURCE_DIR}/src/buffer.cpp"
    "${CMAKE_SOURCE_DIR}/src/error.cpp"
    "${CMAKE_SOURCE_DIR}/src/main.cpp"
    "${CMAKE_SOURCE_DIR}/src/os.cpp"
    "${CMAKE_SOURCE_DIR}/src/util.cpp"
    "${CMAKE_SOURCE_DIR}/src/errmsg.cpp"
    "${CMAKE_SOURCE_DIR}/src/zig_llvm.cpp"
    "${CMAKE_SOURCE_DIR}/src/parseh.cpp"
)

set(TEST_SOURCES
    "${CMAKE_SOURCE_DIR}/src/buffer.cpp"
    "${CMAKE_SOURCE_DIR}/src/util.cpp"
    "${CMAKE_SOURCE_DIR}/src/os.cpp"
    "${CMAKE_SOURCE_DIR}/test/run_tests.cpp"
)

set(C_HEADERS
    "${CMAKE_SOURCE_DIR}/c_headers/adxintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/ammintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/arm_acle.h"
    "${CMAKE_SOURCE_DIR}/c_headers/avx2intrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/avx512bwintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/avx512cdintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/avx512dqintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/avx512erintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/avx512fintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/avx512vlbwintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/avx512vldqintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/avx512vlintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/avxintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/bmi2intrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/bmiintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/cpuid.h"
    "${CMAKE_SOURCE_DIR}/c_headers/cuda_builtin_vars.h"
    "${CMAKE_SOURCE_DIR}/c_headers/emmintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/f16cintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/float.h"
    "${CMAKE_SOURCE_DIR}/c_headers/fma4intrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/fmaintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/fxsrintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/htmintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/htmxlintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/ia32intrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/immintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/Intrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/inttypes.h"
    "${CMAKE_SOURCE_DIR}/c_headers/iso646.h"
    "${CMAKE_SOURCE_DIR}/c_headers/limits.h"
    "${CMAKE_SOURCE_DIR}/c_headers/lzcntintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/mm3dnow.h"
    "${CMAKE_SOURCE_DIR}/c_headers/mmintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/mm_malloc.h"
    "${CMAKE_SOURCE_DIR}/c_headers/nmmintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/pmmintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/popcntintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/prfchwintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/rdseedintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/rtmintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/s390intrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/shaintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/smmintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/stdalign.h"
    "${CMAKE_SOURCE_DIR}/c_headers/stdarg.h"
    "${CMAKE_SOURCE_DIR}/c_headers/stdatomic.h"
    "${CMAKE_SOURCE_DIR}/c_headers/stdbool.h"
    "${CMAKE_SOURCE_DIR}/c_headers/stddef.h"
    "${CMAKE_SOURCE_DIR}/c_headers/__stddef_max_align_t.h"
    "${CMAKE_SOURCE_DIR}/c_headers/stdint.h"
    "${CMAKE_SOURCE_DIR}/c_headers/stdnoreturn.h"
    "${CMAKE_SOURCE_DIR}/c_headers/tbmintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/tgmath.h"
    "${CMAKE_SOURCE_DIR}/c_headers/tmmintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/unwind.h"
    "${CMAKE_SOURCE_DIR}/c_headers/vadefs.h"
    "${CMAKE_SOURCE_DIR}/c_headers/varargs.h"
    "${CMAKE_SOURCE_DIR}/c_headers/vecintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/__wmmintrin_aes.h"
    "${CMAKE_SOURCE_DIR}/c_headers/wmmintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/__wmmintrin_pclmul.h"
    "${CMAKE_SOURCE_DIR}/c_headers/x86intrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/xmmintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/xopintrin.h"
    "${CMAKE_SOURCE_DIR}/c_headers/xtestintrin.h"
)

set(ZIG_STD_SRC
    "${CMAKE_SOURCE_DIR}/std/bootstrap.zig"
    "${CMAKE_SOURCE_DIR}/std/builtin.zig"
    "${CMAKE_SOURCE_DIR}/std/std.zig"
    "${CMAKE_SOURCE_DIR}/std/syscall.zig"
    "${CMAKE_SOURCE_DIR}/std/errno.zig"
    "${CMAKE_SOURCE_DIR}/std/rand.zig"
    "${CMAKE_SOURCE_DIR}/std/mem.zig"
    "${CMAKE_SOURCE_DIR}/std/math.zig"
)

set(C_HEADERS_DEST "lib/zig/include")
set(ZIG_STD_DEST "lib/zig/std")
set(CONFIGURE_OUT_FILE "${CMAKE_BINARY_DIR}/config.h")
configure_file (
    "${CMAKE_SOURCE_DIR}/src/config.h.in"
    ${CONFIGURE_OUT_FILE}
)

include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}
    "${CMAKE_SOURCE_DIR}/src"
)

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -Wno-unused-variable -Wno-unused-but-set-variable")

set(EXE_CFLAGS "-std=c++11 -fno-exceptions -fno-rtti -D_GNU_SOURCE -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS -Werror -Wall -Werror=strict-prototypes -Werror=old-style-definition -Werror=missing-prototypes")

add_executable(zig ${ZIG_SOURCES})
set_target_properties(zig PROPERTIES
    COMPILE_FLAGS ${EXE_CFLAGS})
target_link_libraries(zig LINK_PUBLIC
    ${LLVM_LIBRARIES}
    ${CLANG_LIBRARIES}
)
install(TARGETS zig DESTINATION bin)

install(FILES ${C_HEADERS} DESTINATION ${C_HEADERS_DEST})
install(FILES ${ZIG_STD_SRC} DESTINATION ${ZIG_STD_DEST})

add_executable(run_tests ${TEST_SOURCES})
target_link_libraries(run_tests)
set_target_properties(run_tests PROPERTIES
    COMPILE_FLAGS ${EXE_CFLAGS}
)
