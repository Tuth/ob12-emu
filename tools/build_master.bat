@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d F:\Claude\OB-12
if not exist build mkdir build
set CC="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\clang-cl.exe"
set FLAGS=/O2 /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS /w /Isrc\shim /Isrc\f2mc16 /Isrc /FIemu.h
%CC% %FLAGS% /c src\f2mc16\f2mc16.cpp /Fobuild\f2mc16.obj
%CC% %FLAGS% /c src\ob12_master.cpp /Fobuild\ob12_master.obj
%CC% %FLAGS% /c src\ob12_slave.cpp /Fobuild\ob12_slave.obj
%CC% build\ob12_master.obj build\f2mc16.obj /Febuild\ob12_master.exe
%CC% build\ob12_slave.obj build\f2mc16.obj /Febuild\ob12_slave.exe
echo BUILD_EXIT=%ERRORLEVEL%
