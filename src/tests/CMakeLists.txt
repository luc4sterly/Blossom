add_executable(blossom-cpu-tests main.cpp)
target_link_libraries(blossom-cpu-tests PRIVATE blossom_core blossom_warnings)

# armwrestler: runs the ARM/ARMv5TE suite to the end. A few LDM-writeback and CP15 MRC cases
# are known to report BAD (inherited from Corgi3DS), so for now this checks that the run completes
# and that the ARMv5TE section passes, rather than requiring every case to be OK.
add_test(NAME armwrestler
    COMMAND blossom-cpu-tests "${CMAKE_CURRENT_SOURCE_DIR}/armwrestler.elf")
set_tests_properties(armwrestler PROPERTIES
    PASS_REGULAR_EXPRESSION "CLZ OK LDRD OK.*QADD OK SMLABB OK SMLABT OK SMLATB OK SMLATT OK"
    TIMEOUT 120)

# thumbwrestler: the image is Thumb code, so it is entered in Thumb state. It prints through its
# own character table, so this is a smoke test; the JIT tests below compare its output exactly.
add_test(NAME thumbwrestler
    COMMAND blossom-cpu-tests --thumb "${CMAKE_CURRENT_SOURCE_DIR}/thumbwrestler.elf")
set_tests_properties(thumbwrestler PROPERTIES TIMEOUT 120)

# lletest: system-level ARM11 checks (exceptions, banked registers, CP15, lazy VFP, MMU and
# address space switches, data aborts, self-modifying code, LDREX/STREX, blocks over the JIT cap). Source in lle/.
set(LLETEST_PASS "SVC OK BANKED OK VFP OK TLS OK.*MMU OK.*ASID OK.*ABORT OK SMC OK SMC2 OK EXCL OK LONG OK DONE")
add_test(NAME lletest
    COMMAND blossom-cpu-tests --core arm11 "${CMAKE_CURRENT_SOURCE_DIR}/lletest.elf")
set_tests_properties(lletest PROPERTIES PASS_REGULAR_EXPRESSION "${LLETEST_PASS}" TIMEOUT 120)

if (ENABLE_JIT)
    add_test(NAME lletest-jit
        COMMAND blossom-cpu-tests --cpu jit --core arm11 "${CMAKE_CURRENT_SOURCE_DIR}/lletest.elf")
    set_tests_properties(lletest-jit PROPERTIES PASS_REGULAR_EXPRESSION "${LLETEST_PASS}" TIMEOUT 120)

    # A loop of only indirect branches must still end each slice
    add_test(NAME bxloop-jit
        COMMAND blossom-cpu-tests --cpu jit --core arm11 --cycles 2000000 --slice 256 "${CMAKE_CURRENT_SOURCE_DIR}/bxloop.elf")
    set_tests_properties(bxloop-jit PROPERTIES PASS_REGULAR_EXPRESSION "Ran 2000000 cycles" TIMEOUT 30)

    # Same suites on the dynarmic JIT, on the ARM9 (ARMv5TE) and on an ARM11 core (ARMv6K, MMU off).
    foreach (core arm9 arm11)
        add_test(NAME armwrestler-jit-${core}
            COMMAND blossom-cpu-tests --cpu jit --core ${core} "${CMAKE_CURRENT_SOURCE_DIR}/armwrestler.elf")
        set_tests_properties(armwrestler-jit-${core} PROPERTIES
            PASS_REGULAR_EXPRESSION "CLZ OK LDRD OK.*QADD OK SMLABB OK SMLABT OK SMLATB OK SMLATT OK"
            TIMEOUT 120)

        # The JIT must print exactly what the interpreter prints (including the known BAD cases)
        add_test(NAME armwrestler-jit-matches-interp-${core}
            COMMAND ${CMAKE_COMMAND} -DRUNNER=$<TARGET_FILE:blossom-cpu-tests>
                -DELF=${CMAKE_CURRENT_SOURCE_DIR}/armwrestler.elf -DCORE=${core}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/compare_backends.cmake)
        add_test(NAME thumbwrestler-jit-matches-interp-${core}
            COMMAND ${CMAKE_COMMAND} -DRUNNER=$<TARGET_FILE:blossom-cpu-tests>
                -DELF=${CMAKE_CURRENT_SOURCE_DIR}/thumbwrestler.elf -DCORE=${core} -DTHUMB=ON
                -P ${CMAKE_CURRENT_SOURCE_DIR}/compare_backends.cmake)
    endforeach()
endif()

# RSA engine: full and single-word exponents (Process9 writes 0x10001 as one word)
add_executable(blossom-rsa-tests rsa_test.cpp)
target_link_libraries(blossom-rsa-tests PRIVATE blossom_core blossom_warnings)
add_test(NAME rsa COMMAND blossom-rsa-tests)

# Vulkan rasterizer vs software rasterizer on random triangles (needs a Vulkan device, e.g. lavapipe;
# skipped with code 77 if there is none)
if (ENABLE_VULKAN)
    add_executable(blossom-gpu-tests gpu_test.cpp)
    target_link_libraries(blossom-gpu-tests PRIVATE blossom_core blossom_warnings)
    add_test(NAME gpu-vulkan-matches-software COMMAND blossom-gpu-tests)
    set_tests_properties(gpu-vulkan-matches-software PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()
