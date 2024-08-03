#ifndef FILE_H
#define FILE_H

#include <libavformat/avformat.h>
#include <libavcodec/packet.h>
#include <libavutil/frame.h>
#include <libavutil/avutil.h>

int file_find_first_stream_by_media_type(AVFormatContext *const myctx,
                                         AVStream       **const stream,
                                         enum AVMediaType const media_type);

int file_read_stream(AVFormatContext *const myctx,
                     AVStream *const        stream,
                     AVPacket *const        packet);

int file_open_read_context(AVFormatContext **const myctx,
                           char const *const       filepath);

#endif
