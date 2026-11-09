@echo off
setlocal
rem Build a9tool.exe as native Windows x64 using Visual Studio and CMake.
rem Default is VS 2022. Override A9_VS_GENERATOR for another VS release.
if not defined A9_VS_GENERATOR set "A9_VS_GENERATOR=Visual Studio 17 2022"
set "ROOT=%~dp0"
set "BUILD=%ROOT%build-win64"
where cmake >nul 2>nul
if errorlevel 1 (
  echo ERROR: CMake was not found. Install VS C++ Desktop Development with CMake.
  exit /b 1
)
cmake -S "%ROOT%" -B "%BUILD%" -G "%A9_VS_GENERATOR%" -A x64
if errorlevel 1 exit /b 1
cmake --build "%BUILD%" --config Release --parallel
if errorlevel 1 exit /b 1
ctest --test-dir "%BUILD%" -C Release --output-on-failure
if errorlevel 1 exit /b 1
echo.
echo SUCCESS: %BUILD%\Release\a9tool.exe
exit /b 0
