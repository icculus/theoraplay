// I wrote this with a lot of peeking at the Theora example code in
//  libtheora-1.1.1/examples/player_example.c, but this is all my own
//  code.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <assert.h>

#include "theora/theoradec.h"
#include "vorbis/codec.h"

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

typedef THEORAPLAY_YuvVideoItem YuvVideoItem;
typedef THEORAPLAY_PcmAudioItem PcmAudioItem;

typedef struct TheoraDecoder
{
    // Thread wrangling...
    int lock_created;
    pthread_mutex_t lock;  // !!! FIXME: atomics might be nicer here.
    volatile int halt;
    volatile unsigned int videocount;
    int done;
    pthread_t worker;

    // Ogg, Vorbis, and Theora decoder state...
    int fd;
    int decoder_init;
    ogg_sync_state sync;
    ogg_page page;
    int vpackets;
    vorbis_info vinfo;
    vorbis_comment vcomment;
    ogg_stream_state vstream;
    int vdsp_init;
    vorbis_dsp_state vdsp;
    int vblock_init;
    vorbis_block vblock;
    int tpackets;
    th_info tinfo;
    th_comment tcomment;
    ogg_stream_state tstream;
    th_setup_info *tsetup;
    th_dec_ctx *tdec;

    // API state...
    unsigned int maxframes;  // Max video frames to buffer.

    YuvVideoItem *videolist;
    YuvVideoItem *videolisttail;

    PcmAudioItem *audiolist;
    PcmAudioItem *audiolisttail;
} TheoraDecoder;

typedef struct THEORAPLAY_Decoder THEORAPLAY_Decoder;
THEORAPLAY_Decoder *THEORAPLAY_startDecode(const char *fname,
                                           const unsigned int maxframes);
void THEORAPLAY_stopDecode(THEORAPLAY_Decoder *decoder);
int THEORAPLAY_isDecoding(THEORAPLAY_Decoder *decoder);

const PcmAudioItem *THEORAPLAY_getAudio(THEORAPLAY_Decoder *decoder);
void THEORAPLAY_freeAudio(const PcmAudioItem *item);

const YuvVideoItem *THEORAPLAY_getVideo(THEORAPLAY_Decoder *decoder);
void THEORAPLAY_FreeVideo(const YuvVideoItem *item);

static void CleanupWorkerThread(TheoraDecoder *ctx);
static int FeedMoreOggData(TheoraDecoder *ctx);
static void QueueOggPage(TheoraDecoder *ctx, ogg_page *page);
static void WorkerThread(TheoraDecoder *ctx);
static void *WorkerThreadEntry(void *_this);


static void CleanupWorkerThread(TheoraDecoder *ctx)
{
    if (ctx->fd != -1)
        close(ctx->fd);

    if (ctx->tdec != NULL)
        th_decode_free(ctx->tdec);

    if (ctx->tsetup != NULL)
        th_setup_free(ctx->tsetup);

    if (ctx->vblock_init)
        vorbis_block_clear(&ctx->vblock);

    if (ctx->vdsp_init)
        vorbis_dsp_clear(&ctx->vdsp);

    if (ctx->tpackets)
        ogg_stream_clear(&ctx->tstream);

    if (ctx->vpackets)
        ogg_stream_clear(&ctx->vstream);

    if (ctx->decoder_init)
    {
        th_info_clear(&ctx->tinfo);
        th_comment_clear(&ctx->tcomment);
        vorbis_comment_clear(&ctx->vcomment);
        vorbis_info_clear(&ctx->vinfo);
        ogg_sync_clear(&ctx->sync);
    } // if
} // CleanupWorkerThread

void THEORAPLAY_stopDecode(THEORAPLAY_Decoder *decoder)
{
    TheoraDecoder *ctx = (TheoraDecoder *) decoder;

    if (ctx->lock_created)
    {
        ctx->halt = 1;
        pthread_join(ctx->worker, NULL);
        pthread_mutex_destroy(&ctx->lock);
    } // if

    YuvVideoItem *videolist = ctx->videolist;
    while (videolist)
    {
        YuvVideoItem *next = videolist->next;
        free(videolist->yuv);
        free(videolist);
        videolist = next;
    } // while

    PcmAudioItem *audiolist = ctx->audiolist;
    while (audiolist)
    {
        PcmAudioItem *next = audiolist->next;
        free(audiolist->samples);
        free(audiolist);
        audiolist = next;
    } // while

    free(ctx);
} // THEORAPLAY_stopDecode


THEORAPLAY_Decoder *THEORAPLAY_startDecode(const char *fname,
                                           const unsigned int maxframes)
{
    TheoraDecoder *ctx = malloc(sizeof (TheoraDecoder));
    if (ctx == NULL)
        return NULL;

    memset(ctx, '\0', sizeof (TheoraDecoder));
    ctx->maxframes = maxframes;

    ctx->fd = open(fname, O_RDONLY);
    if (ctx->fd != -1)
    {
        struct stat statbuf;
        if (fstat(ctx->fd, &statbuf) != -1)
        {
            ctx->lock_created = (pthread_mutex_init(&ctx->lock, NULL) == 0);
            if (ctx->lock_created)
            {
                if (pthread_create(&ctx->worker, NULL, WorkerThreadEntry, ctx) == 0)
                    return (THEORAPLAY_Decoder *) ctx;
            } // if

            pthread_mutex_destroy(&ctx->lock);
            ctx->lock_created = 0;
        } // if

        close(ctx->fd);
    } // if

    free(ctx);
    return NULL;
} // THEORAPLAY_startDecode

int THEORAPLAY_isDecoding(THEORAPLAY_Decoder *decoder)
{
    const TheoraDecoder *ctx = (TheoraDecoder *) decoder;
    return (ctx && ctx->lock_created && !ctx->done);
} // THEORAPLAY_isDecoding

const PcmAudioItem *THEORAPLAY_getAudio(THEORAPLAY_Decoder *decoder)
{
    TheoraDecoder *ctx = (TheoraDecoder *) decoder;
    PcmAudioItem *retval;

    pthread_mutex_lock(&ctx->lock);
    retval = ctx->audiolist;
    if (retval)
    {
        ctx->audiolist = retval->next;
        retval->next = NULL;
        if (ctx->audiolist == NULL)
            ctx->audiolisttail = NULL;
    } // if
    pthread_mutex_unlock(&ctx->lock);

    return retval;
} // THEORAPLAY_getAudio

const YuvVideoItem *THEORAPLAY_getVideo(THEORAPLAY_Decoder *decoder)
{
    TheoraDecoder *ctx = (TheoraDecoder *) decoder;
    YuvVideoItem *retval;

    pthread_mutex_lock(&ctx->lock);
    retval = ctx->videolist;
    if (retval)
    {
        ctx->videolist = retval->next;
        retval->next = NULL;
        if (ctx->videolist == NULL)
            ctx->videolisttail = NULL;
        assert(ctx->videocount > 0);
        ctx->videocount--;
    } // if
    pthread_mutex_unlock(&ctx->lock);

    return retval;
} // THEORAPLAY_getVideo

void THEORAPLAY_freeAudio(const PcmAudioItem *_item)
{
    PcmAudioItem *item = (PcmAudioItem *) _item;
    assert(item->next == NULL);
    free(item->samples);
    free(item);
} // THEORAPLAY_freeAudio

void THEORAPLAY_freeVideo(const YuvVideoItem *_item)
{
    YuvVideoItem *item = (YuvVideoItem *) _item;
    assert(item->next == NULL);
    free(item->yuv);
    free(item);
} // THEORAPLAY_freeVideo

static int FeedMoreOggData(TheoraDecoder *ctx)
{
    long buflen = 4096;  // !!! FIXME: tweak this?
    char *buffer = ogg_sync_buffer(&ctx->sync, buflen);
    if (buffer == NULL)
        return 0;

    while ( ((buflen = read(ctx->fd, buffer, buflen)) < 0) && (errno == EINTR) ) {}

    if (buflen <= 0)
        return 0;

    return (ogg_sync_wrote(&ctx->sync, buflen) == 0);
} // FeedMoreOggData

static void QueueOggPage(TheoraDecoder *ctx, ogg_page *page)
{
    // make sure we initialized the stream before using pagein, but the stream
    //  will know to ignore pages that aren't meant for it, so pass to both.
    if (ctx->tpackets) ogg_stream_pagein(&ctx->tstream, page);
    if (ctx->vpackets) ogg_stream_pagein(&ctx->vstream, page);
} // QueueOggPage


static void WorkerThread(TheoraDecoder *ctx)
{
    ogg_packet packet;
    unsigned long audioframes = 0;
    unsigned long videoframes = 0;
    double fps = 0.0;

    ogg_sync_init(&ctx->sync);
    vorbis_info_init(&ctx->vinfo);
    vorbis_comment_init(&ctx->vcomment);
    th_comment_init(&ctx->tcomment);
    th_info_init(&ctx->tinfo);
    ctx->decoder_init = 1;  // !! FIXME: ditch this.

    int bos = 1;
    while (!ctx->halt && bos)
    {
        if (!FeedMoreOggData(ctx))
            break;

        // parse out the initial header.
        while ( (!ctx->halt) && (ogg_sync_pageout(&ctx->sync, &ctx->page) > 0) )
        {
            ogg_stream_state test;

            if (!ogg_page_bos(&ctx->page))  // not a header.
            {
                QueueOggPage(ctx, &ctx->page);
                bos = 0;
                break;
            } // if

            ogg_stream_init(&test, ogg_page_serialno(&ctx->page));
            ogg_stream_pagein(&test, &ctx->page);
            ogg_stream_packetout(&test, &packet);

            if (!ctx->tpackets && (th_decode_headerin(&ctx->tinfo, &ctx->tcomment, &ctx->tsetup, &packet) >= 0))
            {
                memcpy(&ctx->tstream, &test, sizeof (test));
                ctx->tpackets = 1;
            } // if
            else if (!ctx->vpackets && (vorbis_synthesis_headerin(&ctx->vinfo, &ctx->vcomment, &packet) >= 0))
            {
                memcpy(&ctx->vstream, &test, sizeof (test));
                ctx->vpackets = 1;
            } // else if
            else
            {
                // whatever it is, we don't care about it
                ogg_stream_clear(&test);
            } // else
        } // while
    } // while

    // no audio OR video?
    if (ctx->halt || (!ctx->vpackets && !ctx->tpackets))
        return;

    // apparently there are two more theora and two more vorbis headers next.
    while ((!ctx->halt) && ((ctx->tpackets && (ctx->tpackets < 3)) || (ctx->vpackets && (ctx->vpackets < 3))))
    {
        while (!ctx->halt && ctx->tpackets && (ctx->tpackets < 3))
        {
            if (ogg_stream_packetout(&ctx->tstream, &packet) != 1)
                break; // get more data?
            if (!th_decode_headerin(&ctx->tinfo, &ctx->tcomment, &ctx->tsetup, &packet))
                return;
            ctx->tpackets++;
        } // while

        while (!ctx->halt && ctx->vpackets && (ctx->vpackets < 3))
        {
            if (ogg_stream_packetout(&ctx->vstream, &packet) != 1)
                break;  // get more data?
            if (vorbis_synthesis_headerin(&ctx->vinfo, &ctx->vcomment, &packet))
                return;
            ctx->vpackets++;
        } // while

        // get another page, try again?
        if (ogg_sync_pageout(&ctx->sync, &ctx->page) > 0)
            QueueOggPage(ctx, &ctx->page);
        else if (!FeedMoreOggData(ctx))
            return;
    } // while

    // okay, now we have our streams, ready to set up decoding.
    if (!ctx->halt && ctx->tpackets)
    {
        // th_decode_alloc() docs say to check for insanely large frames yourself.
        if ((ctx->tinfo.frame_width > 99999) || (ctx->tinfo.frame_height > 99999))
            return;

        //if (ctx->tinfo.colorspace != TH_CS_ITU_REC_470M) { assert(0); return; } // !!! FIXME
        if (ctx->tinfo.pixel_fmt != TH_PF_420) { assert(0); return; } // !!! FIXME

        if (ctx->tinfo.fps_denominator != 0)
            fps = ((double) ctx->tinfo.fps_numerator) / ((double) ctx->tinfo.fps_denominator);

        ctx->tdec = th_decode_alloc(&ctx->tinfo, ctx->tsetup);
        if (!ctx->tdec)
            return;

        // Set decoder to maximum post-processing level.
        //  Theoretically we could try dropping this level if we're not keeping up.
        int pp_level_max = 0;
        th_decode_ctl(ctx->tdec, TH_DECCTL_GET_PPLEVEL_MAX, &pp_level_max, sizeof(pp_level_max));
        th_decode_ctl(ctx->tdec, TH_DECCTL_SET_PPLEVEL, &pp_level_max, sizeof(pp_level_max));
    } // if

    // Done with this now.
    if (ctx->tsetup != NULL)
    {
        th_setup_free(ctx->tsetup);
        ctx->tsetup = NULL;
    } // if

    if (!ctx->halt && ctx->vpackets)
    {
        ctx->vdsp_init = (vorbis_synthesis_init(&ctx->vdsp, &ctx->vinfo) == 0);
        if (!ctx->vdsp_init)
            return;
        ctx->vblock_init = (vorbis_block_init(&ctx->vdsp, &ctx->vblock) == 0);
        if (!ctx->vblock_init)
            return;
    } // if

    // Now we can start the actual decoding!
    // Note that audio and video don't _HAVE_ to start simultaneously.

    while (!ctx->halt)
    {
        int need_pages = 0;  // need more Ogg pages?
        int saw_video_frame = 0;

        // Try to read as much audio as we can at once. We limit the outer
        //  loop to one video frame and as much audio as we can eat.
        while (!ctx->halt && ctx->vpackets && !need_pages)
        {
            float **pcm = NULL;
            const int frames = vorbis_synthesis_pcmout(&ctx->vdsp, &pcm);
            if (frames > 0)
            {
                const int channels = ctx->vinfo.channels;
                int chanidx, frameidx;
                float *samples;
                PcmAudioItem *item = (PcmAudioItem *) malloc(sizeof (PcmAudioItem));
                if (item == NULL) return;
                item->playms = (unsigned long) ((((double) audioframes) / ((double) ctx->vinfo.rate)) * 1000.0);
                item->channels = channels;
                item->frames = frames;
                item->samples = (float *) malloc(sizeof (float) * frames * channels);
                item->next = NULL;

                if (item->samples == NULL)
                {
                    free(item);
                    return;
                } // if

                // I bet this beats the crap out of the CPU cache...
                samples = item->samples;
                for (frameidx = 0; frameidx < channels; frameidx++)
                {
                    for (chanidx = 0; chanidx < channels; chanidx++)
                        *(samples++) = pcm[chanidx][frameidx];
                } // for

                vorbis_synthesis_read(&ctx->vdsp, frames);  // we ate everything.
                audioframes += frames;

printf("Decoded %d frames of audio.\n", (int) frames);
                pthread_mutex_lock(&ctx->lock);
                if (ctx->audiolisttail)
                {
                    assert(ctx->audiolist);
                    ctx->audiolisttail->next = item;
                } // if
                else
                {
                    assert(!ctx->audiolist);
                    ctx->audiolist = item;
                } // else
                ctx->audiolisttail = item;
                pthread_mutex_unlock(&ctx->lock);
            } // if

            else  // no audio available left in current packet?
            {
                // try to feed another packet to the Vorbis stream...
                if (ogg_stream_packetout(&ctx->vstream, &packet) <= 0)
                    need_pages = 1;  // stream needs another page.
                else
                {
                    if (vorbis_synthesis(&ctx->vblock, &packet) == 0)
                        vorbis_synthesis_blockin(&ctx->vdsp, &ctx->vblock);
                } // else
            } // else
        } // while

        if (!ctx->halt && ctx->tpackets)
        {
            // Theora, according to example_player.c, is
            //  "one [packet] in, one [frame] out."
            if (ogg_stream_packetout(&ctx->tstream, &packet) <= 0)
                need_pages = 1;
            else
            {
                ogg_int64_t granulepos = 0;
                const int rc = th_decode_packetin(ctx->tdec, &packet, &granulepos);
                if (rc == TH_DUPFRAME)
                    videoframes++;  // nothing else to do.
                else if (rc == 0)  // new frame!
                {
                    th_ycbcr_buffer ycbcr;
                    if (th_decode_ycbcr_out(ctx->tdec, ycbcr) == 0)
                    {
                        int i;
                        const int w = ctx->tinfo.pic_width;
                        const int h = ctx->tinfo.pic_height;
                        const int yoff = (ctx->tinfo.pic_x & ~1) + ycbcr[0].stride * (ctx->tinfo.pic_y & ~1);
                        const int uvoff= (ctx->tinfo.pic_x / 2) + (ycbcr[1].stride) * (ctx->tinfo.pic_y / 2);
                        unsigned char *yuv;
                        YuvVideoItem *item = (YuvVideoItem *) malloc(sizeof (YuvVideoItem));
                        if (item == NULL) return;
                        item->playms = (fps == 0) ? 0.0 : (unsigned long) ((((double) videoframes) / fps) * 1000.0);
                        item->width = w;
                        item->height = h;
                        item->yuv = (unsigned char *) malloc(w * h * 2);
                        item->next = NULL;

                        if (item->yuv == NULL)
                        {
                            free(item);
                            return;
                        } // if

                        yuv = item->yuv;
                        for (i = 0; i < h; i++, yuv += w)
                            memcpy(yuv, ycbcr[0].data + yoff + ycbcr[0].stride * i, w);

                        for (i = 0; i < (h / 2); i++)
                        {
                            memcpy(yuv, ycbcr[2].data + uvoff + ycbcr[2].stride * i, w / 2);
                            yuv += w;
                            memcpy(yuv, ycbcr[1].data + uvoff + ycbcr[1].stride * i, w / 2);
                            yuv += w;
                        } // for

                        printf("Decoded another video frame.\n");
                        pthread_mutex_lock(&ctx->lock);
                        if (ctx->videolisttail)
                        {
                            assert(ctx->videolist);
                            ctx->videolisttail->next = item;
                        } // if
                        else
                        {
                            assert(!ctx->videolist);
                            ctx->videolist = item;
                        } // else
                        ctx->videolisttail = item;
                        ctx->videocount++;
                        pthread_mutex_unlock(&ctx->lock);

                        saw_video_frame = 1;
                    } // if
                    videoframes++;
                } // if
            } // else
        } // if

        if (!ctx->halt && need_pages)
        {
            if (!FeedMoreOggData(ctx))
                return;

            while (!ctx->halt && (ogg_sync_pageout(&ctx->sync, &ctx->page) > 0))
                QueueOggPage(ctx, &ctx->page);
        } // if

        // Sleep the process until we have space for more frames.
        if (saw_video_frame)
        {
            int go_on = !ctx->halt;
            printf("Sleeping.\n");
            while (go_on)
            {
                // !!! FIXME: This is stupid. I should use a semaphore for this.
                pthread_mutex_lock(&ctx->lock);
                go_on = !ctx->halt && (ctx->videocount >= ctx->maxframes);
                pthread_mutex_unlock(&ctx->lock);
                if (go_on)
                    usleep(10000);
            } // while
            printf("Awake!\n");
        } // if
    } // while
} // WorkerThread


static void *WorkerThreadEntry(void *_this)
{
    TheoraDecoder *ctx = (TheoraDecoder *) _this;
    WorkerThread(ctx);
    CleanupWorkerThread(ctx);
    ctx->done = 1;
    printf("Worker thread is done.\n");
    return NULL;
} // WorkerThreadEntry



#if 1
int main(int argc, char **argv)
{
    THEORAPLAY_Decoder *decoder;
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

        printf("done with this file!\n");
        THEORAPLAY_stopDecode(decoder);
    } // for

    printf("done all files!\n");
    return 0;
} // main
#endif

// end of theoraplay.cpp ...

