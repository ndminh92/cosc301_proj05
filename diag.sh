#!/bin/bash

make clean
make

rm diag*.txt

./scandisk badimage1.img > diag1.txt
./scandisk badimage2.img > diag2.txt
./scandisk badimage3.img > diag3.txt
./scandisk badimage4.img > diag4.txt
./scandisk badimage5.img > diag5.txt
