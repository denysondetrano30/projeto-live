/*
 * Projeto Live: gravação vertical e preferências da tela vertical (veja vertical-record.c).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void pl_vertical_record_load(void);
void pl_vertical_record_unload(void);

/* Preferências: "record" (gravar o vertical junto) e "embed" (mostrar ao lado do preview). */
bool pl_vertical_pref(const char *key);
void pl_vertical_set_pref(const char *key, bool value);

#ifdef __cplusplus
}
#endif
