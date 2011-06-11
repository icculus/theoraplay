/**
 * TheoraPlay; multithreaded Ogg Theora/Ogg Vorbis decoding.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "theoraplay.h"
#include "SDL.h"

static Uint32 baseticks = 0;

typedef struct AudioQueue
{
    const THEORAPLAY_PcmAudioItem *audio;
    int offset;
    struct AudioQueue *next;
} AudioQueue;

static volatile AudioQueue *audio_queue = NULL;
static volatile AudioQueue *audio_queue_tail = NULL;

static void SDLCALL audio_callback(void *userdata, Uint8 *stream, int len)
{
    // !!! FIXME: this should refuse to play if item->playms is in the future.
    //const Uint32 now = SDL_GetTicks() - baseticks;
    Sint16 *dst = (Sint16 *) stream;

    while (audio_queue && (len > 0))
    {
        volatile AudioQueue *item = audio_queue;
        AudioQueue *next = item->next;
        const int channels = item->audio->channels;

        const float *src = item->audio->samples + (item->offset * channels);
        int cpy = (item->audio->frames - item->offset) * channels;
        int i;

        if (cpy > (len / sizeof (Sint16)))
            cpy = len / sizeof (Sint16);

        for (i = 0; i < cpy; i++)
            *(dst++) = (Sint16) (*(src++) * 32767.0f);

        item->offset += (cpy / channels);
        len -= cpy * sizeof (Sint16);

        if (item->offset >= item->audio->frames)
        {
            THEORAPLAY_freeAudio(item->audio);
            free((void *) item);
            audio_queue = next;
        } // if
    } // while

    if (!audio_queue)
        audio_queue_tail = NULL;

    if (len > 0)
        memset(dst, '\0', len);
} // audio_callback


static void queue_audio(const THEORAPLAY_PcmAudioItem *audio)
{
    AudioQueue *item = (AudioQueue *) malloc(sizeof (AudioQueue));
    if (!item)
    {
        THEORAPLAY_freeAudio(audio);
        return;  // oh well.
    } // if

    item->audio = audio;
    item->offset = 0;
    item->next = NULL;

    SDL_LockAudio();
    if (audio_queue_tail)
        audio_queue_tail->next = item;
    else
        audio_queue = item;
    audio_queue_tail = item;
    SDL_UnlockAudio();
} // queue_audio


static void playfile(const char *fname)
{
    THEORAPLAY_Decoder *decoder = NULL;
    const THEORAPLAY_YuvVideoItem *video = NULL;
    const THEORAPLAY_PcmAudioItem *audio = NULL;
    const char *basefname = NULL;
    SDL_Surface *screen = NULL;
    SDL_Overlay *overlay = NULL;
    SDL_Event event;
    int initfailed = 0;
    int quit = 0;

    printf("Trying file '%s' ...\n", fname);
    decoder = THEORAPLAY_startDecode(fname, 20);
    if (!decoder)
    {
        fprintf(stderr, "Failed to start decoding '%s'!\n", fname);
        return;
    } // if

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) == -1)
    {
        fprintf(stderr, "SDL_Init() failed: %s\n", SDL_GetError());
        return;
    } // if

    basefname = strrchr(fname, '/');
    if (!basefname)
        basefname = fname;
    else
        basefname++;
    SDL_WM_SetCaption(basefname, basefname);

    // wait until we have a video and audio packet, so we can set up hardware.
    // !!! FIXME: we need an API to decide if this file has only audio/video.
    while (!video || !audio)
    {
        SDL_Delay(10);
        if (!video) video = THEORAPLAY_getVideo(decoder);
        if (!audio) audio = THEORAPLAY_getAudio(decoder);
    } // while

    // Set the video mode as soon as we know what it should be.
    if (video)
    {
        screen = SDL_SetVideoMode(video->width, video->height, 0, 0);
        if (!screen)
            fprintf(stderr, "SDL_SetVideoMode() failed: %s\n", SDL_GetError());
        else
        {
            // blank out the screen to start.
            SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));
            SDL_Flip(screen);

            overlay = SDL_CreateYUVOverlay(video->width, video->height,
                                       SDL_YV12_OVERLAY, screen);

            if (!overlay)
                fprintf(stderr, "YUV Overlay failed: %s\n", SDL_GetError());
        } // else

        initfailed = quit = (!screen || !overlay);
    } // if

    // Open the audio device as soon as we know what it should be.
    if (audio)
    {
        SDL_AudioSpec spec;
        memset(&spec, '\0', sizeof (SDL_AudioSpec));
        spec.freq = audio->freq;
        spec.format = AUDIO_S16SYS;
        spec.channels = audio->channels;
        spec.samples = 2048;
        spec.callback = audio_callback;
        initfailed = quit = (SDL_OpenAudio(&spec, NULL) != 0);
    } // if

    baseticks = SDL_GetTicks();

    if (!quit && audio)
        SDL_PauseAudio(0);

    while (!quit && THEORAPLAY_isDecoding(decoder))
    {
        const Uint32 now = SDL_GetTicks() - baseticks;

        if (!video)
            video = THEORAPLAY_getVideo(decoder);

        // Play video frames when it's time.
        if (video && (video->playms <= now))
        {
            //printf("Play video frame (%u ms)!\n", video->playms);
            if (SDL_LockYUVOverlay(overlay) == -1)
            {
                static int warned = 0;
                if (!warned)
                {
                    warned = 1;
                    fprintf(stderr, "Couldn't lock YUV overlay: %s\n", SDL_GetError());
                } // if
            } // if
            else
            {
                SDL_Rect dstrect = { 0, 0, video->width, video->height };
                const int w = video->width;
                const int h = video->height;
                const Uint8 *y = (const Uint8 *) video->yuv;
                const Uint8 *u = y + (w * h);
                const Uint8 *v = u + ((w/2) * (h/2));
                Uint8 *dst;
                int i;

                dst = overlay->pixels[0];
                for (i = 0; i < h; i++, y += w, dst += overlay->pitches[0])
                    memcpy(dst, y, w);

                dst = overlay->pixels[1];
                for (i = 0; i < h/2; i++, u += w/2, dst += overlay->pitches[1])
                    memcpy(dst, u, w/2);

                dst = overlay->pixels[2];
                for (i = 0; i < h/2; i++, v += w/2, dst += overlay->pitches[1])
                    memcpy(dst, v, w/2);

                SDL_UnlockYUVOverlay(overlay);

                if (SDL_DisplayYUVOverlay(overlay, &dstrect) != 0)
                {
                    static int warned = 0;
                    if (!warned)
                    {
                        warned = 1;
                        fprintf(stderr, "Couldn't display YUV overlay: %s\n", SDL_GetError());
                    } // if
                } // if
            } // else

            THEORAPLAY_freeVideo(video);
            video = NULL;
        } // if
        else  // no new video frame? Give up some CPU.
        {
            SDL_Delay(10);
        } // else

        if (!audio)
            audio = THEORAPLAY_getAudio(decoder);

        if (audio)
        {
            //printf("Got %d frames of audio (%u ms)!\n",
            //        audio->frames, audio->playms);
            queue_audio(audio);
            audio = NULL;
        } // if

        // Pump the event loop here.
        while (screen && SDL_PollEvent(&event))
        {
            switch (event.type)
            {
                case SDL_VIDEOEXPOSE:
                {
                    SDL_Rect dstrect = { 0, 0, screen->w, screen->h };
                    SDL_DisplayYUVOverlay(overlay, &dstrect);
                    break;
                } // case

                case SDL_QUIT:
                    quit = 1;
                    break;
                case SDL_KEYDOWN:
                    if (event.key.keysym.sym == SDLK_ESCAPE)
                        quit = 1;
                    break;
            } // switch
        } // while
    } // while

    while (!quit)
    {
        SDL_LockAudio();
        quit = (audio_queue == NULL);
        SDL_UnlockAudio();
        if (!quit)
            SDL_Delay(100);  // wait for final audio packets to play out.
    } // while

    if (initfailed)
        printf("Initialization failed!\n");
    else if (THEORAPLAY_decodingError(decoder))
        printf("There was an error decoding this file!\n");
    else
        printf("done with this file!\n");

    if (overlay) SDL_FreeYUVOverlay(overlay);
    if (video) THEORAPLAY_freeVideo(video);
    if (audio) THEORAPLAY_freeAudio(audio);
    if (decoder) THEORAPLAY_stopDecode(decoder);
    SDL_CloseAudio();
    SDL_Quit();
} // playfile

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++)
        playfile(argv[i]);

    printf("done all files!\n");

    return 0;
} // main

// end of testtheoraplay.c ...

