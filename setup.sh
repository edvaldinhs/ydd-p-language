#!/bin/bash

set -e

echo "      --- starting setup ---"

mkdir build
cd build
cmake ..
make
mv yde ../yde
echo " "
echo "      --- yde file created ---"
echo " "
echo "run compiler with ./yde [path_to_file]"
echo "example: ./yde examples/function.yde"
