#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>

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
	const char *in_audio_filepath = argv[1];
	struct AVFormatContext *in_audio_fmt_ctx;
	struct AVStream *in_audio_stream;
	struct AVCodecContext *audio_dec;
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

	in_audio_stream = in_audio_fmt_ctx->streams[ret];

	audio_dec = NULL;
	if ((ret = codec_open_decoder(&audio_dec, in_audio_stream->codecpar)) < 0) {
		error("%s: failed to open decoder: %s\n",
		      avcodec_get_name(in_audio_stream->codecpar->codec_id),
		      av_err2str(ret));
	}

end:
	if (in_audio_fmt_ctx)
		avformat_close_input(&in_audio_fmt_ctx);

	if (audio_dec)
		avcodec_free_context(&audio_dec);

	return 0;
}
