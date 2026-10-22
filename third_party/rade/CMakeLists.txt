# RADE V1 receive (and later transmit): rade_c + a static Opus with
# LPCNet/FARGAN, both fetched and SHA-checked (see TTC-VENDOR.md).
include(FetchContent)
include(ExternalProject)

FetchContent_Declare(rade_c
  URL https://github.com/freedv/rade_c/archive/c8a3dc156045cae2cd251e1a4be0c304c9ddf2f9.tar.gz
  URL_HASH SHA256=bc89be47a141cca9da1e8cc73e1de9b6e96f05984fda5c30056760d69ba23bff
  DOWNLOAD_EXTRACT_TIMESTAMP NO
  SOURCE_SUBDIR no-cmake-here)                 # sources only, its CMake unused
message(STATUS "RADE: fetching rade_c (once, ~87 MB)")
FetchContent_MakeAvailable(rade_c)
set(RADE_SRC ${rade_c_SOURCE_DIR}/src)
set(RADE_SRC ${RADE_SRC} PARENT_SCOPE)
set(RADE_WAV_DIR ${rade_c_SOURCE_DIR} PARENT_SCOPE)   # FDV_offair.wav for raderxtest

set(_opus ${CMAKE_CURRENT_BINARY_DIR}/opus)
ExternalProject_Add(rade_opus
  URL https://github.com/xiph/opus/archive/940d4e5af64351ca8ba8390df3f555484c567fbb.zip
  URL_HASH SHA256=20e37f9079ac2b80e3235cd8ce2547829e147bf44a2f5dd28a888fa3e9c24341
  DOWNLOAD_EXTRACT_TIMESTAMP NO
  SOURCE_DIR ${_opus}
  BUILD_IN_SOURCE 1
  PATCH_COMMAND sh -c "patch dnn/nnet.h < ${RADE_SRC}/opus-nnet.h.diff && patch dnn/nnet.c < ${RADE_SRC}/opus-nnet.c.diff"
  # autogen.sh fetches the DNN model data and checks its sha256.
  CONFIGURE_COMMAND sh -c "./autogen.sh && ./configure --with-pic --enable-osce --enable-dred --disable-shared --disable-doc --disable-extra-programs"
  BUILD_COMMAND make -j8
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS ${_opus}/.libs/libopus.a)

add_library(rade_opus_lib STATIC IMPORTED GLOBAL)
set_target_properties(rade_opus_lib PROPERTIES IMPORTED_LOCATION ${_opus}/.libs/libopus.a)
add_dependencies(rade_opus_lib rade_opus)

# rade_api.c serves V1 and V2, so both sets compile (the V2 weights are
# ~63 MB more C — slow once, then ccache).
add_library(rade STATIC
  ${RADE_SRC}/rade_api.c
  ${RADE_SRC}/rade_enc.c ${RADE_SRC}/rade_dec.c
  ${RADE_SRC}/rade_enc_data.c ${RADE_SRC}/rade_dec_data.c
  ${RADE_SRC}/rade_dsp.c ${RADE_SRC}/rade_ofdm.c ${RADE_SRC}/rade_bpf.c
  ${RADE_SRC}/rade_acq.c ${RADE_SRC}/rade_tx.c ${RADE_SRC}/rade_rx.c
  ${RADE_SRC}/rade_enc_v2.c ${RADE_SRC}/rade_dec_v2.c ${RADE_SRC}/rade_sync.c
  ${RADE_SRC}/rade_enc_v2_data.c ${RADE_SRC}/rade_dec_v2_data.c ${RADE_SRC}/rade_sync_data.c
  ${RADE_SRC}/rade_v2_ofdm.c ${RADE_SRC}/rade_tx_v2.c ${RADE_SRC}/rade_rx_v2.c
  text/rade_text.c text/mpdecode_core.c text/HRA_56_56.c text/ldpc_codes.c
  text/gp_interleaver.c
  ttc_rade_shim.c)
add_dependencies(rade rade_opus)
set_target_properties(rade PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(rade PRIVATE IS_BUILDING_RADE_API=1)
# Weight arrays: no debug info (it only bloats the objects by hundreds of MB).
set_source_files_properties(
  ${RADE_SRC}/rade_enc_data.c ${RADE_SRC}/rade_dec_data.c
  ${RADE_SRC}/rade_enc_v2_data.c ${RADE_SRC}/rade_dec_v2_data.c ${RADE_SRC}/rade_sync_data.c
  PROPERTIES COMPILE_OPTIONS "-g0")
target_include_directories(rade PUBLIC
  ${RADE_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/text ${CMAKE_CURRENT_SOURCE_DIR}
  ${_opus}/dnn ${_opus}/celt ${_opus}/include ${_opus})
target_link_libraries(rade PUBLIC rade_opus_lib m)
add_library(ttc::rade ALIAS rade)
# The shim sees Opus's configure results (OPUS_HAVE_RTCD -> runtime SIMD).
set_source_files_properties(ttc_rade_shim.c PROPERTIES COMPILE_DEFINITIONS HAVE_CONFIG_H)
