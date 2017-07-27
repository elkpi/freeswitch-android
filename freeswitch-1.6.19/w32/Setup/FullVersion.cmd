REM This script reads the configure.ac 
REM and outputs a version parsing the AC_INIT line

FOR /f "delims=" %%i in ('FIND /N "AC_INIT" ..\..\configure.ac') DO SET version_contained_string=%%i
FOR /f "tokens=2 delims=," %%a in ("%version_contained_string%") DO SET dirty_version=%%a
SET almost_clean_version=%dirty_version:[=%
SET version_with_spaces=%almost_clean_version:]=%
SET full_version=%version_with_spaces: =%
echo %full_version%