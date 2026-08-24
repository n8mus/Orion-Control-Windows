# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)
project(TenTecSDRConsole VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
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

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network SerialPort)

# OpenMP parallelizes the skimmer's channel bank (SkimmerEngine.cpp);
# optional — without it the pragma is ignored and the bank runs serial.
find_package(OpenMP QUIET)

# SDRplay RSP source is optional so the core app always builds. Turn on with
# -DBUILD_SDRPLAY=ON once the SDRplay API service is running (Phase 0).
option(BUILD_SDRPLAY "Build the SDRplay RSP IQ source (needs sdrplay_api)" OFF)

add_executable(tentec-console
  src/app/main.cpp
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
  src/net/SolarClient.cpp       src/net/SolarClient.h
  src/net/FldigiClient.cpp      src/net/FldigiClient.h
  src/net/RotorClient.cpp       src/net/RotorClient.h
  src/net/RotorServer.cpp       src/net/RotorServer.h
  src/net/RotorLink.cpp         src/net/RotorLink.h
  src/ui/DigiWindow.cpp         src/ui/DigiWindow.h
  src/util/CtyLookup.cpp        src/util/CtyLookup.h
  src/util/LogbookIndex.cpp     src/util/LogbookIndex.h
  src/util/WatchList.cpp        src/util/WatchList.h
  src/cw/WinKeyer.cpp           src/cw/WinKeyer.h
                                src/cw/CwKeyer.h
  src/cw/OrionKeyer.cpp         src/cw/OrionKeyer.h
  src/cw/CwWindow.cpp           src/cw/CwWindow.h
  src/cw/CwDecoder.cpp          src/cw/CwDecoder.h
  src/cw/FldigiCwEngine.cpp     src/cw/FldigiCwEngine.h
  src/cw/BayesCwEngine.cpp      src/cw/BayesCwEngine.h
  src/cw/AudioCwSource.cpp      src/cw/AudioCwSource.h
  src/cw/SkimmerEngine.cpp      src/cw/SkimmerEngine.h
  src/cw/SkimServer.cpp         src/cw/SkimServer.h
  src/cw/SkimStft.cpp           src/cw/SkimStft.h
  src/ui/SkimmerWindow.cpp      src/ui/SkimmerWindow.h
  src/ui/SkimViewWindow.cpp     src/ui/SkimViewWindow.h
  src/ui/PanadapterWidget.cpp   src/ui/PanadapterWidget.h
  src/ui/SmithChartWidget.cpp   src/ui/SmithChartWidget.h
  src/ui/SwrHistoryDialog.cpp   src/ui/SwrHistoryDialog.h
  src/ui/DisplayPanel.cpp       src/ui/DisplayPanel.h
  src/ui/SetupDialog.cpp        src/ui/SetupDialog.h
  src/ui/RoutingPanel.cpp       src/ui/RoutingPanel.h
  src/ui/AudioPanel.cpp         src/ui/AudioPanel.h
  src/ui/SMeterWidget.cpp       src/ui/SMeterWidget.h
  src/ui/ControlPanel.cpp       src/ui/ControlPanel.h
  src/ui/FrequencyDisplay.cpp   src/ui/FrequencyDisplay.h
  src/ui/TxBar.cpp              src/ui/TxBar.h
  src/audio/ClipDeck.cpp        src/audio/ClipDeck.h
  src/dsp/Fft.cpp               src/dsp/Fft.h
  src/dsp/SpectrumComputer.cpp  src/dsp/SpectrumComputer.h
  src/sdr/SdrSource.h
  src/sdr/IqRecorder.cpp        src/sdr/IqRecorder.h
  src/sdr/AutoGain.h
  resources/resources.qrc
)
target_include_directories(tentec-console PRIVATE src)
target_link_libraries(tentec-console PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::SerialPort rnnoise)
if(OpenMP_CXX_FOUND)
  target_link_libraries(tentec-console PRIVATE OpenMP::OpenMP_CXX)
endif()

# Offline harnesses (not built by default): synthetic-IQ skimmer test and
# the fldigi XML-RPC client test (pair with tests/fake_fldigi.py).
add_executable(skimtest EXCLUDE_FROM_ALL
  tests/skimtest.cpp
  src/cw/CwDecoder.cpp src/cw/FldigiCwEngine.cpp src/cw/BayesCwEngine.cpp src/cw/SkimmerEngine.cpp)
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
target_link_libraries(dcutest PRIVATE Qt6::Core Qt6::Network)

# LP-100A wattmeter frame decode. Pure parser test — needs no hardware.
add_executable(lptest EXCLUDE_FROM_ALL
  tests/lptest.cpp
  src/radio/LpMeter.cpp src/radio/SerialPort.cpp src/radio/TxMeter.h)
target_include_directories(lptest PRIVATE src)
target_link_libraries(lptest PRIVATE Qt6::Core)

# PowerMaster wattmeter frame decode. Pure parser test — needs no hardware.
add_executable(pmtest EXCLUDE_FROM_ALL
  tests/pmtest.cpp
  src/radio/PmMeter.cpp src/radio/SerialPort.cpp src/radio/TxMeter.h)
target_include_directories(pmtest PRIVATE src)
target_link_libraries(pmtest PRIVATE Qt6::Core)

# Smith-chart render harness: draws a band's stored sweep runs to a PNG
# (offscreen), for eyeballing the chart against real captured data.
add_executable(smithtest EXCLUDE_FROM_ALL
  tests/smithtest.cpp
  src/ui/SmithChartWidget.cpp)
target_include_directories(smithtest PRIVATE src)
target_link_libraries(smithtest PRIVATE Qt6::Widgets)

add_executable(skimsrvtest EXCLUDE_FROM_ALL
  tests/skimsrvtest.cpp
  src/cw/SkimServer.cpp)
target_include_directories(skimsrvtest PRIVATE src)
target_link_libraries(skimsrvtest PRIVATE Qt6::Core Qt6::Network)

add_executable(skimreplay EXCLUDE_FROM_ALL
  tests/skimreplay.cpp
  src/cw/CwDecoder.cpp src/cw/FldigiCwEngine.cpp src/cw/BayesCwEngine.cpp src/cw/SkimmerEngine.cpp
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
  src/cw/CwDecoder.cpp src/cw/FldigiCwEngine.cpp src/cw/BayesCwEngine.cpp)
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
  src/cw/CwDecoder.cpp src/cw/FldigiCwEngine.cpp src/cw/BayesCwEngine.cpp src/dsp/Fft.cpp)
target_include_directories(cohtest PRIVATE src)
target_link_libraries(cohtest PRIVATE Qt6::Core)

add_executable(nrtest EXCLUDE_FROM_ALL
  tests/nrtest.cpp
  src/cw/CwDecoder.cpp src/cw/FldigiCwEngine.cpp src/cw/BayesCwEngine.cpp
  src/audio/SpectralNr.cpp src/dsp/Fft.cpp)
target_include_directories(nrtest PRIVATE src)
target_link_libraries(nrtest PRIVATE Qt6::Core rnnoise)

add_executable(watchtest EXCLUDE_FROM_ALL
  tests/watchtest.cpp
  src/util/WatchList.cpp)
target_include_directories(watchtest PRIVATE src)
target_link_libraries(watchtest PRIVATE Qt6::Core)

if(BUILD_SDRPLAY)
  target_sources(tentec-console PRIVATE src/sdr/SdrPlaySource.cpp src/sdr/SdrPlaySource.h)
  target_compile_definitions(tentec-console PRIVATE HAVE_SDRPLAY)
  target_link_libraries(tentec-console PRIVATE sdrplay_api)
  message(STATUS "SDRplay RSP source: ENABLED")

  # Headless streaming smoke test.
  add_executable(sdr_probe tools/sdr_probe.cpp src/sdr/SdrPlaySource.cpp)
  target_include_directories(sdr_probe PRIVATE src)
  target_link_libraries(sdr_probe PRIVATE sdrplay_api)
else()
  message(STATUS "SDRplay RSP source: disabled (-DBUILD_SDRPLAY=ON to enable)")
endif()
