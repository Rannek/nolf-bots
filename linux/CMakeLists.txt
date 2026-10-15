# Linux cross build of the NOLF Modernizer game code (Final Release config).
#   cmake -S linux -B build-linux -G Ninja -DCMAKE_TOOLCHAIN_FILE=linux/toolchain-clang-cl-x86.cmake
#   ninja -C build-linux
cmake_minimum_required(VERSION 3.20)
project(NOLFLinuxBuild C CXX RC)

get_filename_component(SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(LT2 ${SRC_ROOT}/LT2)
set(LITH ${LT2}/lithshared)
set(NOLF ${SRC_ROOT}/NOLF)
set(LIBS ${SRC_ROOT}/LIBS)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(VFS_OVERLAY ${CMAKE_BINARY_DIR}/case-insensitive.yaml)
execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_vfsoverlay.py ${SRC_ROOT} ${VFS_OVERLAY})

# Static CRT (/MT), like the vcxproj Final Release configs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_compile_options(
  "SHELL:-Xclang -ivfsoverlay -Xclang ${VFS_OVERLAY}"
  /O2 /GF /Gy /EHsc /W0
  /Zc:forScope- /Zc:wchar_t /permissive
  -fms-compatibility -fms-extensions -fdelayed-template-parsing
  -Wno-everything
)
add_compile_definitions(NDEBUG WIN32 _WINDOWS _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS NO_PRAGMA_LIBS)
add_link_options(/SAFESEH:NO)

# The tree comes from Windows: file names in project files often differ in case
# from the files on disk.  Resolve each path component case-insensitively.
function(resolve_ci out path)
  get_filename_component(path "${path}" ABSOLUTE)
  string(REPLACE "/" ";" parts "${path}")
  set(cur "")
  foreach(p ${parts})
    if(p STREQUAL "")
      continue()
    endif()
    if(EXISTS "${cur}/${p}")
      set(cur "${cur}/${p}")
    else()
      file(GLOB cands RELATIVE "${cur}" "${cur}/*")
      string(TOLOWER "${p}" pl)
      set(found "")
      foreach(c ${cands})
        string(TOLOWER "${c}" cl)
        if(cl STREQUAL pl)
          set(found "${c}")
        endif()
      endforeach()
      if(found STREQUAL "")
        message(FATAL_ERROR "resolve_ci: cannot find ${path}")
      endif()
      set(cur "${cur}/${found}")
    endif()
  endforeach()
  set(${out} "${cur}" PARENT_SCOPE)
endfunction()

# Read the ClCompile list straight from a .vcxproj so VS and Linux builds stay in sync.
function(vcxproj_sources out proj)
  get_filename_component(dir ${proj} DIRECTORY)
  file(STRINGS ${proj} lines REGEX "<ClCompile Include=\"[^\"]+\"")
  set(srcs)
  foreach(l ${lines})
    string(REGEX REPLACE ".*<ClCompile Include=\"([^\"]+)\".*" "\\1" f "${l}")
    string(REPLACE "\\" "/" f "${f}")
    resolve_ci(f "${dir}/${f}")
    list(APPEND srcs ${f})
  endforeach()
  set(${out} ${srcs} PARENT_SCOPE)
endfunction()

# --- Monolith shared static libs (built from source) -----------------------
add_library(MFCStub STATIC ${LITH}/mfcstub/mfcs_point.cpp ${LITH}/mfcstub/mfcs_rect.cpp ${LITH}/mfcstub/mfcs_string.cpp)
target_include_directories(MFCStub PRIVATE ${LITH}/stdlith ${LITH}/lith ${LT2}/sdk/inc)

add_library(CryptMgr STATIC ${LITH}/cryptmgr/blowfish.cpp ${LITH}/cryptmgr/CryptMgr.cpp)

add_library(ButeMgr STATIC ${LITH}/butemgr/arange.cpp ${LITH}/butemgr/avector.cpp ${LITH}/butemgr/ButeMgr.cpp)
target_include_directories(ButeMgr PRIVATE ${LITH}/stdlith ${LITH}/mfcstub ${LITH}/cryptmgr ${LITH}/lith ${LT2}/sdk/inc ${LITH}/incs)
target_compile_definitions(ButeMgr PRIVATE _NOMFC _REZFILE _FINAL _NOLFBUILD NO_TRACE)

set(PREBUILT_LIBS ${LITH}/libs/Release)
set(COMMON_INCS ${LT2}/sdk/inc ${LITH}/incs ${NOLF}/Shared ${LITH}/butemgr ${LITH}/cryptmgr ${LITH}/lith ${LITH}/mfcstub ${LIBS}/SDL2-2.0.10/include)

# --- Object.lto (server game code) -----------------------------------------
vcxproj_sources(OBJECT_SRCS ${NOLF}/ObjectDLL/Object.vcxproj)
add_library(Object SHARED ${OBJECT_SRCS})
set_target_properties(Object PROPERTIES OUTPUT_NAME Object SUFFIX ".lto" PREFIX "")
target_include_directories(Object PRIVATE ${COMMON_INCS} ${NOLF}/ObjectDLL)
target_compile_definitions(Object PRIVATE _REZFILE _FINAL _NOLFBUILD)
target_link_directories(Object PRIVATE ${PREBUILT_LIBS} ${LIBS}/SDL2-2.0.10/lib/x86)
target_link_libraries(Object PRIVATE ButeMgr CryptMgr MFCStub GameSpyMgr StdLith RegMgr SDL2 SDL2main
  user32 gdi32 advapi32 wsock32 winmm odbc32 odbccp32)
target_link_options(Object PRIVATE /NODEFAULTLIB:libcimtd.lib)

# --- CShell.dll (client game code) -----------------------------------------
vcxproj_sources(CSHELL_SRCS ${NOLF}/ClientShellDLL/ClientShellDLL.vcxproj)
add_library(CShell SHARED ${CSHELL_SRCS})
set_target_properties(CShell PROPERTIES OUTPUT_NAME CShell PREFIX "")
target_include_directories(CShell PRIVATE ${COMMON_INCS} ${NOLF}/ClientShellDLL ${NOLF}/ClientRes ${LIBS}/detours/include)
target_compile_definitions(CShell PRIVATE _REZFILE _FINAL _NOLFBUILD _CLIENTBUILD)
target_link_directories(CShell PRIVATE ${PREBUILT_LIBS} ${LIBS}/SDL2-2.0.10/lib/x86 ${LIBS}/detours/lib.X86)
target_link_libraries(CShell PRIVATE ButeMgr CryptMgr MFCStub GameSpyClientMgr StdLith RegMgr ltguimgr_LT2 LithFontMgr_LT2 SDL2 SDL2main detours
  user32 gdi32 advapi32 wsock32 winmm odbc32 odbccp32 dinput8 dxguid)
target_link_options(CShell PRIVATE /NODEFAULTLIB:LIBCMTD)

# --- CRes.dll (string resources) ------------------------------------------
add_library(CRes SHARED ${NOLF}/ClientRes/main.cpp ${NOLF}/ClientRes/ClientRes.rc)
set_target_properties(CRes PROPERTIES OUTPUT_NAME CRes PREFIX "")
target_include_directories(CRes PRIVATE ${NOLF}/Shared ${NOLF}/ClientRes ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_source_files_properties(${NOLF}/ClientRes/ClientRes.rc PROPERTIES COMPILE_OPTIONS "/C;1252")

# --- deploy: copy the DLLs into a directory the game loads as a .rez ---------
set(NOLF_GAME_DIR "${SRC_ROOT}/../Games/NOLF" CACHE PATH "NOLF install directory")
add_custom_target(deploy ALL
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${NOLF_GAME_DIR}/BotsRez ${NOLF_GAME_DIR}/BotsClientRez
  COMMAND ${CMAKE_COMMAND} -E make_directory ${NOLF_GAME_DIR}/BotsRez ${NOLF_GAME_DIR}/BotsClientRez
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:Object> ${NOLF_GAME_DIR}/BotsRez/
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:CShell> $<TARGET_FILE:CRes> ${NOLF_GAME_DIR}/BotsClientRez/
  DEPENDS Object CShell CRes
  COMMENT "Deploying server DLL to BotsRez, client DLLs to BotsClientRez")
