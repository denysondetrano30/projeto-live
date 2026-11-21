/*
 * somun live: "Artes" (na mesa) / "No programa" (a tela de configurar) — a camada depois do corte.
 *
 * É o "downstream key" das mesas de corte: o que estiver ligado aqui aparece por cima de tudo que vai
 * ao ar, em qualquer câmera, sem repetir nada dentro de cada cena.
 *
 * Como funciona:
 * - duas cenas privadas (não aparecem nas Entradas): uma para a saída deitada (16:9) e outra para a em
 *   pé (9:16). Cada peça tem um item em cada uma, com posição e tamanho próprios por saída;
 * - a cena 16:9 vai no canal 10 da saída do OBS e a 9:16 no canal 10 da tela em pé (o OBS desenha os
 *   canais em ordem, então o canal 10 fica por cima do programa, que é o canal 0);
 * - entrada marcada "vai limpa" (o telão já vem marcado) tira a camada do ar enquanto estiver no ar;
 * - peça fixa liga e fica; temporária sai sozinha depois de N segundos (padrão 10 s);
 * - entrada e saída com esmaecer (0,4 s) ou sem animação;
 * - tudo fica salvo junto com o projeto (coleção de cenas) do OBS.
 *
 * Leva 1 (20/11): Imagem (logo, QR code em PNG), Retângulo e Letterbox. A leva 2 traz Texto (lower
 * third), deslizar, Look (LUT), teclas de atalho e vídeo em repetição.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "artes.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDialog>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QListWidgetItem>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QRectF>
#include <QPaintEvent>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QString>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "camera-dock.h"
#include "somun-style.h"
#include "vertical.h"

namespace {

/* Canal da saída onde a camada fica (0 = programa; 1 a 6 = sons globais do OBS). */
const uint32_t CHANNEL = 10;

const int OUT_WIDE = 0; /* saída deitada, 16:9 */
const int OUT_TALL = 1; /* saída em pé, 9:16 */

const uint32_t ANIM_MS = 400;

const char *const SAVE_KEY = "somun_artes";
const char *const PIECE_MIME = "application/x-somun-arte";

QString msg(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

enum Kind { KIND_IMAGE = 0, KIND_RECT = 1, KIND_LETTERBOX = 2 };
enum Anim { ANIM_FADE = 0, ANIM_NONE = 1 };

struct Piece {
	std::string id;
	std::string name;
	int kind = KIND_IMAGE;
	std::string file;
	uint32_t color = 0xFF000000; /* ABGR, como o OBS guarda */
	int bar[2] = {12, 12};       /* letterbox: altura de cada barra, em % da altura da saída */
	bool pinned = true;          /* "à mão": aparece na barra da mesa */
	bool temporary = false;
	int seconds = 10;
	int anim = ANIM_FADE;
	bool on = false;
	qint64 onAt = 0;
	qint64 offAt = 0;
	std::vector<obs_source_t *> sources;     /* donos */
	std::vector<obs_sceneitem_t *> items[2]; /* com referência */
};

obs_scene_t *scenes[2] = {};
std::vector<std::unique_ptr<Piece>> pieces;
QTimer *ticker = nullptr;
bool loaded = false;

/* O que a tela "No programa" desenha mesmo desligado (a peça escolhida), lido na thread de vídeo. */
std::mutex selectedMutex;
std::vector<obs_sceneitem_t *> selectedItems[2];

class ArtsBar;
std::vector<ArtsBar *> bars;
void notifyBars(bool structure);
void notifyEditor(bool structure);
void openEditorFor(QWidget *parent, const std::string &id);

qint64 now()
{
	return QDateTime::currentMSecsSinceEpoch();
}

/* Tamanho de cada saída (a deitada é a do OBS; a em pé, a da tela vertical). */
void canvasSize(int out, float &w, float &h)
{
	struct obs_video_info ovi;
	if (out == OUT_TALL) {
		w = PL_VERTICAL_WIDTH;
		h = PL_VERTICAL_HEIGHT;
		obs_canvas_t *canvas = pl_vertical_get_canvas();
		if (canvas) {
			if (obs_canvas_get_video_info(canvas, &ovi) && ovi.base_width && ovi.base_height) {
				w = (float)ovi.base_width;
				h = (float)ovi.base_height;
			}
			obs_canvas_release(canvas);
		}
		return;
	}
	w = 1920.0f;
	h = 1080.0f;
	if (obs_get_video_info(&ovi) && ovi.base_width && ovi.base_height) {
		w = (float)ovi.base_width;
		h = (float)ovi.base_height;
	}
}

void ensureScenes()
{
	if (!scenes[OUT_WIDE]) {
		scenes[OUT_WIDE] = obs_scene_create_private("somun artes 16:9");
	}
	if (!scenes[OUT_TALL]) {
		scenes[OUT_TALL] = obs_scene_create_private("somun artes 9:16");
	}
}

Piece *findPiece(const std::string &id)
{
	for (auto &piece : pieces) {
		if (piece->id == id) {
			return piece.get();
		}
	}
	return nullptr;
}

/* ------------------------------------------------------------------------- */
/* Fontes e itens de cada peça                                               */

obs_source_t *makeColor(uint32_t color, uint32_t w, uint32_t h, const char *name)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "color", color);
	obs_data_set_int(settings, "width", std::max<uint32_t>(w, 1));
	obs_data_set_int(settings, "height", std::max<uint32_t>(h, 1));
	obs_source_t *source = obs_source_create_private("color_source_v3", name, settings);
	obs_data_release(settings);
	return source;
}

obs_source_t *makeImage(const std::string &file, const char *name)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "file", file.c_str());
	obs_source_t *source = obs_source_create_private("image_source", name, settings);
	obs_data_release(settings);
	return source;
}

/* Esmaecer (0,4 s) ou entrar seco, para cada item da peça. Cada item tem a sua transição. */
void applyAnim(Piece *piece)
{
	for (int out = 0; out < 2; out++) {
		for (obs_sceneitem_t *item : piece->items[out]) {
			for (int show = 0; show < 2; show++) {
				if (piece->anim == ANIM_FADE) {
					obs_source_t *fade =
						obs_source_create_private("fade_transition", "somun esmaecer", nullptr);
					obs_sceneitem_set_transition(item, show == 1, fade);
					obs_sceneitem_set_transition_duration(item, show == 1, ANIM_MS);
					obs_source_release(fade);
				} else {
					obs_sceneitem_set_transition(item, show == 1, nullptr);
				}
			}
		}
	}
}

obs_sceneitem_t *addItem(Piece *piece, int out, obs_source_t *source)
{
	obs_sceneitem_t *item = obs_scene_add(scenes[out], source);
	if (!item) {
		return nullptr;
	}
	obs_sceneitem_addref(item);
	obs_sceneitem_set_visible(item, piece->on);
	piece->items[out].push_back(item);
	return item;
}

/* Lugar de começo de uma peça nova: canto de cima à direita (logo), dentro da margem segura. */
void placeDefault(Piece *piece, int out, obs_sceneitem_t *item, obs_source_t *source)
{
	float w, h;
	canvasSize(out, w, h);
	float margin = std::min(w, h) * 0.05f;
	float bw = w * (out == OUT_TALL ? 0.40f : 0.20f);
	float bh = bw * 9.0f / 16.0f;
	uint32_t sw = obs_source_get_width(source);
	uint32_t sh = obs_source_get_height(source);
	if (piece->kind == KIND_IMAGE && sw && sh) {
		bh = bw * (float)sh / (float)sw;
	}
	struct vec2 bounds;
	vec2_set(&bounds, bw, bh);
	obs_sceneitem_set_bounds_type(item, piece->kind == KIND_RECT ? OBS_BOUNDS_STRETCH : OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_bounds(item, &bounds);
	struct vec2 pos;
	if (piece->kind == KIND_RECT) {
		vec2_set(&pos, (w - bw) / 2.0f, (h - bh) / 2.0f);
	} else {
		vec2_set(&pos, w - margin - bw, margin);
	}
	obs_sceneitem_set_pos(item, &pos);
}

/* Lugar guardado (posição e caixa) de uma peça numa saída. */
void placeSaved(Piece *piece, int out, obs_sceneitem_t *item, obs_data_t *saved)
{
	const char *suffix = out == OUT_WIDE ? "16" : "916";
	std::string kx = std::string("x") + suffix, ky = std::string("y") + suffix;
	std::string kw = std::string("w") + suffix, kh = std::string("h") + suffix;
	obs_sceneitem_set_bounds_type(item, piece->kind == KIND_RECT ? OBS_BOUNDS_STRETCH : OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	struct vec2 bounds;
	vec2_set(&bounds, (float)obs_data_get_double(saved, kw.c_str()), (float)obs_data_get_double(saved, kh.c_str()));
	obs_sceneitem_set_bounds(item, &bounds);
	struct vec2 pos;
	vec2_set(&pos, (float)obs_data_get_double(saved, kx.c_str()), (float)obs_data_get_double(saved, ky.c_str()));
	obs_sceneitem_set_pos(item, &pos);
}

/* Letterbox: as duas barras (em cima e embaixo) de cada saída, com a altura escolhida. */
void layoutLetterbox(Piece *piece, int out)
{
	float w, h;
	canvasSize(out, w, h);
	uint32_t barH = (uint32_t)std::lround(h * std::clamp(piece->bar[out], 1, 45) / 100.0);
	std::vector<obs_sceneitem_t *> &items = piece->items[out];
	for (size_t i = 0; i < items.size(); i++) {
		obs_source_t *source = obs_sceneitem_get_source(items[i]);
		obs_data_t *settings = obs_data_create();
		obs_data_set_int(settings, "color", piece->color);
		obs_data_set_int(settings, "width", (uint32_t)w);
		obs_data_set_int(settings, "height", barH);
		obs_source_update(source, settings);
		obs_data_release(settings);
		struct vec2 pos;
		vec2_set(&pos, 0.0f, i == 0 ? 0.0f : h - (float)barH);
		obs_sceneitem_set_pos(items[i], &pos);
	}
}

/* Cria as fontes e os itens da peça nas duas saídas (no lugar guardado, se houver). */
void buildPiece(Piece *piece, obs_data_t *saved)
{
	ensureScenes();
	if (piece->kind == KIND_LETTERBOX) {
		for (int out = 0; out < 2; out++) {
			for (int part = 0; part < 2; part++) {
				obs_source_t *bar = makeColor(piece->color, 16, 16, piece->name.c_str());
				piece->sources.push_back(bar);
				addItem(piece, out, bar);
			}
			layoutLetterbox(piece, out);
		}
	} else {
		obs_source_t *source = piece->kind == KIND_IMAGE
					       ? makeImage(piece->file, piece->name.c_str())
					       : makeColor(piece->color, 400, 225, piece->name.c_str());
		if (!source) {
			return;
		}
		piece->sources.push_back(source);
		for (int out = 0; out < 2; out++) {
			obs_sceneitem_t *item = addItem(piece, out, source);
			if (!item) {
				continue;
			}
			std::string key = out == OUT_WIDE ? "w16" : "w916";
			if (saved && obs_data_has_user_value(saved, key.c_str())) {
				placeSaved(piece, out, item, saved);
			} else {
				placeDefault(piece, out, item, source);
			}
		}
	}
	applyAnim(piece);
}

void releasePiece(Piece *piece)
{
	{
		std::lock_guard<std::mutex> lock(selectedMutex);
		for (int out = 0; out < 2; out++) {
			for (obs_sceneitem_t *item : piece->items[out]) {
				auto &sel = selectedItems[out];
				auto found = std::find(sel.begin(), sel.end(), item);
				if (found != sel.end()) {
					obs_sceneitem_release(*found);
					sel.erase(found);
				}
			}
		}
	}
	for (int out = 0; out < 2; out++) {
		for (obs_sceneitem_t *item : piece->items[out]) {
			obs_sceneitem_remove(item);
			obs_sceneitem_release(item);
		}
		piece->items[out].clear();
	}
	for (obs_source_t *source : piece->sources) {
		obs_source_release(source);
	}
	piece->sources.clear();
}

/* As camadas seguem a ordem da lista: a última peça fica por cima. */
void reorderLayers()
{
	for (int out = 0; out < 2; out++) {
		std::vector<obs_sceneitem_t *> order;
		for (auto &piece : pieces) {
			for (obs_sceneitem_t *item : piece->items[out]) {
				order.push_back(item);
			}
		}
		if (!order.empty() && scenes[out]) {
			obs_scene_reorder_items(scenes[out], order.data(), order.size());
		}
	}
}

void clearAll()
{
	for (auto &piece : pieces) {
		releasePiece(piece.get());
	}
	pieces.clear();
}

/* ------------------------------------------------------------------------- */
/* Ligar, desligar e a camada no ar                                          */

void setOn(Piece *piece, bool on)
{
	piece->on = on;
	piece->onAt = on ? now() : 0;
	piece->offAt = on && piece->temporary ? piece->onAt + (qint64)piece->seconds * 1000 : 0;
	for (int out = 0; out < 2; out++) {
		for (obs_sceneitem_t *item : piece->items[out]) {
			obs_sceneitem_set_visible(item, on);
		}
	}
	notifyBars(false);
	notifyEditor(false);
}

/* A camada vai ao ar nas duas saídas, menos quando a entrada no ar está marcada "vai limpa". */
void applyChannels()
{
	bool clean = false;
	obs_source_t *program = obs_frontend_get_current_scene();
	if (program) {
		clean = pl_entry_goes_clean(program);
		obs_source_release(program);
	}

	obs_source_t *want = loaded && !clean && scenes[OUT_WIDE] ? obs_scene_get_source(scenes[OUT_WIDE]) : nullptr;
	obs_source_t *current = obs_get_output_source(CHANNEL);
	if (current != want) {
		obs_set_output_source(CHANNEL, want);
	}
	obs_source_release(current);

	obs_canvas_t *canvas = pl_vertical_get_canvas();
	if (canvas) {
		obs_source_t *wantTall = loaded && !clean && scenes[OUT_TALL] ? obs_scene_get_source(scenes[OUT_TALL])
									      : nullptr;
		obs_source_t *currentTall = obs_canvas_get_channel(canvas, CHANNEL);
		if (currentTall != wantTall) {
			obs_canvas_set_channel(canvas, CHANNEL, wantTall);
		}
		obs_source_release(currentTall);
		obs_canvas_release(canvas);
	}
}

void tick()
{
	qint64 t = now();
	bool anyTemporary = false;
	for (auto &piece : pieces) {
		if (piece->on && piece->offAt > 0) {
			if (t >= piece->offAt) {
				setOn(piece.get(), false);
			} else {
				anyTemporary = true;
			}
		}
	}
	if (anyTemporary) {
		notifyBars(false);
	}
	applyChannels();
}

/* ------------------------------------------------------------------------- */
/* Guardar junto com o projeto do OBS                                        */

void saveLoad(obs_data_t *data, bool saving, void *)
{
	if (saving) {
		obs_data_array_t *array = obs_data_array_create();
		for (auto &piece : pieces) {
			obs_data_t *item = obs_data_create();
			obs_data_set_string(item, "id", piece->id.c_str());
			obs_data_set_string(item, "nome", piece->name.c_str());
			obs_data_set_int(item, "tipo", piece->kind);
			obs_data_set_string(item, "arquivo", piece->file.c_str());
			obs_data_set_int(item, "cor", piece->color);
			obs_data_set_int(item, "barra16", piece->bar[OUT_WIDE]);
			obs_data_set_int(item, "barra916", piece->bar[OUT_TALL]);
			obs_data_set_bool(item, "a_mao", piece->pinned);
			obs_data_set_bool(item, "temporaria", piece->temporary);
			obs_data_set_int(item, "segundos", piece->seconds);
			obs_data_set_int(item, "animacao", piece->anim);
			obs_data_set_bool(item, "ligada", piece->on && !piece->temporary);
			if (piece->kind != KIND_LETTERBOX) {
				for (int out = 0; out < 2; out++) {
					if (piece->items[out].empty()) {
						continue;
					}
					const char *suffix = out == OUT_WIDE ? "16" : "916";
					struct vec2 pos, bounds;
					obs_sceneitem_get_pos(piece->items[out][0], &pos);
					obs_sceneitem_get_bounds(piece->items[out][0], &bounds);
					obs_data_set_double(item, (std::string("x") + suffix).c_str(), pos.x);
					obs_data_set_double(item, (std::string("y") + suffix).c_str(), pos.y);
					obs_data_set_double(item, (std::string("w") + suffix).c_str(), bounds.x);
					obs_data_set_double(item, (std::string("h") + suffix).c_str(), bounds.y);
				}
			}
			obs_data_array_push_back(array, item);
			obs_data_release(item);
		}
		obs_data_set_array(data, SAVE_KEY, array);
		obs_data_array_release(array);
		return;
	}

	clearAll();
	obs_data_array_t *array = obs_data_get_array(data, SAVE_KEY);
	size_t count = array ? obs_data_array_count(array) : 0;
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		auto piece = std::make_unique<Piece>();
		piece->id = obs_data_get_string(item, "id");
		piece->name = obs_data_get_string(item, "nome");
		piece->kind = (int)obs_data_get_int(item, "tipo");
		piece->file = obs_data_get_string(item, "arquivo");
		piece->color = (uint32_t)obs_data_get_int(item, "cor");
		piece->bar[OUT_WIDE] = (int)obs_data_get_int(item, "barra16");
		piece->bar[OUT_TALL] = (int)obs_data_get_int(item, "barra916");
		piece->pinned = obs_data_get_bool(item, "a_mao");
		piece->temporary = obs_data_get_bool(item, "temporaria");
		piece->seconds = std::max((int)obs_data_get_int(item, "segundos"), 1);
		piece->anim = (int)obs_data_get_int(item, "animacao");
		piece->on = obs_data_get_bool(item, "ligada");
		if (piece->id.empty()) {
			piece->id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
		}
		buildPiece(piece.get(), item);
		pieces.push_back(std::move(piece));
		obs_data_release(item);
	}
	obs_data_array_release(array);
	reorderLayers();
	loaded = true;
	notifyBars(true);
	notifyEditor(true);
}

/* ------------------------------------------------------------------------- */
/* Criar, duplicar, apagar e o menu de cada peça                              */

Piece *createPiece(QWidget *parent, int kind)
{
	auto piece = std::make_unique<Piece>();
	piece->id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
	piece->kind = kind;
	if (kind == KIND_IMAGE) {
		QString file = QFileDialog::getOpenFileName(parent, msg("Artes.ChooseImage"), QString(),
							    msg("Artes.ImageFilter"));
		if (file.isEmpty()) {
			return nullptr;
		}
		piece->file = file.toUtf8().constData();
		piece->name = QFileInfo(file).completeBaseName().toUtf8().constData();
	} else if (kind == KIND_RECT) {
		piece->name = msg("Artes.Rect").toUtf8().constData();
		piece->color = 0xFF141414;
	} else {
		piece->name = msg("Artes.Letterbox").toUtf8().constData();
		piece->color = 0xFF000000;
	}
	buildPiece(piece.get(), nullptr);
	Piece *created = piece.get();
	pieces.push_back(std::move(piece));
	reorderLayers();
	loaded = true;
	notifyBars(true);
	notifyEditor(true);
	return created;
}

void duplicatePiece(Piece *source)
{
	auto copy = std::make_unique<Piece>();
	copy->id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
	copy->name = source->name + " 2";
	copy->kind = source->kind;
	copy->file = source->file;
	copy->color = source->color;
	copy->bar[0] = source->bar[0];
	copy->bar[1] = source->bar[1];
	copy->pinned = source->pinned;
	copy->temporary = source->temporary;
	copy->seconds = source->seconds;
	copy->anim = source->anim;
	/* Começa no mesmo lugar da original. */
	obs_data_t *saved = obs_data_create();
	if (source->kind != KIND_LETTERBOX) {
		for (int out = 0; out < 2; out++) {
			if (source->items[out].empty()) {
				continue;
			}
			const char *suffix = out == OUT_WIDE ? "16" : "916";
			struct vec2 pos, bounds;
			obs_sceneitem_get_pos(source->items[out][0], &pos);
			obs_sceneitem_get_bounds(source->items[out][0], &bounds);
			obs_data_set_double(saved, (std::string("x") + suffix).c_str(), pos.x);
			obs_data_set_double(saved, (std::string("y") + suffix).c_str(), pos.y);
			obs_data_set_double(saved, (std::string("w") + suffix).c_str(), bounds.x);
			obs_data_set_double(saved, (std::string("h") + suffix).c_str(), bounds.y);
		}
	}
	buildPiece(copy.get(), saved);
	obs_data_release(saved);
	pieces.push_back(std::move(copy));
	reorderLayers();
	notifyBars(true);
	notifyEditor(true);
}

void deletePiece(const std::string &id)
{
	for (auto it = pieces.begin(); it != pieces.end(); ++it) {
		if ((*it)->id == id) {
			releasePiece(it->get());
			pieces.erase(it);
			break;
		}
	}
	notifyBars(true);
	notifyEditor(true);
}

/* Muda a peça "dragged" para antes (ou depois) de "target" (barra da mesa ou lista da tela). */
void movePiece(const std::string &dragged, const std::string &target, bool after)
{
	if (dragged == target) {
		return;
	}
	std::unique_ptr<Piece> moving;
	for (auto it = pieces.begin(); it != pieces.end(); ++it) {
		if ((*it)->id == dragged) {
			moving = std::move(*it);
			pieces.erase(it);
			break;
		}
	}
	if (!moving) {
		return;
	}
	auto where = pieces.end();
	for (auto it = pieces.begin(); it != pieces.end(); ++it) {
		if ((*it)->id == target) {
			where = after ? it + 1 : it;
			break;
		}
	}
	pieces.insert(where, std::move(moving));
	reorderLayers();
	notifyBars(true);
	notifyEditor(true);
}

/* Botão direito numa peça (na barra da mesa ou na lista da tela "No programa"). */
void pieceMenu(const std::string &id, const QPoint &where, QWidget *parent)
{
	Piece *piece = findPiece(id);
	if (!piece) {
		return;
	}
	QMenu menu(parent);
	QAction *toggle = menu.addAction(msg(piece->on ? "Artes.TurnOff" : "Artes.TurnOn"));
	QAction *edit = menu.addAction(msg("Artes.Edit"));
	QAction *rename = menu.addAction(msg("Artes.Rename"));
	QAction *duplicate = menu.addAction(msg("Artes.Duplicate"));
	QAction *remove = menu.addAction(msg("Artes.Delete"));
	menu.addSeparator();
	auto *kindGroup = new QActionGroup(&menu);
	QAction *fixed = menu.addAction(msg("Artes.Fixed"));
	QAction *temporary = menu.addAction(msg("Artes.Temporary"));
	for (QAction *action : {fixed, temporary}) {
		action->setCheckable(true);
		kindGroup->addAction(action);
	}
	(piece->temporary ? temporary : fixed)->setChecked(true);
	QAction *duration = menu.addAction(msg("Artes.Duration").arg(piece->seconds));
	duration->setEnabled(piece->temporary);
	QMenu *animMenu = menu.addMenu(msg("Artes.Animation"));
	auto *animGroup = new QActionGroup(&menu);
	QAction *fade = animMenu->addAction(msg("Artes.AnimFade"));
	QAction *none = animMenu->addAction(msg("Artes.AnimNone"));
	for (QAction *action : {fade, none}) {
		action->setCheckable(true);
		animGroup->addAction(action);
	}
	(piece->anim == ANIM_FADE ? fade : none)->setChecked(true);
	menu.addSeparator();
	QAction *pinned = menu.addAction(msg("Artes.Pinned"));
	pinned->setCheckable(true);
	pinned->setChecked(piece->pinned);

	QAction *chosen = menu.exec(where);
	piece = findPiece(id); /* a peça pode ter sumido enquanto o menu estava aberto */
	if (!chosen || !piece) {
		return;
	}
	if (chosen == toggle) {
		setOn(piece, !piece->on);
	} else if (chosen == edit) {
		openEditorFor(parent, id);
	} else if (chosen == rename) {
		bool ok = false;
		QString text = QInputDialog::getText(parent, msg("Artes.Rename"), msg("Artes.RenamePrompt"),
						     QLineEdit::Normal, QString::fromStdString(piece->name), &ok)
				       .trimmed();
		piece = findPiece(id);
		if (ok && !text.isEmpty() && piece) {
			piece->name = text.toUtf8().constData();
			notifyBars(true);
			notifyEditor(true);
		}
	} else if (chosen == duplicate) {
		duplicatePiece(piece);
	} else if (chosen == remove) {
		QString text = msg("Artes.DeleteConfirm").arg(QString::fromStdString(piece->name));
		if (QMessageBox::question(parent, msg("Artes.Delete"), text) == QMessageBox::Yes) {
			deletePiece(id);
		}
	} else if (chosen == fixed || chosen == temporary) {
		piece->temporary = chosen == temporary;
		if (piece->on) {
			setOn(piece, true); /* recomeça a conta (ou para de contar) */
		}
		notifyBars(false);
	} else if (chosen == duration) {
		bool ok = false;
		int seconds = QInputDialog::getInt(parent, msg("Artes.DurationTitle"), msg("Artes.DurationPrompt"),
						   piece->seconds, 1, 600, 1, &ok);
		piece = findPiece(id);
		if (ok && piece) {
			piece->seconds = seconds;
			notifyBars(false);
		}
	} else if (chosen == fade || chosen == none) {
		piece->anim = chosen == fade ? ANIM_FADE : ANIM_NONE;
		applyAnim(piece);
	} else if (chosen == pinned) {
		piece->pinned = pinned->isChecked();
		notifyBars(true);
		notifyEditor(true);
	}
}

/* "+": o que colocar. */
void addMenu(QWidget *parent, const QPoint &where)
{
	QMenu menu(parent);
	QAction *image = menu.addAction(msg("Artes.AddImage"));
	QAction *rect = menu.addAction(msg("Artes.AddRect"));
	QAction *letterbox = menu.addAction(msg("Artes.AddLetterbox"));
	QAction *chosen = menu.exec(where);
	int kind = chosen == image       ? KIND_IMAGE
		   : chosen == rect      ? KIND_RECT
		   : chosen == letterbox ? KIND_LETTERBOX
					 : -1;
	if (kind < 0) {
		return;
	}
	if (Piece *piece = createPiece(parent, kind)) {
		openEditorFor(parent, piece->id);
	}
}

/* ------------------------------------------------------------------------- */
/* A barra "Artes" da mesa                                                    */

/* Um botão de peça: contorno cinza desligada; vermelho com bolinha no ar; temporária no ar com a
 * barrinha do tempo que falta. Clique liga/desliga; botão direito abre o menu; arrastar reordena. */
class ArtButton : public QWidget {
public:
	ArtButton(QWidget *parent, const std::string &id) : QWidget(parent), id(id)
	{
		setCursor(Qt::PointingHandCursor);
		setAcceptDrops(true);
		setMinimumHeight(34);
		setToolTip(msg("Artes.ButtonHint"));
	}

	QSize sizeHint() const override
	{
		Piece *piece = findPiece(id);
		QFontMetrics metrics(font());
		int text = piece ? metrics.horizontalAdvance(QString::fromStdString(piece->name)) : 40;
		return QSize(text + 40, 34);
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		Piece *piece = findPiece(id);
		if (!piece) {
			return;
		}
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		QRectF box = QRectF(rect()).adjusted(1, 1, -1, -1);
		bool on = piece->on;
		QColor border = on ? QColor(SOMUN_RED) : QColor(SOMUN_LINE2);
		QColor text = on ? QColor(SOMUN_TEXT) : QColor(SOMUN_GRAY2);
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(SOMUN_PANEL2));
		painter.drawRoundedRect(box, 6, 6);
		/* Temporária no ar: a barrinha do tempo que falta, dentro do botão. */
		if (on && piece->offAt > 0) {
			double total = std::max<double>((double)(piece->offAt - piece->onAt), 1.0);
			double left = std::clamp((double)(piece->offAt - now()) / total, 0.0, 1.0);
			QRectF bar(box.left() + 3, box.bottom() - 5, (box.width() - 6) * left, 3);
			painter.setBrush(QColor(SOMUN_RED));
			painter.drawRoundedRect(bar, 1.5, 1.5);
		}
		painter.setBrush(Qt::NoBrush);
		painter.setPen(QPen(border, 1.5));
		painter.drawRoundedRect(box, 6, 6);
		int x = 12;
		if (on) {
			painter.setPen(Qt::NoPen);
			painter.setBrush(QColor(SOMUN_RED));
			painter.drawEllipse(QRectF(10, height() / 2.0 - 4, 8, 8));
			x = 24;
		}
		painter.setPen(text);
		painter.drawText(QRect(x, 0, width() - x - 8, height()), Qt::AlignVCenter | Qt::AlignLeft,
				 QString::fromStdString(piece->name));
	}

	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton) {
			pressPos = event->position().toPoint();
			pressing = true;
		}
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (pressing && (event->buttons() & Qt::LeftButton) &&
		    (event->position().toPoint() - pressPos).manhattanLength() >= QApplication::startDragDistance()) {
			pressing = false;
			auto *mime = new QMimeData();
			mime->setData(PIECE_MIME, QByteArray::fromStdString(id));
			auto *drag = new QDrag(this);
			drag->setMimeData(mime);
			drag->setPixmap(grab());
			drag->exec(Qt::MoveAction);
		}
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton && pressing) {
			pressing = false;
			if (Piece *piece = findPiece(id)) {
				setOn(piece, !piece->on);
			}
		}
	}

	void contextMenuEvent(QContextMenuEvent *event) override
	{
		std::string mine = id;
		QWidget *parent = window();
		/* Na fila: o menu pode refazer a barra (e apagar este botão). */
		QPoint where = event->globalPos();
		QTimer::singleShot(0, parent, [mine, where, parent]() { pieceMenu(mine, where, parent); });
		event->accept();
	}

	void dragEnterEvent(QDragEnterEvent *event) override
	{
		if (event->mimeData()->hasFormat(PIECE_MIME)) {
			event->acceptProposedAction();
		}
	}

	void dropEvent(QDropEvent *event) override
	{
		if (!event->mimeData()->hasFormat(PIECE_MIME)) {
			return;
		}
		std::string dragged = event->mimeData()->data(PIECE_MIME).toStdString();
		bool after = event->position().x() > width() / 2.0;
		std::string target = id;
		event->acceptProposedAction();
		QTimer::singleShot(0, [dragged, target, after]() { movePiece(dragged, target, after); });
	}

private:
	std::string id;
	QPoint pressPos;
	bool pressing = false;
};

class ArtsBar : public QWidget {
public:
	explicit ArtsBar(QWidget *parent) : QWidget(parent)
	{
		row = new QHBoxLayout(this);
		row->setContentsMargins(0, 0, 0, 0);
		row->setSpacing(8);
		auto *title = new QLabel(msg("Artes.Title"), this);
		title->setObjectName("somunPanelTitle");
		title->setToolTip(msg("Artes.TitleHint"));
		row->addWidget(title);
		plus = new QPushButton("+", this);
		plus->setFixedSize(34, 34);
		plus->setFocusPolicy(Qt::NoFocus);
		plus->setToolTip(msg("Artes.AddHint"));
		connect(plus, &QPushButton::clicked, this,
			[this]() { addMenu(window(), plus->mapToGlobal(QPoint(0, plus->height()))); });
		row->addWidget(plus);
		row->addStretch(1);
		bars.push_back(this);
		rebuild();
	}

	~ArtsBar() override { bars.erase(std::remove(bars.begin(), bars.end(), this), bars.end()); }

	/* Refaz os botões (peça nova, apagada, renomeada, alfinete ou ordem). */
	void rebuild()
	{
		for (ArtButton *button : buttons) {
			row->removeWidget(button);
			button->hide();
			button->deleteLater();
		}
		buttons.clear();
		int index = 1; /* depois do título */
		for (auto &piece : pieces) {
			if (!piece->pinned) {
				continue;
			}
			auto *button = new ArtButton(this, piece->id);
			row->insertWidget(index++, button);
			buttons.push_back(button);
		}
	}

	void repaintButtons()
	{
		for (ArtButton *button : buttons) {
			button->update();
		}
	}

private:
	QHBoxLayout *row = nullptr;
	QPushButton *plus = nullptr;
	std::vector<ArtButton *> buttons;
};

void notifyBars(bool structure)
{
	for (ArtsBar *bar : bars) {
		if (structure) {
			bar->rebuild();
		} else {
			bar->repaintButtons();
		}
	}
}

/* ------------------------------------------------------------------------- */
/* A tela "No programa"                                                       */

/* Um ponto no espaço da saída. */
vec2 point2(float x, float y)
{
	vec2 point;
	vec2_set(&point, x, y);
	return point;
}

/* Desenha linhas no espaço da saída (efeito "solid" do OBS). */
void drawLines(const std::vector<vec2> &points, bool closed, uint32_t rgba)
{
	if (points.size() < 2) {
		return;
	}
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	struct vec4 value;
	vec4_from_rgba(&value, rgba);
	gs_effect_set_vec4(color, &value);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(true);
		for (const vec2 &point : points) {
			gs_vertex2f(point.x, point.y);
		}
		if (closed) {
			gs_vertex2f(points[0].x, points[0].y);
		}
		gs_render_stop(GS_LINESTRIP);
	}
}

/* A moldura de um item (caixa da peça) no espaço da saída. */
std::vector<vec2> itemCorners(obs_sceneitem_t *item)
{
	struct matrix4 box;
	obs_sceneitem_get_box_transform(item, &box);
	std::vector<vec2> corners;
	const float xs[4] = {0.0f, 1.0f, 1.0f, 0.0f};
	const float ys[4] = {0.0f, 0.0f, 1.0f, 1.0f};
	for (int i = 0; i < 4; i++) {
		struct vec3 point, moved;
		vec3_set(&point, xs[i], ys[i], 0.0f);
		vec3_transform(&moved, &point, &box);
		struct vec2 corner;
		vec2_set(&corner, moved.x, moved.y);
		corners.push_back(corner);
	}
	return corners;
}

class EditorDialog;
QPointer<EditorDialog> editor;

/* Uma das duas saídas (16:9 ou 9:16): o programa por baixo, a camada por cima, a margem segura
 * tracejada e a moldura da peça escolhida. Arrastar move a peça nesta saída. */
class CanvasView : public QWidget {
public:
	CanvasView(QWidget *parent, int out) : QWidget(parent), out(out)
	{
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_StaticContents);
		setAttribute(Qt::WA_NoSystemBackground);
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_DontCreateNativeAncestors);
		setAttribute(Qt::WA_NativeWindow);
		setMinimumSize(120, 120);
		setCursor(Qt::OpenHandCursor);
		if (QWindow *window = windowHandle()) {
			window->installEventFilter(this);
			connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
				if (visible) {
					createDisplay();
					resizeDisplay();
				}
			});
		}
	}

	~CanvasView() override { destroyDisplay(); }

	QPaintEngine *paintEngine() const override { return nullptr; }

	void destroyDisplay()
	{
		if (display) {
			obs_display_remove_draw_callback(display, draw, this);
			obs_display_destroy(display);
			display = nullptr;
		}
	}

	std::string selected; /* a peça escolhida na tela (para arrastar) */

protected:
	void paintEvent(QPaintEvent *event) override
	{
		createDisplay();
		QWidget::paintEvent(event);
	}

	void resizeEvent(QResizeEvent *event) override
	{
		QWidget::resizeEvent(event);
		createDisplay();
		resizeDisplay();
	}

	void showEvent(QShowEvent *event) override
	{
		QWidget::showEvent(event);
		if (display) {
			obs_display_set_enabled(display, true);
		}
	}

	void hideEvent(QHideEvent *event) override
	{
		QWidget::hideEvent(event);
		if (display) {
			obs_display_set_enabled(display, false);
		}
	}

	bool eventFilter(QObject *object, QEvent *event) override
	{
		if (event->type() == QEvent::PlatformSurface) {
			auto *surface = static_cast<QPlatformSurfaceEvent *>(event);
			if (surface->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
				destroyDisplay();
			}
		}
		return QWidget::eventFilter(object, event);
	}

	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *) override { dragging.clear(); }

private:
	/* Ponto do mouse no espaço da saída. */
	QPointF toCanvas(const QPointF &pos) const
	{
		float w, h;
		canvasSize(out, w, h);
		double scale = std::min(width() / (double)w, height() / (double)h);
		double x0 = (width() - w * scale) / 2.0;
		double y0 = (height() - h * scale) / 2.0;
		return QPointF((pos.x() - x0) / scale, (pos.y() - y0) / scale);
	}

	static void draw(void *data, uint32_t cx, uint32_t cy)
	{
		auto *self = static_cast<CanvasView *>(data);
		int out = self->out;
		float w, h;
		canvasSize(out, w, h);
		float scale = std::min((float)cx / w, (float)cy / h);
		int vw = std::max((int)std::lround(w * scale), 1);
		int vh = std::max((int)std::lround(h * scale), 1);
		gs_projection_push();
		gs_viewport_push();
		gs_set_viewport(((int)cx - vw) / 2, ((int)cy - vh) / 2, vw, vh);
		gs_ortho(0.0f, w, 0.0f, h, -100.0f, 100.0f);

		/* 1) O programa por baixo (sem a camada). */
		if (out == OUT_WIDE) {
			obs_source_t *program = obs_get_output_source(0);
			if (program) {
				obs_source_video_render(program);
				obs_source_release(program);
			}
		} else {
			obs_canvas_t *canvas = pl_vertical_get_canvas();
			if (canvas) {
				obs_source_t *program = obs_canvas_get_channel(canvas, 0);
				if (program) {
					obs_source_video_render(program);
					obs_source_release(program);
				}
				obs_canvas_release(canvas);
			}
		}

		/* 2) As peças ligadas. */
		if (scenes[out]) {
			obs_source_video_render(obs_scene_get_source(scenes[out]));
		}

		/* 3) A peça escolhida, mesmo desligada (para dar para arrumar antes de ligar), e a moldura. */
		{
			std::lock_guard<std::mutex> lock(selectedMutex);
			for (obs_sceneitem_t *item : selectedItems[out]) {
				if (!obs_sceneitem_visible(item)) {
					struct matrix4 transform;
					obs_sceneitem_get_draw_transform(item, &transform);
					gs_matrix_push();
					gs_matrix_mul(&transform);
					obs_source_video_render(obs_sceneitem_get_source(item));
					gs_matrix_pop();
				}
				drawLines(itemCorners(item), true, 0xFFFFFFFF);
			}
		}

		/* 4) A margem segura (5%), tracejada. */
		float mx = w * 0.05f, my = h * 0.05f;
		float dash = std::max(w, h) / 80.0f;
		const float left = mx, right = w - mx, top = my, bottom = h - my;
		for (float x = left; x < right; x += dash * 2) {
			float x2 = std::min(x + dash, right);
			drawLines({point2(x, top), point2(x2, top)}, false, 0x80FFFFFF);
			drawLines({point2(x, bottom), point2(x2, bottom)}, false, 0x80FFFFFF);
		}
		for (float y = top; y < bottom; y += dash * 2) {
			float y2 = std::min(y + dash, bottom);
			drawLines({point2(left, y), point2(left, y2)}, false, 0x80FFFFFF);
			drawLines({point2(right, y), point2(right, y2)}, false, 0x80FFFFFF);
		}

		gs_viewport_pop();
		gs_projection_pop();
	}

	QSize pixelSize() const
	{
		qreal ratio = devicePixelRatioF();
		return QSize((int)(width() * ratio), (int)(height() * ratio));
	}

	void createDisplay()
	{
		if (display || !windowHandle() || !windowHandle()->isExposed()) {
			return;
		}
		QSize size = pixelSize();
		gs_init_data info = {};
		info.cx = (uint32_t)std::max(size.width(), 1);
		info.cy = (uint32_t)std::max(size.height(), 1);
		info.format = GS_BGRA;
		info.zsformat = GS_ZS_NONE;
#ifdef _WIN32
		info.window.hwnd = (void *)windowHandle()->winId();
#elif defined(__APPLE__)
		info.window.view = (id)windowHandle()->winId();
#else
		if (obs_get_nix_platform() != OBS_NIX_PLATFORM_X11_EGL) {
			return;
		}
		info.window.id = (uint32_t)windowHandle()->winId();
		info.window.display = obs_get_nix_platform_display();
#endif
		display = obs_display_create(&info, 0xFF141414);
		if (display) {
			obs_display_add_draw_callback(display, draw, this);
		}
	}

	void resizeDisplay()
	{
		if (display && isVisible()) {
			QSize size = pixelSize();
			obs_display_resize(display, (uint32_t)std::max(size.width(), 1),
					   (uint32_t)std::max(size.height(), 1));
		}
	}

	int out = OUT_WIDE;
	obs_display_t *display = nullptr;
	std::string dragging;
	QPointF dragStart;
	vec2 itemStart = {};
};

/* A tela "No programa": as duas saídas lado a lado e, do lado, o Look (leva 2), a lista de peças com o
 * alfinete "à mão" e o "+ adicionar". */
class EditorDialog : public QDialog {
public:
	explicit EditorDialog(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle(msg("Artes.EditorTitle"));
		setObjectName("somunWindow");
		setAttribute(Qt::WA_StyledBackground, true);
		setStyleSheet(somun_window_style());
		resize(1300, 640);

		auto *outer = new QHBoxLayout(this);
		outer->setContentsMargins(14, 12, 14, 14);
		outer->setSpacing(14);

		/* As duas saídas, cada uma com o seu "menor | maior". */
		for (int out = 0; out < 2; out++) {
			auto *column = new QVBoxLayout();
			auto *label = new QLabel(msg(out == OUT_WIDE ? "Artes.Wide" : "Artes.Tall"), this);
			label->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY2));
			column->addWidget(label);
			views[out] = new CanvasView(this, out);
			column->addWidget(views[out], 1);
			auto *sizeRow = new QHBoxLayout();
			sizeRow->addStretch(1);
			auto *smaller = new QPushButton(msg("Artes.Smaller"), this);
			auto *bigger = new QPushButton(msg("Artes.Bigger"), this);
			for (QPushButton *button : {smaller, bigger}) {
				button->setFocusPolicy(Qt::NoFocus);
				button->setMinimumWidth(90);
				sizeRow->addWidget(button);
			}
			sizeRow->addStretch(1);
			column->addLayout(sizeRow);
			connect(smaller, &QPushButton::clicked, this, [this, out]() { resizeSelected(out, false); });
			connect(bigger, &QPushButton::clicked, this, [this, out]() { resizeSelected(out, true); });
			outer->addLayout(column, out == OUT_WIDE ? 16 : 6);
		}

		/* Coluna do lado. */
		auto *side = new QVBoxLayout();
		side->setSpacing(8);
		auto *look = new QPushButton(msg("Artes.Look"), this);
		look->setEnabled(false);
		look->setToolTip(msg("Artes.LookSoon"));
		side->addWidget(look);
		auto *title = new QLabel(msg("Artes.Pieces"), this);
		title->setObjectName("somunPanelTitle");
		side->addWidget(title);
		list = new QListWidget(this);
		list->setDragDropMode(QAbstractItemView::InternalMove);
		list->setDefaultDropAction(Qt::MoveAction);
		list->setContextMenuPolicy(Qt::CustomContextMenu);
		list->setStyleSheet(QString("QListWidget { background: %1; border: 1px solid %2; border-radius: 8px; }"
					    "QListWidget::item:selected { background: %3; }")
					    .arg(QString(SOMUN_PANEL))
					    .arg(QString(SOMUN_LINE))
					    .arg(QString(SOMUN_PANEL2)));
		side->addWidget(list, 1);
		empty = new QLabel(msg("Artes.Empty"), this);
		empty->setWordWrap(true);
		empty->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY));
		side->addWidget(empty);
		auto *add = new QPushButton(msg("Artes.Add"), this);
		somun_make_primary(add);
		connect(add, &QPushButton::clicked, this,
			[this, add]() { addMenu(this, add->mapToGlobal(QPoint(0, add->height()))); });
		side->addWidget(add);
		auto *hint = new QLabel(msg("Artes.EditorHint"), this);
		hint->setWordWrap(true);
		hint->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY));
		side->addWidget(hint);
		auto *sideHolder = new QWidget(this);
		sideHolder->setLayout(side);
		side->setContentsMargins(0, 0, 0, 0);
		sideHolder->setFixedWidth(280);
		outer->addWidget(sideHolder);

		connect(list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *current) {
			if (current && !rebuilding) {
				select(current->data(Qt::UserRole).toString().toStdString());
			}
		});
		connect(list->model(), &QAbstractItemModel::rowsMoved, this, [this]() { applyListOrder(); });
		connect(list, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
			QListWidgetItem *row = list->itemAt(pos);
			if (row) {
				std::string id = row->data(Qt::UserRole).toString().toStdString();
				QPoint where = list->viewport()->mapToGlobal(pos);
				QTimer::singleShot(0, this, [this, id, where]() { pieceMenu(id, where, this); });
			}
		});
		rebuild();
	}

	~EditorDialog() override { setSelectedItems(std::string()); }

	/* Escolhe a peça (moldura nas duas saídas e linha da lista). */
	void select(const std::string &id)
	{
		selectedId = id;
		for (CanvasView *view : views) {
			view->selected = id;
		}
		setSelectedItems(id);
		for (int i = 0; i < list->count(); i++) {
			if (list->item(i)->data(Qt::UserRole).toString().toStdString() == id &&
			    list->currentRow() != i) {
				QSignalBlocker block(list);
				list->setCurrentRow(i);
			}
		}
	}

	/* Lista refeita (peça nova, apagada, renomeada, alfinete ou ordem). */
	void rebuild()
	{
		rebuilding = true;
		list->clear();
		/* A lista mostra a de cima (última camada) primeiro, como no OBS. */
		for (auto it = pieces.rbegin(); it != pieces.rend(); ++it) {
			Piece *piece = it->get();
			auto *row = new QListWidgetItem(list);
			row->setData(Qt::UserRole, QString::fromStdString(piece->id));
			auto *widget = new QWidget(list);
			auto *line = new QHBoxLayout(widget);
			line->setContentsMargins(8, 4, 8, 4);
			auto *dot = new QLabel(widget);
			dot->setFixedSize(8, 8);
			dot->setStyleSheet(
				QString("background: %1; border-radius: 4px;").arg(piece->on ? SOMUN_RED : "#3A3A3A"));
			line->addWidget(dot);
			auto *name = new QLabel(QString::fromStdString(piece->name), widget);
			line->addWidget(name, 1);
			auto *pin = new QCheckBox(msg("Artes.PinShort"), widget);
			pin->setChecked(piece->pinned);
			pin->setToolTip(msg("Artes.PinHint"));
			std::string id = piece->id;
			connect(pin, &QCheckBox::toggled, this, [id](bool on) {
				if (Piece *found = findPiece(id)) {
					found->pinned = on;
					notifyBars(true);
				}
			});
			line->addWidget(pin);
			row->setSizeHint(QSize(0, 34));
			list->setItemWidget(row, widget);
		}
		empty->setVisible(pieces.empty());
		rebuilding = false;
		if (!findPiece(selectedId)) {
			selectedId.clear();
		}
		select(selectedId);
	}

	/* Só as bolinhas de "no ar" mudaram. */
	void refreshDots() { rebuild(); }

private:
	void setSelectedItems(const std::string &id)
	{
		std::lock_guard<std::mutex> lock(selectedMutex);
		for (int out = 0; out < 2; out++) {
			for (obs_sceneitem_t *item : selectedItems[out]) {
				obs_sceneitem_release(item);
			}
			selectedItems[out].clear();
		}
		Piece *piece = findPiece(id);
		if (!piece) {
			return;
		}
		for (int out = 0; out < 2; out++) {
			for (obs_sceneitem_t *item : piece->items[out]) {
				obs_sceneitem_addref(item);
				selectedItems[out].push_back(item);
			}
		}
	}

	/* "menor | maior": muda o tamanho da peça escolhida nesta saída (letterbox: a altura das barras). */
	void resizeSelected(int out, bool bigger)
	{
		Piece *piece = findPiece(selectedId);
		if (!piece) {
			return;
		}
		if (piece->kind == KIND_LETTERBOX) {
			piece->bar[out] = std::clamp(piece->bar[out] + (bigger ? 1 : -1), 1, 45);
			layoutLetterbox(piece, out);
			return;
		}
		float factor = bigger ? 1.1f : 1.0f / 1.1f;
		for (obs_sceneitem_t *item : piece->items[out]) {
			struct vec2 pos, bounds;
			obs_sceneitem_get_pos(item, &pos);
			obs_sceneitem_get_bounds(item, &bounds);
			struct vec2 grown;
			vec2_set(&grown, std::max(bounds.x * factor, 8.0f), std::max(bounds.y * factor, 8.0f));
			/* Cresce e encolhe a partir do meio (não sai andando). */
			pos.x += (bounds.x - grown.x) / 2.0f;
			pos.y += (bounds.y - grown.y) / 2.0f;
			obs_sceneitem_set_bounds(item, &grown);
			obs_sceneitem_set_pos(item, &pos);
		}
	}

	/* A lista foi arrastada: a ordem nova vira a ordem das camadas. */
	void applyListOrder()
	{
		std::vector<std::unique_ptr<Piece>> ordered;
		for (int i = list->count() - 1; i >= 0; i--) {
			std::string id = list->item(i)->data(Qt::UserRole).toString().toStdString();
			for (auto &piece : pieces) {
				if (piece && piece->id == id) {
					ordered.push_back(std::move(piece));
					break;
				}
			}
		}
		for (auto &piece : pieces) {
			if (piece) {
				ordered.push_back(std::move(piece));
			}
		}
		pieces = std::move(ordered);
		reorderLayers();
		notifyBars(true);
	}

	CanvasView *views[2] = {};
	QListWidget *list = nullptr;
	QLabel *empty = nullptr;
	std::string selectedId;
	bool rebuilding = false;

	friend class CanvasView;
};

void CanvasView::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton) {
		return;
	}
	QPointF point = toCanvas(event->position());
	struct vec3 mouse;
	vec3_set(&mouse, (float)point.x(), (float)point.y(), 0.0f);
	/* A de cima primeiro (a última camada). */
	for (auto it = pieces.rbegin(); it != pieces.rend(); ++it) {
		Piece *piece = it->get();
		for (obs_sceneitem_t *item : piece->items[out]) {
			struct matrix4 box, inverse;
			obs_sceneitem_get_box_transform(item, &box);
			matrix4_inv(&inverse, &box);
			struct vec3 local;
			vec3_transform(&local, &mouse, &inverse);
			if (local.x < 0.0f || local.x > 1.0f || local.y < 0.0f || local.y > 1.0f) {
				continue;
			}
			if (editor) {
				editor->select(piece->id);
			}
			/* O letterbox não anda (fica colado em cima e embaixo): só muda de altura. */
			if (piece->kind != KIND_LETTERBOX) {
				dragging = piece->id;
				dragStart = point;
				obs_sceneitem_get_pos(item, &itemStart);
				setCursor(Qt::ClosedHandCursor);
			}
			return;
		}
	}
}

void CanvasView::mouseMoveEvent(QMouseEvent *event)
{
	Piece *piece = dragging.empty() ? nullptr : findPiece(dragging);
	if (!piece || piece->items[out].empty()) {
		return;
	}
	QPointF point = toCanvas(event->position());
	struct vec2 pos;
	vec2_set(&pos, itemStart.x + (float)(point.x() - dragStart.x()),
		 itemStart.y + (float)(point.y() - dragStart.y()));
	obs_sceneitem_set_pos(piece->items[out][0], &pos);
}

void notifyEditor(bool structure)
{
	if (!editor) {
		return;
	}
	if (structure) {
		editor->rebuild();
	} else {
		editor->refreshDots();
	}
}

void openEditorFor(QWidget *parent, const std::string &id)
{
	if (!editor) {
		editor = new EditorDialog(parent ? parent->window() : nullptr);
		editor->setAttribute(Qt::WA_DeleteOnClose);
	}
	if (!id.empty()) {
		editor->select(id);
	}
	editor->show();
	editor->raise();
	editor->activateWindow();
}

/* ------------------------------------------------------------------------- */

void shutdown()
{
	if (ticker) {
		ticker->stop();
	}
	if (editor) {
		delete editor.data();
	}
	/* Já foi solto (o OBS avisa o fim antes de descarregar o plugin). */
	if (!scenes[OUT_WIDE] && !scenes[OUT_TALL] && pieces.empty()) {
		return;
	}
	loaded = false;
	obs_set_output_source(CHANNEL, nullptr);
	obs_canvas_t *canvas = pl_vertical_get_canvas();
	if (canvas) {
		obs_canvas_set_channel(canvas, CHANNEL, nullptr);
		obs_canvas_release(canvas);
	}
	clearAll();
	for (obs_scene_t *&scene : scenes) {
		obs_scene_release(scene);
		scene = nullptr;
	}
}

void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		if (ticker && !ticker->isActive()) {
			ticker->start();
		}
	} else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP) {
		/* Trocando de projeto: o próximo traz as peças dele. */
		loaded = false;
		if (editor) {
			editor->close();
		}
		clearAll();
		notifyBars(true);
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		shutdown();
		/* O relógio sai com o Qt ainda de pé. */
		if (ticker) {
			ticker->deleteLater();
			ticker = nullptr;
		}
	}
}

} // namespace

void pl_artes_load(void)
{
	ticker = new QTimer();
	ticker->setInterval(50);
	QObject::connect(ticker, &QTimer::timeout, []() { tick(); });
	obs_frontend_add_event_callback(frontendEvent, nullptr);
	obs_frontend_add_save_callback(saveLoad, nullptr);
}

void pl_artes_unload(void)
{
	obs_frontend_remove_save_callback(saveLoad, nullptr);
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
	shutdown();
}

QWidget *pl_artes_bar_create(QWidget *parent)
{
	return new ArtsBar(parent);
}

void pl_artes_open_editor(QWidget *parent)
{
	openEditorFor(parent, std::string());
}
