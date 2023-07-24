:loopez
echo by David Reguera Garcia aka Dreg dreg@fr33project.org
echo https://github.com/therealdreg/ https://www.fr33project.org @therealdreg
cd %~dp0
del /F /Q *.lock
del /F /Q bx_enh_dbg.ini
mkdir sharedfolder
bochs.exe
del /F /Q *.lock
del /F /Q bx_enh_dbg.ini
echo press enter to re execute
goto loopez
