#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
#include <libavutil/audio_fifo.h>

#define codec_supports(c, what) ((c)->capabilities & (what))

struct audio_encoder_settings {
	int                 channels;
	int                 sample_rate;
	int                 bit_rate;
	enum AVSampleFormat sample_fmt;
};

static void error(const char *msg, ...)
{
	va_list va;
	va_start(va, msg);
	av_vlog(NULL, AV_LOG_ERROR, msg, va);
	va_end(va);
}

static void warn(const char *msg, ...)
{
	va_list va;
	va_start(va, msg);
	av_vlog(NULL, AV_LOG_WARNING, msg, va);
	va_end(va);
}

static int format_open_input(struct AVFormatContext **fmt_ctx, const char *filepath)
{
	int ret;

	if ((ret = avformat_open_input(fmt_ctx, filepath, NULL, NULL)) < 0)
		return ret;
	if ((ret = avformat_find_stream_info(*fmt_ctx, NULL)) < 0)
		avformat_close_input(fmt_ctx);

	return ret;
}

static int codec_open_decoder(struct AVCodecContext **dec_ctx, struct AVCodecParameters *decpar)
{
	const struct AVCodec *dec;
	int ret;

	if (!(dec = avcodec_find_decoder(decpar->codec_id)))
		return AVERROR_DECODER_NOT_FOUND;

	if (!(*dec_ctx = avcodec_alloc_context3(dec)))
		return AVERROR(ENOMEM);

	if ((ret = avcodec_parameters_to_context(*dec_ctx, decpar)) < 0)
		goto err_free_dec_ctx;

	if ((ret = avcodec_open2(*dec_ctx, dec, NULL)) < 0)
		goto err_free_dec_ctx;

	return 0;

err_free_dec_ctx:
	avcodec_free_context(dec_ctx);

	return ret;
}

static int codec_open_audio_encoder(struct AVCodecContext **enc_ctx, enum AVCodecID id,
                                    struct audio_encoder_settings settings)
{
	const struct AVCodec *enc;
	int ret;

	if (!(enc = avcodec_find_encoder(id)))
		return AVERROR_ENCODER_NOT_FOUND;

	if (!(*enc_ctx = avcodec_alloc_context3(enc)))
		return AVERROR(ENOMEM);

	av_channel_layout_default(&(*enc_ctx)->ch_layout, settings.channels);
	(*enc_ctx)->sample_rate   = settings.sample_rate;
	(*enc_ctx)->bit_rate      = settings.bit_rate;
	(*enc_ctx)->sample_fmt    = settings.sample_fmt;
	(*enc_ctx)->time_base.num = 1;
	(*enc_ctx)->time_base.den = settings.sample_rate;

	if ((ret = avcodec_open2(*enc_ctx, enc, NULL)) < 0)
		avcodec_free_context(enc_ctx);

	return ret;
}

static int resampler_open(struct SwrContext          **resampler,
                          const struct AVCodecContext *enc,
                          const struct AVCodecContext *dec)
{
	int ret;

	if ((ret = swr_alloc_set_opts2(resampler,
	                               &enc->ch_layout,
	                                enc->sample_fmt,
	                                enc->sample_rate,
	                               &dec->ch_layout,
	                                dec->sample_fmt,
	                                dec->sample_rate,
	                               0, NULL)) < 0)
	        return ret;

	if ((ret = swr_init(*resampler)) < 0)
		swr_free(resampler);

	return ret;
}

static int filter_streams(struct AVStream ***dst, struct AVStream **src, int n,
                          enum AVMediaType which)
{
	int i, filtered;

	if (!(*dst = malloc(n * sizeof(struct AVStream *))))
		return -1; /* No memory. */

	for (i = filtered = 0; i < n; ++i) {
		if (which == src[i]->codecpar->codec_type)
			(*dst)[filtered++] = src[i];
	}

	if (!filtered) {
		free(*dst);
		*dst = NULL;
	}

	return filtered;
}

static void show_streams_info(struct AVStream **s, int n)
{
	int i;

	for (i = 0; i < n; ++i) {
		const struct AVDictionaryEntry
			*title = av_dict_get(s[i]->metadata, "title", NULL, 0),
			*lang  = av_dict_get(s[i]->metadata, "language", title, 0);

		printf("#%d %s stream: %s (%s)\n",
		       i + 1, av_get_media_type_string(s[i]->codecpar->codec_type),
		       title ? title->value : "Unknown",
		       lang  ? lang->value  : "Unknown");
	}
}

static int choose_stream(struct AVStream **streams, int nr_streams,
                         enum AVMediaType which)
{
	struct AVStream **filtered, *chosen = NULL;
	int            nr_filtered;

	if ((nr_filtered = filter_streams(&filtered, streams, nr_streams, which)) == -1)
		return AVERROR(ENOMEM);

	if (nr_filtered == 0)
		return AVERROR_STREAM_NOT_FOUND;

	if (nr_filtered == 1) {
		chosen = filtered[0];
		goto end;
	}

	show_streams_info(filtered, nr_filtered);

	while (!chosen) {
		int n;
		if (scanf("%d", &n) == 1
		    && n >= 1 && n <= nr_filtered) {
			chosen = filtered[n-1];
		} else {
			int c;
			warn("> Please, enter a number between [1] and [%d]: ", nr_filtered);
			do {
				/* Clear the stdin. */
				c = getc(stdin);
			} while (c != EOF && c != '\n');
		}
	}

end:
	free(filtered);
	return chosen->index;
}

int main(int argc, const char **argv)
{
	const char *in_audio_filepath = argv[1], *out_audio_filepath = argv[2];
	struct AVFormatContext *in_audio_fmt_ctx, *out_audio_fmt_ctx;
	struct AVStream *in_audio_st, *out_audio_st;
	struct AVCodecContext *audio_dec, *audio_enc;
	struct SwrContext *resampler;
	struct AVAudioFifo *resampled_queue;
	int ret;

	in_audio_fmt_ctx = NULL;
	if ((ret = format_open_input(&in_audio_fmt_ctx, in_audio_filepath)) < 0) {
		error("%s: failed to open media file: %s\n",
		      in_audio_filepath, av_err2str(ret));
		goto end;
	}

	if ((ret = choose_stream(in_audio_fmt_ctx->streams, in_audio_fmt_ctx->nb_streams,
	                         AVMEDIA_TYPE_AUDIO)) < 0) {
	        if (ret == AVERROR_STREAM_NOT_FOUND)
	        	error("%s: no audio streams found.\n", in_audio_filepath);
		else
			error("%s: failed to choose audio stream: %s\n", in_audio_filepath, av_err2str(ret));
		goto end;
	}

	in_audio_st = in_audio_fmt_ctx->streams[ret];

	audio_dec = NULL;
	if ((ret = codec_open_decoder(&audio_dec, in_audio_st->codecpar)) < 0) {
		error("%s: failed to open decoder: %s\n",
		      avcodec_get_name(in_audio_st->codecpar->codec_id),
		      av_err2str(ret));
	}

	/*
	 * At this point, we can start setting up the output MP3 media file.
	 * This is the file where the extracted speech will be into.
	 *
	 * Before we set up the container format, we first need to set up the encoder (codec).
	 * This is necessary so we can properly record some codec settings of
	 * the audio track that will be embedded into the container format.
	 */
	audio_enc = NULL;
	ret = codec_open_audio_encoder(
		&audio_enc,
		AV_CODEC_ID_MP3,
	        (struct audio_encoder_settings){
			.channels    = 2,
			.sample_rate = 48000,
			.bit_rate    = 256000,
			.sample_fmt  = AV_SAMPLE_FMT_S16P
	        });
	if (ret < 0) {
		error("%s: failed to open encoder: %s\n",
		      avcodec_get_name(AV_CODEC_ID_MP3), av_err2str(ret));
		goto end;
	}

	out_audio_fmt_ctx = NULL;
	if ((ret = avformat_alloc_output_context2(&out_audio_fmt_ctx, NULL, NULL, out_audio_filepath)) < 0) {
		error("%s: failed to open media file: %s\n", out_audio_filepath, av_err2str(ret));
		goto end;
	}

	if (!(out_audio_fmt_ctx->oformat->flags & AVFMT_NOFILE)
	    && (ret = avio_open(&out_audio_fmt_ctx->pb, out_audio_filepath, AVIO_FLAG_WRITE)) < 0) {
		error("%s: failed to open media file: %s\n", out_audio_filepath, av_err2str(ret));
		goto end;
	}

	/* Embed the audio track. */
	if (!(out_audio_st = avformat_new_stream(out_audio_fmt_ctx, NULL))) {
		error("%s: failed to attach audio track: out of memory.\n", out_audio_filepath);
		goto end;
	}

	/* Save the audio track codec settings. */
	if ((ret = avcodec_parameters_from_context(out_audio_st->codecpar, audio_enc)) < 0) {
		error("%s: failed to record encoder settings: %s\n",
		       avcodec_get_name(audio_enc->codec->id), av_err2str(ret));
		goto end;
	}
	out_audio_st->time_base.num = 1;
	out_audio_st->time_base.den = audio_enc->sample_rate;

	if ((ret = avformat_write_header(out_audio_fmt_ctx, NULL)) < 0) {
		error("%s: failed to open media file: %s\n", out_audio_filepath, av_err2str(ret));
		goto end;
	}

	/* The output MP3 media file is all set. */

	resampler = NULL;
	if ((ret = resampler_open(&resampler, audio_enc, audio_dec)) < 0) {
		error("Failed to initialize audio resampler: %s\n", av_err2str(ret));
		goto end;
	}

	/*
	 * I don't know if the MP3 encoder (codec) accepts variable frame sizes,
	 * but if it doesn't, we need a queue to keep track of the desired frame size.
	 */
	if (!codec_supports(audio_enc->codec, AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
	    && !(resampled_queue = av_audio_fifo_alloc(audio_enc->sample_fmt,
	    	                                      audio_enc->ch_layout.nb_channels,
	    	                                      1))) {
		error("Failed to alloc queue: out of memory.\n");
		goto end;
	}

end:
	if (in_audio_fmt_ctx)
		avformat_close_input(&in_audio_fmt_ctx);

	if (out_audio_fmt_ctx) {
		if (out_audio_fmt_ctx->pb)
			avio_closep(&out_audio_fmt_ctx->pb);
		avformat_free_context(out_audio_fmt_ctx);
	}

	if (audio_dec)
		avcodec_free_context(&audio_dec);

	if (audio_enc)
		avcodec_free_context(&audio_enc);

	if (resampler)
		swr_free(&resampler);

	if (resampled_queue)
		av_audio_fifo_free(resampled_queue);

	return 0;
}
