cmake_minimum_required(VERSION 3.28...3.30)

find_package(Qt6 REQUIRED Widgets)
find_package(CURL REQUIRED)
find_package(FFmpeg REQUIRED avcodec avformat avutil)
find_package(LibDataChannel 0.20 REQUIRED)

add_library(projeto-live MODULE)
add_library(OBS::projeto-live ALIAS projeto-live)

target_sources(
  projeto-live
  PRIVATE
    artes.cpp
    artes.h
    audio-wizard.cpp
    audio-wizard.h
    caption-source.c
    control-server.c
    control-server.h
    audio-dock.cpp
    audio-dock.h
    camera-dock.cpp
    camera-dock.h
    color-mode.cpp
    color-mode.h
    director-dock.cpp
    director-dock.h
    intercom-audio.c
    intercom-audio.h
    intercom-stream.h
    intercom.c
    intercom.h
    mdns-announce.c
    mdns-announce.h
    mirror-source.c
    multistream.c
    multistream.h
    net-info.c
    net-info.h
    phone-source.c
    phone-source.h
    phone-video.c
    phone-video.h
    photo.cpp
    photo.h
    pl-clock.c
    pl-account.cpp
    pl-account.h
    pl-clock.h
    pl-link.h
    pl-socket.h
    pl-sync.c
    pl-sync.h
    projection-link.c
    projection-link.h
    projection-ui.cpp
    projeto-live.c
    qrcodegen.c
    qrcodegen.h
    sha256.c
    sha256.h
    layout.cpp
    layout.h
    media-bar.cpp
    media-bar.h
    scene-audio.cpp
    scene-audio.h
    somun-style.cpp
    somun-style.h
    source-row.hpp
    stats-dock.cpp
    stats-dock.h
    stream-dock.cpp
    studio-window.cpp
    studio-window.h
    talk-key.cpp
    talk-key.h
    team-panel.cpp
    team-panel.h
    transitions-dialog.cpp
    transitions-dialog.h
    stream-dock.h
    usb-relay.c
    usb-relay.h
    vertical-dock.cpp
    vertical-dock.h
    vertical-pan.cpp
    vertical-pan.h
    vertical-record.c
    vertical-record.h
    webcam-source.cpp
    webcam-source.h
    roteiro-strip.cpp
    roteiro-strip.h
    deck.cpp
    deck.h
    relatar.cpp
    relatar.h
    updater.cpp
    updater.h
    vertical.c
    vertical.h
    watch-window.cpp
    watch-window.h
)

target_link_libraries(
  projeto-live
  PRIVATE
    OBS::frontend-api
    OBS::libobs
    Qt::Widgets
    CURL::libcurl
    FFmpeg::avcodec
    FFmpeg::avformat
    FFmpeg::avutil
    LibDataChannel::LibDataChannel
    $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>
    $<$<PLATFORM_ID:Windows>:ws2_32>
    $<$<PLATFORM_ID:Windows>:iphlpapi>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:${CMAKE_DL_LIBS}>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:m>
    "$<$<PLATFORM_ID:Darwin>:$<LINK_LIBRARY:FRAMEWORK,CoreFoundation.framework>>"
    "$<$<PLATFORM_ID:Darwin>:$<LINK_LIBRARY:FRAMEWORK,CoreAudio.framework>>"
    "$<$<PLATFORM_ID:Darwin>:$<LINK_LIBRARY:FRAMEWORK,AudioToolbox.framework>>"
)

target_compile_options(projeto-live PRIVATE $<$<PLATFORM_ID:Darwin>:-Wno-quoted-include-in-framework-header>)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in projeto-live.rc)
  target_sources(projeto-live PRIVATE projeto-live.rc)
endif()

set_target_properties_obs(projeto-live PROPERTIES FOLDER plugins PREFIX "")

# qrcodegen.c (MIT) and deps/miniaudio (public domain or MIT-0) are third-party code; their warnings
# must not stop the build.
set_target_properties(projeto-live PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
