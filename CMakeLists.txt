cmake_minimum_required(VERSION 3.5)

find_package(PkgConfig)

pkg_check_modules(pulse-simple libpulse-simple)
pkg_check_modules(avcodec libavcodec)
pkg_check_modules(avformat libavformat)
pkg_check_modules(avutil libavutil)
pkg_check_modules(swresample libswresample)
pkg_check_modules(swscale libswscale)
pkg_check_modules(glfw3 glfw3)

project(libavdecode LANGUAGES C)

add_executable(audio audio.c)
add_executable(video 
    video.c
    glad/src/glad.c
)

target_include_directories(video PRIVATE glad/include)

target_link_libraries(audio
    ${avcodec_LIBRARIES}
    ${avformat_LIBRARIES}
    ${avutil_LIBRARIES}

    ${pulse-simple_LIBRARIES}
    ${swresample_LIBRARIES}
)

target_link_libraries(video
    ${avcodec_LIBRARIES}
    ${avformat_LIBRARIES}
    ${avutil_LIBRARIES}

    ${glfw3_LIBRARIES}
    ${swscale_LIBRARIES}
)
