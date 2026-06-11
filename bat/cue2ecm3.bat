for /r %%i in (*.cue) do ecm3 -i "%%i" -o "%%~ni.ecm3" -c 9 -e -a flac -d lzma2
