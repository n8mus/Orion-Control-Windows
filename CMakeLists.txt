# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)
project(TenTecSDRConsole VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
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

# SDRplay RSP source is optional so the core app always builds. Turn on with
# -DBUILD_SDRPLAY=ON once the SDRplay API service is running (Phase 0).
option(BUILD_SDRPLAY "Build the SDRplay RSP IQ source (needs sdrplay_api)" OFF)

add_executable(tentec-console
  src/app/main.cpp
  src/app/MainWindow.cpp        src/app/MainWindow.h
  src/app/MainWindowTuning.cpp  src/app/MainWindowOps.cpp
  src/app/MainWindowTools.cpp
  src/app/MainWindowInternal.h
  src/app/Bands.h
  src/radio/RadioController.h
  src/radio/SerialPort.cpp      src/radio/SerialPort.h
  src/radio/TenTecOrion.cpp     src/radio/TenTecOrion.h
  src/radio/TenTecOmni7.cpp     src/radio/TenTecOmni7.h
  src/net/RigctldServer.cpp     src/net/RigctldServer.h
  src/net/SpotClient.cpp        src/net/SpotClient.h
  src/net/PotaClient.cpp        src/net/PotaClient.h
  src/net/SolarClient.cpp       src/net/SolarClient.h
  src/net/FldigiClient.cpp      src/net/FldigiClient.h
  src/net/RotorClient.cpp       src/net/RotorClient.h
  src/ui/DigiWindow.cpp         src/ui/DigiWindow.h
  src/util/CtyLookup.cpp        src/util/CtyLookup.h
  src/util/LogbookIndex.cpp     src/util/LogbookIndex.h
  src/util/WatchList.cpp        src/util/WatchList.h
  src/cw/WinKeyer.cpp           src/cw/WinKeyer.h
  src/cw/CwWindow.cpp           src/cw/CwWindow.h
  src/cw/CwDecoder.cpp          src/cw/CwDecoder.h
  src/cw/SkimmerEngine.cpp      src/cw/SkimmerEngine.h
  src/cw/SkimServer.cpp         src/cw/SkimServer.h
  src/ui/SkimmerWindow.cpp      src/ui/SkimmerWindow.h
  src/ui/PanadapterWidget.cpp   src/ui/PanadapterWidget.h
  src/ui/DisplayPanel.cpp       src/ui/DisplayPanel.h
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
  resources/resources.qrc
)
target_include_directories(tentec-console PRIVATE src)
target_link_libraries(tentec-console PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::SerialPort)

# Offline harnesses (not built by default): synthetic-IQ skimmer test and
# the fldigi XML-RPC client test (pair with tests/fake_fldigi.py).
add_executable(skimtest EXCLUDE_FROM_ALL
  tests/skimtest.cpp
  src/cw/CwDecoder.cpp src/cw/SkimmerEngine.cpp)
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

add_executable(skimsrvtest EXCLUDE_FROM_ALL
  tests/skimsrvtest.cpp
  src/cw/SkimServer.cpp)
target_include_directories(skimsrvtest PRIVATE src)
target_link_libraries(skimsrvtest PRIVATE Qt6::Core Qt6::Network)

add_executable(cwtest EXCLUDE_FROM_ALL
  tests/cwtest.cpp
  src/cw/CwDecoder.cpp)
target_include_directories(cwtest PRIVATE src)
target_link_libraries(cwtest PRIVATE Qt6::Core)

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
