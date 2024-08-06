#include <libavcodec/codec.h>
#include <libavcodec/codec_id.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <stdio.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

AVCodecContext *codec_open_decoder_context(AVCodecParameters *const params);

int file_read(AVFormatContext *const file,
              AVPacket        *const packet,
              AVStream        *const stream);

AVStream *file_find_stream_by_type(AVFormatContext *const file,
                              enum AVMediaType      const type);

AVFormatContext *file_open_write_context(char const *const filepath);
AVFormatContext *file_open_read_context(char const *const filepath);


int main(int const argc, char const *const argv[])
{
    AVFormatContext *input_audio_file_ctx             = NULL;
    AVFormatContext *output_audio_file_ctx            = NULL;

    AVCodecContext  *input_audio_stream_decoder       = NULL;
    AVStream        *input_audio_stream               = NULL;
    AVPacket        *input_packet                     = NULL;
    AVFrame         *input_frame                      = NULL;
 
    AVStream        *output_audio_stream              = NULL;

    int error = 0;
    
    if (NULL == (input_packet = av_packet_alloc()) ||
        NULL == (input_frame  = av_frame_alloc())) {
        fprintf(stderr, "Error: Could not allocate packet or frame.\n");
        goto error;
    }
    
    input_audio_file_ctx = file_open_read_context(argv[1]);
    if (NULL == input_audio_file_ctx) goto error;

    input_audio_stream = file_find_stream_by_type(input_audio_file_ctx,
                                                  AVMEDIA_TYPE_AUDIO);
    if (NULL == input_audio_stream) goto error;

    input_audio_file_ctx = file_open_read_context(argv[1]);
    if (NULL == input_audio_stream) goto error;

    input_audio_stream_decoder = codec_open_decoder_context(input_audio_stream->codecpar);  
    if (NULL == input_audio_stream_decoder) goto error;
    /* Until this point the input audio is ready to be read and decoded. */ 

    output_audio_file_ctx = file_open_write_context(argv[2]);
    if (error < 0) goto error;

    output_audio_stream = avformat_new_stream(output_audio_file_ctx, NULL);
    if (NULL == output_audio_stream) goto error;

            goto error;
        }

        while (0 == (error = avcodec_receive_frame(input_audio_stream_decoder, input_frame))) {
            printf("pkt pts: %ld\n", input_packet->pts);
            printf("pkt dts: %ld\n", input_packet->dts);
            printf("pkt dur: %ld\n", input_packet->duration);
            printf("\n");

            printf("frm pts: %ld\n", input_frame->pts);
            printf("frm dts: %ld\n", input_frame->pkt_dts);
            printf("frm dur: %ld\n", input_frame->duration);
            printf("\n");

            av_frame_unref(input_frame);
        }

        if (AVERROR(EAGAIN) != error) {
            fprintf(stderr, "Error: could not decode input audio stream.\n");
            goto error;
        }

        av_packet_unref(input_packet);
    }

    error = 0;

    exit:
        input_audio_stream  = NULL;
        output_audio_stream = NULL;

        if (NULL != input_packet)               av_packet_free(&input_packet);
        if (NULL != input_frame)                av_frame_free(&input_frame);
        if (NULL != input_audio_file_ctx)       avformat_close_input(&input_audio_file_ctx);
        if (NULL != input_audio_stream_decoder) avcodec_free_context(&input_audio_stream_decoder);

        if (NULL != output_audio_file_ctx) {
            avio_closep(&(output_audio_file_ctx->pb));
            avformat_free_context(output_audio_file_ctx);
            output_audio_file_ctx = NULL;
        }
       return error; 

    error:
        error = 1; 
        goto exit;
}

AVCodecContext *codec_open_decoder_context(AVCodecParameters *const params)
{
    AVCodecContext *ret   = NULL;
    AVCodec const  *codec = NULL;

    int error = 0;
    
    codec = avcodec_find_decoder(params->codec_id);
    if (NULL == codec) goto error;

    ret = avcodec_alloc_context3(codec);
    if (NULL == ret) goto error;

    error = avcodec_open2(ret, codec, NULL);
    if (error < 0) goto error;
    
    return ret;

    error:
        if (NULL != ret) avcodec_free_context(&ret);

        fprintf(stderr, "Error: could not open decoder '%s'.\n", avcodec_get_name(params->codec_id));
        return NULL;
}

AVStream *file_find_stream_by_type(AVFormatContext *const file,
                              enum AVMediaType      const type)
{
    for (size_t i = 0; i < file->nb_streams; ++i)
        if (type == file->streams[i]->codecpar->codec_type)
            return file->streams[i];

    fprintf(stderr, "Error: No %s was found in file '%s'.\n",
                     av_get_media_type_string(type),
                     file->url);
    return NULL;
}

int file_read(AVFormatContext *const file,
              AVPacket        *const packet,
              AVStream        *const stream)
{
    int error = 0;
    while (0 == (error = av_read_frame(file, packet))) {
        if (packet->stream_index == stream->index) return 0;

        av_packet_unref(packet);
    }

    return error;
}
AVFormatContext *file_open_write_context(char const *const filepath)
{
    AVFormatContext *ret = NULL;
    int error = 0;

    ret = avformat_alloc_context();
    if (NULL == ret) goto error;

    error = avio_open(&(ret->pb), filepath, AVIO_FLAG_WRITE);
    if (error < 0) goto error;

    ret->url     = av_strdup(filepath);
    ret->oformat = av_guess_format(NULL, filepath, NULL);
    if (NULL == ret->url ||
        NULL == ret->oformat) goto error;

    return ret;

    error:
        if (NULL != ret) {
            if (NULL != ret->pb) avio_closep(&(ret->pb));
            avformat_free_context(ret);
        }

        fprintf(stderr, "Error: could not open file '%s'.\n", filepath);
        return NULL;
}

AVFormatContext *file_open_read_context(const char *const filepath)
{
    AVFormatContext *ret = NULL;
    int error = 0;

    error = avformat_open_input(&ret, filepath, NULL, NULL);
    if (error < 0) goto error;

    error = avformat_find_stream_info(ret, NULL);
    if (error < 0) goto error;

    return ret;

    error:
        fprintf(stderr, "Error: Could not open file '%s'.\n", filepath);
        fprintf(stderr, "Reason: %s\n", av_err2str(error));
        avformat_close_input(&ret);
        
        return NULL;
}
