# Converts a binary file into a C header containing an unsigned char array.
# Usage: cmake -DINPUT=file -DOUTPUT=header -DNAME=symbol -P EmbedSpirv.cmake

if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED NAME)
    message(FATAL_ERROR "EmbedSpirv requires INPUT, OUTPUT and NAME")
endif()

file(READ "${INPUT}" hex_data HEX)
string(LENGTH "${hex_data}" hex_length)
math(EXPR byte_length "${hex_length} / 2")

set(bytes "")
math(EXPR last_index "${byte_length} - 1")
foreach(index RANGE ${last_index})
    math(EXPR offset "${index} * 2")
    string(SUBSTRING "${hex_data}" ${offset} 2 byte)
    if(index EQUAL 0)
        set(bytes "0x${byte}")
    else()
        string(APPEND bytes ", 0x${byte}")
    endif()
endforeach()

file(WRITE "${OUTPUT}"
"// Generated from ${INPUT}. Do not edit.\n"
"#pragma once\n"
"static const unsigned char ${NAME}[] = { ${bytes} };\n"
"static const unsigned int ${NAME}_len = ${byte_length}u;\n")
