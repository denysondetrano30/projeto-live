/*
 * Projeto Live: imagem do iPhone pelo cabo USB.
 *
 * O cabo do iPhone passa pelo serviço "usbmuxd" da Apple (no Windows, o "Apple Mobile Device
 * Service", que vem com o iTunes, em 127.0.0.1:27015). Ele só leva conexões TCP do computador
 * para o iPhone. O relé abre uma porta local (127.0.0.1); quando a Fonte de mídia conecta nela,
 * o relé liga para o app no iPhone (porta 9010) pelo cabo e repassa o vídeo (MPEG-TS).
 * No sentido contrário, pelo mesmo túnel, o relé avisa o app se a câmera está no ar
 * ("TALLY on" / "TALLY off", uma linha de texto por mudança), para o app 0.6.
 *
 * A partir do app 0.7 há um segundo túnel, para a porta de controle 9011, com as mesmas linhas
 * do canal de controle do Wi-Fi (veja control-server.h), sem senha: "TALLY", "CMD <n> <texto>"
 * e "PING" do PC; "HELLO usb <nome da câmera>", "ACK <n>" e "PING" do app.
 *
 * Com mais de um iPhone no cabo, cada fonte "Celular" pega um iPhone que nenhuma outra esteja usando.
 *
 * Android (app 0.3 em diante): o mesmo, pela Depuração USB. Em vez do usbmuxd, o relé fala com o
 * servidor do adb (127.0.0.1:5037; se não estiver rodando, liga o adb.exe que o instalador põe em
 * data/obs-plugins/projeto-live/adb) e abre "tcp:9010", "tcp:9011" e "tcp:9012" no celular. O app
 * Android escuta nessas portas igual ao do iPhone, com as mesmas mensagens.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <stdbool.h>

#include "pl-link.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PL_USB_PHONE_PORT 9010
#define PL_USB_CONTROL_PORT 9011
#define PL_USB_VOICE_PORT 9012

struct pl_usb_relay;
struct pl_voice_peer;
struct pl_clock;

/* Cria o relé e começa a esperar o receptor da imagem. `on_air`, `voice` (o participante desta
 * fonte no intercom) e `clock` (sincronia, pode ser NULL) são usados pela thread do relé e precisam
 * durar mais que ele. Devolve NULL se não conseguir. */
struct pl_usb_relay *pl_usb_relay_create(const char *source_name, const volatile bool *on_air,
					 struct pl_voice_peer *voice, struct pl_clock *clock);

/* Porta local (em 127.0.0.1) onde a Fonte de mídia deve conectar. */
int pl_usb_relay_local_port(const struct pl_usb_relay *relay);

/* Manda uma linha ao app pelo túnel de controle (sem o "\n"). Devolve false se não houver app
 * conectado. Pode ser chamada de qualquer thread. */
bool pl_usb_relay_send(struct pl_usb_relay *relay, const char *line);

/* O que o painel do diretor mostra: app conectado, nome e último "Entendi". */
void pl_usb_relay_get_status(struct pl_usb_relay *relay, struct pl_link_status *status);

void pl_usb_relay_destroy(struct pl_usb_relay *relay);

#ifdef __cplusplus
}
#endif
