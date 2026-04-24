#!/bin/bash

set -e

if [ ! -d "build" ]; then
echo "      --- starting setup ---"
    mkdir build
fi

cd build

if [ ! -f "Makefile" ]; then
    cmake ..
fi

echo "      --- compile ---"
make

if [ -f "yde" ]; then


    mv yde ../yde
fi

echo " "
echo "      --- yde file created ---"
echo " "
echo "run compiler with ./yde [path_to_file]"
echo "example: ./yde examples/function.yde"
