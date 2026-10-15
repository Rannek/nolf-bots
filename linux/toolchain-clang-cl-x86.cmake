# Cross-compile NOLF game DLLs on Linux with clang-cl + lld-link against an
# xwin-splatted MSVC CRT / Windows SDK (x86).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(XWIN_DIR "$ENV{HOME}/.local/share/xwin" CACHE PATH "xwin splat output")

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)

set(CMAKE_C_COMPILER_TARGET i686-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET i686-pc-windows-msvc)

set(_xwin_inc
  "/imsvc${XWIN_DIR}/crt/include"
  "/imsvc${XWIN_DIR}/sdk/include/ucrt"
  "/imsvc${XWIN_DIR}/sdk/include/um"
  "/imsvc${XWIN_DIR}/sdk/include/shared")
string(JOIN " " _xwin_inc_str ${_xwin_inc})

set(CMAKE_C_FLAGS_INIT   "--target=i686-pc-windows-msvc -fms-compatibility-version=19.29 ${_xwin_inc_str}")
set(CMAKE_CXX_FLAGS_INIT "--target=i686-pc-windows-msvc -fms-compatibility-version=19.29 ${_xwin_inc_str}")
set(CMAKE_RC_FLAGS_INIT  "-I${XWIN_DIR}/sdk/include/um -I${XWIN_DIR}/sdk/include/shared")

set(_xwin_lib "/libpath:${XWIN_DIR}/crt/lib/x86 /libpath:${XWIN_DIR}/sdk/lib/um/x86 /libpath:${XWIN_DIR}/sdk/lib/ucrt/x86")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_xwin_lib}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_xwin_lib}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_xwin_lib}")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
