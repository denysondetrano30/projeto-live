/*
 * Projeto Live: recursos do OBS feitos para transmissões de igrejas.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include <obs-module.h>

#include "director-dock.h"
#include "intercom.h"
#include "multistream.h"
#include "stream-dock.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("projeto-live", "en-US")

extern struct obs_source_info projeto_live_phone_source;

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Projeto Live: celulares como câmeras e outros recursos para igrejas";
}

bool obs_module_load(void)
{
	pl_intercom_start();
	obs_register_source(&projeto_live_phone_source);
	pl_director_dock_load();
	pl_multistream_load();
	pl_stream_dock_load();
	return true;
}

void obs_module_unload(void)
{
	pl_multistream_unload();
	pl_intercom_stop();
}
