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

#define AUDIO 0

struct Audio {
    AVFormatContext *file;
    AVCodecContext  *decoder;
    AVCodecContext  *encoder;
    AVStream        *stream;
    AVPacket        *pkt;
    AVFrame         *frm;
};

struct ResamplerParams {
    uint8_t **channels;
    size_t    channels_cnt;
    size_t    samples_cnt;
    enum AVSampleFormat sample_fmt;
};

int receive_encoded(AVCodecContext *const encoder, AVPacket *const pkt)
{
    int error = avcodec_receive_packet(encoder, pkt);
    return error;
}

int send_decoded(AVCodecContext *const encoder, AVFrame *const frm)
{
    int error = avcodec_send_frame(encoder, frm);
    return error;
}

int receive_decoded(AVCodecContext *const decoder, AVFrame *const frm)
{
    int error = avcodec_receive_frame(decoder, frm);
    return error;
}

int send_encoded(AVCodecContext *const decoder, AVPacket *const pkt)
{
    int error = avcodec_send_packet(decoder, pkt);
    return error;
}

AVAudioFifo *alloc_samples_queue(enum AVSampleFormat const samplefmt,
                                 int const channels_cnt, int const samples_cnt)
{
    AVAudioFifo *queue = av_audio_fifo_alloc(samplefmt, channels_cnt, samples_cnt);
    return queue;
}

uint8_t **resample(SwrContext *const resampler, struct ResamplerParams const params)
{
    uint8_t **converted_samples = NULL;
    int error = av_samples_alloc_array_and_samples(&converted_samples,
                                                    NULL,
                                                    params.channels_cnt,
                                                    params.samples_cnt,
                                                    params.sample_fmt,
                                                    0);
    if (error < 0) return NULL;

    error = swr_convert(resampler,
                        converted_samples, 
                        params.samples_cnt,
                        (uint8_t const *const *)(params.channels),
                        params.samples_cnt);
    if (error < 0) {
        av_freep(converted_samples);
        av_freep(&converted_samples);
    }

    return converted_samples;
}

SwrContext *open_resampler(AVCodecContext *const dst_codec,
                           AVCodecContext *const src_codec)
{
    SwrContext *resampler = NULL;
    int error = 0;

    error = swr_alloc_set_opts2(&resampler,
                                    &(dst_codec->ch_layout),
                                      dst_codec->sample_fmt,
                                      dst_codec->sample_rate,
                                    &(src_codec->ch_layout),
                                      src_codec->sample_fmt,
                                      src_codec->sample_rate,
                                      0, NULL);
    if (error < 0) goto error;

    error = swr_init(resampler);
    if (error < 0) goto error;
    
    return resampler;

    error:
        if (NULL != resampler) swr_free(&resampler);

        fprintf(stderr, "Error: could not open resampler.\n");
        return NULL;
}

AVCodecContext *open_audio_encoder(AVStream *const audio)
{
    AVCodecContext *encoder = NULL;
    AVCodec const  *codec   = NULL;

    int error = 0;

    codec = avcodec_find_encoder(AV_CODEC_ID_MP3);
    if (NULL == codec) goto error;

    encoder = avcodec_alloc_context3(codec);
    if (NULL == encoder) goto error;

    encoder->sample_fmt  = AV_SAMPLE_FMT_S16P;
    encoder->bit_rate    = 256000;
    encoder->sample_rate = 48000;
    av_channel_layout_default(&(encoder->ch_layout), 2);

    audio->time_base.num = 1;
    audio->time_base.den = encoder->sample_rate;

    error = avcodec_open2(encoder, codec, NULL);
    if (error < 0) goto error;

    error = avcodec_parameters_from_context(audio->codecpar, encoder);
    if (error < 0) goto error;

    return encoder;

    error:
        if (NULL != encoder) avcodec_free_context(&encoder);

        fprintf(stderr, "Error: could not open encoder 'MP3'.\n");
        return NULL;
}


AVCodecContext *open_decoder(AVCodecParameters *const params)
{
    AVCodecContext *decoder = NULL;
    AVCodec const  *codec   = NULL;

    int error = 0;
    
    codec = avcodec_find_decoder(params->codec_id);
    if (NULL == codec) goto error;

    decoder = avcodec_alloc_context3(codec);
    if (NULL == decoder) goto error;

    error = avcodec_parameters_to_context(decoder, params);
    if (error < 0) goto error;

    error = avcodec_open2(decoder, codec, NULL);
    if (error < 0) goto error;
    
    return decoder;

    error:
        if (NULL != decoder) avcodec_free_context(&decoder);

        fprintf(stderr, "Error: could not open decoder '%s'.\n", avcodec_get_name(params->codec_id));
        return NULL; 
}

int readfile(AVPacket *const pkt, AVStream *const stream, AVFormatContext *const file)
{
    int error = 0;

    while (0 == (error = av_read_frame(file, pkt))) {
        if (pkt->stream_index == stream->index) return 0;

        av_packet_unref(pkt);
    }

    return error;
}


AVStream *ask_to_select_stream(AVFormatContext *const file, uint8_t const which)
{
    static enum AVMediaType const dict[] = {AVMEDIA_TYPE_AUDIO};

    for (size_t i = 0; i < file->nb_streams; ++i)
        if (dict[which] == file->streams[i]->codecpar->codec_type)
            return file->streams[i];

    fprintf(stderr, "Error: No %s was found in file '%s'.\n",
                     av_get_media_type_string(dict[which]),
                     file->url);
    return NULL; 
}

int write_file_header(AVFormatContext *const file)
{
    int error = avformat_write_header(file, NULL);
    if (error < 0) {
        fprintf(stderr, "Error: could not write header to file '%s'.\n", file->url);
        return -1;
    }

    return 0;
}

AVFormatContext *open_file_for_write(char const *const filepath)
{
    AVFormatContext *file = NULL;
    int error = 0;

    file = avformat_alloc_context();
    if (NULL == file) goto error;

    error = avio_open(&(file->pb), filepath, AVIO_FLAG_WRITE);
    if (error < 0) goto error;

    file->url     = av_strdup(filepath);
    file->oformat = av_guess_format(NULL, filepath, NULL);
    if (NULL == file->url ||
        NULL == file->oformat) goto error;

    return file;

    error:
        if (NULL != file) {
            if (NULL != file->pb) avio_closep(&(file->pb));
            avformat_free_context(file);
        }

        fprintf(stderr, "Error: could not open file '%s'.\n", filepath);
        return NULL;
}

AVFormatContext *open_file_for_read(char const *const filepath)
{ 
    AVFormatContext *file = NULL;
    int error = 0;

    error = avformat_open_input(&file, filepath, NULL, NULL);
    if (error < 0) goto failure;

    error = avformat_find_stream_info(file, NULL);
    if (error < 0) goto failure;

    return file;

    failure:
        fprintf(stderr, "Error: Could not open file '%s'.\n", filepath);
        fprintf(stderr, "Reason: %s\n", av_err2str(error));

        if (NULL != file)
            avformat_close_input(&file);
        
        return NULL;
}

int main(int const argc, char const *const argv[])
{
    struct Audio in_audio  = {0};
    struct Audio out_audio = {0};

    SwrContext  *resampler = NULL;
    AVAudioFifo *samples_to_encode = NULL;

    int error = 0;
    
    if (NULL == (in_audio.pkt  = av_packet_alloc()) ||
        NULL == (in_audio.frm  = av_frame_alloc())  ||
        NULL == (out_audio.pkt = av_packet_alloc()) ||
        NULL == (out_audio.frm = av_frame_alloc())) {
        fprintf(stderr, "Error: Could not allocate packet or frame.\n");
        goto failure;
    }
    
    in_audio.file = open_file_for_read(argv[1]);
    if (NULL == in_audio.file) goto failure;

    in_audio.stream = ask_to_select_stream(in_audio.file, AUDIO);
    if (NULL == in_audio.stream) goto failure;

    in_audio.decoder = open_decoder(in_audio.stream->codecpar);  
    if (NULL == in_audio.decoder) goto failure;

    in_audio.decoder->pkt_timebase = in_audio.stream->time_base;
    /* Until this point the input audio is ready to be read and decoded. */ 

    out_audio.file = open_file_for_write(argv[2]);
    if (NULL == out_audio.file) goto failure;

    out_audio.stream = avformat_new_stream(out_audio.file, NULL);
    if (NULL == out_audio.stream) goto failure;

    out_audio.encoder = open_audio_encoder(out_audio.stream);
    if (NULL == out_audio.encoder) goto failure;

    error = write_file_header(out_audio.file);
    if (error < 0) goto failure;

    resampler = open_resampler(out_audio.encoder, 
                               in_audio.decoder);
    if (NULL == resampler) goto failure;

    samples_to_encode = alloc_samples_queue(out_audio.encoder->sample_fmt,
                                            out_audio.encoder->ch_layout.nb_channels,
                                            out_audio.encoder->frame_size);
    if (NULL == samples_to_encode) {
        fprintf(stderr, "Error: could not alloc queue.\n");
        goto failure;
    }

    do {
        size_t samples_encoded_cnt = 0;
        bool   has_some_file_been_fully_read = false;

        while (!has_some_file_been_fully_read) {
            error = readfile(in_audio.pkt, in_audio.stream, in_audio.file);

            if (error < 0) {
                if (AVERROR_EOF != error) goto failure;
                has_some_file_been_fully_read = true;
            }

            error = send_encoded(in_audio.decoder, has_some_file_been_fully_read
                                                   ? NULL 
                                                   : in_audio.pkt);
            if (error < 0) goto failure;
            
            while (0 == (error = receive_decoded(in_audio.decoder, in_audio.frm))) {
                struct ResamplerParams params = {
                    .channels     = in_audio.frm->extended_data,
                    .channels_cnt = in_audio.decoder->ch_layout.nb_channels,
                    .samples_cnt  = in_audio.frm->nb_samples,
                    .sample_fmt   = in_audio.decoder->sample_fmt
                };

                uint8_t **converted_samples = resample(resampler, params);
                if (NULL == converted_samples) goto failure;

                error = av_audio_fifo_write(samples_to_encode,
                                            (void**)(converted_samples),
                                            in_audio.frm->nb_samples);
                av_frame_unref(in_audio.frm);

                av_freep(converted_samples);
                av_freep(&converted_samples);

                if (error < 0) goto failure;
            }
            if (AVERROR(EAGAIN) != error &&
                AVERROR_EOF     != error) goto failure;

            while (av_audio_fifo_size(samples_to_encode) >= out_audio.encoder->frame_size ||
                  (has_some_file_been_fully_read && av_audio_fifo_size(samples_to_encode) > 0)) {
                size_t const samples_to_encode_cnt = FFMIN(av_audio_fifo_size(samples_to_encode),
                                                           out_audio.encoder->frame_size);

                out_audio.frm->nb_samples  = samples_to_encode_cnt;
                out_audio.frm->sample_rate = out_audio.encoder->sample_rate;
                out_audio.frm->format      = out_audio.encoder->sample_fmt;

                av_channel_layout_copy(&(out_audio.frm->ch_layout),
                                       &(out_audio.encoder->ch_layout));

                out_audio.frm->pts   = samples_encoded_cnt;
                samples_encoded_cnt += samples_to_encode_cnt;

                error = av_frame_get_buffer(out_audio.frm, 0);
                if (error < 0) goto failure;

                error = av_audio_fifo_read(samples_to_encode,
                                           (void**)(out_audio.frm->extended_data),
                                           samples_to_encode_cnt);
                if (error < 0) goto failure;

                error = send_decoded(out_audio.encoder, out_audio.frm);
                if (error < 0) goto failure;

                while (0 == (error = receive_encoded(out_audio.encoder, out_audio.pkt))) {
                    error = av_write_frame(out_audio.file, out_audio.pkt);                                  
                    if (error < 0) goto failure;

                    av_packet_unref(out_audio.pkt);
                }

                av_frame_unref(out_audio.frm);

                if (AVERROR(EAGAIN) != error &&
                    AVERROR_EOF     != error) goto failure;
            }

            if (has_some_file_been_fully_read) {
                error = avcodec_send_frame(out_audio.encoder, NULL);
                if (error < 0) goto failure;

                while (0 == (error = avcodec_receive_packet(out_audio.encoder, out_audio.pkt))) {
                    error = av_write_frame(out_audio.file, out_audio.pkt);                                  
                    if (error < 0) goto failure;

                    av_packet_unref(out_audio.pkt);
                }

                if (AVERROR(EAGAIN) != error &&
                    AVERROR_EOF     != error) goto failure;
            }

            av_packet_unref(in_audio.pkt);
        }
    } while (0);

    error = av_write_trailer(out_audio.file);
    if (error < 0) goto failure;

    error = 0;

    exit:
        in_audio.stream  = NULL;
        out_audio.stream = NULL;

        if (NULL != in_audio.pkt)               av_packet_free(&(in_audio.pkt));
        if (NULL != in_audio.frm)               av_frame_free(&(in_audio.frm));
        if (NULL != out_audio.pkt)              av_packet_free(&(out_audio.pkt));
        if (NULL != out_audio.frm)              av_frame_free(&(out_audio.frm));

        if (NULL != in_audio.file)              avformat_close_input(&(in_audio.file));
        if (NULL != in_audio.decoder)           avcodec_free_context(&(in_audio.decoder));

        if (NULL != out_audio.file) {
            avio_closep(&(out_audio.file->pb));
            avformat_free_context(out_audio.file);
            out_audio.file = NULL;
        }
        if (NULL != out_audio.encoder) avcodec_free_context(&(out_audio.encoder));

        if (NULL != resampler)                   swr_free(&resampler);
        if (NULL != samples_to_encode)           av_audio_fifo_free(samples_to_encode);

       return error; 

    failure:
        error = 1; 
        goto exit;
}
