#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavutil/frame.h>

#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>

#include <libavformat/avio.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_id.h>
#include <libavcodec/codec_par.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>

#include <libavutil/avutil.h>
#include <libavutil/mem.h>
#include <libavutil/macros.h>
#include <libavutil/error.h>


SwrContext *resampler_open_context(AVCodecContext *const out_codec,
                                   AVCodecContext *const src_codec);

AVCodecContext *config_output_audio_stream_encoder(AVStream *const audio_stream);

AVCodecContext *codec_open_decoder_context(AVCodecParameters *const params);

int file_read(AVFormatContext *const file,
              AVPacket        *const packet,
              AVStream        *const stream);

AVStream *file_find_stream_by_type(AVFormatContext *const file,
                              enum AVMediaType      const type);

int file_write_header(AVFormatContext *const file);

AVFormatContext *file_open_write_context(char const *const filepath);
AVFormatContext *file_open_read_context(char const *const filepath);


int main(int const argc, char const *const argv[])
{
    AVFormatContext *input_audio_file_ctx                  = NULL;
    AVFormatContext *output_audio_file_ctx                 = NULL;

    AVCodecContext  *input_audio_stream_decoder            = NULL;
    AVStream        *input_audio_stream                    = NULL;
    AVPacket        *input_packet                          = NULL;
    AVFrame         *input_frame                           = NULL;
 
    AVCodecContext  *output_audio_stream_encoder           = NULL;
    AVStream        *output_audio_stream                   = NULL;
    AVPacket        *output_packet                         = NULL;
    AVFrame         *output_frame                          = NULL;

    SwrContext      *resampler                             = NULL;
    AVAudioFifo     *samples_to_encode                     = NULL;

    size_t           samples_encoded_count                 = 0;
bool             has_input_audio_file_been_fully_read  = false;

    int error = 0;
    
    if (NULL == (input_packet  = av_packet_alloc()) ||
        NULL == (input_frame   = av_frame_alloc())  ||
        NULL == (output_packet = av_packet_alloc()) ||
        NULL == (output_frame  = av_frame_alloc())) {
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

    output_audio_stream_encoder = config_output_audio_stream_encoder(output_audio_stream);
    if (NULL == output_audio_stream_encoder) goto error;

    error = file_write_header(output_audio_file_ctx);
    if (error < 0) goto error;

    resampler = resampler_open_context(output_audio_stream_encoder, 
                                       input_audio_stream_decoder);
    if (NULL == resampler) goto error;

    samples_to_encode = av_audio_fifo_alloc(output_audio_stream_encoder->sample_fmt,
                                            output_audio_stream_encoder->ch_layout.nb_channels,
                                            output_audio_stream_encoder->frame_size);
    if (NULL == samples_to_encode) {
        fprintf(stderr, "Error: could not alloc queue.\n");
            goto error;
        }

    /* The real fun starts here... */
    while (!has_input_audio_file_been_fully_read) {
        error = file_read(input_audio_file_ctx, input_packet, input_audio_stream);
        if (error < 0 && AVERROR_EOF != error) goto error;

        has_input_audio_file_been_fully_read = AVERROR_EOF == error;

        /* Send encoded input audio data to the decoder if there's any.
         *
         * Otherwise signal the decoder to release any internal
         * buffered decoded data. */
        error = avcodec_send_packet(input_audio_stream_decoder,
                                    has_input_audio_file_been_fully_read
                                        ? NULL 
                                        : input_packet);
        if (error < 0) goto error;

        /* Resample each decoded input audio data from the decoder. */
        while (0 == (error = avcodec_receive_frame(input_audio_stream_decoder, input_frame))) {
            uint8_t                   **converted_samples  = NULL;
            uint8_t const *const *const samples_to_convert = (uint8_t const *const *const)(input_frame->extended_data); 

            /* Alloc temporary storage for the converted 
             * input audio samples. */
            error = av_samples_alloc_array_and_samples(&converted_samples,
                                                        NULL,
                                                        output_audio_stream_encoder->ch_layout.nb_channels,
                                                        input_frame->nb_samples,
                                                        output_audio_stream_encoder->sample_fmt,
                                                        0);
            if (error < 0) goto error;

            error = swr_convert(resampler,
                                converted_samples,  input_frame->nb_samples,
                                samples_to_convert, input_frame->nb_samples);
            if (error >= 0) {
                /* Samples was succesfully converted now save them
                 * to be encoded later. */
                error = av_audio_fifo_write(samples_to_encode,
                                    (void**)converted_samples,
                                            input_frame->nb_samples);
            }

            av_frame_unref(input_frame);
            av_freep(converted_samples);
            converted_samples = NULL;

            if (error < 0) goto error;
        }

        if (AVERROR(EAGAIN) != error &&
            AVERROR_EOF     != error) goto error;

        /* Encode the converted input audio samples and write
         * them into the output audio file. */
        while (av_audio_fifo_size(samples_to_encode) >= output_audio_stream_encoder->frame_size ||
              (has_input_audio_file_been_fully_read && av_audio_fifo_size(samples_to_encode) > 0)) {
            /* Number of samples to encode should ALWAYS be equal to
             * the maximum size of samples the encoder supports.
             *
             * When the number of samples to encode is less than the
             * maximum size the encoder supports, this mean we
             * reached the last chunk of data from input audio. */
            size_t const nb_samples_to_encode = FFMIN(av_audio_fifo_size(samples_to_encode),
                                                      output_audio_stream_encoder->frame_size);

            output_frame->nb_samples  = nb_samples_to_encode;
            output_frame->sample_rate = output_audio_stream_encoder->sample_rate;
            output_frame->format      = output_audio_stream_encoder->sample_fmt;
            av_channel_layout_copy(&(output_frame->ch_layout),
                                   &(output_audio_stream_encoder->ch_layout));

            output_frame->pts      = samples_encoded_count;
            samples_encoded_count += nb_samples_to_encode;

            error = av_frame_get_buffer(output_frame, 0);
            if (error < 0) goto error;

            error = av_audio_fifo_read(samples_to_encode,
                              (void**)(output_frame->extended_data),
                                       nb_samples_to_encode);
            if (error < 0) goto error;

            error = avcodec_send_frame(output_audio_stream_encoder, output_frame);
            if (error < 0) goto error;

            while (0 == (error = avcodec_receive_packet(output_audio_stream_encoder, output_packet))) {
                error = av_write_frame(output_audio_file_ctx, output_packet);                                  
                if (error < 0) goto error;

                av_packet_unref(output_packet);
            }

            av_frame_unref(output_frame);

            if (AVERROR(EAGAIN) != error &&
                AVERROR_EOF     != error) goto error;
        }

        if (has_input_audio_file_been_fully_read) {
            /* Tell the output audio encoder to release 
             * any internal buffered data. */
            error = avcodec_send_frame(output_audio_stream_encoder, NULL);
            if (error < 0) goto error;

            while (0 == (error = avcodec_receive_packet(output_audio_stream_encoder, output_packet))) {
                error = av_write_frame(output_audio_file_ctx, output_packet);                                  
                if (error < 0) goto error;

                av_packet_unref(output_packet);
            }

            if (AVERROR(EAGAIN) != error &&
                AVERROR_EOF     != error) goto error;
        }

        av_packet_unref(input_packet);
    }

    error = 0;

    exit:
        input_audio_stream  = NULL;
        output_audio_stream = NULL;

        if (NULL != input_packet)               av_packet_free(&input_packet);
        if (NULL != input_frame)                av_frame_free(&input_frame);
        if (NULL != output_packet)              av_packet_free(&output_packet);
        if (NULL != output_frame)               av_frame_free(&output_frame);

        if (NULL != input_audio_file_ctx)       avformat_close_input(&input_audio_file_ctx);
        if (NULL != input_audio_stream_decoder) avcodec_free_context(&input_audio_stream_decoder);

        if (NULL != output_audio_file_ctx) {
            avio_closep(&(output_audio_file_ctx->pb));
            avformat_free_context(output_audio_file_ctx);
            output_audio_file_ctx = NULL;
        }
        if (NULL != output_audio_stream_encoder) avcodec_free_context(&output_audio_stream_encoder);

        if (NULL != resampler)                   swr_free(&resampler);
        if (NULL != samples_to_encode)           av_audio_fifo_free(samples_to_encode);

       return error; 

    error:
        error = 1; 
        goto exit;
}

SwrContext *resampler_open_context(AVCodecContext *const dst_codec,
                                   AVCodecContext *const src_codec)
{
    SwrContext *ret = NULL;
    int error = 0;

    error = swr_alloc_set_opts2(&ret,
                                    &(dst_codec->ch_layout),
                                      dst_codec->sample_fmt,
                                      dst_codec->sample_rate,
                                    &(src_codec->ch_layout),
                                      src_codec->sample_fmt,
                                      src_codec->sample_rate,
                                      0, NULL);
    if (error < 0) goto error;

    error = swr_init(ret);
    if (error < 0) goto error;
    
    return ret;

    error:
        if (NULL != ret) swr_free(&ret);

        fprintf(stderr, "Error: could not open resampler.\n");
        return NULL;
}

AVCodecContext *config_output_audio_stream_encoder(AVStream *const audio_stream)
{
    AVCodecContext *ret   = NULL;
    AVCodec const  *codec = NULL;
    int error = 0;

    codec = avcodec_find_encoder(AV_CODEC_ID_MP3);
    if (NULL == codec) goto error;

    ret = avcodec_alloc_context3(codec);
    if (NULL == ret) goto error;

    ret->sample_fmt  = AV_SAMPLE_FMT_S16P;
    ret->bit_rate    = 256000;
    ret->sample_rate = 48000;
    av_channel_layout_default(&(ret->ch_layout), 2);

    audio_stream->time_base.num = 1;
    audio_stream->time_base.den = ret->sample_rate;

    error = avcodec_open2(ret, codec, NULL);
    if (error < 0) goto error;

    error = avcodec_parameters_from_context(audio_stream->codecpar, ret);
    if (error < 0) goto error;

    return ret;

    error:
        if (NULL != ret) avcodec_free_context(&ret);

        fprintf(stderr, "Error: could not open encoder 'MP3'.\n");
        return NULL;
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

    error = avcodec_parameters_to_context(ret, params);
    if (error < 0) goto error;

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

int file_write_header(AVFormatContext *const file)
{
    int error = avformat_write_header(file, NULL);
    if (error < 0) {
        fprintf(stderr, "Error: could not write header to file '%s'.\n", file->url);
        return -1;
    }

    return 0;
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
