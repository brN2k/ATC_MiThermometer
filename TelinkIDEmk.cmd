@pushd "%~dp0" || exit /b 1
@set "TLSDK=C:\TelinkSDK"
@set "TC32_PATH=/c/TelinkSDK/opt/tc32/bin/"
@set "PATH=%TLSDK%\jre\bin;%TLSDK%\bin;%TLSDK%\opt\tc32\bin;%TLSDK%\usr\bin;%TLSDK%\opt\tc32\tools;%PATH%"
@make -s -j PROJECT_NAME=ATC_v55 POJECT_DEF="-DDEVICE_TYPE=DEVICE_LYWSD03MMC"
@set "BUILD_EXIT=%ERRORLEVEL%"
@popd
@exit /b %BUILD_EXIT%
