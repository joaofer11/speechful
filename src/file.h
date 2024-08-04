#ifndef FILE_H
#define FILE_H

#include <libavformat/avformat.h>
#include <libavcodec/packet.h>
#include <libavutil/frame.h>
#include <libavutil/avutil.h>

int file_find_first_stream_by_media_type(AVFormatContext *const ctx,
                                         AVStream       **const stream,
                                         enum AVMediaType const media_type);

int file_create_write_context(AVFormatContext **const ctx,
                              char const *const       filepath);

int file_read_stream(AVFormatContext *const ctx,
                     AVStream *const        stream,
                     AVPacket *const        packet);

int file_create_read_context(AVFormatContext **const ctx,
                           char const *const       filepath);

#endif
