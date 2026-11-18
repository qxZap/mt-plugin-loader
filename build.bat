@echo off
rem Builds the loader (version.dll) with MSVC 2022, then runs the ini merge test.
setlocal
rem any Visual Studio edition with the C++ tools (Community here, Enterprise on GitHub's runners)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do set "VCVARS=%%i"
if not exist "%VCVARS%" (echo MSVC with C++ tools not found & exit /b 1)
call "%VCVARS%" >nul
cd /d "%~dp0"
if not exist out\obj mkdir out\obj
rc /nologo /fo out\obj\version.res src\version.rc || exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 /LD /Fo:out\obj\ src\loader.cpp /Fe:out\version.dll /link /NOLOGO user32.lib out\obj\version.res || exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 /Fo:out\obj\ /Fe:out\ini_test.exe tools\ini_test.cpp /link /NOLOGO user32.lib || exit /b 1
out\ini_test.exe || (echo ini merge test FAILED & exit /b 1)
echo built %~dp0out
