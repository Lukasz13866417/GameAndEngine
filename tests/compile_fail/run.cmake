if(NOT DEFINED VNG_CXX OR NOT DEFINED VNG_INCLUDE_DIR OR NOT DEFINED VNG_SOURCE)
    message(FATAL_ERROR "compile-failure test is missing its compiler, include directory, or source")
endif()

execute_process(
    COMMAND "${VNG_CXX}" -std=c++23 -I${VNG_INCLUDE_DIR} -fsyntax-only "${VNG_SOURCE}"
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_output
    ERROR_VARIABLE compile_error
)

set(compiler_diagnostic "${compile_output}${compile_error}")
if(compile_result EQUAL 0)
    message(FATAL_ERROR
        "${VNG_SOURCE} unexpectedly compiled; the negative API contract is not enforced")
endif()

if(DEFINED VNG_EXPECT)
    string(FIND "${compiler_diagnostic}" "${VNG_EXPECT}" expected_position)
    if(expected_position EQUAL -1)
        message(FATAL_ERROR
            "${VNG_SOURCE} failed for an unexpected reason. Expected '${VNG_EXPECT}'.\n"
            "Compiler output:\n${compiler_diagnostic}")
    endif()
endif()
