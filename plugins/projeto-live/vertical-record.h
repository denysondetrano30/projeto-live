/*
 * Projeto Live: gravação vertical e preferências da tela vertical (veja vertical-record.c).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <obs-data.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void pl_vertical_record_load(void);
void pl_vertical_record_unload(void);

/*
 * Preferências: "record" (gravar o vertical junto), "embed" (mostrar ao lado do preview),
 * "record_folder" (pasta da gravação vertical; "" = a mesma da gravação normal), "record_kbps" e a
 * transição do vertical: "transition_own" (false = acompanha a do Programa), "transition_id",
 * "transition_ms" e "transition_settings" (+ "transition_settings_id").
 */
bool pl_vertical_pref(const char *key);
void pl_vertical_set_pref(const char *key, bool value);
const char *pl_vertical_pref_string(const char *key);
void pl_vertical_set_pref_string(const char *key, const char *value);
int pl_vertical_pref_int(const char *key);
void pl_vertical_set_pref_int(const char *key, int value);
/* Nova referência (ou NULL). */
obs_data_t *pl_vertical_pref_obj(const char *key);
void pl_vertical_set_pref_obj(const char *key, obs_data_t *value);

#ifdef __cplusplus
}
#endif
