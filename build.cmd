@echo off
rem Builds 'Coffee Explorer' from a cmd prompt, the arguments go to 'build.ps1'.
rem Starting 'build.ps1' directly from cmd opens it through the file association, which drops the arguments.
where pwsh > nul 2>&1 && (set COFFEE_PS=pwsh) || (set COFFEE_PS=powershell)
%COFFEE_PS% -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*