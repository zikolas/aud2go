@echo off
rem Build AUD2GO on DOS with OpenWatcom (16-bit real mode, C89).
set WATCOM=C:\WATCOM
set INCLUDE=C:\WATCOM\H
set PATH=C:\WATCOM\BINW;C:\DOS
wcc -ms %1.c -fo=%1.obj
if errorlevel 1 goto end
wlink system dos name %1.exe file %1.obj
:end
