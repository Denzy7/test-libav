#include <stdio.h>

#include <time.h>

#include <libavutil/timestamp.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <pulse/simple.h>

FILE*  x;
pa_simple* simple = NULL; 
static const int play_sample_rate = 44100;
static const int max_decode_samples = 1024;
static int max_resample_samples;
static int resampled_samples;
int resample(
        SwrContext* swr,
        AVFrame* frame,
        uint8_t** dst, int* dst_linesz, AVChannelLayout* dst_chlyt,  int dst_rate,  enum AVSampleFormat dst_fmt
        )
{
    int ret;
    if(resampled_samples == 0)
        resampled_samples = max_resample_samples;

    if(!swr_is_initialized(swr))
    {
        av_opt_set_chlayout(swr, "in_chlayout",    &frame->ch_layout, 0);
        av_opt_set_int(swr, "in_sample_rate",       frame->sample_rate, 0);
        av_opt_set_sample_fmt(swr, "in_sample_fmt", frame->format, 0);

        av_opt_set_chlayout(swr, "out_chlayout",    dst_chlyt, 0);
        av_opt_set_int(swr, "out_sample_rate",       play_sample_rate, 0);
        av_opt_set_sample_fmt(swr, "out_sample_fmt", dst_fmt, 0);
        

        if(swr_init(swr) < 0)
        {
            printf("cannot init swr\n");
            return 0;
        }
    }

    /* compute destination number of samples */
    resampled_samples = av_rescale_rnd(swr_get_delay(swr, frame->sample_rate) +
            frame->nb_samples, dst_rate, frame->sample_rate, AV_ROUND_UP);
    if(resampled_samples > max_resample_samples)
    {
        av_freep(&dst[0]);
        ret = av_samples_alloc(dst, dst_linesz, dst_chlyt->nb_channels,
                                   resampled_samples, dst_fmt, 1);
        if(ret < 0)
        {
            perror("av_samples_alloc");
            return 0;
        }
        max_resample_samples = resampled_samples;
    }

    ret = swr_convert(swr, dst, resampled_samples, (const uint8_t**)frame->extended_data, frame->nb_samples);
    if(ret < 0)
    {
        printf("conversion failed\n");
        return 0;
    }

    return ret;
}
int decode(AVCodecContext *ctx_codec, AVPacket *packet, AVFrame *frame)
{
    int res;
    int data_size;
    res = avcodec_send_packet(ctx_codec, packet);
    if(res < 0)
    {
        printf("error sending packet %s\n", av_err2str(res));
        return 0;
    }

    while(res >= 0)
    {
        res = avcodec_receive_frame(ctx_codec, frame);
        if (res == AVERROR(EAGAIN))
        {
            printf("retrying...\n");
            return 0;
        }else if(res == AVERROR_EOF)
        {
            printf("EOF\n");
            return 0;
        }
        else if(res < 0)
        {
            printf("decode failed!\n");
            return 0;
        }

        data_size = av_get_bytes_per_sample(ctx_codec->sample_fmt);
        if(data_size < 0)
        {
            printf("fail to calc datasize\n");
            return 1;
        }


        /*printf("frame %lu ch:%d, rate:%d, fmt:%d, samples:%d\n", frame->pts, frame->ch_layout.nb_channels, frame->sample_rate, frame->format, frame->nb_samples);*/

        return 1;
    }
    return 1;
}


int main(int argc, char* argv[])
{
    int res;
    AVStream* stream = NULL;
    AVFrame* frame = NULL;

    AVFormatContext* ctx_format = NULL;
    const AVCodec* codec;

    SwrContext* swr;

    pa_sample_spec ss;
    AVChannelLayout dst_chlyt = AV_CHANNEL_LAYOUT_STEREO;
    enum AVSampleFormat dst_fmt = AV_SAMPLE_FMT_S16;

    uint8_t** conv_data; int conv_linesize;

    if(argc < 2)
    {
        printf("usage: ./audio audio_file\n");
        return 1;
    }

    /* must be same as dst_fmt */
    ss.format = PA_SAMPLE_S16LE;
    ss.channels = dst_chlyt.nb_channels;
    ss.rate = play_sample_rate;

    if(avformat_open_input(&ctx_format, argv[1], NULL, NULL) != 0)
    {
        printf("unable to open file\n");
        return -1;
    }

    if(avformat_find_stream_info(ctx_format, NULL) < 0)
    {
        printf("could not find stream info\n");
        return -1;
    }
    
    for(int i = 0; i < ctx_format->nb_streams; i++)
    {
        if(ctx_format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            stream = ctx_format->streams[i];
            break;
        }
    }
    if(stream == NULL)
    {
        printf("could not find an audio stream\n");
        return 1;
    }
    codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if(!codec)
    {
        printf("unsupported codec!\n");
        return -1;
    }

    AVCodecContext* ctx_codec = avcodec_alloc_context3(codec);
    if(!ctx_codec)
    {
        printf("cannot alloc ctx_codec\n");
        return -1;
    }

    if(avcodec_parameters_to_context(ctx_codec, stream->codecpar) < 0)
    {
        printf("failed to copy codec params\n");
        return -1;
    }

    max_resample_samples = resampled_samples = av_rescale_rnd(max_decode_samples, play_sample_rate, stream->codecpar->sample_rate,  AV_ROUND_UP);
    res = av_samples_alloc_array_and_samples(&conv_data, &conv_linesize, dst_chlyt.nb_channels,
            max_resample_samples, dst_fmt, 0);
    if(res < 0)
    {
        perror("av_samples_alloc_array_and_samples");
        return 1;
    }


    if(avcodec_open2(ctx_codec, codec, NULL) < 0)
    {
        printf("failed to open codec\n");
        return 1;
    }
    AVPacket* packet = av_packet_alloc();
    if(!packet)
    {
        printf("failed to alloc packet\n");
        return -1;
    }
    swr = swr_alloc();
    if(swr == NULL)
    {
        perror("swr_alloc");
        return 1;
    }

    simple    = pa_simple_new(NULL,
            "LibavDecode",
            PA_STREAM_PLAYBACK,
            NULL,
            "NowPlaying",
            &ss,
            NULL,
            NULL,
            NULL);
    if(!simple)
    {
        printf("cannot connect to pulseaudio\n");
        return -1;
    }
            
    printf("codec : %s, bitrate : %lu, samplerate:%d, duration : %lus\n",
            codec->name,
            stream->codecpar->bit_rate,
            ctx_codec->sample_rate,
            ctx_format->duration/AV_TIME_BASE
            );

    while(av_read_frame(ctx_format, packet) >= 0)
    {
        if(frame == NULL)
        {
            if(!(frame = av_frame_alloc()))
            {
                printf("couldnt alloc frame\n");
                return 1;
            }
        }

        if(packet->size && packet->stream_index == stream->index && decode(ctx_codec, packet, frame))
        {
            res = resample(swr, frame, conv_data, &conv_linesize, &dst_chlyt, play_sample_rate, dst_fmt);
            if(res < 0)
            {
                printf("failed resample\n");
            }else {
                /*printf("resampled %d\n", res);*/
                pa_simple_write(simple, conv_data[0],
                        av_samples_get_buffer_size(&conv_linesize, dst_chlyt.nb_channels, res, dst_fmt, 1),
                        NULL);
            }
        }
        av_frame_unref(frame);
        av_packet_unref(packet);
    }
    packet->data = NULL;
    packet->size = 0;
    decode(ctx_codec, packet, frame);

    pa_simple_free(simple);
    av_freep(&conv_data[0]);
    av_freep(&conv_data);
    av_packet_free(&packet);
    av_frame_free(&frame);
    swr_free(&swr);
    avcodec_free_context(&ctx_codec);
    avformat_close_input(&ctx_format);

    return 0;
}
