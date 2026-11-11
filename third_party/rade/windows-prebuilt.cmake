# RADE on Windows: an imported target for the prebuilt MSVC libraries
# (x64, /MD, Release) that third_party/rade/windows/ builds - see the README
# there. Included from the top-level CMakeLists when -DRADE_WIN_ROOT points
# at that build's install folder:
#
#   <RADE_WIN_ROOT>/lib/rade.lib      rade_c + the rade_text codec + ttc_rade_shim
#   <RADE_WIN_ROOT>/lib/opus.lib      Opus with DRED/OSCE (LPCNet + FARGAN),
#                                     runtime SIMD dispatch
#   <RADE_WIN_ROOT>/include/...       the headers RadeRx / RadeTx reach
#   <RADE_WIN_ROOT>/rade_c_src/       the two recordings the tests read
#
# Static, so nothing extra ships beside the exe. The Linux build fetches and
# compiles the same pinned sources inside CMake (CMakeLists.txt here); that
# recipe leans on autotools, which is why Windows takes them prebuilt.
add_library(rade_opus_lib STATIC IMPORTED GLOBAL)
set_target_properties(rade_opus_lib PROPERTIES
  IMPORTED_LOCATION "${RADE_WIN_ROOT}/lib/opus.lib")

add_library(rade STATIC IMPORTED GLOBAL)
set_target_properties(rade PROPERTIES
  IMPORTED_LOCATION "${RADE_WIN_ROOT}/lib/rade.lib"
  IMPORTED_LINK_INTERFACE_LANGUAGES C)
# Same order as the Linux target: rade src, text, the shim's dir, Opus.
target_include_directories(rade INTERFACE
  "${RADE_WIN_ROOT}/include/rade"
  "${RADE_WIN_ROOT}/include/text"
  "${RADE_WIN_ROOT}/include"
  "${RADE_WIN_ROOT}/include/opus/dnn"
  "${RADE_WIN_ROOT}/include/opus/celt"
  "${RADE_WIN_ROOT}/include/opus/include")
# Static library: no dllimport. Without this rade_api.h falls back to
# "__declspec(dllimport) __stdcall", which MSVC cannot even parse.
target_compile_definitions(rade INTERFACE "RADE_EXPORT=")
target_link_libraries(rade INTERFACE rade_opus_lib)
add_library(ttc::rade ALIAS rade)

set(RADE_WAV_DIR "${RADE_WIN_ROOT}/rade_c_src")
