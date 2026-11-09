/*
 * Projeto Live: atraso fixo das câmeras e atraso automático do som (sincronia, etapa 7a).
 *
 * Todas as câmeras de celular aparecem com o mesmo atraso fixo depois de filmadas (padrão 400 ms,
 * ajustável nas propriedades de qualquer fonte "Celular"; vale para todas). Enquanto houver câmera
 * sincronizada, o som das entradas de áudio do PC (mesa de som, placa, microfone) recebe o mesmo
 * atraso em "Sincronização" (Propriedades avançadas de áudio), para a voz ficar junto da boca.
 *
 * Se a pessoa mudar o atraso de uma entrada de áudio na mão, aquela entrada deixa de ser ajustada
 * sozinha. Sem câmera sincronizada, o atraso automático volta a zero.
 *
 * O valor fica em "sincronia.json", na pasta de configuração do plugin.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PL_SYNC_DEFAULT_MS 400
#define PL_SYNC_MIN_MS 100
#define PL_SYNC_MAX_MS 2000

void pl_sync_load(void);
void pl_sync_unload(void);

/* Atraso fixo das câmeras, em ms. */
int pl_sync_delay_ms(void);
void pl_sync_set_delay_ms(int ms);

#ifdef __cplusplus
}
#endif
