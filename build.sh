#!/bin/bash

GCCFLAGS="-Wall -Wextra -pedantic -std=c99 -g"
FFMPEG="-I$HOME/opt/include -L$HOME/opt/lib -lavformat -lavcodec -lswresample -lavutil"

gcc $GCCFLAGS -o speechful main.c $FFMPEG
