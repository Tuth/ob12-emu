@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d F:\Claude\OB-12
if not exist build mkdir build
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\clang-cl.exe" /O2 /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS /w /Isrc\shim /Isrc\f2mc16 /FIemu.h src\ob12_master.cpp src\f2mc16\f2mc16.cpp /Fobuild\ /Febuild\ob12_master.exe
echo BUILD_EXIT=%ERRORLEVEL%
