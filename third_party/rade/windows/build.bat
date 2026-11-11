@echo off
rem Build RADE for Windows with MSVC, after fetch-and-patch.sh.
rem Run from the "x64 Native Tools Command Prompt for VS 2022":
rem
rem     third_party\rade\windows\build.bat <work-dir>
rem
rem <work-dir> is the folder given to fetch-and-patch.sh. The result is
rem <work-dir>\install, which the console takes as -DRADE_WIN_ROOT=...
setlocal
if "%~1"=="" (echo usage: build.bat ^<work-dir^> & exit /b 1)
set W=%~f1
set HERE=%~dp0
rem RADE_EXPORT empty: rade_c's nnet.h patch otherwise defines it as a GCC
rem attribute, which cl.exe cannot parse.
set CFLAGS=/DRADE_EXPORT=
cmake -S "%W%\src\opus" -B "%W%\build\opus-msvc" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DOPUS_DRED=ON -DOPUS_OSCE=ON -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_PROGRAMS=OFF ^
  -DOPUS_BUILD_TESTING=OFF -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF ^
  -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF || exit /b 1
cmake --build "%W%\build\opus-msvc" -j 8 || exit /b 1
set CFLAGS=
cmake -S "%HERE%." -B "%W%\build\rade-msvc" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  "-DRADE_C_DIR=%W%\src\rade_c" "-DTTC_RADE_DIR=%HERE%.." ^
  "-DOPUS_SRC=%W%\src\opus" "-DOPUS_BIN=%W%\build\opus-msvc" ^
  "-DCMAKE_INSTALL_PREFIX=%W%\install" || exit /b 1
cmake --build "%W%\build\rade-msvc" -j 8 || exit /b 1
cmake --install "%W%\build\rade-msvc" || exit /b 1
echo.
echo RADE for Windows is in %W%\install  (pass -DRADE_WIN_ROOT=%W%\install)
