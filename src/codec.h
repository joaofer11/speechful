#ifndef CODEC_H
#define CODEC_H

#include <stdbool.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/codec_id.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavutil/frame.h>

int codec_packet_create(AVPacket **const packet);

int codec_frame_create(AVFrame **const frame);

int codec_open_context(AVCodecContext *const ctx, AVCodec const *const codec);

int codec_copy_params_to_context(AVCodecContext *const    ctx,
                                 AVCodecParameters *const params);

int codec_create_decode_context(AVCodecContext **const ctx, 
                                enum AVCodecID const   id);

#endif
