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

struct YuvVideoItem
{
    unsigned int playms;
    unsigned int width;
    unsigned int height;
    unsigned char *yuv;
    struct YuvVideoItem *next;
};

struct PcmAudioItem
{
    unsigned int playms;  // playback start time in milliseconds.
    int channels;
    int frames;
    float *samples;  // frames * channels float32 samples.
    struct PcmAudioItem *next;
};

class TheoraDecoder
{
public:
    TheoraDecoder();
    ~TheoraDecoder();
    bool StartDecode(const char *fname, const unsigned int _maxframes);
    void StopDecode();
    bool IsDecoding() const { return lock_created && !done; }
    const PcmAudioItem *GetAudio();
    const YuvVideoItem *GetVideo();
    void FreeAudio(const PcmAudioItem *item);
    void FreeVideo(const YuvVideoItem *item);

private:
    void CleanupWorkerThread();
    bool FeedMoreOggData();
    void QueueOggPage(ogg_page *page);
    bool InitDecoderState();
    void WorkerThread();
    static void *WorkerThreadEntry(void *_this);

    // Thread wrangling...
    bool lock_created;
    pthread_mutex_t lock;  // !!! FIXME: atomics might be nicer here.
    volatile bool halt;
    volatile unsigned int videocount;
    bool done;
    pthread_t worker;

    // Ogg, Vorbis, and Theora decoder state...
    int fd;
    bool decoder_init;
    ogg_sync_state sync;
    ogg_page page;
    int vpackets;
    vorbis_info vinfo;
    vorbis_comment vcomment;
    ogg_stream_state vstream;
    bool vdsp_init;
    vorbis_dsp_state vdsp;
    bool vblock_init;
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
};


TheoraDecoder::TheoraDecoder()
    : lock_created(false)
    , halt(false)
    , videocount(0)
    , done(false)
    , fd(-1)
    , decoder_init(false)
    , vpackets(0)
    , vdsp_init(false)
    , vblock_init(false)
    , tpackets(0)
    , tsetup(NULL)
    , tdec(NULL)
    , maxframes(0)
    , videolist(NULL)
    , videolisttail(NULL)
    , audiolist(NULL)
    , audiolisttail(NULL)
{
} // TheoraDecoder::TheoraDecoder


TheoraDecoder::~TheoraDecoder()
{
    StopDecode();
} // TheoraDecoder::~TheoraDecoder

void TheoraDecoder::CleanupWorkerThread()
{
    if (fd != -1)
        close(fd);

    if (tdec != NULL)
        th_decode_free(tdec);

    if (tsetup != NULL)
        th_setup_free(tsetup);

    if (vblock_init)
        vorbis_block_clear(&vblock);

    if (vdsp_init)
        vorbis_dsp_clear(&vdsp);

    if (tpackets)
        ogg_stream_clear(&tstream);

    if (vpackets)
        ogg_stream_clear(&vstream);

    if (decoder_init)
    {
        th_info_clear(&tinfo);
        th_comment_clear(&tcomment);
        vorbis_comment_clear(&vcomment);
        vorbis_info_clear(&vinfo);
        ogg_sync_clear(&sync);
    } // if

    fd = -1;
    decoder_init = false;
    tpackets = 0;
    vpackets = 0;
    tsetup = NULL;
    tdec = NULL;
    vdsp_init = false;
    vblock_init = false;
} // TheoraDecoder::CleanupWorkerThread

void TheoraDecoder::StopDecode()
{
    if (lock_created)
    {
        halt = true;
        pthread_join(worker, NULL);
        halt = false;
        done = false;
    } // if

    if (lock_created)
    {
        pthread_mutex_destroy(&lock);
        lock_created = false;
    } // if

    while (videolist)
    {
        YuvVideoItem *next = videolist->next;
        delete[] videolist->yuv;
        delete videolist;
        videolist = next;
    } // while

    videolisttail = NULL;
    videocount = 0;

    while (audiolist)
    {
        PcmAudioItem *next = audiolist->next;
        delete[] audiolist->samples;
        delete audiolist;
        audiolist = next;
    } // while

    audiolisttail = NULL;

    maxframes = 0;
} // TheoraDecoder::StopDecode

bool TheoraDecoder::StartDecode(const char *fname, const unsigned int _maxframes)
{
    StopDecode();

    assert(fd == -1);
    assert(!halt);
    assert(!videocount);
    assert(!done);
    assert(!lock_created);
    assert(!decoder_init);
    assert(!vpackets);
    assert(!tpackets);
    assert(!tsetup);
    assert(!tdec);
    assert(!vdsp_init);
    assert(!vblock_init);
    assert(!maxframes);
    assert(!videolist);
    assert(!videolisttail);
    assert(!audiolist);
    assert(!audiolisttail);

    maxframes = _maxframes;

    fd = open(fname, O_RDONLY);
    if (fd != -1)
    {
        struct stat statbuf;
        if (fstat(fd, &statbuf) != -1)
        {
            lock_created = (pthread_mutex_init(&lock, NULL) == 0);
            if (lock_created)
            {
                if (pthread_create(&worker, NULL, &TheoraDecoder::WorkerThreadEntry, this) == 0)
                    return true;
            } // if

            pthread_mutex_destroy(&lock);
            lock_created = false;
        } // if

        close(fd);
        fd = -1;
    } // if

    return false;
} // TheoraDecoder::StartDecode


const PcmAudioItem *TheoraDecoder::GetAudio()
{
    PcmAudioItem *retval;

    pthread_mutex_lock(&lock);
    retval = audiolist;
    if (retval)
    {
        audiolist = retval->next;
        retval->next = NULL;
        if (audiolist == NULL)
            audiolisttail = NULL;
    } // if
    pthread_mutex_unlock(&lock);

    return retval;
} // TheoraDecoder::GetAudio

const YuvVideoItem *TheoraDecoder::GetVideo()
{
    YuvVideoItem *retval;

    pthread_mutex_lock(&lock);
    retval = videolist;
    if (retval)
    {
        videolist = retval->next;
        retval->next = NULL;
        if (videolist == NULL)
            videolisttail = NULL;
        assert(videocount > 0);
        videocount--;
    } // if
    pthread_mutex_unlock(&lock);

    return retval;
} // TheoraDecoder::GetVideo

void TheoraDecoder::FreeAudio(const PcmAudioItem *_item)
{
    PcmAudioItem *item = (PcmAudioItem *) _item;
    assert(item->next == NULL);
    delete[] item->samples;
    delete item;
} // TheoraDecoder::FreeAudio

void TheoraDecoder::FreeVideo(const YuvVideoItem *_item)
{
    YuvVideoItem *item = (YuvVideoItem *) _item;
    assert(item->next == NULL);
    delete[] item->yuv;
    delete item;
} // TheoraDecoder::FreeVideo

bool TheoraDecoder::FeedMoreOggData()
{
    long buflen = 4096;  // !!! FIXME: tweak this?
    char *buffer = ogg_sync_buffer(&sync, buflen);
    if (buffer == NULL)
        return false;

    while ( ((buflen = read(fd, buffer, buflen)) < 0) && (errno == EINTR) ) {}

    if (buflen <= 0)
        return false;

    return (ogg_sync_wrote(&sync, buflen) == 0);
} // TheoraDecoder::FeedMoreOggData

void TheoraDecoder::QueueOggPage(ogg_page *page)
{
    // make sure we initialized the stream before using pagein, but the stream
    //  will know to ignore pages that aren't meant for it, so pass to both.
    if(tpackets) ogg_stream_pagein(&tstream, page);
    if(vpackets) ogg_stream_pagein(&vstream, page);
} // TheoraDecoder::QueueOggPage


void TheoraDecoder::WorkerThread()
{
    ogg_packet packet;
    unsigned long audioframes = 0;
    unsigned long videoframes = 0;
    double fps = 0.0;

    ogg_sync_init(&sync);
    vorbis_info_init(&vinfo);
    vorbis_comment_init(&vcomment);
    th_comment_init(&tcomment);
    th_info_init(&tinfo);
    decoder_init = true;  // !! FIXME: ditch this.

    bool bos = true;
    while (!halt && bos)
    {
        if (!FeedMoreOggData())
            break;

        // parse out the initial header.
        while ( (!halt) && (ogg_sync_pageout(&sync, &page) > 0) )
        {
            ogg_stream_state test;

            if (!ogg_page_bos(&page))  // not a header.
            {
                QueueOggPage(&page);
                bos = false;
                break;
            } // if

            ogg_stream_init(&test, ogg_page_serialno(&page));
            ogg_stream_pagein(&test, &page);
            ogg_stream_packetout(&test, &packet);

            if (!tpackets && (th_decode_headerin(&tinfo, &tcomment, &tsetup, &packet) >= 0))
            {
                memcpy(&tstream, &test, sizeof (test));
                tpackets = 1;
            } // if
            else if (!vpackets && (vorbis_synthesis_headerin(&vinfo, &vcomment, &packet) >= 0))
            {
                memcpy(&vstream, &test, sizeof (test));
                vpackets = 1;
            } // else if
            else
            {
                // whatever it is, we don't care about it
                ogg_stream_clear(&test);
            } // else
        } // while
    } // while

    // no audio OR video?
    if (halt || (!vpackets && !tpackets))
        return;

    // apparently there are two more theora and two more vorbis headers next.
    while ((!halt) && ((tpackets && (tpackets < 3)) || (vpackets && (vpackets < 3))))
    {
        while (!halt && tpackets && (tpackets < 3))
        {
            if (ogg_stream_packetout(&tstream, &packet) != 1)
                break; // get more data?
            if (!th_decode_headerin(&tinfo, &tcomment, &tsetup, &packet))
                return;
            tpackets++;
        } // while

        while (!halt && vpackets && (vpackets < 3))
        {
            if (ogg_stream_packetout(&vstream, &packet) != 1)
                break;  // get more data?
            if (vorbis_synthesis_headerin(&vinfo, &vcomment, &packet))
                return;
            vpackets++;
        } // while

        // get another page, try again?
        if (ogg_sync_pageout(&sync, &page) > 0)
            QueueOggPage(&page);
        else if (!FeedMoreOggData())
            return;
    } // while

    // okay, now we have our streams, ready to set up decoding.
    if (!halt && tpackets)
    {
        // th_decode_alloc() docs say to check for insanely large frames yourself.
        if ((tinfo.frame_width > 99999) || (tinfo.frame_height > 99999))
            return;

        //if (tinfo.colorspace != TH_CS_ITU_REC_470M) { assert(0); return; } // !!! FIXME
        if (tinfo.pixel_fmt != TH_PF_420) { assert(0); return; } // !!! FIXME

        if (tinfo.fps_denominator != 0)
            fps = ((double) tinfo.fps_numerator) / ((double) tinfo.fps_denominator);

        tdec = th_decode_alloc(&tinfo, tsetup);
        if (!tdec)
            return;

        // Set decoder to maximum post-processing level.
        //  Theoretically we could try dropping this level if we're not keeping up.
        int pp_level_max = 0;
        th_decode_ctl(tdec, TH_DECCTL_GET_PPLEVEL_MAX, &pp_level_max, sizeof(pp_level_max));
        th_decode_ctl(tdec, TH_DECCTL_SET_PPLEVEL, &pp_level_max, sizeof(pp_level_max));
    } // if

    // Done with this now.
    if (tsetup != NULL)
    {
        th_setup_free(tsetup);
        tsetup = NULL;
    } // if

    if (!halt && vpackets)
    {
        vdsp_init = (vorbis_synthesis_init(&vdsp, &vinfo) == 0);
        if (!vdsp_init)
            return;
        vblock_init = (vorbis_block_init(&vdsp, &vblock) == 0);
        if (!vblock_init)
            return;
    } // if

    // Now we can start the actual decoding!
    // Note that audio and video don't _HAVE_ to start simultaneously.

    while (!halt)
    {
        bool need_pages = false;  // need more Ogg pages?
        bool saw_video_frame = false;

        // Try to read as much audio as we can at once. We limit the outer
        //  loop to one video frame and as much audio as we can eat.
        while (!halt && vpackets && !need_pages)
        {
            float **pcm = NULL;
            const int frames = vorbis_synthesis_pcmout(&vdsp, &pcm);
            if (frames > 0)
            {
                const int channels = vinfo.channels;
                int chanidx, frameidx;
                float *samples;
                PcmAudioItem *item = new PcmAudioItem;
                item->playms = (unsigned long) ((((double) audioframes) / ((double) vinfo.rate)) * 1000.0);
                item->channels = channels;
                item->frames = frames;
                item->samples = new float[frames * channels];
                item->next = NULL;

                // I bet this beats the crap out of the CPU cache...
                samples = item->samples;
                for (frameidx = 0; frameidx < channels; frameidx++)
                {
                    for (chanidx = 0; chanidx < channels; chanidx++)
                        *(samples++) = pcm[chanidx][frameidx];
                } // for

                vorbis_synthesis_read(&vdsp, frames);  // we ate everything.
                audioframes += frames;

printf("Decoded %d frames of audio.\n", (int) frames);
                pthread_mutex_lock(&lock);
                if (audiolisttail)
                {
                    assert(audiolist);
                    audiolisttail->next = item;
                } // if
                else
                {
                    assert(!audiolist);
                    audiolist = item;
                } // else
                audiolisttail = item;
                pthread_mutex_unlock(&lock);
            } // if

            else  // no audio available left in current packet?
            {
                // try to feed another packet to the Vorbis stream...
                if (ogg_stream_packetout(&vstream, &packet) <= 0)
                    need_pages = true;  // stream needs another page.
                else
                {
                    if (vorbis_synthesis(&vblock, &packet) == 0)
                        vorbis_synthesis_blockin(&vdsp, &vblock);
                } // else
            } // else
        } // while

        if (!halt && tpackets)
        {
            // Theora, according to example_player.c, is
            //  "one [packet] in, one [frame] out."
            if (ogg_stream_packetout(&tstream, &packet) <= 0)
                need_pages = true;
            else
            {
                ogg_int64_t granulepos = 0;
                const int rc = th_decode_packetin(tdec, &packet, &granulepos);
                if (rc == TH_DUPFRAME)
                    videoframes++;  // nothing else to do.
                else if (rc == 0)  // new frame!
                {
                    th_ycbcr_buffer ycbcr;
                    if (th_decode_ycbcr_out(tdec, ycbcr) == 0)
                    {
                        int i;
                        const int w = tinfo.pic_width;
                        const int h = tinfo.pic_height;
                        const int yoff = (tinfo.pic_x & ~1) + ycbcr[0].stride * (tinfo.pic_y & ~1);
                        const int uvoff= (tinfo.pic_x / 2) + (ycbcr[1].stride) * (tinfo.pic_y / 2);
                        unsigned char *yuv;
                        YuvVideoItem *item = new YuvVideoItem;
                        item->playms = (fps == 0) ? 0.0 : (unsigned long) ((((double) videoframes) / fps) * 1000.0);
                        item->width = w;
                        item->height = h;
                        item->yuv = new unsigned char[(w * h) * 2];
                        item->next = NULL;

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
                        pthread_mutex_lock(&lock);
                        if (videolisttail)
                        {
                            assert(videolist);
                            videolisttail->next = item;
                        } // if
                        else
                        {
                            assert(!videolist);
                            videolist = item;
                        } // else
                        videolisttail = item;
                        videocount++;
                        pthread_mutex_unlock(&lock);

                        saw_video_frame = true;
                    } // if
                    videoframes++;
                } // if
            } // else
        } // if

        if (!halt && need_pages)
        {
            if (!FeedMoreOggData())
                return;

            while (!halt && (ogg_sync_pageout(&sync, &page) > 0))
                QueueOggPage(&page);
        } // if

        // Sleep the process until we have space for more frames.
        if (saw_video_frame)
        {
            bool go_on = !halt;
            printf("Sleeping.\n");
            while (go_on)
            {
                // !!! FIXME: This is stupid. I should use a semaphore for this.
                pthread_mutex_lock(&lock);
                go_on = !halt && (videocount >= maxframes);
                pthread_mutex_unlock(&lock);
                if (go_on)
                    usleep(10000);
            } // while
            printf("Awake!\n");
        } // if
    } // while
} // TheoraDecoder::WorkerThread


void *TheoraDecoder::WorkerThreadEntry(void *_this)
{
    TheoraDecoder *decoder = (TheoraDecoder *) _this;
    decoder->WorkerThread();
    decoder->CleanupWorkerThread();
    decoder->done = true;
    printf("Worker thread is done.\n");
    return NULL;
} // TheoraDecoder::WorkerThreadEntry



#if 1
int main(int argc, char **argv)
{
    TheoraDecoder decoder;
    const YuvVideoItem *video = NULL;
    const PcmAudioItem *audio = NULL;

    int i;
    for (i = 1; i < argc; i++)
    {
        printf("Trying file '%s' ...\n", argv[i]);
        decoder.StartDecode(argv[i], 20);
        while (decoder.IsDecoding())
        {
            video = decoder.GetVideo();
            if (video)
            {
                printf("Got video frame (%u ms)!\n", video->playms);
                decoder.FreeVideo(video);
            } // if

            audio = decoder.GetAudio();
            if (audio)
            {
                printf("Got %d frames of audio (%u ms)!\n", audio->frames, audio->playms);
                decoder.FreeAudio(audio);
            } // if

            if (!video && !audio)
                usleep(10000);
        } // while

        printf("done with this file!\n");
        decoder.StopDecode();
    } // for

    printf("done all files!\n");
    return 0;
} // main
#endif

// end of theoraplay.cpp ...

