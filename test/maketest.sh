#!/bin/sh

set -x
set -e

OSTYPE=`uname -s`
if [ "$OSTYPE" = "Linux" ]; then
    PTHREADLINK="-lpthread"
fi

CFLAGS="-O0 -ggdb3 -Wall -I.."
gcc -o ./testtheoraplay $CFLAGS ../theoraplay.c ./testtheoraplay.c -logg -lvorbis -ltheoradec $PTHREADLINK
gcc -o ./sdltheoraplay $CFLAGS ../theoraplay.c ./sdltheoraplay.c `sdl-config --cflags --libs`  -logg -lvorbis -ltheoradec $PTHREADLINK

