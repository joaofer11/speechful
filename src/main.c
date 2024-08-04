#include <stdio.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavutil/frame.h>
#include <libavutil/avutil.h>

#include "file.h"
#include "codec.h"

int main(int const argc, char const *const argv[])
{
    AVFormatContext * in_audio_file_ctx = NULL;
    AVStream * in_audio_stream = NULL;
    AVPacket * in_packet = NULL;
    AVFrame  * in_frame = NULL;
    AVCodecContext * in_audio_decoder_ctx = NULL;

    int error = 0;

    error = file_create_read_context(&in_audio_file_ctx, argv[1]);
    if (error < 0) return -1;

    error = file_find_first_stream_by_media_type(in_audio_file_ctx,
                                                 &in_audio_stream, 
                                                 AVMEDIA_TYPE_AUDIO);
    if (error < 0) goto file_destroy_readable_audio_ctx;

    error = codec_packet_create(&in_packet);
    if (error < 0) goto file_destroy_readable_audio_ctx;

    error = codec_frame_create(&in_frame);
    if (error < 0) goto codec_packet_destroy_input;

    error = codec_create_decode_context(&in_audio_decoder_ctx,
                                        in_audio_stream->codecpar->codec_id);
    if (error < 0) goto codec_frame_destroy_input;

    error = codec_copy_params_to_context(in_audio_decoder_ctx,
                                         in_audio_stream->codecpar);
    if (error < 0) goto codec_destroy_audio_decode_ctx;

    error = codec_open_context(in_audio_decoder_ctx,
                               in_audio_decoder_ctx->codec);
    if (error < 0) goto codec_destroy_audio_decode_ctx;
    /* Until this point the input audio file context is all set. */

    error = file_create_write_context(&out_audio_file_ctx, argv[2]);
    if (error < 0) goto codec_destroy_audio_decode_ctx;

    error = file_add_empty_stream_to_context(out_audio_file_ctx,
                                             &out_audio_stream);
    if (error < 0) goto file_destroy_writable_audio_ctx;

    error = codec_create_encode_context(&out_audio_encoder_ctx,
                                        AV_CODEC_ID_MP3);
    if (error < 0) goto file_destroy_writable_audio_ctx;

    while (1)
    {
        error = file_read_stream(in_audio_file_ctx, in_audio_stream, in_packet);
        if (error < 0) break;

        error = avcodec_send_packet(in_audio_decoder_ctx, in_packet);
        if (error < 0) break;

        while (1)
        {
            error = avcodec_receive_frame(in_audio_decoder_ctx, in_frame);
            if (error < 0) break;

            printf("pkt pts: %ld\n", in_packet->pts);
            printf("pkt dts: %ld\n", in_packet->dts);
            printf("pkt dur: %ld\n", in_packet->duration);
            printf("\n");

            printf("frm pts: %ld\n", in_frame->pts);
            printf("frm dts: %ld\n", in_frame->pkt_dts);
            printf("frm dur: %ld\n", in_frame->duration);
            printf("\n");

            av_frame_unref(in_frame);
        }

        av_packet_unref(in_packet);
    }

    file_destroy_writable_audio_ctx:
        out_audio_stream = NULL;
        avio_closep(&(out_audio_file_ctx->pb));     
        avformat_free_context(out_audio_file_ctx);
        out_audio_file_ctx = NULL;

    codec_destroy_audio_decode_ctx:
        avcodec_free_context(&in_audio_decoder_ctx);

    codec_frame_destroy_input:
        av_frame_free(&in_frame);

    codec_packet_destroy_input:
        av_packet_free(&in_packet);

    file_destroy_readable_audio_ctx:
        in_audio_stream = NULL;
        avformat_close_input(&in_audio_file_ctx);

    return 0;
}
