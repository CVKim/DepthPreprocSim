@echo off
rem build.bat [Release|Debug]  - configure with CMake (VS-bundled cmake first) and build depth_sim.exe
setlocal EnableDelayedExpansion
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BUILD=%ROOT%\build"
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"

set "CMAKE_VS=C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "CMAKE_SYS=C:\Program Files\CMake\bin\cmake.exe"

if not exist "%BUILD%" mkdir "%BUILD%"
set "CMAKE="

rem 1) VS 18 Insiders bundled cmake, VS 18 generator with the v142 toolset (MSVC 14.29)
if exist "%CMAKE_VS%" (
  echo [build] configuring with VS-bundled cmake: "Visual Studio 18 2026" -A x64 -T v142
  "%CMAKE_VS%" -S "%ROOT%" -B "%BUILD%" -G "Visual Studio 18 2026" -A x64 -T v142
  if not errorlevel 1 set "CMAKE=%CMAKE_VS%"
)

rem 2) fallback: system cmake with VS 2019 (v142 is its default toolset)
if not defined CMAKE (
  echo [build] fallback: system cmake with "Visual Studio 16 2019" -A x64
  if exist "%BUILD%\CMakeCache.txt" del /q "%BUILD%\CMakeCache.txt"
  if exist "%BUILD%\CMakeFiles" rmdir /s /q "%BUILD%\CMakeFiles"
  "%CMAKE_SYS%" -S "%ROOT%" -B "%BUILD%" -G "Visual Studio 16 2019" -A x64 -T v142
  if errorlevel 1 (
    echo [build] configure failed. Alternative: open "x64 Native Tools Command Prompt for VS" with the v142 toolset
    echo         ^(vcvarsall.bat x64 -vcvars_ver=14.29^) and run: cmake -S . -B build_ninja -G Ninja -DCMAKE_BUILD_TYPE=Release ^&^& cmake --build build_ninja
    exit /b 1
  )
  set "CMAKE=%CMAKE_SYS%"
)

echo [build] building %CONFIG%
"!CMAKE!" --build "%BUILD%" --config %CONFIG% -- /m /nr:false
if errorlevel 1 (
  echo [build] build failed
  exit /b 1
)
echo [build] done: %BUILD%\%CONFIG%\depth_sim.exe
endlocal
exit /b 0
