@echo off
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8b-live
findstr /n /c:"--frames" /c:"--novideo" /c:"--verify" /c:"--maxdelta" /c:"--region" /c:"--swapstorage" /c:"--scale" main.cpp
