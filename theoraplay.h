/**
 * TheoraPlay; multithreaded Ogg Theora/Ogg Vorbis decoding.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

#ifndef _INCL_THEORAPLAY_H_
#define _INCL_THEORAPLAY_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef struct THEORAPLAY_Decoder THEORAPLAY_Decoder;

typedef struct THEORAPLAY_YuvVideoItem
{
    unsigned int playms;
    unsigned int width;
    unsigned int height;
    unsigned char *yuv;
    struct THEORAPLAY_YuvVideoItem *next;
} THEORAPLAY_YuvVideoItem;

typedef struct THEORAPLAY_PcmAudioItem
{
    unsigned int playms;  // playback start time in milliseconds.
    int channels;
    int frames;
    float *samples;  // frames * channels float32 samples.
    struct THEORAPLAY_PcmAudioItem *next;
} THEORAPLAY_PcmAudioItem;

THEORAPLAY_Decoder *THEORAPLAY_startDecode(const char *fname,
                                           const unsigned int maxframes);
void THEORAPLAY_stopDecode(THEORAPLAY_Decoder *decoder);
int THEORAPLAY_isDecoding(THEORAPLAY_Decoder *decoder);

const THEORAPLAY_PcmAudioItem *THEORAPLAY_getAudio(THEORAPLAY_Decoder *decoder);
void THEORAPLAY_freeAudio(const THEORAPLAY_PcmAudioItem *item);

const THEORAPLAY_YuvVideoItem *THEORAPLAY_getVideo(THEORAPLAY_Decoder *decoder);
void THEORAPLAY_freeVideo(const THEORAPLAY_YuvVideoItem *item);

#ifdef __cplusplus
}
#endif

#endif  /* include-once blocker. */

/* end of theoraplay.h ... */

