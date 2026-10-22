cmake_minimum_required(VERSION 3.25)

if(NOT FFTW3_FOUND)
    message(FATAL_ERROR "WDSP 2.10 requires FFTW3")
endif()

find_package(Threads REQUIRED)

file(GLOB WDSP_UPSTREAM_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/upstream/*.c")

# tentec-console: the two generated NNR model sources (34.7 MB of C) are
# not committed here — they are fetched from TAPR's upstream at the pinned
# revision (COMMIT) and SHA-256 checked, so the public repo stays small.
# NNR builds its networks when the RX channel opens, so they are required.
set(WDSP_NNR_BASE "https://raw.githubusercontent.com/TAPR/OpenHPSDR-wdsp/b02d5bac675dd2f33ec2bab2b339f79a597c47dd/wdsp%202.10/Source")
set(WDSP_NNR_0_SHA256 af9174a63cd0683efed03feb97d45419ca52a881943c25153299b02aed607a98)
set(WDSP_NNR_1_SHA256 62593d935c5a7b4970fac988b76aa28d038e97f7b56a48480a3be7346749a331)
foreach(m 0 1)
    set(_f "${CMAKE_CURRENT_BINARY_DIR}/nnr_model_${m}.c")
    if(NOT EXISTS "${_f}")
        message(STATUS "WDSP: fetching nnr_model_${m}.c from TAPR (once)")
        file(DOWNLOAD "${WDSP_NNR_BASE}/nnr_model_${m}.c" "${_f}"
             EXPECTED_HASH SHA256=${WDSP_NNR_${m}_SHA256}
             STATUS _st TLS_VERIFY ON)
        list(GET _st 0 _code)
        if(NOT _code EQUAL 0)
            file(REMOVE "${_f}")
            message(FATAL_ERROR "WDSP: could not fetch nnr_model_${m}.c (${_st}). "
                "Copy it from ${WDSP_NNR_BASE} into ${CMAKE_CURRENT_BINARY_DIR}/ "
                "(SHA-256 ${WDSP_NNR_${m}_SHA256}) and re-run cmake.")
        endif()
    endif()
    list(APPEND WDSP_UPSTREAM_SOURCES "${_f}")
endforeach()
# Upstream's unreferenced debug-only snoop TU uses a pre-C99 implicit-int
# function definition. Keep the snapshot pristine and leave that dormant
# diagnostic out of the portable library.
list(FILTER WDSP_UPSTREAM_SOURCES EXCLUDE REGEX "/snoop\\.c$")

add_library(aether_wdsp STATIC
    ${WDSP_UPSTREAM_SOURCES}
    port/wdsp_port.c
)
add_library(aether::wdsp ALIAS aether_wdsp)

set_target_properties(aether_wdsp PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_include_directories(aether_wdsp
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/upstream"
        ${FFTW3_INCLUDE_DIRS}
        $<$<NOT:$<PLATFORM_ID:Windows>>:${CMAKE_CURRENT_SOURCE_DIR}/port/include>
)

target_link_directories(aether_wdsp PRIVATE ${FFTW3_LIBRARY_DIRS})
target_link_libraries(aether_wdsp PRIVATE ${FFTW3_LIBRARIES} Threads::Threads)

if(WIN32)
    target_compile_definitions(aether_wdsp PRIVATE FFTW_DLL)
    if(MSVC)
        target_compile_options(aether_wdsp PRIVATE
            "/FI${CMAKE_CURRENT_SOURCE_DIR}/port/include/wdsp_alloc_guard.h")
    else()
        target_compile_options(aether_wdsp PRIVATE
            -include "${CMAKE_CURRENT_SOURCE_DIR}/port/include/wdsp_alloc_guard.h")
    endif()
    target_link_libraries(aether_wdsp PRIVATE avrt)
else()
    # WDSP 2.10 introduced three TUs -- extrapolate.c, nurbs_fit.c and
    # nurbs_spline.c -- that call _aligned_malloc()/_aligned_free() without
    # including comm.h. On MSVC those resolve to the CRT and the omission is
    # invisible; everywhere else they are macros in the port header, which
    # those TUs never reach, and the build fails with implicit declarations.
    #
    # Force-include the port header for the same reason Windows force-includes
    # its allocation guard above: the portability surface is the build's job,
    # not a patch to the pristine snapshot. The header is #pragma once and its
    # whole body is #ifndef _WIN32, so this is idempotent for the TUs that
    # already reach it through comm.h -> Windows.h.
    target_compile_options(aether_wdsp PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/port/include/wdsp_port.h")
endif()

if(MSVC)
    target_compile_options(aether_wdsp PRIVATE /w)
else()
    target_compile_options(aether_wdsp PRIVATE -w)
endif()
