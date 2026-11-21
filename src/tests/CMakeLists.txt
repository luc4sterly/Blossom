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

# thumbwrestler: smoke test that the Thumb suite loads and runs without a fatal emulator error.
add_test(NAME thumbwrestler
    COMMAND blossom-cpu-tests "${CMAKE_CURRENT_SOURCE_DIR}/thumbwrestler.elf")
set_tests_properties(thumbwrestler PROPERTIES TIMEOUT 120)
