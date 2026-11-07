cmake_minimum_required(VERSION 3.28...3.30)

find_package(Qt6 REQUIRED Widgets)

add_library(projeto-live MODULE)
add_library(OBS::projeto-live ALIAS projeto-live)

target_sources(
  projeto-live
  PRIVATE
    control-server.c
    control-server.h
    director-dock.cpp
    director-dock.h
    intercom-audio.c
    intercom-audio.h
    intercom-stream.h
    intercom.c
    intercom.h
    multistream.c
    multistream.h
    net-info.c
    net-info.h
    phone-source.c
    phone-source.h
    pl-link.h
    pl-socket.h
    projeto-live.c
    qrcodegen.c
    qrcodegen.h
    sha256.c
    sha256.h
    stream-dock.cpp
    stream-dock.h
    usb-relay.c
    usb-relay.h
    vertical-dock.cpp
    vertical-dock.h
    vertical-record.c
    vertical-record.h
    vertical.c
    vertical.h
)

target_link_libraries(
  projeto-live
  PRIVATE
    OBS::frontend-api
    OBS::libobs
    Qt::Widgets
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
