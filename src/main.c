#include <libavformat/avformat.h>
#include <stddef.h>
#include <stdio.h>

#include "file.h"

int main(int const argc, char const *const argv[])
{
    AVFormatContext * in_file_audio_ctx = NULL;

    int error = 0;

    error = file_open_read_context(&in_file_audio_ctx, argv[1]);
    if (error < 0) return -1;
    
    printf("%s\n", in_file_audio_ctx->iformat->name);

    file_close_input_audio_ctx:
        avformat_close_input(&in_file_audio_ctx);

    return 0;
}
