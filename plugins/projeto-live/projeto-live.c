/*
 * Projeto Live: recursos do OBS feitos para transmissões de igrejas.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include <obs-module.h>

#include "audio-dock.h"
#include "camera-dock.h"
#include "director-dock.h"
#include "intercom.h"
#include "multistream.h"
#include "audio-wizard.h"
#include "phone-video.h"
#include "pl-sync.h"
#include "projection-link.h"
#include "stats-dock.h"
#include "stream-dock.h"
#include "vertical-dock.h"
#include "vertical-record.h"
#include "vertical.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("projeto-live", "en-US")

extern struct obs_source_info projeto_live_phone_source;
extern struct obs_source_info pl_caption_source;
extern struct obs_source_info pl_mirror_source;

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Projeto Live: celulares como câmeras e outros recursos para igrejas";
}

bool obs_module_load(void)
{
	pl_intercom_start();
	pl_sync_load();
	pl_phone_video_register();
	obs_register_source(&projeto_live_phone_source);
	obs_register_source(&pl_caption_source);
	obs_register_source(&pl_mirror_source);
	pl_director_dock_load();
	pl_camera_dock_load();
	pl_stats_dock_load();
	pl_audio_dock_load();
	pl_vertical_load();
	pl_vertical_record_load();
	pl_vertical_dock_load();
	pl_multistream_load();
	pl_multistream_ui_load();
	pl_projection_ui_load();
	pl_audio_wizard_load();
	pl_projection_start();
	return true;
}

void obs_module_unload(void)
{
	pl_projection_stop();
	pl_stats_dock_unload();
	pl_sync_unload();
	pl_multistream_unload();
	pl_vertical_record_unload();
	pl_vertical_unload();
	pl_intercom_stop();
}
