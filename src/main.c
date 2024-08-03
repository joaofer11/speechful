#include <stdio.h>

int file_open_read(AVFormatContext **const file, char const *const filepath);

int main(int const argc, char const *const argv[])
{
    AVFormatContext * in_file = NULL;
    int error = 0;

    error = file_open_read(&in_file, argv[1]);
    if (error < 0) return 1;
    file_close_input:
        in_audio = NULL;
        avformat_close_input(&in_file);

    return error < 0 ? 1 : 0;
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
