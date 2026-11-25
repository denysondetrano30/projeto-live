/*
 * somun live: o deck pelo celular (decidido com ela em 25/11).
 *
 * O celular abre uma página servida pelo somun tela (QR com código secreto). O tela conversa com o
 * live pela ligação que já existe (projection-link.c): manda os toques ("DECK <json>") e recebe o
 * estado ("DECKSTATE <json>"). Aqui, na janela do live (a cada 250 ms):
 * - pega os toques da fila e faz o mesmo que a mesa faria: entrada → Prévia, "Cortar" (o mesmo da
 *   mesa), arte liga/desliga (as da grade), volume (fader com a mesma curva do mixer) e mudo;
 * - monta o estado (entradas com no ar/prévia, artes com no ar, canais do Som com fader/dB/mudo,
 *   transmissão no ar e o tempo) e entrega para a ligação, que só manda quando muda.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "deck.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "artes.h"
#include "camera-dock.h"
#include "multistream.h"
#include "projection-link.h"
#include "studio-window.h"

namespace {

QTimer *timer = nullptr;

/* A mesma curva do fader do mixer do OBS (OBS_FADER_LOG), para o deck e a mesa baterem. */
const double LOG_OFFSET_DB = 6.0;
const double LOG_RANGE_DB = 96.0;
const double LOG_OFFSET_VAL = -0.77815125038364363;
const double LOG_RANGE_VAL = -2.00860017176191756;

double dbToDeflection(double db)
{
	if (db >= 0.0) {
		return 1.0;
	}
	if (db <= -LOG_RANGE_DB) {
		return 0.0;
	}
	return (-std::log10(-db + LOG_OFFSET_DB) - LOG_RANGE_VAL) / (LOG_OFFSET_VAL - LOG_RANGE_VAL);
}

double deflectionToDb(double def)
{
	if (def >= 1.0) {
		return 0.0;
	}
	if (def <= 0.0) {
		return -INFINITY;
	}
	return -(LOG_RANGE_DB + LOG_OFFSET_DB) * std::pow((LOG_RANGE_DB + LOG_OFFSET_DB) / LOG_OFFSET_DB, -def) +
	       LOG_OFFSET_DB;
}

/* Os canais do Som: os globais com som e os sons da cena no ar (como o mixer da mesa). */
std::vector<obs_source_t *> soundSources()
{
	std::vector<obs_source_t *> list;
	for (int channel = 1; channel <= 6; channel++) {
		obs_source_t *source = obs_get_output_source(channel);
		if (!source) {
			continue;
		}
		if (obs_source_audio_active(source)) {
			list.push_back(source);
		} else {
			obs_source_release(source);
		}
	}
	obs_enum_sources(
		[](void *data, obs_source_t *source) {
			auto *found = static_cast<std::vector<obs_source_t *> *>(data);
			uint32_t flags = obs_source_get_output_flags(source);
			if ((flags & OBS_SOURCE_AUDIO) == 0 || !obs_source_audio_active(source) ||
			    !obs_source_active(source)) {
				return true;
			}
			for (obs_source_t *already : *found) {
				if (already == source) {
					return true;
				}
			}
			if (obs_source_t *held = obs_source_get_ref(source)) {
				found->push_back(held);
			}
			return true;
		},
		&list);
	return list;
}

void act(const QJsonObject &action)
{
	QString kind = action.value("a").toString();
	QByteArray id = action.value("id").toString().toUtf8();
	if (kind == "previa") {
		obs_source_t *scene = obs_get_source_by_name(id.constData());
		if (scene && obs_scene_from_source(scene)) {
			if (obs_frontend_preview_program_mode_active()) {
				obs_frontend_set_current_preview_scene(scene);
			} else {
				obs_frontend_set_current_scene(scene);
			}
		}
		obs_source_release(scene);
	} else if (kind == "cortar") {
		pl_studio_cut();
	} else if (kind == "arte") {
		pl_artes_deck_toggle(id.toStdString());
	} else if (kind == "vol" || kind == "mudo") {
		obs_source_t *source = obs_get_source_by_name(id.constData());
		if (source) {
			if (kind == "vol") {
				double def = std::clamp(action.value("v").toDouble(), 0.0, 1.0);
				double db = deflectionToDb(def);
				obs_source_set_volume(source, std::isinf(db) ? 0.0f : (float)std::pow(10.0, db / 20.0));
			} else {
				obs_source_set_muted(source, action.value("on").toBool());
			}
		}
		obs_source_release(source);
	}
}

QString stateJson()
{
	QJsonObject state;

	obs_source_t *program = obs_frontend_get_current_scene();
	obs_source_t *preview = obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene()
									   : nullptr;
	std::string programName = program ? obs_source_get_name(program) : "";
	std::string previewName = preview ? obs_source_get_name(preview) : "";
	obs_source_release(program);
	obs_source_release(preview);
	QJsonArray entries;
	for (const std::string &name : pl_entries_visible()) {
		QJsonObject entry;
		entry["id"] = QString::fromStdString(name);
		entry["ar"] = name == programName;
		entry["previa"] = !previewName.empty() && name == previewName;
		entries.append(entry);
	}
	state["entradas"] = entries;

	QJsonArray arts;
	for (const PLDeckArt &art : pl_artes_deck_list()) {
		QJsonObject item;
		item["id"] = QString::fromStdString(art.id);
		item["nome"] = QString::fromStdString(art.name);
		item["on"] = art.on;
		arts.append(item);
	}
	state["artes"] = arts;

	QJsonArray sounds;
	for (obs_source_t *source : soundSources()) {
		float mul = obs_source_get_volume(source);
		double db = mul > 0.0f ? 20.0 * std::log10((double)mul) : -INFINITY;
		QJsonObject item;
		item["id"] = QString::fromUtf8(obs_source_get_name(source));
		item["v"] = std::round(dbToDeflection(db) * 1000.0) / 1000.0;
		item["db"] = std::isinf(db) ? -100.0 : std::round(db * 10.0) / 10.0;
		item["mudo"] = obs_source_muted(source);
		sounds.append(item);
		obs_source_release(source);
	}
	state["som"] = sounds;

	state["noAr"] = pl_multistream_active();
	state["segundos"] = (double)(pl_multistream_active() ? pl_multistream_seconds() : 0);
	return QString::fromUtf8(QJsonDocument(state).toJson(QJsonDocument::Compact));
}

void tick()
{
	char buffer[512];
	int done = 0;
	while (done < 16 && pl_projection_take_deck(buffer, sizeof(buffer))) {
		QJsonObject action = QJsonDocument::fromJson(QByteArray(buffer)).object();
		if (!action.isEmpty()) {
			act(action);
		}
		done++;
	}
	pl_projection_set_deck(stateJson().toUtf8().constData());
}

void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING && !timer) {
		timer = new QTimer();
		timer->setInterval(250);
		QObject::connect(timer, &QTimer::timeout, []() { tick(); });
		timer->start();
	} else if (event == OBS_FRONTEND_EVENT_EXIT && timer) {
		timer->stop();
		delete timer;
		timer = nullptr;
	}
}

} // namespace

void pl_deck_load(void)
{
	obs_frontend_add_event_callback(frontendEvent, nullptr);
}

void pl_deck_unload(void)
{
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
}
