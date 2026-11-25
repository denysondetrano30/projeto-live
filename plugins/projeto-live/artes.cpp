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
 * Peças: Imagem (logo, QR code em PNG), Texto (lower third, desenhado num PNG com a Outfit), Retângulo,
 * Letterbox e Vídeo em repetição. O Look é um LUT nas transições do programa, por baixo das peças.
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
#include <QColorDialog>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QMetaObject>
#include <QSlider>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDialog>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QComboBox>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGridLayout>
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
#include <functional>
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

/* Onde a peça aparece. */
enum Where { WHERE_BOTH = 0, WHERE_WIDE = 1, WHERE_TALL = 2 };

const uint32_t ANIM_MS = 400;

const char *const SAVE_KEY = "somun_artes";
const char *const PIECE_MIME = "application/x-somun-arte";

QString msg(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

enum Kind { KIND_IMAGE = 0, KIND_RECT = 1, KIND_LETTERBOX = 2, KIND_TEXT = 3, KIND_VIDEO = 4 };
/* Deslizar: de onde a peça vem (a saída volta pelo mesmo lado). */
enum Anim {
	ANIM_FADE = 0,
	ANIM_NONE = 1,
	ANIM_FROM_LEFT = 2,
	ANIM_FROM_RIGHT = 3,
	ANIM_FROM_TOP = 4,
	ANIM_FROM_BOTTOM = 5
};

const char *const LOOK_NAME = "somun look";
const char *const LOOK_KEY = "somun_look";

struct Piece {
	std::string id;
	std::string name;
	int kind = KIND_IMAGE;
	std::string file;
	uint32_t color = 0xFF000000; /* ABGR, como o OBS guarda */
	int bar[2] = {12, 12};       /* letterbox: altura de cada barra, em % da altura da saída */
	bool pinned = true;          /* "à mão": aparece na barra da mesa */
	int where = WHERE_BOTH;      /* deitado e em pé, só deitado ou só em pé */
	bool behind = false;         /* em pé: fica atrás da câmera */
	bool temporary = false;
	int seconds = 10;
	int anim = ANIM_FADE;
	bool on = false;
	qint64 onAt = 0;
	qint64 offAt = 0;
	std::vector<obs_source_t *> sources;     /* donos */
	std::vector<obs_sceneitem_t *> items[2]; /* com referência */

	/* Texto (lower third): vira um PNG desenhado aqui, com a Outfit. */
	std::string line1;
	std::string line2;
	int size1 = 64;
	int size2 = 40;
	bool bold1 = true;
	bool bold2 = false;
	std::string textColor = "#F2F2F2";
	bool barOn = true;
	std::string barColor = "#141414";
	int barOpacity = 85;
	int imageW = 1;
	int imageH = 1;

	/* Tecla de atalho (aparece em Configurações > Teclas de atalho). */
	obs_hotkey_id hotkey = OBS_INVALID_HOTKEY_ID;
	std::string *hotkeyId = nullptr;
};

obs_scene_t *scenes[2] = {};
obs_scene_t *backScene = nullptr; /* em pé, atrás da câmera */
std::vector<std::unique_ptr<Piece>> pieces;
QTimer *ticker = nullptr;
bool loaded = false;

/* O que a tela "No programa" desenha mesmo desligado (a peça escolhida), lido na thread de vídeo. */
std::mutex selectedMutex;
std::vector<obs_sceneitem_t *> selectedItems[2];

/* O Look: um LUT no programa (fica por baixo das peças; não vale em entrada "vai limpa"). */
std::string lookFile;
bool lookOn = false;
int lookAmount = 100;
std::vector<obs_weak_source_t *> lookSources;

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
	if (!backScene) {
		backScene = obs_scene_create_private("somun artes fundo 9:16");
	}
}

bool showsIn(const Piece *piece, int out)
{
	return piece->where == WHERE_BOTH || (piece->where == WHERE_WIDE) == (out == OUT_WIDE);
}

obs_scene_t *sceneFor(const Piece *piece, int out)
{
	return out == OUT_TALL && piece->behind ? backScene : scenes[out];
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

obs_source_t *makeVideo(const std::string &file, const char *name)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_string(settings, "local_file", file.c_str());
	obs_data_set_bool(settings, "looping", true);
	obs_source_t *source = obs_source_create_private("ffmpeg_source", name, settings);
	obs_data_release(settings);
	if (source) {
		/* A animação de canto não entra no som da live. */
		obs_source_set_muted(source, true);
	}
	return source;
}

/* A transição de entrada ou de saída. Deslizar: entra vindo do lado escolhido e sai por ele. */
obs_source_t *makeTransition(int anim, bool show)
{
	if (anim == ANIM_FADE) {
		return obs_source_create_private("fade_transition", "somun esmaecer", nullptr);
	}
	const char *in = "up";
	const char *back = "down";
	if (anim == ANIM_FROM_LEFT) {
		in = "right";
		back = "left";
	} else if (anim == ANIM_FROM_RIGHT) {
		in = "left";
		back = "right";
	} else if (anim == ANIM_FROM_TOP) {
		in = "down";
		back = "up";
	}
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "direction", show ? in : back);
	obs_source_t *slide = obs_source_create_private("slide_transition", "somun deslizar", settings);
	obs_data_release(settings);
	return slide;
}

/* Esmaecer, deslizar (0,4 s) ou entrar seco, para cada item da peça. Cada item tem a sua transição. */
void applyAnim(Piece *piece)
{
	for (int out = 0; out < 2; out++) {
		for (obs_sceneitem_t *item : piece->items[out]) {
			for (int show = 0; show < 2; show++) {
				if (piece->anim != ANIM_NONE) {
					obs_source_t *transition = makeTransition(piece->anim, show == 1);
					obs_sceneitem_set_transition(item, show == 1, transition);
					obs_sceneitem_set_transition_duration(item, show == 1, ANIM_MS);
					obs_source_release(transition);
				} else {
					obs_sceneitem_set_transition(item, show == 1, nullptr);
				}
			}
		}
	}
}

obs_sceneitem_t *addItem(Piece *piece, int out, obs_source_t *source)
{
	obs_sceneitem_t *item = obs_scene_add(sceneFor(piece, out), source);
	if (!item) {
		return nullptr;
	}
	obs_sceneitem_addref(item);
	obs_sceneitem_set_visible(item, piece->on && showsIn(piece, out));
	piece->items[out].push_back(item);
	return item;
}

/* O arquivo PNG de uma peça de texto. */
std::string textImagePath(const std::string &id)
{
	char *dir = obs_module_config_path("artes");
	std::string path;
	if (dir) {
		os_mkdirs(dir);
		path = std::string(dir) + "/" + id + ".png";
		bfree(dir);
	}
	return path;
}

/* Desenha o lower third (até duas linhas e a barra de fundo) num PNG transparente. */
void renderText(Piece *piece)
{
	QString family = somun_font_family();
	QString first = QString::fromStdString(piece->line1);
	QString second = QString::fromStdString(piece->line2);
	QFont font1(family);
	font1.setPixelSize(std::max(piece->size1, 8));
	font1.setWeight(piece->bold1 ? QFont::ExtraBold : QFont::Normal);
	QFont font2(family);
	font2.setPixelSize(std::max(piece->size2, 8));
	font2.setWeight(piece->bold2 ? QFont::ExtraBold : QFont::Normal);
	QFontMetrics metrics1(font1);
	QFontMetrics metrics2(font2);
	bool hasSecond = !second.trimmed().isEmpty();
	int padX = std::max(piece->size1 * 6 / 10, 12);
	int padY = std::max(piece->size1 * 3 / 10, 8);
	int gap = hasSecond ? std::max(piece->size2 / 6, 2) : 0;
	int textW = std::max(metrics1.horizontalAdvance(first), hasSecond ? metrics2.horizontalAdvance(second) : 0);
	int width = std::max(textW, 40) + padX * 2;
	int height = metrics1.height() + (hasSecond ? gap + metrics2.height() : 0) + padY * 2;

	QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setRenderHint(QPainter::TextAntialiasing);
	if (piece->barOn) {
		QColor bar(QString::fromStdString(piece->barColor));
		bar.setAlpha(std::clamp(piece->barOpacity, 0, 100) * 255 / 100);
		painter.fillRect(image.rect(), bar);
	}
	painter.setPen(QColor(QString::fromStdString(piece->textColor)));
	painter.setFont(font1);
	painter.drawText(padX, padY + metrics1.ascent(), first);
	if (hasSecond) {
		painter.setFont(font2);
		painter.drawText(padX, padY + metrics1.height() + gap + metrics2.ascent(), second);
	}
	painter.end();

	piece->file = textImagePath(piece->id);
	image.save(QString::fromStdString(piece->file), "PNG");
	piece->imageW = width;
	piece->imageH = height;
}

/* O texto mudou: redesenha mantendo a escala e o canto de cima à esquerda em cada saída. */
void updateText(Piece *piece)
{
	int oldW = std::max(piece->imageW, 1);
	renderText(piece);
	if (!piece->sources.empty()) {
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "file", piece->file.c_str());
		obs_source_update(piece->sources[0], settings);
		obs_data_release(settings);
	}
	for (int out = 0; out < 2; out++) {
		for (obs_sceneitem_t *item : piece->items[out]) {
			struct vec2 bounds;
			obs_sceneitem_get_bounds(item, &bounds);
			float scale = bounds.x / (float)oldW;
			vec2_set(&bounds, (float)piece->imageW * scale, (float)piece->imageH * scale);
			obs_sceneitem_set_bounds(item, &bounds);
		}
	}
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
	if (piece->kind == KIND_TEXT) {
		bw = std::min((float)piece->imageW * w / (out == OUT_TALL ? 1350.0f : 1920.0f), w * 0.9f);
		bh = bw * (float)piece->imageH / (float)piece->imageW;
	}
	struct vec2 bounds;
	vec2_set(&bounds, bw, bh);
	obs_sceneitem_set_bounds_type(item, piece->kind == KIND_RECT ? OBS_BOUNDS_STRETCH : OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_bounds(item, &bounds);
	struct vec2 pos;
	if (piece->kind == KIND_RECT) {
		vec2_set(&pos, (w - bw) / 2.0f, (h - bh) / 2.0f);
	} else if (piece->kind == KIND_TEXT) {
		vec2_set(&pos, w * 0.05f, h * 0.95f - bh); /* lower third: embaixo, à esquerda */
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

void setOn(Piece *piece, bool on);

/* Tecla de atalho da peça: liga e desliga (vem de outra thread, por isso vai pela fila do Qt). */
void hotkeyPressed(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed || !data) {
		return;
	}
	std::string id = *static_cast<std::string *>(data);
	QMetaObject::invokeMethod(
		qApp,
		[id]() {
			if (Piece *piece = findPiece(id)) {
				setOn(piece, !piece->on);
			}
		},
		Qt::QueuedConnection);
}

std::string hotkeyDescription(const Piece *piece)
{
	return msg("Artes.HotkeyName").arg(QString::fromStdString(piece->name)).toUtf8().constData();
}

void registerHotkey(Piece *piece, obs_data_t *saved)
{
	piece->hotkeyId = new std::string(piece->id);
	std::string name = "somun.arte." + piece->id;
	piece->hotkey = obs_hotkey_register_frontend(name.c_str(), hotkeyDescription(piece).c_str(), hotkeyPressed,
						     piece->hotkeyId);
	if (saved) {
		obs_data_array_t *keys = obs_data_get_array(saved, "tecla");
		if (keys) {
			obs_hotkey_load(piece->hotkey, keys);
			obs_data_array_release(keys);
		}
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
		if (piece->kind == KIND_TEXT) {
			renderText(piece);
		}
		obs_source_t *source = nullptr;
		if (piece->kind == KIND_IMAGE || piece->kind == KIND_TEXT) {
			source = makeImage(piece->file, piece->name.c_str());
		} else if (piece->kind == KIND_VIDEO) {
			source = makeVideo(piece->file, piece->name.c_str());
		} else {
			source = makeColor(piece->color, 400, 225, piece->name.c_str());
		}
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
	registerHotkey(piece, saved);
}

void releasePiece(Piece *piece)
{
	if (piece->hotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(piece->hotkey);
		piece->hotkey = OBS_INVALID_HOTKEY_ID;
	}
	delete piece->hotkeyId;
	piece->hotkeyId = nullptr;
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
	for (int layer = 0; layer < 3; layer++) {
		int out = layer == OUT_WIDE ? OUT_WIDE : OUT_TALL;
		bool behind = layer == 2;
		obs_scene_t *scene = behind ? backScene : scenes[out];
		std::vector<obs_sceneitem_t *> order;
		for (auto &piece : pieces) {
			if (out == OUT_TALL && piece->behind != behind) {
				continue;
			}
			for (obs_sceneitem_t *item : piece->items[out]) {
				order.push_back(item);
			}
		}
		if (!order.empty() && scene) {
			obs_scene_reorder_items(scene, order.data(), order.size());
		}
	}
}

/* Mostra ou esconde os itens conforme a peça está ligada e onde ela aparece. */
void applyVisible(Piece *piece)
{
	for (int out = 0; out < 2; out++) {
		bool visible = piece->on && showsIn(piece, out);
		for (obs_sceneitem_t *item : piece->items[out]) {
			if (obs_sceneitem_visible(item) != visible) {
				obs_sceneitem_set_visible(item, visible);
			}
		}
	}
}

/* Passa a peça em pé para trás da câmera (ou de volta), no mesmo lugar e tamanho. */
void moveTallLayer(Piece *piece, bool behind)
{
	if (piece->behind == behind) {
		return;
	}
	ensureScenes();
	std::vector<obs_sceneitem_t *> old = std::move(piece->items[OUT_TALL]);
	piece->items[OUT_TALL].clear();
	{
		std::lock_guard<std::mutex> lock(selectedMutex);
		auto &sel = selectedItems[OUT_TALL];
		for (obs_sceneitem_t *item : old) {
			auto found = std::find(sel.begin(), sel.end(), item);
			if (found != sel.end()) {
				obs_sceneitem_release(*found);
				sel.erase(found);
			}
		}
	}
	piece->behind = behind;
	for (obs_sceneitem_t *item : old) {
		struct obs_transform_info info = {};
		obs_sceneitem_get_info2(item, &info);
		obs_sceneitem_t *fresh = addItem(piece, OUT_TALL, obs_sceneitem_get_source(item));
		if (fresh) {
			obs_sceneitem_set_info2(fresh, &info);
		}
		obs_sceneitem_remove(item);
		obs_sceneitem_release(item);
	}
	applyAnim(piece);
	reorderLayers();
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
	applyVisible(piece);
	notifyBars(false);
	notifyEditor(false);
}

/* Põe (ou acerta) o filtro do Look numa fonte do programa. */
void ensureLook(obs_source_t *source, bool enabled)
{
	obs_source_t *filter = obs_source_get_filter_by_name(source, LOOK_NAME);
	double amount = lookAmount / 100.0;
	if (!filter) {
		if (!enabled) {
			return;
		}
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "image_path", lookFile.c_str());
		obs_data_set_double(settings, "clut_amount", amount);
		filter = obs_source_create_private("clut_filter", LOOK_NAME, settings);
		obs_data_release(settings);
		if (!filter) {
			return;
		}
		obs_source_filter_add(source, filter);
		lookSources.push_back(obs_source_get_weak_source(source));
	} else if (enabled) {
		obs_data_t *settings = obs_source_get_settings(filter);
		if (lookFile != obs_data_get_string(settings, "image_path") ||
		    std::abs(obs_data_get_double(settings, "clut_amount") - amount) > 0.001) {
			obs_data_set_string(settings, "image_path", lookFile.c_str());
			obs_data_set_double(settings, "clut_amount", amount);
			obs_source_update(filter, settings);
		}
		obs_data_release(settings);
	}
	if (obs_source_enabled(filter) != enabled) {
		obs_source_set_enabled(filter, enabled);
	}
	obs_source_release(filter);
}

/* O Look vai em todas as transições do programa (o corte troca de transição por um instante) e na
 * saída em pé. Entrada "vai limpa" fica sem ele. */
void applyLook(bool clean)
{
	if (lookFile.empty() && lookSources.empty()) {
		return;
	}
	bool enabled = loaded && lookOn && !clean && !lookFile.empty();
	struct obs_frontend_source_list transitions = {};
	obs_frontend_get_transitions(&transitions);
	for (size_t i = 0; i < transitions.sources.num; i++) {
		ensureLook(transitions.sources.array[i], enabled);
	}
	obs_frontend_source_list_free(&transitions);
	obs_canvas_t *canvas = pl_vertical_get_canvas();
	if (canvas) {
		/* Só no programa: a arte de trás não ganha a cor do Look. */
		obs_source_t *tall = obs_canvas_get_channel(canvas, PL_VERTICAL_PROGRAM_CHANNEL);
		if (tall) {
			ensureLook(tall, enabled);
			obs_source_release(tall);
		}
		obs_canvas_release(canvas);
	}
}

/* Tira o filtro do Look de tudo (trocando de projeto ou fechando). */
void clearLookFilters()
{
	for (obs_weak_source_t *weak : lookSources) {
		obs_source_t *source = obs_weak_source_get_source(weak);
		if (source) {
			obs_source_t *filter = obs_source_get_filter_by_name(source, LOOK_NAME);
			if (filter) {
				obs_source_filter_remove(source, filter);
				obs_source_release(filter);
			}
			obs_source_release(source);
		}
		obs_weak_source_release(weak);
	}
	lookSources.clear();
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
		obs_source_t *wantBack = loaded && !clean && backScene ? obs_scene_get_source(backScene) : nullptr;
		obs_source_t *currentBack = obs_canvas_get_channel(canvas, PL_VERTICAL_BACK_CHANNEL);
		if (currentBack != wantBack) {
			obs_canvas_set_channel(canvas, PL_VERTICAL_BACK_CHANNEL, wantBack);
		}
		obs_source_release(currentBack);
		obs_canvas_release(canvas);
	}
	applyLook(clean);
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
			obs_data_set_int(item, "onde", piece->where);
			obs_data_set_bool(item, "atras", piece->behind);
			obs_data_set_bool(item, "temporaria", piece->temporary);
			obs_data_set_int(item, "segundos", piece->seconds);
			obs_data_set_int(item, "animacao", piece->anim);
			obs_data_set_bool(item, "ligada", piece->on && !piece->temporary);
			obs_data_set_string(item, "linha1", piece->line1.c_str());
			obs_data_set_string(item, "linha2", piece->line2.c_str());
			obs_data_set_int(item, "tamanho1", piece->size1);
			obs_data_set_int(item, "tamanho2", piece->size2);
			obs_data_set_bool(item, "negrito1", piece->bold1);
			obs_data_set_bool(item, "negrito2", piece->bold2);
			obs_data_set_string(item, "cor_texto", piece->textColor.c_str());
			obs_data_set_bool(item, "barra_ligada", piece->barOn);
			obs_data_set_string(item, "cor_barra", piece->barColor.c_str());
			obs_data_set_int(item, "opacidade_barra", piece->barOpacity);
			if (piece->hotkey != OBS_INVALID_HOTKEY_ID) {
				obs_data_array_t *keys = obs_hotkey_save(piece->hotkey);
				obs_data_set_array(item, "tecla", keys);
				obs_data_array_release(keys);
			}
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
		obs_data_t *look = obs_data_create();
		obs_data_set_string(look, "arquivo", lookFile.c_str());
		obs_data_set_bool(look, "ligado", lookOn);
		obs_data_set_int(look, "forca", lookAmount);
		obs_data_set_obj(data, LOOK_KEY, look);
		obs_data_release(look);
		return;
	}

	clearAll();
	clearLookFilters();
	lookFile.clear();
	lookOn = false;
	lookAmount = 100;
	if (obs_data_t *look = obs_data_get_obj(data, LOOK_KEY)) {
		lookFile = obs_data_get_string(look, "arquivo");
		lookOn = obs_data_get_bool(look, "ligado");
		lookAmount = std::clamp((int)obs_data_get_int(look, "forca"), 0, 100);
		obs_data_release(look);
	}
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
		piece->where = std::clamp((int)obs_data_get_int(item, "onde"), 0, 2);
		piece->behind = obs_data_get_bool(item, "atras") && piece->where != WHERE_WIDE;
		piece->temporary = obs_data_get_bool(item, "temporaria");
		piece->seconds = std::max((int)obs_data_get_int(item, "segundos"), 1);
		piece->anim = (int)obs_data_get_int(item, "animacao");
		piece->on = obs_data_get_bool(item, "ligada");
		piece->line1 = obs_data_get_string(item, "linha1");
		piece->line2 = obs_data_get_string(item, "linha2");
		if (piece->kind == KIND_TEXT) {
			piece->size1 = std::clamp((int)obs_data_get_int(item, "tamanho1"), 16, 200);
			piece->size2 = std::clamp((int)obs_data_get_int(item, "tamanho2"), 16, 200);
			piece->bold1 = obs_data_get_bool(item, "negrito1");
			piece->bold2 = obs_data_get_bool(item, "negrito2");
			piece->textColor = obs_data_get_string(item, "cor_texto");
			piece->barOn = obs_data_get_bool(item, "barra_ligada");
			piece->barColor = obs_data_get_string(item, "cor_barra");
			piece->barOpacity = std::clamp((int)obs_data_get_int(item, "opacidade_barra"), 0, 100);
		}
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
	} else if (kind == KIND_VIDEO) {
		QString file = QFileDialog::getOpenFileName(parent, msg("Artes.ChooseVideo"), QString(),
							    msg("Artes.VideoFilter"));
		if (file.isEmpty()) {
			return nullptr;
		}
		piece->file = file.toUtf8().constData();
		piece->name = QFileInfo(file).completeBaseName().toUtf8().constData();
	} else if (kind == KIND_TEXT) {
		piece->name = msg("Artes.Text").toUtf8().constData();
		piece->line1 = msg("Artes.TextLine1").toUtf8().constData();
		piece->line2 = msg("Artes.TextLine2").toUtf8().constData();
		piece->anim = ANIM_FROM_LEFT;
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
	copy->where = source->where;
	copy->behind = source->behind;
	copy->temporary = source->temporary;
	copy->seconds = source->seconds;
	copy->anim = source->anim;
	copy->line1 = source->line1;
	copy->line2 = source->line2;
	copy->size1 = source->size1;
	copy->size2 = source->size2;
	copy->bold1 = source->bold1;
	copy->bold2 = source->bold2;
	copy->textColor = source->textColor;
	copy->barOn = source->barOn;
	copy->barColor = source->barColor;
	copy->barOpacity = source->barOpacity;
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
			bool text = (*it)->kind == KIND_TEXT;
			std::string file = (*it)->file;
			releasePiece(it->get());
			pieces.erase(it);
			if (text) {
				QFile::remove(QString::fromStdString(file));
			}
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
	QAction *fromLeft = animMenu->addAction(msg("Artes.AnimLeft"));
	QAction *fromRight = animMenu->addAction(msg("Artes.AnimRight"));
	QAction *fromTop = animMenu->addAction(msg("Artes.AnimTop"));
	QAction *fromBottom = animMenu->addAction(msg("Artes.AnimBottom"));
	QAction *none = animMenu->addAction(msg("Artes.AnimNone"));
	/* Na mesma ordem dos valores de Anim. */
	QAction *animActions[6] = {fade, none, fromLeft, fromRight, fromTop, fromBottom};
	for (QAction *action : animActions) {
		action->setCheckable(true);
		animGroup->addAction(action);
	}
	animActions[std::clamp(piece->anim, 0, 5)]->setChecked(true);
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
			if (piece->hotkey != OBS_INVALID_HOTKEY_ID) {
				obs_hotkey_set_description(piece->hotkey, hotkeyDescription(piece).c_str());
			}
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
	} else if (chosen == pinned) {
		piece->pinned = pinned->isChecked();
		notifyBars(true);
		notifyEditor(true);
	} else {
		for (int i = 0; i < 6; i++) {
			if (chosen == animActions[i]) {
				piece->anim = i;
				applyAnim(piece);
			}
		}
	}
}

/* "+": o que colocar. */
void addMenu(QWidget *parent, const QPoint &where)
{
	QMenu menu(parent);
	QAction *image = menu.addAction(msg("Artes.AddImage"));
	QAction *text = menu.addAction(msg("Artes.AddText"));
	QAction *rect = menu.addAction(msg("Artes.AddRect"));
	QAction *letterbox = menu.addAction(msg("Artes.AddLetterbox"));
	QAction *video = menu.addAction(msg("Artes.AddVideo"));
	QAction *chosen = menu.exec(where);
	int kind = -1;
	if (chosen == image) {
		kind = KIND_IMAGE;
	} else if (chosen == text) {
		kind = KIND_TEXT;
	} else if (chosen == rect) {
		kind = KIND_RECT;
	} else if (chosen == letterbox) {
		kind = KIND_LETTERBOX;
	} else if (chosen == video) {
		kind = KIND_VIDEO;
	}
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
	ArtButton(QWidget *parent, const std::string &id, bool big = false) : QWidget(parent), id(id), big(big)
	{
		setCursor(Qt::PointingHandCursor);
		setAcceptDrops(true);
		setMinimumHeight(big ? 56 : 34);
		if (big) {
			/* Grade da mesa: botão grande (com o culto rodando, alvo grande é menos erro). */
			setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
			setFixedHeight(56);
			QFont larger = font();
			larger.setPointSizeF(larger.pointSizeF() + 1.5);
			setFont(larger);
		}
		setToolTip(msg("Artes.ButtonHint"));
	}

	QSize sizeHint() const override
	{
		Piece *piece = findPiece(id);
		QFontMetrics metrics(font());
		int text = piece ? metrics.horizontalAdvance(QString::fromStdString(piece->name)) : 40;
		return QSize(text + 40, big ? 56 : 34);
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
		if (big) {
			/* Na grade, todos têm a bolinha: vermelha no ar, cinza desligada. */
			painter.setPen(Qt::NoPen);
			painter.setBrush(on ? QColor(SOMUN_RED) : QColor(SOMUN_LINE2));
			painter.drawEllipse(QRectF(18, height() / 2.0 - 5, 10, 10));
			x = 40;
		} else if (on) {
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
	bool big = false;
	QPoint pressPos;
	bool pressing = false;
};

/* O "+" da grade: um quadro tracejado do tamanho de um botão. */
class AddTile : public QWidget {
public:
	explicit AddTile(QWidget *parent) : QWidget(parent)
	{
		setCursor(Qt::PointingHandCursor);
		setFixedHeight(56);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		setToolTip(msg("Artes.AddHint"));
	}

	std::function<void()> onClick;

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		QPen pen(QColor(SOMUN_LINE2), 1.2, Qt::DashLine);
		painter.setPen(pen);
		painter.setBrush(Qt::NoBrush);
		painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 6, 6);
		painter.setPen(QColor(SOMUN_GRAY));
		QFont larger = font();
		larger.setPointSizeF(larger.pointSizeF() + 3);
		painter.setFont(larger);
		painter.drawText(rect(), Qt::AlignCenter, "+");
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()) && onClick) {
			onClick();
		}
	}
};

class ArtsBar : public QWidget {
public:
	explicit ArtsBar(QWidget *parent, bool asGrid = false) : QWidget(parent), asGrid(asGrid)
	{
		if (asGrid) {
			buildGrid();
			return;
		}
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

	/* A grade da mesa (desenho de 24/11): título "Artes" e os botões grandes, duas colunas, com o "+"
	 * tracejado no fim. */
	void buildGrid()
	{
		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(12, 10, 12, 12);
		outer->setSpacing(10);
		auto *title = new QLabel(msg("Artes.Title"), this);
		title->setObjectName("somunPanelTitle");
		title->setToolTip(msg("Artes.TitleHint"));
		outer->addWidget(title);
		grid = new QGridLayout();
		grid->setContentsMargins(0, 0, 0, 0);
		grid->setHorizontalSpacing(14);
		grid->setVerticalSpacing(12);
		grid->setColumnStretch(0, 1);
		grid->setColumnStretch(1, 1);
		outer->addLayout(grid);
		outer->addStretch(1);
		addTile = new AddTile(this);
		addTile->onClick = [this]() {
			addMenu(window(), addTile->mapToGlobal(QPoint(0, addTile->height())));
		};
		bars.push_back(this);
		rebuild();
	}

	/* Refaz os botões (peça nova, apagada, renomeada, alfinete ou ordem). */
	void rebuild()
	{
		if (asGrid) {
			for (ArtButton *button : buttons) {
				grid->removeWidget(button);
				button->hide();
				button->deleteLater();
			}
			buttons.clear();
			grid->removeWidget(addTile);
			int index = 0;
			for (auto &piece : pieces) {
				if (!piece->pinned) {
					continue;
				}
				auto *button = new ArtButton(this, piece->id, true);
				grid->addWidget(button, index / 2, index % 2);
				buttons.push_back(button);
				index++;
			}
			grid->addWidget(addTile, index / 2, index % 2);
			return;
		}
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
	bool asGrid = false;
	QHBoxLayout *row = nullptr;
	QGridLayout *grid = nullptr;
	AddTile *addTile = nullptr;
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
			if (backScene) {
				obs_source_video_render(obs_scene_get_source(backScene));
			}
			obs_canvas_t *canvas = pl_vertical_get_canvas();
			if (canvas) {
				obs_source_t *program = obs_canvas_get_channel(canvas, PL_VERTICAL_PROGRAM_CHANNEL);
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

/* Um fader: nome, barrinha de arrastar e o valor escrito do lado. */
class Fader : public QWidget {
public:
	Fader(QWidget *parent, const QString &title, const QString &unit) : QWidget(parent), unit(unit)
	{
		auto *row = new QHBoxLayout(this);
		row->setContentsMargins(0, 0, 0, 0);
		row->setSpacing(8);
		label = new QLabel(title, this);
		label->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY2));
		row->addWidget(label);
		slider = new QSlider(Qt::Horizontal, this);
		slider->setFocusPolicy(Qt::NoFocus);
		row->addWidget(slider, 1);
		value = new QLabel(this);
		value->setMinimumWidth(46);
		value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		row->addWidget(value);
		connect(slider, &QSlider::valueChanged, this, [this](int current) {
			showValue(current);
			if (changed) {
				changed(current);
			}
		});
	}

	/* Põe o valor sem avisar ninguém (ao escolher outra peça). */
	void setQuietly(int minimum, int maximum, int current)
	{
		QSignalBlocker block(slider);
		slider->setRange(minimum, maximum);
		slider->setValue(current);
		showValue(slider->value());
	}

	void setTitle(const QString &title) { label->setText(title); }

	int current() const { return slider->value(); }

	std::function<void(int)> changed;

private:
	void showValue(int current) { value->setText(QString::number(current) + unit); }

	QString unit;
	QLabel *label = nullptr;
	QSlider *slider = nullptr;
	QLabel *value = nullptr;
};

/* Um botãozinho que mostra a cor escolhida e abre o seletor. */
class ColorButton : public QPushButton {
public:
	explicit ColorButton(QWidget *parent) : QPushButton(parent)
	{
		setFixedSize(44, 24);
		setFocusPolicy(Qt::NoFocus);
		connect(this, &QPushButton::clicked, this, [this]() {
			QColor chosen = QColorDialog::getColor(QColor(color), window());
			if (chosen.isValid()) {
				setColor(chosen.name());
				if (changed) {
					changed(color);
				}
			}
		});
	}

	void setColor(const QString &value)
	{
		color = value;
		setStyleSheet(QString("QPushButton { background: %1; border: 1px solid %2; border-radius: 4px; }")
				      .arg(color)
				      .arg(QString(SOMUN_LINE2)));
	}

	QString current() const { return color; }

	std::function<void(const QString &)> changed;

private:
	QString color = "#FFFFFF";
};

/* A tela "No programa": as duas saídas lado a lado e, do lado, o Look, a lista de peças com o
 * alfinete "à mão", os campos do texto (quando a peça é de texto) e o "+ adicionar". */
class EditorDialog : public QDialog {
public:
	explicit EditorDialog(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle(msg("Artes.EditorTitle"));
		setObjectName("somunWindow");
		setAttribute(Qt::WA_StyledBackground, true);
		setStyleSheet(somun_window_style() +
			      QString("QSlider::groove:horizontal { height: 4px; background: %1; border-radius: 2px; }"
				      "QSlider::sub-page:horizontal { background: %2; border-radius: 2px; }"
				      "QSlider::handle:horizontal { background: %3; width: 14px; height: 14px;"
				      " margin: -5px 0; border-radius: 7px; }"
				      "QLineEdit { background: %4; border: 1px solid %5; border-radius: 6px;"
				      " padding: 4px 6px; }")
				      .arg(QString(SOMUN_LINE2))
				      .arg(QString(SOMUN_GRAY2))
				      .arg(QString(SOMUN_WHITE))
				      .arg(QString(SOMUN_BLACK))
				      .arg(QString(SOMUN_LINE2)));
		resize(1400, 800);

		auto *outer = new QHBoxLayout(this);
		outer->setContentsMargins(14, 12, 14, 14);
		outer->setSpacing(14);

		/* As duas saídas, cada uma com o seu fader de tamanho. */
		for (int out = 0; out < 2; out++) {
			auto *column = new QVBoxLayout();
			auto *label = new QLabel(msg(out == OUT_WIDE ? "Artes.Wide" : "Artes.Tall"), this);
			label->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY2));
			column->addWidget(label);
			views[out] = new CanvasView(this, out);
			column->addWidget(views[out], 1);
			sizeFaders[out] = new Fader(this, msg("Artes.Size"), "%");
			sizeFaders[out]->changed = [this, out](int value) {
				resizeSelected(out, value);
			};
			column->addWidget(sizeFaders[out]);
			outer->addLayout(column, out == OUT_WIDE ? 16 : 6);
		}

		/* Coluna do lado. */
		auto *side = new QVBoxLayout();
		side->setSpacing(8);
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
		buildWherePanel(side);
		buildTextPanel(side);
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
		sideHolder->setFixedWidth(310);
		outer->addWidget(sideHolder);

		/* O texto é redesenhado logo depois de parar de digitar ou de mexer no fader. */
		textTimer.setSingleShot(true);
		textTimer.setInterval(120);
		connect(&textTimer, &QTimer::timeout, this, [this]() { applyTextPanel(); });

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
		bool changedPiece = id != selectedId || !panelsLoaded;
		if (changedPiece && textTimer.isActive()) {
			textTimer.stop();
			applyTextPanel(); /* não perde o que acabou de digitar na peça anterior */
		}
		selectedId = id;
		for (CanvasView *view : views) {
			view->selected = id;
		}
		setSelectedItems(id);
		if (changedPiece) {
			panelsLoaded = true;
			loadTextPanel();
		}
		loadWherePanel();
		syncSizeFaders();
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
			panelsLoaded = false; /* a peça foi apagada: os campos do texto somem */
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
			if (!showsIn(piece, out)) {
				continue;
			}
			for (obs_sceneitem_t *item : piece->items[out]) {
				obs_sceneitem_addref(item);
				selectedItems[out].push_back(item);
			}
		}
	}

	/* Onde a peça aparece e, em pé, se fica atrás da câmera. */
	void buildWherePanel(QVBoxLayout *side)
	{
		wherePanel = new QWidget(this);
		auto *layout = new QVBoxLayout(wherePanel);
		layout->setContentsMargins(0, 4, 0, 4);
		layout->setSpacing(6);
		auto *row = new QHBoxLayout();
		auto *label = new QLabel(msg("Artes.Where"), wherePanel);
		label->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY2));
		row->addWidget(label);
		whereBox = new QComboBox(wherePanel);
		whereBox->setFocusPolicy(Qt::NoFocus);
		whereBox->addItem(msg("Artes.WhereBoth"), WHERE_BOTH);
		whereBox->addItem(msg("Artes.WhereWide"), WHERE_WIDE);
		whereBox->addItem(msg("Artes.WhereTall"), WHERE_TALL);
		row->addWidget(whereBox, 1);
		layout->addLayout(row);
		behindCheck = new QCheckBox(msg("Artes.Behind"), wherePanel);
		behindCheck->setFocusPolicy(Qt::NoFocus);
		behindCheck->setToolTip(msg("Artes.BehindHint"));
		layout->addWidget(behindCheck);
		connect(whereBox, &QComboBox::currentIndexChanged, this, [this](int index) {
			Piece *piece = findPiece(selectedId);
			if (!piece || index < 0) {
				return;
			}
			if (index == WHERE_WIDE) {
				moveTallLayer(piece, false);
			}
			piece->where = index;
			applyVisible(piece);
			select(selectedId);
			setSelectedItems(selectedId);
		});
		connect(behindCheck, &QCheckBox::toggled, this, [this](bool on) {
			Piece *piece = findPiece(selectedId);
			if (piece) {
				moveTallLayer(piece, on);
				setSelectedItems(selectedId);
			}
		});
		wherePanel->setVisible(false);
		side->addWidget(wherePanel);
	}

	void loadWherePanel()
	{
		Piece *piece = findPiece(selectedId);
		wherePanel->setVisible(piece != nullptr);
		if (!piece) {
			return;
		}
		QSignalBlocker blockWhere(whereBox);
		QSignalBlocker blockBehind(behindCheck);
		whereBox->setCurrentIndex(piece->where);
		behindCheck->setChecked(piece->behind);
		behindCheck->setEnabled(piece->where != WHERE_WIDE);
	}

	/* Os campos do lower third (só aparecem com uma peça de texto escolhida). */
	void buildTextPanel(QVBoxLayout *side)
	{
		textPanel = new QWidget(this);
		auto *layout = new QVBoxLayout(textPanel);
		layout->setContentsMargins(0, 4, 0, 4);
		layout->setSpacing(6);
		auto *title = new QLabel(msg("Artes.TextTitle"), textPanel);
		title->setObjectName("somunPanelTitle");
		layout->addWidget(title);
		for (int i = 0; i < 2; i++) {
			auto *line = new QHBoxLayout();
			lineEdits[i] = new QLineEdit(textPanel);
			lineEdits[i]->setPlaceholderText(msg(i == 0 ? "Artes.TextLine1" : "Artes.TextLine2"));
			line->addWidget(lineEdits[i], 1);
			boldChecks[i] = new QCheckBox(msg("Artes.Bold"), textPanel);
			boldChecks[i]->setFocusPolicy(Qt::NoFocus);
			line->addWidget(boldChecks[i]);
			layout->addLayout(line);
			lineFaders[i] = new Fader(textPanel, msg("Artes.Size"), "");
			layout->addWidget(lineFaders[i]);
			lineFaders[i]->changed = [this](int) {
				textTimer.start();
			};
			connect(lineEdits[i], &QLineEdit::textChanged, this, [this]() { textTimer.start(); });
			connect(boldChecks[i], &QCheckBox::toggled, this, [this]() { textTimer.start(); });
		}
		auto *colors = new QHBoxLayout();
		auto *colorLabel = new QLabel(msg("Artes.TextColor"), textPanel);
		colorLabel->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY2));
		colors->addWidget(colorLabel);
		textColorButton = new ColorButton(textPanel);
		textColorButton->changed = [this](const QString &) {
			textTimer.start();
		};
		colors->addWidget(textColorButton);
		colors->addStretch(1);
		barCheck = new QCheckBox(msg("Artes.TextBar"), textPanel);
		barCheck->setFocusPolicy(Qt::NoFocus);
		connect(barCheck, &QCheckBox::toggled, this, [this]() { textTimer.start(); });
		colors->addWidget(barCheck);
		barColorButton = new ColorButton(textPanel);
		barColorButton->changed = [this](const QString &) {
			textTimer.start();
		};
		colors->addWidget(barColorButton);
		layout->addLayout(colors);
		barFader = new Fader(textPanel, msg("Artes.TextOpacity"), "%");
		barFader->changed = [this](int) {
			textTimer.start();
		};
		layout->addWidget(barFader);
		textPanel->setVisible(false);
		side->addWidget(textPanel);
	}

	/* Põe nos campos os valores da peça de texto escolhida. */
	void loadTextPanel()
	{
		Piece *piece = findPiece(selectedId);
		bool isText = piece && piece->kind == KIND_TEXT;
		textPanel->setVisible(isText);
		if (!isText) {
			return;
		}
		for (int i = 0; i < 2; i++) {
			QSignalBlocker blockLine(lineEdits[i]);
			QSignalBlocker blockBold(boldChecks[i]);
			lineEdits[i]->setText(QString::fromStdString(i == 0 ? piece->line1 : piece->line2));
			boldChecks[i]->setChecked(i == 0 ? piece->bold1 : piece->bold2);
			lineFaders[i]->setQuietly(16, 200, i == 0 ? piece->size1 : piece->size2);
		}
		QSignalBlocker blockBar(barCheck);
		barCheck->setChecked(piece->barOn);
		textColorButton->setColor(QString::fromStdString(piece->textColor));
		barColorButton->setColor(QString::fromStdString(piece->barColor));
		barFader->setQuietly(0, 100, piece->barOpacity);
	}

	/* Leva os campos para a peça e redesenha. */
	void applyTextPanel()
	{
		Piece *piece = findPiece(selectedId);
		if (!piece || piece->kind != KIND_TEXT) {
			return;
		}
		piece->line1 = lineEdits[0]->text().toUtf8().constData();
		piece->line2 = lineEdits[1]->text().toUtf8().constData();
		piece->bold1 = boldChecks[0]->isChecked();
		piece->bold2 = boldChecks[1]->isChecked();
		piece->size1 = lineFaders[0]->current();
		piece->size2 = lineFaders[1]->current();
		piece->textColor = textColorButton->current().toUtf8().constData();
		piece->barOn = barCheck->isChecked();
		piece->barColor = barColorButton->current().toUtf8().constData();
		piece->barOpacity = barFader->current();
		updateText(piece);
		syncSizeFaders();
	}

	/* Os faders de tamanho mostram a peça escolhida em cada saída. */
	void syncSizeFaders()
	{
		Piece *piece = findPiece(selectedId);
		for (int out = 0; out < 2; out++) {
			bool here = piece && showsIn(piece, out);
			sizeFaders[out]->setEnabled(here);
			if (!here || piece->items[out].empty()) {
				sizeFaders[out]->setTitle(msg("Artes.Size"));
				sizeFaders[out]->setQuietly(2, 100, 2);
				continue;
			}
			if (piece->kind == KIND_LETTERBOX) {
				sizeFaders[out]->setTitle(msg("Artes.BarHeight"));
				sizeFaders[out]->setQuietly(1, 45, piece->bar[out]);
				continue;
			}
			float w, h;
			canvasSize(out, w, h);
			struct vec2 bounds;
			obs_sceneitem_get_bounds(piece->items[out][0], &bounds);
			sizeFaders[out]->setTitle(msg("Artes.Size"));
			sizeFaders[out]->setQuietly(2, 100, (int)std::lround(bounds.x * 100.0f / w));
		}
	}

	/* Fader de tamanho: largura da peça em % da saída, a partir do meio (letterbox: altura das barras). */
	void resizeSelected(int out, int value)
	{
		Piece *piece = findPiece(selectedId);
		if (!piece) {
			return;
		}
		if (piece->kind == KIND_LETTERBOX) {
			piece->bar[out] = std::clamp(value, 1, 45);
			layoutLetterbox(piece, out);
			return;
		}
		float w, h;
		canvasSize(out, w, h);
		for (obs_sceneitem_t *item : piece->items[out]) {
			struct vec2 pos, bounds;
			obs_sceneitem_get_pos(item, &pos);
			obs_sceneitem_get_bounds(item, &bounds);
			float factor = (w * (float)value / 100.0f) / std::max(bounds.x, 1.0f);
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
	Fader *sizeFaders[2] = {};
	QListWidget *list = nullptr;
	QLabel *empty = nullptr;
	std::string selectedId;
	bool rebuilding = false;
	bool panelsLoaded = false;

	QWidget *wherePanel = nullptr;
	QComboBox *whereBox = nullptr;
	QCheckBox *behindCheck = nullptr;

	QWidget *textPanel = nullptr;
	QLineEdit *lineEdits[2] = {};
	QCheckBox *boldChecks[2] = {};
	Fader *lineFaders[2] = {};
	ColorButton *textColorButton = nullptr;
	ColorButton *barColorButton = nullptr;
	QCheckBox *barCheck = nullptr;
	Fader *barFader = nullptr;
	QTimer textTimer;

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
		if (!showsIn(piece, out)) {
			continue;
		}
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
	clearLookFilters();
	obs_set_output_source(CHANNEL, nullptr);
	obs_canvas_t *canvas = pl_vertical_get_canvas();
	if (canvas) {
		obs_canvas_set_channel(canvas, CHANNEL, nullptr);
		obs_canvas_set_channel(canvas, PL_VERTICAL_BACK_CHANNEL, nullptr);
		obs_canvas_release(canvas);
	}
	clearAll();
	for (obs_scene_t *&scene : scenes) {
		obs_scene_release(scene);
		scene = nullptr;
	}
	obs_scene_release(backScene);
	backScene = nullptr;
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
		clearLookFilters();
		lookFile.clear();
		lookOn = false;
		lookAmount = 100;
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

QWidget *pl_artes_grid_create(QWidget *parent)
{
	return new ArtsBar(parent, true);
}

void pl_artes_open_editor(QWidget *parent)
{
	openEditorFor(parent, std::string());
}

/* O Look é configurado no modo Cor (color-mode.cpp). */
QString pl_look_file(void)
{
	return QString::fromStdString(lookFile);
}

bool pl_look_on(void)
{
	return lookOn;
}

int pl_look_amount(void)
{
	return lookAmount;
}

void pl_look_set_file(const QString &file)
{
	lookFile = file.toUtf8().constData();
}

void pl_look_set_on(bool on)
{
	lookOn = on;
}

void pl_look_set_amount(int amount)
{
	lookAmount = std::clamp(amount, 0, 100);
}
