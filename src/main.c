#include <stdio.h>
#include <libavcodec/packet.h>
#include <libavutil/frame.h>

int frame_create(AVFrame **const frame);
int packet_create(AVPacket **const packet);
int file_open_read(AVFormatContext **const file, char const *const filepath);

int main(int const argc, char const *const argv[])
{
    AVFormatContext * in_file = NULL;
    AVPacket        * in_pkt  = NULL;
    AVFrame         * in_frm  = NULL;


    int error = 0;

    error = file_open_read(&in_file, argv[1]);
    if (error < 0) return 1;

    error = packet_create(&in_pkt);
    if (error < 0) goto file_close_input;

    error = frame_create(&in_frm);
    if (error < 0) goto packet_destroy;


    frame_destroy:
        av_frame_free(&in_frm);

    packet_destroy:
        av_packet_free(&in_pkt);

    file_close_input:
        in_audio = NULL;
        avformat_close_input(&in_file);

    return error < 0 ? 1 : 0;
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
