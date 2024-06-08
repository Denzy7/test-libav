#include <stdio.h>
#include <stdint.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include <libavutil/imgutils.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#define arysz(ary) sizeof(ary)/sizeof(ary[0])
#if _NDEBUG
#define glerr _glerr(__LINE__, __FILE__)
#else
#define glerr
#endif
#define EDGEBORDERPAD 0.975f


void fbszcb(GLFWwindow* window, int width, int height)
{
    printf("resize to %dx%d\n", width, height);
    glViewport(0, 0, width, height);
}

void _glerr(int ln, const char* file)
{
    GLenum e = glGetError();
    if(e)
    {
        printf("glerr: code=%d, ln:%d, file:%s\n", e, ln, file);
        exit(1);
    }
}

int compile_and_attach(GLuint* ref, const char* src, GLenum type, GLuint program)
{
    GLint i;
    GLuint shader;
    char* str;

    shader = glCreateShader(type); glerr;
    glShaderSource(shader, 1, &src, NULL); glerr;
    glCompileShader(shader); glerr;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &i); glerr;
    if(i)
    {
        str = malloc(i);
        glGetShaderInfoLog(shader, i, NULL, str); glerr;
        printf("compiler error: %s\n", str);
        free(str);
        return 0;
    }
    glAttachShader(program, shader); glerr;
    if(ref != NULL)
        *ref = shader;
    return 1;
}

void averr(int res)
{
    printf("averr: %s\n", av_err2str(res));
}

int main(int argc, char* argv[])
{
    int i;
    const char* cstr;
    char* str;

    GLFWwindow* window;
    int w,h;
    const float windowscalefactor = EDGEBORDERPAD;

    int res;
    AVStream* stream = NULL;
    AVFrame* frame = NULL;
    AVFrame* muxframe = NULL;
    AVPacket* packet = NULL;

    AVFormatContext* input = NULL;
    const AVCodec* codec;
    AVCodecContext* codec_ctx;

    GLuint img, vert, frag, program;
    GLuint vbo_ibo[2];
    static const char* shadersrc_vert = 
        "#version 100\n"
        "attribute vec2 aPos;\n"
        "attribute vec2 aTexCoord;\n"
        "varying vec2 TexCoord;\n"
        "void main() {\n"
            "gl_Position = vec4(aPos, 0.0, 1.0);\n"
            "TexCoord = aTexCoord;\n"
        "}\n"
        "";
    static const char* shadersrc_frag = 
        "#version 100\n"
        "precision mediump float;\n"
        "varying vec2 TexCoord;\n"
        "uniform sampler2D img;\n"
        "void main() {\n"
            "gl_FragColor = vec4(texture2D(img, TexCoord).rgb, 1.0);\n"
        "}\n"
        "";

    struct SwsContext* sws;
    uint8_t* conv_data[4];
    int conv_linesz[4];
    int imgbufsz;

    static const float vertices[]= 
    {
        /* aPos                         aTexCoord (inverted coz GL) */
        -EDGEBORDERPAD, -EDGEBORDERPAD, 0.0f, 1.0f,
        -EDGEBORDERPAD, EDGEBORDERPAD, 0.0f, 0.0f,
        EDGEBORDERPAD, EDGEBORDERPAD, 1.0f, 0.0f,
        EDGEBORDERPAD, -EDGEBORDERPAD, 1.0f, 1.0f,
    };
    static const uint16_t indices[] = 
    {
        0, 3, 2, 2, 1, 0,
    };

    if(argc < 2)
    {
        printf("usage: ./video video_file\n");
        return 1;
    }

    if((res = avformat_open_input(&input, argv[1], NULL, NULL)) < 0)
    {
        averr(res);
        return 1;
    }

    if((res = avformat_find_stream_info(input, NULL)) < 0)
    {
        averr(res);
        return 1;
    }

    for(i = 0; i < input->nb_streams; i++)
    {
        if(input->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            stream = input->streams[i];
            break;
        }
    }

    if(stream == NULL)
    {
        printf("couldn't find a video stream\n");
        return 1;
    }

    if((codec = avcodec_find_decoder(stream->codecpar->codec_id)) == NULL)
    {
        printf("can't find decoder\n");
        return 1;
    }

    if((codec_ctx = avcodec_alloc_context3(codec)) == NULL)
    {
        printf("cant alloc context\n");
        return -1;
    }

    if((res = avcodec_parameters_to_context(codec_ctx, stream->codecpar)) < 0)
    {
        averr(res);
        return 1;
    }

    if((res = avcodec_open2(codec_ctx, codec, NULL)) < 0)
    {
        averr(res);
        return 1;
    }

    if((packet = av_packet_alloc()) == NULL || (frame = av_frame_alloc()) == NULL)
    {
        printf("cant alloc frame or packet\n");
        return 1;
    }

    
    if(!glfwInit())
    {
        glfwGetError(&cstr);
        printf("glfw err: %s\n", cstr);
        return 1;
    }

    /* limit to window size */
    glfwGetMonitorWorkarea(glfwGetPrimaryMonitor(), NULL, NULL, &w, &h);
    /* squeeze video in aspect, might cause issue if playing portrait video on 
     * landscape screen
     */
    w = (int)(codec_ctx->width * windowscalefactor) % w;
    h = (int)(codec_ctx->height * windowscalefactor) % h;
    printf("w:%d h:%d\n", w, h);
    
    if((sws = sws_getContext(codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt, w, h, AV_PIX_FMT_RGB24, SWS_FAST_BILINEAR, NULL, NULL, NULL)) == NULL)
    {
        printf("cannot alloc sws\n");
        return 1;
    }


    if((imgbufsz = av_image_alloc(conv_data, conv_linesz, w, h, AV_PIX_FMT_RGB24, 1)) < 0)
    {
        printf("cannot alloc image\n");
        return 1;
    }

    if((window = glfwCreateWindow(w, h, "LibAVVideo", NULL, NULL)) == NULL)
    {
        glfwGetError(&cstr);
        printf("glfw err: %s\n", cstr);
        return 1;
    }

    glfwSetFramebufferSizeCallback(window, fbszcb);
    glfwMakeContextCurrent(window);
    /*
     * will be ok if video is fps is same as our refresh rate
     * but you wanna stall updating the picture so pacing isnt off
     */
    /*
     * here:

    * and in update
    * nanosleep(fps/60);
    */
    if(!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        printf("cannot load gl\n");
        return 1;
    }
    printf("GL_VERSION:%s\n", glGetString(GL_VERSION)); glerr;
    
    program = glCreateProgram(); glerr;
    if(!compile_and_attach(&vert, shadersrc_vert, GL_VERTEX_SHADER, program) || !compile_and_attach(&frag, shadersrc_frag, GL_FRAGMENT_SHADER, program))
    {
        printf("compilation or attaching failed\n");
        return 1;
    }
    glLinkProgram(program); glerr;
    glGetProgramiv(program, GL_LINK_STATUS, &i); glerr;
    if(!i)
    {
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &i); glerr;
        str = malloc(i);
        glGetProgramInfoLog(program, i, NULL, str); glerr;
        printf("link error: %s\n", str);
        free(str);
        return 1;
    }
    glDetachShader(vert, program); glerr;
    glDeleteShader(vert); glerr;
    glDetachShader(frag, program); glerr;
    glDeleteShader(frag); glerr;
    glUseProgram(program); glerr;

    glGenTextures(1, &img); glerr;
    glBindTexture(GL_TEXTURE_2D, img); glerr;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL); glerr;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glActiveTexture(GL_TEXTURE0); glerr;
    glUniform1i(glGetUniformLocation(program, "img"), 0); glerr;

    glGenBuffers(2, vbo_ibo); glerr;
    glBindBuffer(GL_ARRAY_BUFFER, vbo_ibo[0]); glerr;
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW); glerr;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vbo_ibo[1]); glerr;
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW); glerr;

    i = glGetAttribLocation(program, "aPos"); glerr;
    glVertexAttribPointer(i, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, NULL); glerr;
    glEnableVertexAttribArray(i); glerr;

    i = glGetAttribLocation(program, "aTexCoord"); glerr;
    glVertexAttribPointer(i, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, (void*)(sizeof(float) * 2)); glerr;
    glEnableVertexAttribArray(i); glerr;

    glClearColor(1.0, 0.5, 0.3, 1.0); glerr;
    glEnable(GL_CULL_FACE); glerr;

    while(!glfwWindowShouldClose(window) && !av_read_frame(input, packet))
    {
        glfwPollEvents();

        glClear(GL_COLOR_BUFFER_BIT); glerr;

        if(packet->size && packet->stream_index == stream->index)
        {
            if((res = avcodec_send_packet(codec_ctx, packet)) < 0)
            {
                averr(res);
                return 1;
            }
            while(res >= 0)
            {
                res = avcodec_receive_frame(codec_ctx, frame);
                if(res == AVERROR(EAGAIN))
                {
                    /*printf("retrying...\n");*/
                    break;
                }else if(res == AVERROR_EOF)
                {
                    printf("EOF\n");
                    break;
                }else if(res < 0){
                    averr(res);
                    return 1;
                }

                /*printf("frame %lu fmt:%d, samples:%d imgsz:%d\n", frame->pts, frame->format, frame->nb_samples, imgbufsz);*/
                sws_scale(sws, (const uint8_t * const*)frame->data, frame->linesize, 0, frame->height, conv_data, conv_linesz);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,  w, h, GL_RGB, GL_UNSIGNED_BYTE, conv_data[0]); glerr;
                
            }

        }

        glDrawElements(GL_TRIANGLES, arysz(indices), GL_UNSIGNED_SHORT, NULL); glerr;

        av_packet_unref(packet);
        av_frame_unref(frame);

        glfwSwapBuffers(window);
    }
    
    glDeleteBuffers(2, vbo_ibo);
    glDeleteTextures(1, &img);
    glDeleteProgram(program);

    av_freep(&conv_data[0]);
    avcodec_free_context(&codec_ctx);
    av_packet_free(&packet);
    av_frame_free(&frame);

    sws_freeContext(sws);
    avformat_close_input(&input);

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
