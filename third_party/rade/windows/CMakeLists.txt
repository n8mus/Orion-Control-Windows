# Static MSVC build of the console's "rade" library (rade_c + rade_text codec
# + ttc_rade_shim) against an already-built static Opus (see README.md here).
#
#   cmake -S cmake -B build/rade-msvc -G Ninja -DCMAKE_BUILD_TYPE=Release
#         -DRADE_C_DIR=<rade_c root> -DTTC_RADE_DIR=<app>/third_party/rade
#         -DOPUS_SRC=<opus source> -DOPUS_BIN=<opus cmake build dir>
#         -DCMAKE_INSTALL_PREFIX=<install>
cmake_minimum_required(VERSION 3.20)
project(ttc_rade_win C)

foreach(v RADE_C_DIR TTC_RADE_DIR OPUS_SRC OPUS_BIN)
  file(TO_CMAKE_PATH "${${v}}" ${v})          # tolerate backslashes
  if(NOT ${v})
    message(FATAL_ERROR "set -D${v}=...")
  endif()
endforeach()
set(RADE_SRC ${RADE_C_DIR}/src)
set(TXT ${TTC_RADE_DIR}/text)

set(CMAKE_C_STANDARD 11)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")   # /MD, as Qt and Opus

# Same source list as third_party/rade/CMakeLists.txt in the app.
add_library(rade STATIC
  ${RADE_SRC}/rade_api.c
  ${RADE_SRC}/rade_enc.c ${RADE_SRC}/rade_dec.c
  ${RADE_SRC}/rade_enc_data.c ${RADE_SRC}/rade_dec_data.c
  ${RADE_SRC}/rade_dsp.c ${RADE_SRC}/rade_ofdm.c ${RADE_SRC}/rade_bpf.c
  ${RADE_SRC}/rade_acq.c ${RADE_SRC}/rade_tx.c ${RADE_SRC}/rade_rx.c
  ${RADE_SRC}/rade_enc_v2.c ${RADE_SRC}/rade_dec_v2.c ${RADE_SRC}/rade_sync.c
  ${RADE_SRC}/rade_enc_v2_data.c ${RADE_SRC}/rade_dec_v2_data.c ${RADE_SRC}/rade_sync_data.c
  ${RADE_SRC}/rade_v2_ofdm.c ${RADE_SRC}/rade_tx_v2.c ${RADE_SRC}/rade_rx_v2.c
  ${TXT}/rade_text.c ${TXT}/mpdecode_core.c ${TXT}/HRA_56_56.c ${TXT}/ldpc_codes.c
  ${TXT}/gp_interleaver.c
  ${TTC_RADE_DIR}/ttc_rade_shim.c)

target_include_directories(rade PRIVATE
  ${RADE_SRC} ${TXT} ${TTC_RADE_DIR}
  ${OPUS_SRC}/dnn ${OPUS_SRC}/celt ${OPUS_SRC}/include ${OPUS_SRC} ${OPUS_BIN})

# RADE_EXPORT empty: static library, and the GCC attribute rade_c's nnet.h
# patch would otherwise supply does not parse under MSVC.
target_compile_definitions(rade PRIVATE
  IS_BUILDING_RADE_API=1 "RADE_EXPORT=" _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
target_compile_options(rade PRIVATE /W3 /wd4244 /wd4305 /wd4267)

# The shim must see what Opus was configured with (autotools puts this in
# config.h; Opus's CMake passes it as target definitions and leaves config.h
# empty). Without OPUS_HAVE_RTCD + the MAY_HAVE set, cpu_support.h turns
# opus_select_arch() into "return 0" and LPCNet feature extraction never
# leaves the plain-C kernels. Keep this list equal to the opus target's
# DEFINES in <OPUS_BIN>/build.ninja.
set_source_files_properties(${TTC_RADE_DIR}/ttc_rade_shim.c PROPERTIES
  COMPILE_DEFINITIONS
  "HAVE_CONFIG_H;OPUS_HAVE_RTCD;OPUS_X86_MAY_HAVE_SSE;OPUS_X86_MAY_HAVE_SSE2;OPUS_X86_MAY_HAVE_SSE4_1;OPUS_X86_MAY_HAVE_AVX2;OPUS_X86_PRESUME_SSE;OPUS_X86_PRESUME_SSE2;ENABLE_DEEP_PLC;ENABLE_DRED;ENABLE_OSCE;ENABLE_HARDENING;DISABLE_DEBUG_FLOAT;HAVE_LRINT;HAVE_LRINTF;USE_ALLOCA")

install(TARGETS rade ARCHIVE DESTINATION lib)
install(FILES ${OPUS_BIN}/opus.lib DESTINATION lib)

# Headers, mirrored so the consumer's include path can match the Linux one
# (rade src, text, shim dir, opus dnn/celt/include).
file(GLOB _rade_h ${RADE_SRC}/*.h)
install(FILES ${_rade_h} DESTINATION include/rade)
file(GLOB _text_h ${TXT}/*.h)
install(FILES ${_text_h} DESTINATION include/text)
install(FILES ${TTC_RADE_DIR}/ttc_rade_shim.h DESTINATION include)
install(DIRECTORY ${OPUS_SRC}/include/ DESTINATION include/opus/include FILES_MATCHING PATTERN "*.h")
install(DIRECTORY ${OPUS_SRC}/celt/ DESTINATION include/opus/celt FILES_MATCHING PATTERN "*.h"
  PATTERN "tests" EXCLUDE PATTERN "dump_modes" EXCLUDE PATTERN "meson*" EXCLUDE)
install(DIRECTORY ${OPUS_SRC}/dnn/ DESTINATION include/opus/dnn FILES_MATCHING PATTERN "*.h"
  PATTERN "torch" EXCLUDE PATTERN "training_tf2" EXCLUDE PATTERN "models" EXCLUDE
  PATTERN "datasets" EXCLUDE PATTERN "meson*" EXCLUDE PATTERN "adaconvtest*" EXCLUDE)

# The two recordings the app's tests read through RADE_WAV_DIR
# (raderxtest/sdrrxtest: FDV_offair.wav; radetxtest/asrtest: input_sample.wav).
install(FILES ${RADE_C_DIR}/FDV_offair.wav ${RADE_C_DIR}/input_sample.wav ${RADE_C_DIR}/LICENSE
  DESTINATION rade_c_src)
install(FILES ${RADE_C_DIR}/LICENSE DESTINATION licenses RENAME rade_c-LICENSE.txt)
install(FILES ${OPUS_SRC}/COPYING DESTINATION licenses RENAME opus-COPYING.txt)
