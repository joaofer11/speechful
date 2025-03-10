#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <assert.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
#include <libavutil/audio_fifo.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define codec_supports(c, what) ((c)->capabilities & (what))

typedef uint8_t u8;
typedef int64_t i64;

struct range {
	i64 start;
	i64 end;
};

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

static int extract_audio_region(u8 ***dst, const u8 *const *src, int samples,
                                int channels, enum AVSampleFormat sample_fmt,
                                struct range length, struct range region)
{
	int skip, extract;
	int ret;

	assert(region.start >= length.start && region.end <= length.end);
	assert(length.end - length.start > 0);
	assert(region.end - region.start > 0);

	{
		i64 whole = length.end - length.start;
		double fraction;

		/* The offset of the region mark from the beginning. */
		fraction = (double)(region.start - length.start) / whole;
		skip = fraction * samples;

		fraction = (double)(region.end - region.start) / whole;
		extract = fraction * samples;
	}

	if ((ret = av_samples_alloc_array_and_samples(dst, NULL, channels, extract, sample_fmt, 0)) < 0)
		return ret;

	if ((ret = av_samples_copy(*dst, (u8 *const *)src, 0, skip, extract, channels, sample_fmt)) < 0) {
		av_freep(*dst);
		av_freep(dst);
		return ret;
	}

	return extract;
}

static inline i64 tb2ms(struct AVRational timebase, i64 n)
{
	i64 ret = av_rescale_q(n, timebase, (struct AVRational){1, 1000});
	return ret;
}

static inline i64 ms2tb(struct AVRational timebase, i64 ms)
{
	i64 ret = av_rescale_q(ms, (struct AVRational){1, 1000}, timebase);
	return ret;
}

static struct range get_overlapped_region(struct range a, struct range b)
{
	struct range ret;

	assert(!(a.end <= b.start) && !(a.start >= b.end));

	ret.start = (a.start - b.start < 0) ? b.start : a.start;
	ret.end   = (a.end   - b.end   < 0) ? a.end   : b.end;

	return ret;
}

static int prepare_audio_frame_for_encoding(struct AVFrame* frame, int samples,
                                            const struct AVCodecContext *enc)
{
	int ret;

	if ((ret = av_channel_layout_copy(&frame->ch_layout, &enc->ch_layout)) < 0)
		return ret;

	frame->format      = enc->sample_fmt;
	frame->sample_rate = enc->sample_rate;
	frame->time_base   = enc->time_base;
	frame->nb_samples  = samples;

	if ((ret = av_frame_get_buffer(frame, 0)) < 0)
		av_frame_unref(frame);

	return ret;
}

/* TODO: Add the optional options for the underlying FFmpeg. */
static int format_open_input(struct AVFormatContext **fmt_ctx, const char *filepath)
{
	int ret;

	if ((ret = avformat_open_input(fmt_ctx, filepath, NULL, NULL)) < 0)
		return ret;
	if ((ret = avformat_find_stream_info(*fmt_ctx, NULL)) < 0)
		avformat_close_input(fmt_ctx);

	return ret;
}

static int format_write_audio_data(struct AVFormatContext *fmt,
                                   struct AVCodecContext *enc,
                                   struct AVAudioFifo *queue,
                                   const u8 *const *buf, int samples,
                                   i64 *next_pts)
{
	struct AVPacket *pkt;
	struct AVFrame *frame;
	bool eof_received = !buf || !samples;
	int ret = 0;

	if (!(pkt = av_packet_alloc()))
		return AVERROR(ENOMEM);

	if (!(frame = av_frame_alloc())) {
		av_packet_free(&pkt);
		return AVERROR(ENOMEM);
	}

	if (buf) {
		if (queue) {
			if ((ret = av_audio_fifo_write(queue, (void *const *)buf, samples)) < 0)
				goto end;
		}
	}

	/* TODO: Handle encoders that doesn't accept variable frame sizes. */
	for (;;) {
		if (queue) {
			int dequeued = MIN(av_audio_fifo_size(queue), enc->frame_size);

			if (!dequeued) {
				if (!eof_received) {
					ret = AVERROR(EAGAIN);
					goto end;
				}

				if ((ret = avcodec_send_frame(enc, NULL)) < 0)
					goto end;
			} else {
				if (dequeued < enc->frame_size && !eof_received)
					break;

				if ((ret = prepare_audio_frame_for_encoding(frame, dequeued, enc)) < 0)
					goto end;

				/* WARNING: This is wrong! Use `av_rescale_q()`. */
				frame->pts = *next_pts;
				*next_pts += dequeued;

				if ((ret = av_audio_fifo_read(queue, (void *const *)frame->extended_data, dequeued)) < 0)
					goto end;

				if ((ret = avcodec_send_frame(enc, frame)) < 0)
					goto end;

				av_frame_unref(frame);
			}
		}

		while ((ret = avcodec_receive_packet(enc, pkt)) == 0) {
			ret = av_write_frame(fmt, pkt);
			av_packet_unref(pkt);
			if (ret < 0)
				goto end;
		}

		if (ret == AVERROR_EOF) {
			ret = av_write_trailer(fmt);
			break;
		}

		if (ret != AVERROR(EAGAIN))
			break;
	}

end:
	av_packet_free(&pkt);
	av_frame_free(&frame);
	return ret;
}

static int read_packet(struct AVFormatContext *fmt_ctx, int stream_idx,
                       struct AVPacket *pkt)
{
	int ret;

	while ((ret = av_read_frame(fmt_ctx, pkt)) == 0) {
		if (pkt->stream_index == stream_idx)
			break;
		av_packet_unref(pkt);
	}

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

static int resample(struct SwrContext *resampler, u8 ***dst, const u8 *const *src,
                    int samples, int dst_channels, enum AVSampleFormat dst_sample_fmt)
{
	int ret;

	if ((ret = av_samples_alloc_array_and_samples(dst, NULL, dst_channels, samples, dst_sample_fmt, 0)) < 0)
		return ret;

	if ((ret = swr_convert(resampler, *dst, samples, src, samples)) < 0) {
		av_freep(*dst);
		av_freep(dst);
	}

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

	printf("> Choose the %s stream you wish: ", av_get_media_type_string(which));
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
	/* TODO?: Parse the argv. Some arguments will be important. */
	const char *in_audio_filepath = argv[1], *out_audio_filepath = argv[2], *sub_filepath = argv[3];
	struct AVFormatContext *sub_fmt_ctx = NULL, *in_audio_fmt_ctx = NULL, *out_audio_fmt_ctx = NULL;
	struct AVStream *sub_st = NULL, *in_audio_st = NULL, *out_audio_st = NULL;
	struct AVCodecContext *audio_dec = NULL, *audio_enc = NULL;
	struct SwrContext *resampler = NULL;
	struct AVAudioFifo *resampled_queue = NULL;
	struct AVPacket *pkt = NULL;
	struct AVFrame *frame = NULL;
	i64 prev_sub_ended_at = 0;
	i64 next_audio_pts = 0;
	int ret;

	if ((ret = format_open_input(&in_audio_fmt_ctx, in_audio_filepath)) < 0) {
		error("%s: failed to open media file: %s\n", in_audio_filepath, av_err2str(ret));
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

	if (argc - 1 >= 3) {
		/* We got a subtitle file. */
		if ((ret = format_open_input(&sub_fmt_ctx, sub_filepath)) < 0) {
			error("%s: failed to open media file: %s\n", sub_filepath, av_err2str(ret));
			goto end;
		}

		if (sub_fmt_ctx->nb_streams != 1) {
			error("%s: inavalid subtitle media file.\n");
			error("Expected only one stream but got %d.\n", sub_fmt_ctx->nb_streams);
			goto end;
		}

		if (sub_fmt_ctx->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
			error("%s: inavalid subtitle media file.\n");
			error("Found only one stream of type %s\n",
			      av_get_media_type_string(sub_fmt_ctx->streams[0]->codecpar->codec_type));
			goto end;
		}

		sub_st = sub_fmt_ctx->streams[0];
	} else {
		warn("No subtitle file was provided.\n");
		warn("Using file '%s' instead.\n", in_audio_filepath);

		if ((ret = choose_stream(in_audio_fmt_ctx->streams, in_audio_fmt_ctx->nb_streams,
		                         AVMEDIA_TYPE_SUBTITLE)) < 0) {
			if (ret == AVERROR_STREAM_NOT_FOUND)
				error("%s: no subtitle streams found.\n", in_audio_filepath);
			else
				error("%s: failed to choose subtitle stream: %s\n", in_audio_filepath, av_err2str(ret));
			goto end;
		}

		sub_st = in_audio_fmt_ctx->streams[ret];

		/*
		 * Processing audio and subtitle from the same container sucks.
		 * For that reason, we will always have a different context for the subtitle,
		 * even if it was found inside the same container as the input audio.
		 */
		if ((ret = format_open_input(&sub_fmt_ctx, in_audio_filepath)) < 0) {
			error("%s: failed to open media file: %s\n", in_audio_filepath, av_err2str(ret));
			goto end;
		}

		sub_filepath = in_audio_filepath;
	}

	if ((ret = codec_open_decoder(&audio_dec, in_audio_st->codecpar)) < 0) {
		error("%s: failed to open decoder: %s\n",
		      avcodec_get_name(in_audio_st->codecpar->codec_id),
		      av_err2str(ret));
	}

	/* TODO: Let the user choose the quality of audio: low, medium or high. */
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

	if (!(out_audio_st = avformat_new_stream(out_audio_fmt_ctx, NULL))) {
		error("%s: failed to attach audio track: out of memory.\n", out_audio_filepath);
		goto end;
	}

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

	if ((ret = resampler_open(&resampler, audio_enc, audio_dec)) < 0) {
		error("Failed to initialize audio resampler: %s\n", av_err2str(ret));
		goto end;
	}

	/*
	 * Some encoders doesn't accept variable frame sizes, in such case it
	 * is pretty convenient to use a queue in order to keep track of desired encoder
	 * frame size.
	 */
	if (!codec_supports(audio_enc->codec, AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
	    && !(resampled_queue = av_audio_fifo_alloc(audio_enc->sample_fmt,
	    	                                      audio_enc->ch_layout.nb_channels,
	    	                                      1))) {
		error("Failed to alloc queue: out of memory.\n");
		goto end;
	}

	if (!(pkt = av_packet_alloc()) || !(frame = av_frame_alloc())) {
		error("Failed to alloc packet or frame: out of memory.\n");
		goto end;
	}

	/* NOTE: It seems that this code is leaking the memory somewhere. */
	while ((ret = read_packet(sub_fmt_ctx, sub_st->index, pkt)) == 0) {
		struct range sub_time_in_ms = {0};

		sub_time_in_ms.start = tb2ms(sub_st->time_base, pkt->pts) - 1000;
		sub_time_in_ms.end   = tb2ms(sub_st->time_base, pkt->pts + pkt->duration) + 1000;

		av_packet_unref(pkt);

		if (sub_time_in_ms.start < prev_sub_ended_at)
			sub_time_in_ms.start = prev_sub_ended_at;

		prev_sub_ended_at = sub_time_in_ms.end;

		if ((ret = av_seek_frame(
		               in_audio_fmt_ctx,
		               in_audio_st->index,
		               ms2tb(in_audio_st->time_base, sub_time_in_ms.start),
		               AVSEEK_FLAG_BACKWARD)) < 0) {
			error("Failed to sync audio with subtitle: %s\n", av_err2str(ret));
			goto end;
		}

		while ((ret = read_packet(in_audio_fmt_ctx, in_audio_st->index, pkt)) == 0) {
			struct range audio_time_in_ms = {0};

			audio_time_in_ms.start = tb2ms(in_audio_st->time_base, pkt->pts);
			audio_time_in_ms.end   = tb2ms(in_audio_st->time_base, pkt->pts + pkt->duration);

			if (audio_time_in_ms.end <= sub_time_in_ms.start) {
				av_packet_unref(pkt);
				continue;
			}

			if (audio_time_in_ms.start >= sub_time_in_ms.end) {
				av_packet_unref(pkt);
				break;
			}

			if ((ret = avcodec_send_packet(audio_dec, pkt)) < 0) {
				error("Failed to decode audio data: %s\n", av_err2str(ret));
				goto end;
			}

			av_packet_unref(pkt);

			while ((ret = avcodec_receive_frame(audio_dec, frame)) == 0) {
				struct range region;
				u8         **speech_buf, **resampled_buf;
				int          speech_samples;

				audio_time_in_ms.start = tb2ms(in_audio_st->time_base, frame->pts);
				audio_time_in_ms.end   = tb2ms(in_audio_st->time_base, frame->pts + frame->duration);

				if (audio_time_in_ms.end <= sub_time_in_ms.start) {
					av_frame_unref(frame);
					continue;
				}

				if (audio_time_in_ms.start >= sub_time_in_ms.end) {
					av_frame_unref(frame);
					avcodec_flush_buffers(audio_dec);
					break;
				}

				region = get_overlapped_region(audio_time_in_ms, sub_time_in_ms);

				ret = speech_samples =
					extract_audio_region(&speech_buf, (const u8 *const *)frame->extended_data,
					                     frame->nb_samples, audio_dec->ch_layout.nb_channels,
					                     audio_dec->sample_fmt, audio_time_in_ms, region);

				av_frame_unref(frame);

				if (ret < 0) {
					error("Failed to extract audio region: %s\n", av_err2str(ret));
					goto end;
				}

				ret = speech_samples =
					resample(resampler, &resampled_buf, (const u8 *const *)speech_buf,
					         speech_samples, audio_enc->ch_layout.nb_channels, audio_enc->sample_fmt);

				av_freep(speech_buf);
				av_freep(&speech_buf);

				if (ret < 0) {
					error("Failed to resample audio samples: %s\n", av_err2str(ret));
					goto end;
				}

				ret = format_write_audio_data(out_audio_fmt_ctx, audio_enc, resampled_queue,
				                               (const u8 *const *)resampled_buf, speech_samples,
				                               &next_audio_pts);

				av_freep(resampled_buf);
				av_freep(&resampled_buf);

				if (ret < 0 && ret != AVERROR(EAGAIN)) {
					error("%s: failed to write audio data: %s\n", out_audio_filepath, av_err2str(ret));
					goto end;
				}
			}

			if (ret < 0 && ret != AVERROR(EAGAIN)) {
				error("Failed to decode audio data: %s\n", av_err2str(ret));
				goto end;
			}
		}

		if (ret < 0) {
			if (ret == AVERROR_EOF)
				break;
			error("%s: failed to read audio data: %s\n", in_audio_filepath, av_err2str(ret));
			goto end;
		}
	}

	if (ret != AVERROR_EOF) {
		error("%s: failed to read subtitle data: %s\n", sub_filepath, av_err2str(ret));
		goto end;
	}

	if ((ret = avcodec_send_packet(audio_dec, NULL)) < 0) {
		error("Failed to flush audio decoder: %s\n", av_err2str(ret));
		goto end;
	}

	while ((ret = avcodec_receive_frame(audio_dec, frame)) == 0) {
		u8 **resampled_buf;
		int  resampled_samples;

		ret = resampled_samples =
			resample(resampler, &resampled_buf, (const u8 *const *)frame->extended_data,
			         frame->nb_samples, audio_enc->ch_layout.nb_channels, audio_enc->sample_fmt);

		av_frame_unref(frame);

		if (ret < 0) {
			error("Failed to resample audio samples: %s\n", av_err2str(ret));
			goto end;
		}

		ret = format_write_audio_data(out_audio_fmt_ctx, audio_enc, resampled_queue,
		            (const u8 *const *)resampled_buf, resampled_samples, &next_audio_pts);

		av_freep(resampled_buf);
		av_freep(&resampled_buf);

		if (ret < 0 && ret != AVERROR(EAGAIN)) {
			error("%s: failed to write audio data: %s\n", out_audio_filepath, av_err2str(ret));
			goto end;
		}
	}

	if (ret != AVERROR_EOF) {
		error("Failed to flush audio decoder: %s\n", av_err2str(ret));
		goto end;
	}

	/* Flush the encoder and the container format. */
	if ((ret = format_write_audio_data(out_audio_fmt_ctx, audio_enc, resampled_queue, NULL, 0,
	                                    &next_audio_pts)) < 0) {
	        error("%s: failed to write audio data: %s\n", out_audio_filepath, av_err2str(ret));
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

	if (sub_fmt_ctx)
		avformat_close_input(&sub_fmt_ctx);

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
