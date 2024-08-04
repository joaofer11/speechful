#include "file.h"
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/mem.h>

int file_find_first_stream_by_media_type(AVFormatContext *const ctx,
                                         AVStream       **const stream,
                                         enum AVMediaType const media_type)
{
    for (size_t i = 0; i < ctx->nb_streams; ++i)
    {
        if (media_type == ctx->streams[i]->codecpar->codec_type) {
            *stream = ctx->streams[i];
            return 0;
        }
    }

    return -1;
}

int file_add_empty_stream_to_context(AVFormatContext *const ctx, 
                                     AVStream **const       stream)
{
    *stream = avformat_new_stream(ctx, NULL);

    return (NULL == *stream) 
        ? -1
        :  0;
}

int file_create_write_context(AVFormatContext **const ctx,
                              const char *const       filepath)
{
    int error = 0;

    *ctx = avformat_alloc_context();
    if (NULL == *ctx) return -1;

    error = avio_open(&((*ctx)->pb), filepath, AVIO_FLAG_WRITE);
    if (error < 0) goto file_destroy_context;

    (*ctx)->oformat = av_guess_format(NULL, filepath, NULL);
    if (NULL == (*ctx)->oformat) goto file_destroy_writable_io;

    (*ctx)->url = av_strdup(filepath);
    if (NULL == (*ctx)->url) goto file_destroy_writable_io;

    return 0;

    file_destroy_writable_io:
        avio_closep(&((*ctx)->pb));

    file_destroy_context:
        avformat_free_context(*ctx);
        *ctx = NULL;

    return -1;
}

int file_read_stream(AVFormatContext *const ctx,
                     AVStream        *const stream,
                     AVPacket        *const packet)
{
    int error = 0;
    while (1)
    {
        error = av_read_frame(ctx, packet);
        if (error < 0) return -1;

        if (packet->stream_index == stream->index) return 0;

        av_packet_unref(packet);
    }
}

int file_create_read_context(AVFormatContext **const ctx,
                             char const *const       filepath)
{
    AVFormatContext * out = NULL;
    int error = 0;

    error = avformat_open_input(&out, filepath, NULL, NULL);
    if (error < 0) return -1;

    error = avformat_find_stream_info(out, NULL);
    if (error < 0) {
        avformat_close_input(&out);
        return -1;
    }

    *ctx = out;
    return 0;
}
