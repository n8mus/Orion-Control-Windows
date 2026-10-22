# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.25)
project(TenTecSDRConsole VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(MSVC)
  # /utf-8: the source is UTF-8 (arrows/dashes in UI strings) and MSVC's
  # default codepage mangles the literals. _USE_MATH_DEFINES: M_PI et al.
  # NOMINMAX: windows.h's min/max macros collide with std::min/std::max.
  add_compile_options(/utf-8)
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX)
endif()
if(NOT CMAKE_BUILD_TYPE)
  # RelWithDebInfo, NOT Debug: the DSP chain (16k FFT, 24-channel skimmer
  # bank, the SKIM view's STFT) runs 10-30x slower unoptimized, and at
  # the 1 MHz capture rate (since 2026-07-31) a Debug build could not
  # keep up with the IQ stream — the SDRplay service silently dropped
  # transfers to ~40% and every CW decoder garbled on spliced time while
  # the radio-audio path read clean. A full evening of decoder suspicion
  # traced back to this line (2026-08-07). Keep it optimized; -g stays
  # for backtraces.
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Compile cache if available (sudo pacman -S ccache) — MainWindow's split
# translation units all pull the big Qt headers, ccache makes re-configure
# and branch-switch rebuilds near-free.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  message(STATUS "ccache: enabled")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network SerialPort Sql WebSockets)

# Qt Multimedia backs the Windows audio endpoints (src/audio/AudioIo) —
# capture/playback where the PipeWire CLI tools don't exist. Required there;
# optional on Linux, where the parec/pacat paths remain the live-verified
# default and this stays unused.
if(WIN32)
  find_package(Qt6 REQUIRED COMPONENTS Multimedia)
else()
  find_package(Qt6 QUIET COMPONENTS Multimedia)
endif()

# OpenMP parallelizes the skimmer's channel bank (SkimmerEngine.cpp);
# optional — without it the pragma is ignored and the bank runs serial.
find_package(OpenMP QUIET)

# SDRplay RSP source is optional so the core app always builds. Turn on with
# -DBUILD_SDRPLAY=ON once the SDRplay API service is running (Phase 0).
option(BUILD_SDRPLAY "Build the SDRplay RSP IQ source (needs sdrplay_api)" OFF)

# WDSP (Thetis's DSP library, via AetherSDR's vendored copy) for SDR receive
# audio: on when FFTW3 is found (the double-precision libfftw3), else the
# console builds without SDR RX. See third_party/wdsp/TTC-VENDOR.md.
find_path(FFTW3_INCLUDE_DIRS fftw3.h)
find_library(FFTW3_LIBRARIES fftw3)
if(FFTW3_INCLUDE_DIRS AND FFTW3_LIBRARIES AND NOT WIN32)
  set(FFTW3_FOUND TRUE)
  set(FFTW3_LIBRARY_DIRS "")
  enable_language(C)
  add_subdirectory(third_party/wdsp)
  set(HAVE_WDSP TRUE)
  # FreeDV RADE digital voice (SDR receive): fetches rade_c + Opus (see
  # third_party/rade/TTC-VENDOR.md); needs autotools for Opus's build.
  option(BUILD_RADE "FreeDV RADE digital voice on the SDR receiver" ON)
  if(BUILD_RADE)
    add_subdirectory(third_party/rade)
    set(HAVE_RADE TRUE)
  endif()
  message(STATUS "WDSP: SDR receive audio enabled")
else()
  message(STATUS "WDSP: FFTW3 not found — building without SDR receive audio")
endif()

# rnnoise: on Linux a plain -lrnnoise from the distro package; on Windows it
# comes from vcpkg (-DCMAKE_TOOLCHAIN_FILE=.../vcpkg.cmake), where the lib
# and header land on CMake's default search path — find them explicitly so a
# missing port fails at configure time, not deep in the link.
if(WIN32)
  find_path(RNNOISE_INCLUDE_DIR rnnoise.h REQUIRED)
  find_library(RNNOISE_LIBRARY rnnoise REQUIRED)
  add_library(rnnoise INTERFACE IMPORTED)
  target_include_directories(rnnoise INTERFACE "${RNNOISE_INCLUDE_DIR}")
  target_link_libraries(rnnoise INTERFACE "${RNNOISE_LIBRARY}")
endif()

# SDRplay API on Windows installs to a fixed vendor path (the service +
# import lib + header); on Linux the headers/libs are on the system paths.
if(WIN32)
  set(SDRPLAY_ROOT "C:/Program Files/SDRplay/API" CACHE PATH
      "SDRplay API install root (inc/ + x64/)")
endif()

add_executable(tentec-console
  src/app/main.cpp
  src/app/UiScript.cpp          src/app/UiScript.h
  src/app/ClassicLayout.cpp     src/app/ClassicLayout.h src/app/LayoutHost.h
  src/app/PanelLayout.cpp       src/app/PanelLayout.h
  src/app/SideBar.cpp           src/app/SideBar.h
  src/ui/MeterHost.cpp          src/ui/MeterHost.h
  src/ui/CompassRoseWidget.cpp  src/ui/CompassRoseWidget.h
  src/ui/TuneBoxWidget.cpp      src/ui/TuneBoxWidget.h
  src/ui/SolarWidget.cpp        src/ui/SolarWidget.h
  src/app/MainWindow.cpp        src/app/MainWindow.h
  src/app/MainWindowTuning.cpp  src/app/MainWindowOps.cpp
  src/app/MainWindowTools.cpp
  src/app/MainWindowInternal.h
  src/app/Voacap.cpp            src/app/Voacap.h
  src/app/Bands.h
  src/radio/RadioController.h
  src/radio/SerialPort.cpp      src/radio/SerialPort.h
  src/radio/TxMeter.h
  src/radio/LpMeter.cpp         src/radio/LpMeter.h
  src/radio/PmMeter.cpp         src/radio/PmMeter.h
  src/radio/DcuRotor.cpp        src/radio/DcuRotor.h
  src/radio/TenTecOrion.cpp     src/radio/TenTecOrion.h
  src/radio/TenTecOmni7.cpp     src/radio/TenTecOmni7.h
  src/radio/RipAudio.cpp        src/radio/RipAudio.h
  src/radio/TripAudio.cpp       src/radio/TripAudio.h
  src/net/RigctldServer.cpp     src/net/RigctldServer.h
  src/net/SpotClient.cpp        src/net/SpotClient.h
  src/net/PotaClient.cpp        src/net/PotaClient.h
  src/net/FreeDvReporter.cpp    src/net/FreeDvReporter.h
  src/net/PatMonitor.cpp        src/net/PatMonitor.h
  src/net/PskReporter.cpp       src/net/PskReporter.h
  src/ui/FreeDvWindow.cpp       src/ui/FreeDvWindow.h
  src/ui/PanGpuView.h
  src/ui/dss/DssRenderer.cpp
  src/ui/dss/DssRenderer.h
  src/log/Not1mmLog.cpp         src/log/Not1mmLog.h
  src/net/SolarClient.cpp       src/net/SolarClient.h
  src/net/FldigiClient.cpp      src/net/FldigiClient.h
  src/net/RotorClient.cpp       src/net/RotorClient.h
  src/net/RotorServer.cpp       src/net/RotorServer.h
  src/net/RotorLink.cpp         src/net/RotorLink.h
  src/ui/DigiWindow.cpp         src/ui/DigiWindow.h
  src/contest/ContestDef.h
  src/contest/ContestDefs.cpp
  src/contest/ContestEngine.cpp src/contest/ContestEngine.h
  src/contest/ContestDb.cpp     src/contest/ContestDb.h
  src/contest/Cabrillo.cpp      src/contest/Cabrillo.h
  src/contest/CallHistory.cpp   src/contest/CallHistory.h
  src/contest/ContestWindow.cpp src/contest/ContestWindow.h
  src/contest/ContestDeck.cpp   src/contest/ContestDeck.h
  src/contest/PracticeEngine.cpp src/contest/PracticeEngine.h
  src/contest/QtcDialog.cpp     src/contest/QtcDialog.h
  src/log/Adif.cpp              src/log/Adif.h
  src/log/CqrlogSync.cpp        src/log/CqrlogSync.h
  src/log/LogDb.cpp             src/log/LogDb.h
                                src/log/QsoContext.h
  src/log/QslUploader.cpp       src/log/QslUploader.h
  src/log/WsjtxListener.cpp     src/log/WsjtxListener.h
  src/log/QrzLookup.cpp         src/log/QrzLookup.h
  src/ui/OnlineLogsDialog.cpp   src/ui/OnlineLogsDialog.h
  src/ui/LogWindow.cpp          src/ui/LogWindow.h
  src/ui/LogbookWindow.cpp      src/ui/LogbookWindow.h
  src/ui/SpotTableWindow.cpp    src/ui/SpotTableWindow.h
  src/util/BrowserGlobe.cpp     src/util/BrowserGlobe.h
                                src/util/SpotMode.h
  src/util/CtyLookup.cpp        src/util/CtyLookup.h
  src/util/LogbookIndex.cpp     src/util/LogbookIndex.h
  src/util/WatchList.cpp        src/util/WatchList.h
  src/util/TuneTable.cpp        src/util/TuneTable.h
  src/cw/WinKeyer.cpp           src/cw/WinKeyer.h
                                src/cw/CwKeyer.h
  src/cw/OrionKeyer.cpp         src/cw/OrionKeyer.h
  src/cw/CwWindow.cpp           src/cw/CwWindow.h
  src/cw/CwMacros.cpp           src/cw/CwMacros.h
  src/cw/CwDecoder.cpp          src/cw/CwDecoder.h
  src/cw/FldigiCwEngine.cpp     src/cw/FldigiCwEngine.h
  src/cw/AudioCwSource.cpp      src/cw/AudioCwSource.h
  src/cw/SkimmerEngine.cpp      src/cw/SkimmerEngine.h
  src/cw/SkimServer.cpp         src/cw/SkimServer.h
  src/cw/SkimStft.cpp           src/cw/SkimStft.h
  src/ui/SkimmerWindow.cpp      src/ui/SkimmerWindow.h
  src/ui/SkimViewWindow.cpp     src/ui/SkimViewWindow.h
  src/ui/PanadapterWidget.cpp   src/ui/PanadapterWidget.h
  src/ui/SmithChartWidget.cpp   src/ui/SmithChartWidget.h
  src/ui/SwrHistoryDialog.cpp   src/ui/SwrHistoryDialog.h
  src/ui/TuneDialogs.cpp        src/ui/TuneDialogs.h
  src/ui/WinKeyerPanel.cpp      src/ui/WinKeyerPanel.h
  src/ui/DisplayPanel.cpp       src/ui/DisplayPanel.h
  src/ui/SetupDialog.cpp        src/ui/SetupDialog.h
  src/ui/RoutingPanel.cpp       src/ui/RoutingPanel.h
  src/ui/AudioPanel.cpp         src/ui/AudioPanel.h
  src/ui/SMeterWidget.cpp       src/ui/SMeterWidget.h
  src/ui/SMeterSmartMtr.cpp
  src/ui/ControlPanel.cpp       src/ui/ControlPanel.h
  src/ui/VfoBlock.cpp           src/ui/VfoBlock.h
  src/ui/ZoomBlock.cpp          src/ui/ZoomBlock.h
  src/ui/SdrGainPanel.cpp       src/ui/SdrGainPanel.h
  src/ui/FrequencyDisplay.cpp   src/ui/FrequencyDisplay.h
  src/ui/TxBar.cpp              src/ui/TxBar.h
  src/ui/containers/ContainerWidget.cpp src/ui/containers/ContainerWidget.h
  src/ui/containers/ContainerTitleBar.cpp src/ui/containers/ContainerTitleBar.h
  src/ui/containers/ContainerManager.cpp src/ui/containers/ContainerManager.h
  src/ui/containers/FloatingContainerWindow.cpp src/ui/containers/FloatingContainerWindow.h
  src/ui/containers/FramelessResizer.cpp src/ui/containers/FramelessResizer.h src/ui/containers/FramelessMoveHelper.h
  src/audio/ClipDeck.cpp        src/audio/ClipDeck.h
  src/audio/VoiceEq.cpp         src/audio/VoiceEq.h
  src/audio/LiveVoice.cpp       src/audio/LiveVoice.h
  src/dsp/ClientEq.cpp          src/dsp/ClientEq.h
  src/dsp/ClientGate.cpp        src/dsp/ClientGate.h
  src/dsp/ClientDeEss.cpp       src/dsp/ClientDeEss.h
  src/dsp/ClientComp.cpp        src/dsp/ClientComp.h
  src/dsp/ClientPhaseRotator.cpp src/dsp/ClientPhaseRotator.h
  src/dsp/ClientFinalLimiter.cpp src/dsp/ClientFinalLimiter.h
  src/ui/VoiceEqDialog.cpp      src/ui/VoiceEqDialog.h
  src/dsp/Fft.cpp               src/dsp/Fft.h
  src/dsp/TraceShaping.cpp      src/dsp/TraceShaping.h
  src/dsp/SpectrumComputer.cpp  src/dsp/SpectrumComputer.h
  src/sdr/SdrSource.h
  src/sdr/IqRecorder.cpp        src/sdr/IqRecorder.h
  src/sdr/AutoGain.h
  resources/resources.qrc
)
target_include_directories(tentec-console PRIVATE src)
# Version for what the console reports about itself (FreeDV Reporter, PSK
# Reporter): git describe at configure time.
execute_process(COMMAND git describe --tags --always
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE TTC_GIT_VERSION
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT TTC_GIT_VERSION)
  set(TTC_GIT_VERSION ${PROJECT_VERSION})
endif()
target_compile_definitions(tentec-console PRIVATE TTC_VERSION="${TTC_GIT_VERSION}")
# GPU panadapter (PanGpuView): QRhiWidget is Qt 6.7+, QRhi needs the
# semi-public Qt GUI headers (Qt6::GuiPrivate), and its shaders are compiled
# to .qsb at build time (Qt ShaderTools). GLSL 330 — texelFetch and integer
# maths in the shaders; the station's Mesa radeonsi does 4.6. Optional: an
# older Qt (Ubuntu 24.04's 6.4, i.e. CI and the AppImage) builds the CPU paint.
option(BUILD_GPU_PAN "GPU panadapter when Qt allows it (OFF = CPU paint only)" ON)
set(TTC_GPU_PAN OFF)
if(BUILD_GPU_PAN AND Qt6_VERSION VERSION_GREATER_EQUAL 6.7)
  find_package(Qt6 QUIET COMPONENTS GuiPrivate ShaderTools)
  if(TARGET Qt6::GuiPrivate AND COMMAND qt_add_shaders)
    set(TTC_GPU_PAN ON)
  endif()
endif()
if(TTC_GPU_PAN)
  message(STATUS "GPU panadapter: on (Qt ${Qt6_VERSION})")
  target_sources(tentec-console PRIVATE src/ui/PanGpuView.cpp)
  target_compile_definitions(tentec-console PRIVATE HAVE_GPU_PAN)
  qt_add_shaders(tentec-console "pan_shaders"
  PREFIX "/shaders"
  BASE src/ui/shaders
  GLSL "330"
  NOHLSL NOMSL
  FILES
    src/ui/shaders/pan_quad.vert
    src/ui/shaders/pan_overlay.frag
    src/ui/shaders/pan_waterfall.frag
    src/ui/shaders/pan_fill.frag
    src/ui/shaders/pan_line.vert
    src/ui/shaders/pan_line.frag
    src/ui/shaders/pan_trace.vert
    src/ui/shaders/pan_trace.frag
    src/ui/shaders/dss_mesh.vert
    src/ui/shaders/dss_mesh.frag)
target_link_libraries(tentec-console PRIVATE Qt6::GuiPrivate)
else()
  message(STATUS "GPU panadapter: off (needs Qt >= 6.7 with ShaderTools; have ${Qt6_VERSION})")
endif()
target_link_libraries(tentec-console PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::SerialPort Qt6::Sql Qt6::WebSockets rnnoise)

# AetherSDR's meters (GPL-3), their files unchanged in src/ui/aether with
# small stand-ins for the core/ pieces they include (theme from their own
# Default Dark JSON, QSettings under aether/). The Panel layout's meter box
# offers them as faces; Classic never builds one.
# Built against the Qt AetherSDR targets (6.7+, like the GPU pan); older Qt
# (CI's Ubuntu 6.4) builds the meter box with our own face only.
if(Qt6_VERSION VERSION_GREATER_EQUAL 6.7)
  file(GLOB AETHER_METER_SRC CONFIGURE_DEPENDS
    src/ui/aether/*.cpp src/ui/aether/*.h src/ui/aether/core/*.cpp src/ui/aether/core/*.h)
  add_library(aethermeters STATIC ${AETHER_METER_SRC})
  target_include_directories(aethermeters PUBLIC src/ui/aether)
  target_link_libraries(aethermeters PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets)
  target_link_libraries(tentec-console PRIVATE aethermeters)
  target_compile_definitions(tentec-console PRIVATE HAVE_AETHER_METERS)
  # Their faces + theme JSON, in the APP (resources inside a static library
  # are never registered without Q_INIT_RESOURCE — the faces fell back).
  target_sources(tentec-console PRIVATE resources/aether.qrc)
  message(STATUS "AetherSDR meter faces: on")
else()
  message(STATUS "AetherSDR meter faces: off (needs Qt >= 6.7; have ${Qt6_VERSION})")
endif()
if(WIN32)
  # GUI subsystem: no stray console window on double-click. stderr then has
  # no tty, so main.cpp's session-log redirect engages — same behavior as a
  # Linux desktop-icon launch. Explicit redirects (selftest harness) still
  # capture stderr; only the auto-console goes away.
  set_target_properties(tentec-console PROPERTIES WIN32_EXECUTABLE ON)
  # Explorer/taskbar icon, embedded the Windows way (the in-window icon
  # still comes from resources.qrc via setWindowIcon).
  enable_language(RC)
  target_sources(tentec-console PRIVATE packaging/tentec-console.rc)
endif()
if(TARGET Qt6::Multimedia)
  target_sources(tentec-console PRIVATE
    src/audio/AudioIo.cpp src/audio/AudioIo.h)
  target_compile_definitions(tentec-console PRIVATE HAVE_QTMULTIMEDIA)
  target_link_libraries(tentec-console PRIVATE Qt6::Multimedia)

  # Headless capture smoke test (the audio sibling of sdr_probe).
  add_executable(audio_probe EXCLUDE_FROM_ALL
    tools/audio_probe.cpp
    src/audio/AudioIo.cpp src/audio/AudioIo.h)
  target_include_directories(audio_probe PRIVATE src)
  target_compile_definitions(audio_probe PRIVATE HAVE_QTMULTIMEDIA)
  target_link_libraries(audio_probe PRIVATE Qt6::Core Qt6::Multimedia)
endif()
if(OpenMP_CXX_FOUND)
  target_link_libraries(tentec-console PRIVATE OpenMP::OpenMP_CXX)
endif()

# Offline harnesses (not built by default): synthetic-IQ skimmer test and
# the fldigi XML-RPC client test (pair with tests/fake_fldigi.py).
add_executable(skimtest EXCLUDE_FROM_ALL
  tests/skimtest.cpp
  src/cw/CwDecoder.cpp src/cw/FldigiCwEngine.cpp src/cw/SkimmerEngine.cpp)
target_include_directories(skimtest PRIVATE src)
target_link_libraries(skimtest PRIVATE Qt6::Core)

add_executable(fldigitest EXCLUDE_FROM_ALL
  tests/fldigitest.cpp
  src/net/FldigiClient.cpp)
target_include_directories(fldigitest PRIVATE src)
target_link_libraries(fldigitest PRIVATE Qt6::Core Qt6::Network)

add_executable(rotortest EXCLUDE_FROM_ALL
  tests/rotortest.cpp
  src/net/RotorClient.cpp)
target_include_directories(rotortest PRIVATE src)
target_link_libraries(rotortest PRIVATE Qt6::Core Qt6::Network)

# DCU-3 rotor: frame parser against the bench captures of a controller
# streaming while it turns, plus the rotctld server we put in front of it.
# Pure logic — needs no rotor.
add_executable(dcutest EXCLUDE_FROM_ALL
  tests/dcutest.cpp
  src/radio/DcuRotor.cpp src/radio/SerialPort.cpp
  src/net/RotorServer.cpp)
target_include_directories(dcutest PRIVATE src)
target_link_libraries(dcutest PRIVATE Qt6::Core Qt6::Network Qt6::SerialPort)

# WinKeyer wire format against a pty playing the keyer. Asserts the exact
# command bytes for every timing parameter against the K1EL WK3 manual's
# own worked examples. Needs no hardware and must never touch /dev/cwkeyer.
# Plain-language CW macros (no Qt GUI, no keyer).
add_executable(cwmacrotest EXCLUDE_FROM_ALL
  tests/cwmacrotest.cpp src/cw/CwMacros.cpp src/log/QsoContext.h)
target_include_directories(cwmacrotest PRIVATE src)
target_link_libraries(cwmacrotest PRIVATE Qt6::Core)

add_executable(wktest EXCLUDE_FROM_ALL
  tests/wktest.cpp
  src/cw/WinKeyer.cpp  src/cw/WinKeyer.h  src/cw/CwKeyer.h)
target_include_directories(wktest PRIVATE src)
target_link_libraries(wktest PRIVATE Qt6::Core Qt6::SerialPort)

# LP-100A wattmeter frame decode. Pure parser test — needs no hardware.
add_executable(lptest EXCLUDE_FROM_ALL
  tests/lptest.cpp
  src/radio/LpMeter.cpp src/radio/SerialPort.cpp src/radio/TxMeter.h)
target_include_directories(lptest PRIVATE src)
target_link_libraries(lptest PRIVATE Qt6::Core Qt6::SerialPort)

# PowerMaster wattmeter frame decode. Pure parser test — needs no hardware.
add_executable(pmtest EXCLUDE_FROM_ALL
  tests/pmtest.cpp
  src/radio/PmMeter.cpp src/radio/SerialPort.cpp src/radio/TxMeter.h)
target_include_directories(pmtest PRIVATE src)
target_link_libraries(pmtest PRIVATE Qt6::Core Qt6::SerialPort)

# Station log: ADIF round-trip, SQLite insert/dupe-import, the worked-before
# index's merged view + country/band/mode needed-status, bearing math.
add_executable(logtest EXCLUDE_FROM_ALL
  tests/logtest.cpp
  src/log/Adif.cpp src/log/LogDb.cpp
  src/util/LogbookIndex.cpp src/util/CtyLookup.cpp)
target_include_directories(logtest PRIVATE src)
target_link_libraries(logtest PRIVATE Qt6::Core Qt6::Sql)

# Contest engine: normalization, serials, dupe scopes, per-contest scoring,
# ContestDb round-trip, and the Cabrillo build + parse-back self-check.
add_executable(contesttest EXCLUDE_FROM_ALL
  tests/contesttest.cpp
  src/contest/ContestDefs.cpp src/contest/ContestEngine.cpp
  src/contest/ContestDb.cpp src/contest/Cabrillo.cpp
  src/contest/CallHistory.cpp
  src/util/CtyLookup.cpp)
target_include_directories(contesttest PRIVATE src)
target_link_libraries(contesttest PRIVATE Qt6::Core Qt6::Sql)

# cqrlog->console mirror: the TSV->ADIF converter + idempotent re-pull.
add_executable(synctest EXCLUDE_FROM_ALL
  tests/synctest.cpp
  src/log/CqrlogSync.cpp src/log/LogDb.cpp src/log/Adif.cpp
  src/util/CtyLookup.cpp src/util/LogbookIndex.cpp)
target_include_directories(synctest PRIVATE src)
target_link_libraries(synctest PRIVATE Qt6::Core Qt6::Network Qt6::Sql)

# Practice-mode engine: morse timing, the sim caller's QSO protocol,
# per-contest exchange truth, adaptive speed — all on the deterministic
# pump clock (no wall time, no audio, no db: the isolation IS the test).
add_executable(practicetest EXCLUDE_FROM_ALL
  tests/practicetest.cpp
  src/contest/PracticeEngine.cpp
  src/contest/ContestDefs.cpp src/contest/ContestEngine.cpp
  src/util/CtyLookup.cpp)
target_include_directories(practicetest PRIVATE src)
target_link_libraries(practicetest PRIVATE Qt6::Core)

# Contest-window smoke harness: drives the real window offscreen (logs
# QSOs through the widgets, checks db + score), saves a PNG to eyeball.
add_executable(contestuitest EXCLUDE_FROM_ALL
  tests/contestuitest.cpp
  src/contest/ContestWindow.cpp src/contest/ContestDefs.cpp
  src/contest/ContestEngine.cpp src/contest/ContestDb.cpp
  src/contest/Cabrillo.cpp src/contest/CallHistory.cpp
  src/contest/ContestDeck.cpp src/contest/QtcDialog.cpp
  src/contest/PracticeEngine.cpp
  src/net/RotorLink.cpp src/net/RotorClient.cpp src/net/RotorServer.cpp
  src/radio/DcuRotor.cpp src/radio/SerialPort.cpp
  src/cw/CwWindow.cpp src/cw/WinKeyer.cpp src/cw/OrionKeyer.cpp
  src/cw/CwKeyer.h
  src/util/CtyLookup.cpp src/util/LogbookIndex.cpp
  src/log/LogDb.cpp src/log/Adif.cpp src/log/QrzLookup.cpp
  src/ui/SpotTableWindow.cpp src/ui/SpotTableWindow.h)
target_include_directories(contestuitest PRIVATE src)
target_link_libraries(contestuitest PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets
  Qt6::Network Qt6::SerialPort Qt6::Sql)

# Smith-chart render harness: draws a band's stored sweep runs to a PNG
# (offscreen), for eyeballing the chart against real captured data.
add_executable(smithtest EXCLUDE_FROM_ALL
  tests/smithtest.cpp
  src/ui/SmithChartWidget.cpp)
target_include_directories(smithtest PRIVATE src)
target_link_libraries(smithtest PRIVATE Qt6::Widgets)

# Not1MM dupe sheet from a synthetic Not1MM config + SQLite log + plugin.
add_executable(not1mmtest EXCLUDE_FROM_ALL
  tests/not1mmtest.cpp src/log/Not1mmLog.cpp src/log/Not1mmLog.h
  src/util/LogbookIndex.cpp src/util/CtyLookup.cpp src/log/LogDb.cpp src/log/Adif.cpp)
target_include_directories(not1mmtest PRIVATE src)
target_link_libraries(not1mmtest PRIVATE Qt6::Core Qt6::Sql)

# FreeDV reporting offline: PSK Reporter datagram + FreeDV Reporter role.
add_executable(psktest EXCLUDE_FROM_ALL tests/psktest.cpp
  src/net/PskReporter.cpp src/net/PskReporter.h src/net/FreeDvReporter.cpp src/net/FreeDvReporter.h)
target_include_directories(psktest PRIVATE src)
target_link_libraries(psktest PRIVATE Qt6::Core Qt6::Network Qt6::WebSockets)

# Winlink: Pat's websocket JSON -> session / progress, offline.
add_executable(wltest EXCLUDE_FROM_ALL tests/wltest.cpp src/net/PatMonitor.cpp src/net/PatMonitor.h)
target_include_directories(wltest PRIVATE src)
target_link_libraries(wltest PRIVATE Qt6::Core Qt6::Network Qt6::WebSockets)

# WSJT-X UDP listener offline: decodes -> heard list, status, close.
add_executable(wsjtxtest EXCLUDE_FROM_ALL
  tests/wsjtxtest.cpp src/log/WsjtxListener.cpp src/log/WsjtxListener.h
  src/log/Adif.cpp src/log/LogDb.cpp src/util/CtyLookup.cpp
  src/util/LogbookIndex.cpp)
target_include_directories(wsjtxtest PRIVATE src)
target_link_libraries(wsjtxtest PRIVATE Qt6::Core Qt6::Network Qt6::Sql)

# rigctld server vs Pat's (Winlink) client sequence, no radio attached.
add_executable(rigctldtest EXCLUDE_FROM_ALL
  tests/rigctldtest.cpp src/net/RigctldServer.cpp src/net/RigctldServer.h)
target_include_directories(rigctldtest PRIVATE src)
target_link_libraries(rigctldtest PRIVATE Qt6::Core Qt6::Network)

# FreeDV Reporter Socket.IO frame handling, offline.
add_executable(freedvtest EXCLUDE_FROM_ALL
  tests/freedvtest.cpp src/net/FreeDvReporter.cpp src/net/FreeDvReporter.h)
target_include_directories(freedvtest PRIVATE src)
target_link_libraries(freedvtest PRIVATE Qt6::Core Qt6::Network Qt6::WebSockets)

add_executable(skimsrvtest EXCLUDE_FROM_ALL
  tests/skimsrvtest.cpp
  src/cw/SkimServer.cpp)
target_include_directories(skimsrvtest PRIVATE src)
target_link_libraries(skimsrvtest PRIVATE Qt6::Core Qt6::Network)

add_executable(skimreplay EXCLUDE_FROM_ALL
  tests/skimreplay.cpp
  src/cw/CwDecoder.cpp src/cw/FldigiCwEngine.cpp src/cw/SkimmerEngine.cpp
  src/dsp/SpectrumComputer.cpp src/dsp/Fft.cpp
  src/util/CtyLookup.cpp
  resources/resources.qrc)
target_include_directories(skimreplay PRIVATE src)
target_link_libraries(skimreplay PRIVATE Qt6::Core Qt6::Gui)

add_executable(keyertest EXCLUDE_FROM_ALL
  tests/keyertest.cpp
  src/cw/OrionKeyer.cpp src/cw/OrionKeyer.h
  src/cw/CwKeyer.h                     # Q_OBJECT, no .cpp -> list for moc
  src/radio/RadioController.h)
target_include_directories(keyertest PRIVATE src)
target_link_libraries(keyertest PRIVATE Qt6::Core)

add_executable(cwtest EXCLUDE_FROM_ALL
  tests/cwtest.cpp
  src/cw/CwDecoder.cpp src/cw/FldigiCwEngine.cpp)
target_include_directories(cwtest PRIVATE src)
target_link_libraries(cwtest PRIVATE Qt6::Core)

add_executable(autogaintest EXCLUDE_FROM_ALL
  tests/autogaintest.cpp)
target_include_directories(autogaintest PRIVATE src)
target_link_libraries(autogaintest PRIVATE Qt6::Core)

add_executable(trimtest EXCLUDE_FROM_ALL
  tests/trimtest.cpp)
target_include_directories(trimtest PRIVATE src)

add_executable(cohtest EXCLUDE_FROM_ALL
  tests/cohtest.cpp
  src/cw/CwDecoder.cpp src/cw/FldigiCwEngine.cpp src/dsp/Fft.cpp)
target_include_directories(cohtest PRIVATE src)
target_link_libraries(cohtest PRIVATE Qt6::Core)

add_executable(nrtest EXCLUDE_FROM_ALL
  tests/nrtest.cpp
  src/cw/CwDecoder.cpp src/cw/FldigiCwEngine.cpp
  src/audio/SpectralNr.cpp src/dsp/Fft.cpp)
target_include_directories(nrtest PRIVATE src)
target_link_libraries(nrtest PRIVATE Qt6::Core rnnoise)

add_executable(watchtest EXCLUDE_FROM_ALL
  tests/watchtest.cpp
  src/util/WatchList.cpp)
target_include_directories(watchtest PRIVATE src)
target_link_libraries(watchtest PRIVATE Qt6::Core)

# Voice keyer EQ: the ported AetherSDR ClientEq against its own analytic
# response, the offline-render snap, and the VK render (EQ + normalize).
add_executable(vkeqtest EXCLUDE_FROM_ALL
  tests/vkeqtest.cpp
  src/audio/VoiceEq.cpp src/audio/ClipDeck.cpp src/audio/ClipDeck.h
  src/dsp/ClientEq.cpp src/dsp/ClientGate.cpp src/dsp/ClientDeEss.cpp
  src/dsp/ClientComp.cpp src/dsp/ClientPhaseRotator.cpp
  src/dsp/ClientFinalLimiter.cpp)
target_include_directories(vkeqtest PRIVATE src)
target_link_libraries(vkeqtest PRIVATE Qt6::Core)

# Panel-layout boxes (ported from AetherSDR): float/dock/hide/pin and the
# saved state, offscreen.
add_executable(containertest EXCLUDE_FROM_ALL
  tests/containertest.cpp
  src/ui/containers/ContainerWidget.cpp src/ui/containers/ContainerWidget.h
  src/ui/containers/ContainerTitleBar.cpp src/ui/containers/ContainerTitleBar.h
  src/ui/containers/ContainerManager.cpp src/ui/containers/ContainerManager.h
  src/ui/containers/FloatingContainerWindow.cpp src/ui/containers/FloatingContainerWindow.h
  src/ui/containers/FramelessResizer.cpp src/ui/containers/FramelessResizer.h src/ui/containers/FramelessMoveHelper.h)
target_include_directories(containertest PRIVATE src)
target_link_libraries(containertest PRIVATE Qt6::Widgets)

# Tuner/amp settings table behind the pan's tuner box: nearest-point
# lookup, same-kHz overwrite, clamping, JSON round trip. Pure logic.
# AetherSDR's 3D stacked-trace renderer, their own test (src/ui/dss).
add_executable(dsstest EXCLUDE_FROM_ALL
  tests/dsstest.cpp
  src/ui/dss/DssRenderer.cpp)
target_include_directories(dsstest PRIVATE src)
target_link_libraries(dsstest PRIVATE Qt6::Core Qt6::Gui)

add_executable(tunetest EXCLUDE_FROM_ALL
  tests/tunetest.cpp
  src/util/TuneTable.cpp)
target_include_directories(tunetest PRIVATE src)
target_link_libraries(tunetest PRIVATE Qt6::Core)

if(HAVE_WDSP)
  target_sources(tentec-console PRIVATE src/sdr/SdrRx.cpp src/sdr/SdrRx.h
    src/ui/SdrRxDialog.cpp src/ui/SdrRxDialog.h)
  target_compile_definitions(tentec-console PRIVATE HAVE_WDSP)
  # Copy Assist: speech-to-text via the system whisper.cpp (Arch's
  # whisper-cpp; its ggml picks a CPU variant at runtime). Optional.
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(WHISPER QUIET IMPORTED_TARGET whisper)
  endif()
  if(WHISPER_FOUND)
    target_sources(tentec-console PRIVATE src/asr/AsrEngine.cpp src/asr/AsrEngine.h
      src/asr/CallExtract.h src/ui/CopyAssistWindow.cpp src/ui/CopyAssistWindow.h)
    target_compile_definitions(tentec-console PRIVATE HAVE_WHISPER)
    target_link_libraries(tentec-console PRIVATE PkgConfig::WHISPER)
    message(STATUS "Copy Assist: whisper ${WHISPER_VERSION} found")
    add_executable(asrtest EXCLUDE_FROM_ALL tests/asrtest.cpp
      src/asr/AsrEngine.cpp src/asr/AsrEngine.h src/asr/CallExtract.h)
    target_include_directories(asrtest PRIVATE src)
    target_link_libraries(asrtest PRIVATE Qt6::Core PkgConfig::WHISPER)
    if(HAVE_RADE)
      target_compile_definitions(asrtest PRIVATE RADE_WAV_DIR="${RADE_WAV_DIR}")
    endif()
  else()
    message(STATUS "Copy Assist: no whisper (pkg-config whisper) — building without")
  endif()
  if(HAVE_RADE)
    target_sources(tentec-console PRIVATE src/sdr/RadeRx.cpp src/sdr/RadeRx.h
      src/sdr/RadeTx.cpp src/sdr/RadeTx.h)
    target_compile_definitions(tentec-console PRIVATE HAVE_RADE)
    target_link_libraries(tentec-console PRIVATE ttc::rade)
  endif()
  target_link_libraries(tentec-console PRIVATE aether::wdsp)
endif()

# SDR receive chain end to end on synthetic IQ (USB tone, LSB rejection,
# CW pitch, AM) — WDSP through the decimator, audio captured from a pipe.
if(HAVE_RADE)
  # RADE receive on rade_c's off-air recording: sync, speech, EOO callsign.
  add_executable(raderxtest EXCLUDE_FROM_ALL tests/raderxtest.cpp src/sdr/RadeRx.cpp src/sdr/RadeRx.h)
  target_include_directories(raderxtest PRIVATE src)
  target_compile_definitions(raderxtest PRIVATE RADE_WAV_DIR="${RADE_WAV_DIR}")
  target_link_libraries(raderxtest PRIVATE ttc::rade)
  # RADE transmit, offline loopback through RadeRx (EOO callsign, band,
  # level) — never near the radio.
  add_executable(radetxtest EXCLUDE_FROM_ALL tests/radetxtest.cpp
    src/sdr/RadeTx.cpp src/sdr/RadeTx.h src/sdr/RadeRx.cpp src/sdr/RadeRx.h)
  target_include_directories(radetxtest PRIVATE src)
  target_compile_definitions(radetxtest PRIVATE RADE_WAV_DIR="${RADE_WAV_DIR}")
  target_link_libraries(radetxtest PRIVATE ttc::rade)
endif()

if(HAVE_WDSP)
  add_executable(sdrrxtest EXCLUDE_FROM_ALL
    tests/sdrrxtest.cpp src/sdr/SdrRx.cpp src/sdr/SdrRx.h
    src/dsp/ClientEq.cpp src/dsp/ClientGate.cpp src/dsp/ClientComp.cpp
    src/dsp/ClientPhaseRotator.cpp src/dsp/ClientFinalLimiter.cpp)
  target_include_directories(sdrrxtest PRIVATE src)
  target_link_libraries(sdrrxtest PRIVATE Qt6::Core aether::wdsp rnnoise)
  if(HAVE_RADE)
    target_sources(sdrrxtest PRIVATE src/sdr/RadeRx.cpp)
    target_compile_definitions(sdrrxtest PRIVATE HAVE_RADE RADE_WAV_DIR="${RADE_WAV_DIR}")
    target_link_libraries(sdrrxtest PRIVATE ttc::rade)
  endif()
endif()

if(BUILD_SDRPLAY)
  target_sources(tentec-console PRIVATE src/sdr/SdrPlaySource.cpp src/sdr/SdrPlaySource.h)
  target_compile_definitions(tentec-console PRIVATE HAVE_SDRPLAY)
  target_link_libraries(tentec-console PRIVATE sdrplay_api)
  message(STATUS "SDRplay RSP source: ENABLED")

  # Headless streaming smoke test.
  add_executable(sdr_probe tools/sdr_probe.cpp src/sdr/SdrPlaySource.cpp)
  target_include_directories(sdr_probe PRIVATE src)
  target_link_libraries(sdr_probe PRIVATE sdrplay_api)

  if(WIN32)  # vendor install: header + import lib are not on default paths
    foreach(t tentec-console sdr_probe)
      target_include_directories(${t} PRIVATE "${SDRPLAY_ROOT}/inc")
      target_link_directories(${t} PRIVATE "${SDRPLAY_ROOT}/x64")
      if(MSVC)
        # Delay-load the vendor DLL: the release zip ships without it (never
        # redistributable) and the API installer's folder is not on the DLL
        # search path, so a load-time import dies before main() even on a
        # machine with the API correctly installed (live-found v0.1.6 at the
        # operator's own station: "instantly fails"). SdrPlaySource resolves
        # the DLL from the installer's registry breadcrumb before the first
        # API call.
        target_link_options(${t} PRIVATE "/DELAYLOAD:sdrplay_api.dll")
        target_link_libraries(${t} PRIVATE delayimp)
      endif()
    endforeach()
  endif()
else()
  message(STATUS "SDRplay RSP source: disabled (-DBUILD_SDRPLAY=ON to enable)")
endif()
