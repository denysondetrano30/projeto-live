/*
 * Projeto Live: túnel de voz do intercom pelo cabo USB (veja intercom.h).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include "intercom.h"
#include "pl-socket.h"

#ifdef __cplusplus
extern "C" {
#endif

/* O relé entrega o túnel de voz já conectado; a central passa a ser dona dele e o fecha. */
void pl_voice_peer_attach_stream(struct pl_voice_peer *peer, pl_socket_t sock);
void pl_voice_peer_detach_stream(struct pl_voice_peer *peer);
bool pl_voice_peer_has_stream(struct pl_voice_peer *peer);

#ifdef __cplusplus
}
#endif
