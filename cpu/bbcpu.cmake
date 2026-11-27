# libbbcpu.so: guest_cpu.h on FEXCore, linked statically against FEX's libraries.
set(BBCPU_DIR "${CMAKE_CURRENT_LIST_DIR}")
add_library(bbcpu SHARED "${BBCPU_DIR}/guest_cpu_fex.cpp" "${BBCPU_DIR}/host_call_arm64.S")
target_include_directories(bbcpu PRIVATE "${BBCPU_DIR}/../src" "${CMAKE_SOURCE_DIR}/Source" "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(bbcpu PRIVATE FEXCore Common JemallocLibs fmt::fmt)
target_link_options(bbcpu PRIVATE -Wl,--no-undefined)
target_compile_options(bbcpu PRIVATE ${FEX_TUNE_COMPILE_FLAGS})
set_target_properties(bbcpu PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN TRUE)

if(ANDROID)
  # Embedded FEX runs no FEXServer. Its Linux IPC client also requires a noexcept
  # std::function assignment unsupported by libc++; keep it out of Android Common.
  # Adapted from Bloodborne-Android cpu/bbcpu.cmake at 2e472653.
  get_target_property(bb_common_sources Common SOURCES)
  list(FILTER bb_common_sources EXCLUDE REGEX "FEXServerClient\\.cpp$")
  set_target_properties(Common PROPERTIES SOURCES "${bb_common_sources}")
endif()
if(ANDROID)
  target_compile_options(FEXCore_Base PRIVATE -include "${BBCPU_DIR}/bionic_compat.h")
endif()
