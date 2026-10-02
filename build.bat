@echo off
cd /d "%~dp0"
gcc -O2 -D_7ZIP_ST -o UWT.exe uwt.c lzma/LzmaDec.c lzma/LzmaEnc.c lzma/LzFind.c