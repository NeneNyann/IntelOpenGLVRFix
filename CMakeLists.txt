cmake_minimum_required(VERSION 4.2)

project(IntelOpenGLVRFix LANGUAGES CXX)

if(NOT WIN32)
   message(FATAL_ERROR "IntelOpenGLVRFix only supports Windows")
endif()
if(NOT MSVC)
   message(FATAL_ERROR "IntelOpenGLVRFix requires the MSVC toolchain")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
   message(FATAL_ERROR "IntelOpenGLVRFix must be built for x64")
endif()

get_filename_component(MSVC_TOOL_DIRECTORY "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_program(DUMPBIN_EXECUTABLE NAMES dumpbin
   HINTS "${MSVC_TOOL_DIRECTORY}" REQUIRED)
find_program(MSVC_LIB_EXECUTABLE NAMES lib
   HINTS "${MSVC_TOOL_DIRECTORY}" REQUIRED)

file(TO_CMAKE_PATH "$ENV{SystemRoot}/System32/opengl32.dll" SYSTEM_OPENGL)
if(NOT EXISTS "${SYSTEM_OPENGL}")
   message(FATAL_ERROR "System OpenGL loader not found: ${SYSTEM_OPENGL}")
endif()

set(OPENGL_EXPORT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/opengl_exports")
set(OPENGL_EXPORT_DEFINITION "${OPENGL_EXPORT_DIRECTORY}/opengl32.def")
set(OPENGL_EXPORT_LIBRARY "${OPENGL_EXPORT_DIRECTORY}/forwarders.lib")
set(OPENGL_EXPORT_OBJECT "${OPENGL_EXPORT_DIRECTORY}/forwarders.exp")
set(OPENGL_EXPORT_TOOL "${CMAKE_CURRENT_SOURCE_DIR}/cmake/OpenGLExports.cmake")

add_custom_command(
   OUTPUT "${OPENGL_EXPORT_DEFINITION}" "${OPENGL_EXPORT_LIBRARY}" "${OPENGL_EXPORT_OBJECT}"
   COMMAND "${CMAKE_COMMAND}" -E make_directory "${OPENGL_EXPORT_DIRECTORY}"
   COMMAND "${CMAKE_COMMAND}"
      "-DDUMPBIN=${DUMPBIN_EXECUTABLE}"
      "-DSYSTEM_OPENGL=${SYSTEM_OPENGL}"
      "-DOUTPUT_DEFINITION=${OPENGL_EXPORT_DEFINITION}"
      -P "${OPENGL_EXPORT_TOOL}"
   COMMAND "${MSVC_LIB_EXECUTABLE}" /nologo /machine:x64
      "/def:${OPENGL_EXPORT_DEFINITION}" "/out:${OPENGL_EXPORT_LIBRARY}"
   DEPENDS "${OPENGL_EXPORT_TOOL}"
   COMMENT "Generating the Windows OpenGL export table"
   VERBATIM
)

set_source_files_properties("${OPENGL_EXPORT_OBJECT}" PROPERTIES
   EXTERNAL_OBJECT TRUE
   GENERATED TRUE
)

add_library(opengl32 SHARED
   patch/runtime.h
   patch/proxy.cpp
   patch/dxinterop.cpp
   "${OPENGL_EXPORT_OBJECT}"
)

target_compile_features(opengl32 PRIVATE cxx_std_17)
target_compile_definitions(opengl32 PRIVATE UNICODE _UNICODE)
target_compile_options(opengl32 PRIVATE
   /W4 /WX /utf-8 /permissive-
   $<$<CONFIG:Release>:/Brepro>
   $<$<CONFIG:Release>:/experimental:deterministic>
   $<$<CONFIG:Release>:/pathmap:${CMAKE_CURRENT_SOURCE_DIR}=.>
)
target_link_libraries(opengl32 PRIVATE Kernel32)
target_link_options(opengl32 PRIVATE
   /INCREMENTAL:NO /OPT:REF /OPT:ICF
   $<$<CONFIG:Release>:/Brepro>
   $<$<CONFIG:Release>:/DEBUG:NONE>
)

set_target_properties(opengl32 PROPERTIES
   PREFIX ""
   OUTPUT_NAME "opengl32"
   MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
   INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
   RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin"
   RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/bin"
   ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib"
   ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/lib"
   PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/pdb"
   PDB_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/pdb"
)

set(OPENVR_API_DLL "" CACHE FILEPATH "Original openvr_api.dll used to generate proxy exports")
if(NOT OPENVR_API_DLL)
   file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/openvr/openvrpaths.vrpath" OPENVR_PATHS_FILE)
   if(EXISTS "${OPENVR_PATHS_FILE}")
      file(READ "${OPENVR_PATHS_FILE}" OPENVR_PATHS_JSON)
      string(JSON STEAMVR_DIRECTORY ERROR_VARIABLE OPENVR_PATHS_ERROR GET "${OPENVR_PATHS_JSON}" runtime 0)
      if(NOT OPENVR_PATHS_ERROR)
         set(OPENVR_API_DLL "${STEAMVR_DIRECTORY}/bin/win64/openvr_api.dll")
      endif()
   endif()
endif()
if(NOT EXISTS "${OPENVR_API_DLL}")
   message(FATAL_ERROR "SteamVR bin/win64/openvr_api.dll not found; set OPENVR_API_DLL to its path")
endif()

set(OPENVR_EXPORT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/openvr_exports")
set(OPENVR_EXPORT_DEFINITION "${OPENVR_EXPORT_DIRECTORY}/openvr_api.def")
set(OPENVR_EXPORT_LIBRARY "${OPENVR_EXPORT_DIRECTORY}/forwarders.lib")
set(OPENVR_EXPORT_OBJECT "${OPENVR_EXPORT_DIRECTORY}/forwarders.exp")
set(OPENVR_EXPORT_TOOL "${CMAKE_CURRENT_SOURCE_DIR}/cmake/OpenVRExports.cmake")

add_custom_command(
   OUTPUT "${OPENVR_EXPORT_DEFINITION}" "${OPENVR_EXPORT_LIBRARY}" "${OPENVR_EXPORT_OBJECT}"
   COMMAND "${CMAKE_COMMAND}" -E make_directory "${OPENVR_EXPORT_DIRECTORY}"
   COMMAND "${CMAKE_COMMAND}"
      "-DDUMPBIN=${DUMPBIN_EXECUTABLE}"
      "-DOPENVR_API=${OPENVR_API_DLL}"
      "-DOUTPUT_DEFINITION=${OPENVR_EXPORT_DEFINITION}"
      -P "${OPENVR_EXPORT_TOOL}"
   COMMAND "${MSVC_LIB_EXECUTABLE}" /nologo /machine:x64
      "/def:${OPENVR_EXPORT_DEFINITION}" "/out:${OPENVR_EXPORT_LIBRARY}"
   DEPENDS "${OPENVR_EXPORT_TOOL}" "${OPENVR_API_DLL}"
   COMMENT "Generating the OpenVR forwarding export table"
   VERBATIM
)

set_source_files_properties("${OPENVR_EXPORT_OBJECT}" PROPERTIES
   EXTERNAL_OBJECT TRUE
   GENERATED TRUE
)

add_library(openvr_api SHARED
   preloader/preloader.cpp
   "${OPENVR_EXPORT_OBJECT}"
)

add_dependencies(openvr_api opengl32)
target_compile_features(openvr_api PRIVATE cxx_std_17)
target_compile_definitions(openvr_api PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(openvr_api PRIVATE
   /W4 /WX /utf-8 /permissive-
   $<$<CONFIG:Release>:/Brepro>
   $<$<CONFIG:Release>:/experimental:deterministic>
   $<$<CONFIG:Release>:/pathmap:${CMAKE_CURRENT_SOURCE_DIR}=.>
)
target_link_libraries(openvr_api PRIVATE OpenGL32.lib)
target_link_options(openvr_api PRIVATE
   /NOENTRY /INCREMENTAL:NO /OPT:REF /OPT:ICF /DEPENDENTLOADFLAG:0x100
   $<$<CONFIG:Release>:/Brepro>
   $<$<CONFIG:Release>:/DEBUG:NONE>
)

set_target_properties(openvr_api PROPERTIES
   PREFIX ""
   OUTPUT_NAME "openvr_api"
   INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
   RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin"
   RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/bin"
   ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib"
   ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/lib"
   PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/pdb"
   PDB_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/pdb"
)
