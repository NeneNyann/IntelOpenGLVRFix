execute_process(
   COMMAND "${DUMPBIN}" /nologo /exports "${OPENVR_API}"
   OUTPUT_VARIABLE DUMP_OUTPUT
   COMMAND_ERROR_IS_FATAL ANY
)

string(REPLACE "\n" ";" DUMP_LINES "${DUMP_OUTPUT}")
set(DEFINITION "LIBRARY openvr_api\nEXPORTS\n")

foreach(LINE IN LISTS DUMP_LINES)
   if(LINE MATCHES
      "^[ \t]+([0-9]+)[ \t]+[0-9A-Fa-f]+[ \t]+[0-9A-Fa-f]+[ \t]+([^ \t\r]+)")
      set(ORDINAL "${CMAKE_MATCH_1}")
      set(NAME "${CMAKE_MATCH_2}")
      string(APPEND DEFINITION "    ${NAME}=openvr_api_original.${NAME} @${ORDINAL}\n")
   endif()
endforeach()

file(WRITE "${OUTPUT_DEFINITION}" "${DEFINITION}")
