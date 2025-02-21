#!/bin/bash

# MSYS path
export PATH="/mingw64/bin:$PATH"

# resource file
windres -o resources.o resources.rc

# compile src
g++ -o SnapKey SnapKey.cpp resources.o -mwindows -std=c++11 -static  -static -O3 -march=mavx2

# success yes/no
if [ $? -eq 0 ]; then
    echo "Compilation successful: SnapKey.exe created"
else
    echo "Compilation failed."
fi
