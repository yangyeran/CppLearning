@echo off
setlocal
REM Generate Visual Studio 2022 solution and build all chapters.
REM Output: build\CppLearning.sln  and  build\bin\Debug\*.exe

set "CMAKE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" set "CMAKE=cmake"

cd /d "%~dp0.."

echo [1/2] Configuring...
"%CMAKE%" -S . -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 goto fail

echo.
echo [2/2] Building Debug...
"%CMAKE%" --build build --config Debug
if errorlevel 1 goto fail

echo.
echo ==================================================
echo  BUILD OK
echo    Solution : build\CppLearning.sln
echo    Binaries : build\bin\
echo ==================================================
exit /b 0

:fail
echo.
echo BUILD FAILED - see errors above.
exit /b 1
