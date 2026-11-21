# Runs a test ELF on the interpreter and on the JIT and fails unless both print the same thing.
# Usage: cmake -DRUNNER=<blossom-cpu-tests> -DELF=<file> -DCORE=arm9|arm11 [-DTHUMB=ON] -P compare_backends.cmake
set(extra_args)
if (THUMB)
    list(APPEND extra_args --thumb)
endif()

foreach (cpu interp jit)
    execute_process(
        COMMAND "${RUNNER}" --cpu ${cpu} --core ${CORE} ${extra_args} "${ELF}"
        OUTPUT_VARIABLE out_${cpu}
        ERROR_VARIABLE err_${cpu}
        RESULT_VARIABLE res_${cpu}
        TIMEOUT 120)
    # The backend banner is the only line that is expected to differ
    string(REGEX REPLACE "\\[Emulator\\] CPU backend:[^\n]*\n" "" out_${cpu} "${out_${cpu}}")
endforeach()

if (NOT res_interp EQUAL res_jit)
    message(FATAL_ERROR "Exit codes differ: interpreter ${res_interp}, JIT ${res_jit}\n${err_jit}")
endif()
if (NOT out_interp STREQUAL out_jit)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${CORE}-interp.txt" "${out_interp}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${CORE}-jit.txt" "${out_jit}")
    message(FATAL_ERROR "Interpreter and JIT output differ (see ${CORE}-interp.txt / ${CORE}-jit.txt)")
endif()
message(STATUS "Interpreter and JIT agree (${CORE})")
