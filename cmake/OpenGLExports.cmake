execute_process(
   COMMAND "${DUMPBIN}" /nologo /exports "${SYSTEM_OPENGL}"
   OUTPUT_VARIABLE DUMP_OUTPUT
   COMMAND_ERROR_IS_FATAL ANY
)

get_filename_component(SYSTEM_DIRECTORY "${SYSTEM_OPENGL}" DIRECTORY)
file(TO_NATIVE_PATH "${SYSTEM_DIRECTORY}/opengl32" SYSTEM_OPENGL_MODULE)

string(REPLACE "\n" ";" DUMP_LINES "${DUMP_OUTPUT}")
set(DEFINITION "LIBRARY opengl32\nEXPORTS\n")

foreach(LINE IN LISTS DUMP_LINES)
   if(LINE MATCHES
      "^[ \t]+([0-9]+)[ \t]+[0-9A-Fa-f]+[ \t]+[0-9A-Fa-f]+[ \t]+([^ \t\r]+)")
      set(ORDINAL "${CMAKE_MATCH_1}")
      set(NAME "${CMAKE_MATCH_2}")
      if(NAME STREQUAL "wglGetProcAddress")
         set(TARGET "ProxyWGLGetProcAddress")
      else()
         set(TARGET "${SYSTEM_OPENGL_MODULE}.${NAME}")
      endif()
      string(APPEND DEFINITION "    ${NAME}=${TARGET} @${ORDINAL}\n")
   endif()
endforeach()

file(WRITE "${OUTPUT_DEFINITION}" "${DEFINITION}")
