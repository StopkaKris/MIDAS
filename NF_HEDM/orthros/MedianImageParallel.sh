#!/bin/bash

source ${HOME}/.MIDAS/pathsNF
LD_LIBRARY_PATH=$LD_LIBRARY_PATH:${HOME}/.MIDAS/LIBTIFF/lib/
export $LD_LIBRARY_PATH

${BINFOLDER}/MedianImageLibTiff $1 $2 $3
