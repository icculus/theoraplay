/**
 * TheoraPlay; multithreaded Ogg Theora/Ogg Vorbis decoding.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

#include <stdio.h>
#include <unistd.h>
#include "theoraplay.h"

int main(int argc, char **argv)
{
    THEORAPLAY_Decoder *decoder = NULL;
    const THEORAPLAY_YuvVideoItem *video = NULL;
    const THEORAPLAY_PcmAudioItem *audio = NULL;

    int i;
    for (i = 1; i < argc; i++)
    {
        printf("Trying file '%s' ...\n", argv[i]);
        decoder = THEORAPLAY_startDecode(argv[i], 20);
        while (THEORAPLAY_isDecoding(decoder))
        {
            video = THEORAPLAY_getVideo(decoder);
            if (video)
            {
                printf("Got video frame (%u ms)!\n", video->playms);
                THEORAPLAY_freeVideo(video);
            } // if

            audio = THEORAPLAY_getAudio(decoder);
            if (audio)
            {
                printf("Got %d frames of audio (%u ms)!\n", audio->frames, audio->playms);
                THEORAPLAY_freeAudio(audio);
            } // if

            if (!video && !audio)
                usleep(10000);
        } // while

        if (THEORAPLAY_decodingError(decoder))
            printf("There was an error decoding this file!\n");
        else
            printf("done with this file!\n");

        THEORAPLAY_stopDecode(decoder);
    } // for

    printf("done all files!\n");
    return 0;
} // main

// end of testtheoraplay.c ...

