cmake_minimum_required(VERSION 3.16)
project(devinfo VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# These binaries are meant to be dropped onto a machine nobody administers, so
# they must not depend on a runtime that machine may not have: MSVC defaults to
# /MD, which needs the VC++ redistributable on the target, and gcc defaults to a
# shared libstdc++, which needs a matching distro package.
if(MSVC)
  cmake_policy(SET CMP0091 NEW)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(NOT APPLE AND NOT WIN32)
  add_link_options(-static-libstdc++ -static-libgcc)
endif()

# ---- platform sources: one file per OS, everything else shared ----
set(DEVINFO_SRC src/core.cpp src/table.cpp src/emit.cpp)
if(APPLE)
  list(APPEND DEVINFO_SRC src/apple.cpp)
elseif(WIN32)
  list(APPEND DEVINFO_SRC src/win32.cpp)
else()
  list(APPEND DEVINFO_SRC src/linux.cpp)
endif()

find_package(Threads REQUIRED)

add_library(devinfo_core STATIC ${DEVINFO_SRC})
target_include_directories(devinfo_core PUBLIC include)
target_link_libraries(devinfo_core PUBLIC Threads::Threads)

# The static library is also linked into the shared plugin. macOS defaults to
# PIC and hides this; Linux and Windows do not, so be explicit.
set_target_properties(devinfo_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(APPLE)
  target_compile_options(devinfo_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(devinfo_core PUBLIC "-framework IOKit" "-framework CoreFoundation")
elseif(WIN32)
  # CPU Sets (hybrid P/E topology) need the Windows 10 SDK surface. Set for
  # every Windows toolchain, not just MSVC, or mingw compiles the block out.
  target_compile_definitions(devinfo_core PUBLIC _WIN32_WINNT=0x0A00)
  target_link_libraries(devinfo_core PUBLIC dxgi setupapi version)
  if(MSVC)
    target_compile_options(devinfo_core PRIVATE /W3 /utf-8)
  else()
    target_compile_options(devinfo_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
    # GetProcAddress returns FARPROC; casting it to the real signature is the
    # canonical Win32 idiom, and MSVC does not diagnose it at all. ggc and clang
    # each spell the warning differently.
    target_compile_options(devinfo_core PRIVATE
      -Wno-cast-function-type -Wno-cast-function-type-mismatch)
  endif()
else()
  target_compile_options(devinfo_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(devinfo_core PUBLIC ${CMAKE_DL_LIBS})
endif()

# ---- plugin proper: C ABI shared library ----
add_library(devinfo SHARED src/capi.cpp)
target_link_libraries(devinfo PRIVATE devinfo_core)
set_target_properties(devinfo PROPERTIES
  OUTPUT_NAME devinfo
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
if(WIN32)
  # mingw prepends "lib"; Windows users expect devinfo.dll from either toolchain.
  set_target_properties(devinfo PROPERTIES PREFIX "")
endif()
if(MSVC)
  target_compile_options(devinfo PRIVATE /utf-8)
endif()

# ---- command line ----
add_executable(devinfo-cli src/main.cpp)
target_link_libraries(devinfo-cli PRIVATE devinfo_core)
set_target_properties(devinfo-cli PROPERTIES OUTPUT_NAME devinfo)

if(WIN32 AND NOT MSVC)
  # mingw/llvm-mingw link libc++ and libunwind dynamically by default, which
  # leaves an .exe that will not start on a machine without those DLLs. The
  # point of this tool is to be dropped onto a strange box and run.
  target_link_options(devinfo-cli PRIVATE -static)
  target_link_options(devinfo PRIVATE -static)
endif()
