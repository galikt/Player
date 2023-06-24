#!/bin/bash
gcc -o player player.c -lavutil -lavformat -lavcodec -lswscale -lz -lm -lSDL2
