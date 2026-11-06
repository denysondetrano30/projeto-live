cmake_minimum_required(VERSION 3.28...3.30)

add_library(projeto-live MODULE)
add_library(OBS::projeto-live ALIAS projeto-live)

target_sources(
  projeto-live
  PRIVATE mdns.h phone-discovery.c phone-discovery.h phone-source.c projeto-live.c
)

target_link_libraries(
  projeto-live
  PRIVATE
    OBS::libobs
    $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>
    $<$<PLATFORM_ID:Windows>:ws2_32>
    $<$<PLATFORM_ID:Windows>:iphlpapi>
)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in projeto-live.rc)
  target_sources(projeto-live PRIVATE projeto-live.rc)
endif()

set_target_properties_obs(projeto-live PROPERTIES FOLDER plugins PREFIX "")

# mdns.h is a third-party public domain header; its warnings must not stop the build.
set_target_properties(projeto-live PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
