#include <stdio.h>
#include <stddef.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/packet.h>
#include <libavutil/frame.h>

#include <libavutil/avutil.h>
#include <libavutil/error.h>

int codec_open_stream_decoder(AVCodecContext **const decoder,
                              AVStream        *const stream);

int file_read_from_stream(AVFormatContext *const file,
                          AVStream        *const stream,
                          AVPacket        *const packet);

int file_find_first_stream_by_media_type(AVFormatContext *const file,
                                         AVStream       **const stream,
                                         enum AVMediaType const media_type);

int frame_create(AVFrame **const frame);
int packet_create(AVPacket **const packet);
int file_open_read(AVFormatContext **const file, char const *const filepath);

int main(int const argc, char const *const argv[])
{
    AVFormatContext * in_file = NULL;
    AVPacket        * in_pkt  = NULL;
    AVFrame         * in_frm  = NULL;

    AVStream        * in_audio = NULL;
    AVCodecContext  * in_audio_decoder = NULL;
    
    int error = 0;

    error = file_open_read(&in_file, argv[1]);
    if (error < 0) return 1;

    error = packet_create(&in_pkt);
    if (error < 0) goto file_close_input;

    error = frame_create(&in_frm);
    if (error < 0) goto packet_destroy;

    error = file_find_first_stream_by_media_type(in_file, 
                                                 &in_audio, 
                                                 AVMEDIA_TYPE_AUDIO);
    if (error < 0) goto frame_destroy;

    error = codec_open_stream_decoder(&in_audio_decoder, in_audio);
    if (error < 0) goto frame_destroy;

    while (1)
    {
        error = file_read_from_stream(in_file, in_audio, in_pkt); 
        if (error < 0) break;

        error = avcodec_send_packet(in_audio_decoder, in_pkt);
        if (error < 0) break;

        while (1)
        {
            error = avcodec_receive_frame(in_audio_decoder, in_frm);
            if (error < 0) break;

            printf("pkt pts: %ld\n", in_pkt->pts);
            printf("pkt dts: %ld\n", in_pkt->dts);
            printf("pkt dur: %ld\n", in_pkt->duration);
            printf("\n");

            printf("frm pts: %ld\n", in_frm->pts);
            printf("frm dts: %ld\n", in_frm->pkt_dts);
            printf("frm dur: %ld\n", in_frm->duration);
            printf("\n");

            av_frame_unref(in_frm);
        }

        av_packet_unref(in_pkt);
    }

    codec_close_decoder:
        avcodec_free_context(&in_audio_decoder);

    frame_destroy:
        av_frame_free(&in_frm);

    packet_destroy:
        av_packet_free(&in_pkt);

    file_close_input:
        in_audio = NULL;
        avformat_close_input(&in_file);

    return error < 0 ? 1 : 0;
}

int codec_open_stream_decoder(AVCodecContext **const decoder,
                              AVStream        *const stream)
{
    AVCodec const * codec = NULL;
    AVCodecContext * out  = NULL;

    int error = 0;

    codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (NULL == codec) return -1;

    out = avcodec_alloc_context3(codec);
    if (NULL == out) return -1;

    error = avcodec_parameters_to_context(out, stream->codecpar);
    if (error < 0) goto codec_destroy_context;

    error = avcodec_open2(out, codec, NULL);
    if (error < 0) goto codec_destroy_context;

    out->pkt_timebase = stream->time_base;
    *decoder = out;

    return 0;

    codec_destroy_context:
        avcodec_free_context(&out);

    return -1;
}

int frame_create(AVFrame **const frame)
{
    AVFrame *const out = av_frame_alloc();
    if (NULL == out) return -1;

    *frame = out;
    return 0;
}

int packet_create(AVPacket **const packet)
{
    AVPacket *const out = av_packet_alloc();
    if (NULL == out) return -1;
    
    *packet = out;
    return 0;
}

int file_read_from_stream(AVFormatContext *const file,
                          AVStream        *const stream,
                          AVPacket        *const packet)
{
    int error = 0;
    while (1)
    {
        error = av_read_frame(file, packet);
        if (error < 0) return -1;

        if (packet->stream_index == stream->index) return 0;

        av_packet_unref(packet);
    }
}

int file_find_first_stream_by_media_type(AVFormatContext *const file,
                                         AVStream       **const stream,
                                         enum AVMediaType const media_type)
{
    for (size_t i = 0; i < file->nb_streams; ++i)
    {
        if (media_type == file->streams[i]->codecpar->codec_type) {
            *stream = file->streams[i];
            return 0;
        }
    }

    return -1;
}

int file_open_read(AVFormatContext **const file, char const *const filepath)
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

    *file = out;
    return 0;
}
