#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>

#include "codec.h"

int codec_frame_create(AVFrame **const frame)
{
    *frame = av_frame_alloc();
    if (NULL == *frame) return -1;

    return 0;
}

int codec_packet_create(AVPacket **const packet)
{
    *packet = av_packet_alloc();
    if (NULL == *packet) return -1;
    
    return 0;
}

int codec_open_context(AVCodecContext *const ctx, AVCodec const *const codec)
{
    int error = avcodec_open2(ctx, codec, NULL);
    return error < 0 ? -1 : 0;
}

int codec_copy_params_to_context(AVCodecContext *const    ctx,
                                 AVCodecParameters *const params)
{
    int error = avcodec_parameters_to_context(ctx, params);
    return error < 0 ? -1 : 0;
}

int codec_create_decode_context(AVCodecContext **const ctx, 
                                enum AVCodecID const   id)
{
    AVCodec const * codec = NULL;

    codec = avcodec_find_decoder(id);
    if (NULL == codec) return -1;

    *ctx = avcodec_alloc_context3(codec);
    if (NULL == *ctx) return -1;

    return 0;
}
