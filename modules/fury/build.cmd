@echo off
rem Builds furyled.exe with the C# compiler that ships with Windows (.NET Framework 4.x).
%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe /nologo /optimize /target:exe /out:"%~dp0furyled.exe" "%~dp0furyled.cs"
