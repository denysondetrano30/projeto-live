/*
 * Projeto Live: imagem do iPhone pelo cabo USB.
 *
 * O cabo do iPhone passa pelo serviço "usbmuxd" da Apple (no Windows, o "Apple Mobile Device
 * Service", que vem com o iTunes, em 127.0.0.1:27015). Ele só leva conexões TCP do computador
 * para o iPhone. O relé abre uma porta local (127.0.0.1); quando a Fonte de mídia conecta nela,
 * o relé liga para o app no iPhone (porta 9010) pelo cabo e repassa o vídeo (MPEG-TS).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define PL_USB_PHONE_PORT 9010

struct pl_usb_relay;

/* Cria o relé e começa a esperar a Fonte de mídia. Devolve NULL se não conseguir. */
struct pl_usb_relay *pl_usb_relay_create(const char *source_name);

/* Porta local (em 127.0.0.1) onde a Fonte de mídia deve conectar. */
int pl_usb_relay_local_port(const struct pl_usb_relay *relay);

void pl_usb_relay_destroy(struct pl_usb_relay *relay);

#ifdef __cplusplus
}
#endif
