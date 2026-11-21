/*
 * somun live: a janela 9:16 dentro da câmera.
 *
 * A câmera filma deitado e a live vertical é em pé. Cada câmera escolhe (botão direito da entrada >
 * "Enquadrar no vertical") uma de duas maneiras de encher a tela:
 * - Encher a tela: a janela 9:16 pega um pedaço da largura, sempre com a altura inteira da câmera. A
 *   câmera tem um lugar de descanso, escolhido arrastando;
 * - Inteira em cima do fundo: a imagem larga no meio e uma arte atrás (uma geral, que cada câmera
 *   pode trocar). Sem arte, o fundo é preto.
 *
 * Durante o culto, os botões (ou as teclas) deslizam a janela: um clique anda um pedacinho, segurar
 * continua andando, e "voltar ao lugar" desliza até o descanso. Ao cortar, a câmera que entra já está
 * no descanso: as que estão fora do ar voltam para lá na hora, sem ninguém ver.
 *
 * A guia é um retângulo claro em cima do No ar, mostrando que parte do 16:9 vai para o vertical.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "vertical-pan.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <QApplication>
#include <QButtonGroup>
#include <QDateTime>
#include <QDialog>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "somun-style.h"
#include "vertical-dock.h"
#include "vertical-record.h"
#include "vertical.h"

namespace {

const char *const FUNDO_ID = "somun_fundo_vertical";
const char *const KEY_FRAMED = "pl_v_enquadrado";
const char *const KEY_MODE = "pl_v_modo";
const char *const KEY_REST = "pl_v_descanso";
const char *const KEY_ART = "pl_v_arte";
const char *const PREF_ART = "arte_fundo";
const char *const PREF_GUIDE_OFF = "guia_oculta";
const char *const SAVE_HOTKEYS = "somun_vertical_teclas";

const int MODE_FILL = 0;
const int MODE_WHOLE = 1;

const double CLICK_STEP = 0.10; /* um clique: 10% do caminho */
const int GLIDE_MS = 380;       /* deslizar: menos de um segundo, com aceleração macia */
const int HOLD_AFTER_MS = 280;  /* segurou mais que isto: continua andando */
const double HOLD_SPEED = 0.22; /* caminho inteiro em uns 4,5 s */

QString msg(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

qint64 nowMs()
{
	return QDateTime::currentMSecsSinceEpoch();
}

/* ------------------------------------------------------------------------- */
/* A fonte da arte de fundo (uma imagem que não aparece em "Adicionar fonte")  */

struct Fundo {
	obs_source_t *image = nullptr;
};

const char *fundoName(void *)
{
	return "somun fundo vertical";
}

void fundoUpdate(void *data, obs_data_t *settings)
{
	auto *fundo = static_cast<Fundo *>(data);
	if (fundo->image) {
		obs_data_t *image = obs_data_create();
		obs_data_set_string(image, "file", obs_data_get_string(settings, "file"));
		obs_source_update(fundo->image, image);
		obs_data_release(image);
	}
}

void *fundoCreate(obs_data_t *settings, obs_source_t *)
{
	auto *fundo = new Fundo();
	fundo->image = obs_source_create_private("image_source", "somun fundo imagem", nullptr);
	fundoUpdate(fundo, settings);
	return fundo;
}

void fundoDestroy(void *data)
{
	auto *fundo = static_cast<Fundo *>(data);
	obs_source_release(fundo->image);
	delete fundo;
}

uint32_t fundoWidth(void *data)
{
	auto *fundo = static_cast<Fundo *>(data);
	uint32_t w = fundo->image ? obs_source_get_width(fundo->image) : 0;
	return w ? w : PL_VERTICAL_WIDTH;
}

uint32_t fundoHeight(void *data)
{
	auto *fundo = static_cast<Fundo *>(data);
	uint32_t h = fundo->image ? obs_source_get_height(fundo->image) : 0;
	return h ? h : PL_VERTICAL_HEIGHT;
}

void fundoRender(void *data, gs_effect_t *)
{
	auto *fundo = static_cast<Fundo *>(data);
	if (fundo->image) {
		obs_source_video_render(fundo->image);
	}
}

/* ------------------------------------------------------------------------- */
/* Câmera de cada entrada                                                     */

bool isCamera(obs_source_t *source)
{
	const char *id = source ? obs_source_get_unversioned_id(source) : nullptr;
	if (!id) {
		return false;
	}
	for (const char *known : {"projeto_live_phone_source", "dshow_input", "av_capture_input", "macos-avcapture",
				  "macos-avcapture-fast", "v4l2_input", "decklink-input", "aja_source", "ndi_source"}) {
		if (strcmp(id, known) == 0) {
			return true;
		}
	}
	return false;
}

struct FoundItem {
	obs_sceneitem_t *item = nullptr; /* com referência */
};

bool findCameraItem(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *found = static_cast<FoundItem *>(param);
	if (isCamera(obs_sceneitem_get_source(item))) {
		obs_sceneitem_addref(item);
		found->item = item;
		return false;
	}
	return true;
}

/* O item da câmera na entrada deitada (com referência; quem chama solta). */
obs_sceneitem_t *wideItem(obs_source_t *scene)
{
	FoundItem found;
	obs_scene_t *real = obs_scene_from_source(scene);
	if (real) {
		obs_scene_enum_items(real, findCameraItem, &found);
	}
	return found.item;
}

obs_source_t *cameraOf(obs_source_t *scene)
{
	obs_sceneitem_t *item = wideItem(scene);
	obs_source_t *camera = item ? obs_source_get_ref(obs_sceneitem_get_source(item)) : nullptr;
	obs_sceneitem_release(item);
	return camera;
}

struct TwinSearch {
	obs_source_t *camera = nullptr;
	obs_sceneitem_t *item = nullptr;
};

bool findTwinItem(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *search = static_cast<TwinSearch *>(param);
	if (strcmp(obs_source_get_unversioned_id(obs_sceneitem_get_source(item)), FUNDO_ID) == 0) {
		return true;
	}
	obs_source_t *original = pl_vertical_item_original(item);
	bool same = original == search->camera;
	obs_source_release(original);
	if (same) {
		obs_sceneitem_addref(item);
		search->item = item;
		return false;
	}
	return true;
}

/* O item da câmera na gêmea vertical da entrada (com referência). */
obs_sceneitem_t *tallItem(obs_source_t *scene, obs_source_t *camera)
{
	obs_source_t *twin = pl_vertical_twin_of(scene);
	TwinSearch search;
	search.camera = camera;
	if (twin) {
		obs_scene_enum_items(obs_scene_from_source(twin), findTwinItem, &search);
		obs_source_release(twin);
	}
	return search.item;
}

bool findFundoItem(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	if (strcmp(obs_source_get_unversioned_id(obs_sceneitem_get_source(item)), FUNDO_ID) == 0) {
		obs_sceneitem_addref(item);
		static_cast<FoundItem *>(param)->item = item;
		return false;
	}
	return true;
}

/* ------------------------------------------------------------------------- */
/* O que cada câmera guarda                                                   */

bool isFramed(obs_source_t *camera)
{
	obs_data_t *data = obs_source_get_private_settings(camera);
	bool framed = data && obs_data_get_bool(data, KEY_FRAMED);
	obs_data_release(data);
	return framed;
}

int modeOf(obs_source_t *camera)
{
	obs_data_t *data = obs_source_get_private_settings(camera);
	int mode = data ? (int)obs_data_get_int(data, KEY_MODE) : MODE_FILL;
	obs_data_release(data);
	return mode;
}

double restOf(obs_source_t *camera)
{
	obs_data_t *data = obs_source_get_private_settings(camera);
	double rest = data && obs_data_has_user_value(data, KEY_REST) ? obs_data_get_double(data, KEY_REST) : 0.5;
	obs_data_release(data);
	return std::clamp(rest, 0.0, 1.0);
}

std::string ownArt(obs_source_t *camera)
{
	obs_data_t *data = obs_source_get_private_settings(camera);
	std::string art = data ? obs_data_get_string(data, KEY_ART) : "";
	obs_data_release(data);
	return art;
}

std::string artFor(obs_source_t *camera)
{
	std::string art = ownArt(camera);
	if (art.empty()) {
		const char *general = pl_vertical_pref_string(PREF_ART);
		art = general ? general : "";
	}
	return art;
}

void setCameraValue(obs_source_t *camera, const char *key, int value)
{
	obs_data_t *data = obs_source_get_private_settings(camera);
	if (data) {
		obs_data_set_int(data, key, value);
		obs_data_set_bool(data, KEY_FRAMED, true);
		obs_data_release(data);
	}
}

void setCameraRest(obs_source_t *camera, double rest)
{
	obs_data_t *data = obs_source_get_private_settings(camera);
	if (data) {
		obs_data_set_double(data, KEY_REST, std::clamp(rest, 0.0, 1.0));
		obs_data_set_bool(data, KEY_FRAMED, true);
		obs_data_release(data);
	}
}

void setCameraArt(obs_source_t *camera, const std::string &file)
{
	obs_data_t *data = obs_source_get_private_settings(camera);
	if (data) {
		obs_data_set_string(data, KEY_ART, file.c_str());
		obs_data_release(data);
	}
}

/* ------------------------------------------------------------------------- */
/* Pôr a câmera no lugar                                                      */

/* Onde a janela está em cada entrada (0 = esquerda da câmera, 1 = direita). */
std::map<std::string, double> applied;

/* Posição do meio da imagem para a janela ficar em "position". */
float centerFor(float scaledW, double position)
{
	if (scaledW <= PL_VERTICAL_WIDTH) {
		return PL_VERTICAL_WIDTH / 2.0f;
	}
	return scaledW / 2.0f - (float)position * (scaledW - PL_VERTICAL_WIDTH);
}

float scaledWidth(obs_sceneitem_t *item)
{
	obs_source_t *source = obs_sceneitem_get_source(item);
	uint32_t sw = obs_source_get_width(source);
	uint32_t sh = obs_source_get_height(source);
	if (!sw || !sh) {
		return 0.0f;
	}
	return (float)sw * (float)PL_VERTICAL_HEIGHT / (float)sh;
}

/* Encher a tela: a imagem com a altura inteira, andando de lado (o que sobra fica fora da tela). */
void placeFill(obs_sceneitem_t *item, double position)
{
	float scaledW = scaledWidth(item);
	if (scaledW <= 0.0f) {
		return;
	}
	struct obs_transform_info info = {};
	obs_sceneitem_get_info2(item, &info);
	float x = centerFor(scaledW, position);
	bool shaped = info.bounds_type == OBS_BOUNDS_SCALE_INNER && !info.crop_to_bounds &&
		      info.alignment == OBS_ALIGN_CENTER && std::abs(info.bounds.x - scaledW) < 0.5f &&
		      std::abs(info.bounds.y - (float)PL_VERTICAL_HEIGHT) < 0.5f;
	if (shaped) {
		struct vec2 pos;
		vec2_set(&pos, x, PL_VERTICAL_HEIGHT / 2.0f);
		obs_sceneitem_set_pos(item, &pos);
		return;
	}
	info.pos.x = x;
	info.pos.y = PL_VERTICAL_HEIGHT / 2.0f;
	info.rot = 0.0f;
	info.scale.x = 1.0f;
	info.scale.y = 1.0f;
	info.alignment = OBS_ALIGN_CENTER;
	info.bounds_type = OBS_BOUNDS_SCALE_INNER;
	info.bounds_alignment = OBS_ALIGN_CENTER;
	info.bounds.x = scaledW;
	info.bounds.y = PL_VERTICAL_HEIGHT;
	info.crop_to_bounds = false;
	obs_sceneitem_set_info2(item, &info);
}

/* A arte de fundo na gêmea da entrada: aparece só no modo "Inteira". */
void syncFundo(obs_source_t *scene, obs_source_t *camera, bool whole)
{
	obs_source_t *twin = pl_vertical_twin_of(scene);
	if (!twin) {
		return;
	}
	FoundItem found;
	obs_scene_enum_items(obs_scene_from_source(twin), findFundoItem, &found);
	std::string art = whole ? artFor(camera) : std::string();
	if (!found.item && !art.empty()) {
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "file", art.c_str());
		std::string name = std::string("somun fundo ") + obs_source_get_name(scene);
		obs_source_t *fundo = obs_source_create(FUNDO_ID, name.c_str(), settings, nullptr);
		obs_data_release(settings);
		if (fundo) {
			obs_sceneitem_t *item = pl_vertical_add_extra_source(twin, fundo);
			if (item) {
				obs_sceneitem_addref(item);
				found.item = item;
				pl_vertical_fit_item(item, PL_FIT_FILL);
				obs_sceneitem_set_order(item, OBS_ORDER_MOVE_BOTTOM);
				obs_sceneitem_set_locked(item, true);
			}
			obs_source_release(fundo);
		}
	}
	if (found.item) {
		if (!art.empty()) {
			obs_source_t *fundo = obs_sceneitem_get_source(found.item);
			obs_data_t *current = obs_source_get_settings(fundo);
			if (art != obs_data_get_string(current, "file")) {
				obs_data_set_string(current, "file", art.c_str());
				obs_source_update(fundo, current);
			}
			obs_data_release(current);
		}
		bool show = !art.empty();
		if (obs_sceneitem_visible(found.item) != show) {
			obs_sceneitem_set_visible(found.item, show);
		}
		obs_sceneitem_release(found.item);
	}
	obs_source_release(twin);
}

/* Põe a janela da entrada nesse lugar, na hora. */
void placeNow(obs_source_t *scene, obs_source_t *camera, double position)
{
	position = std::clamp(position, 0.0, 1.0);
	obs_sceneitem_t *item = tallItem(scene, camera);
	if (item) {
		placeFill(item, position);
		obs_sceneitem_release(item);
		applied[obs_source_get_uuid(scene)] = position;
	}
}

/* Aplica o modo da câmera nesta entrada. */
void applyMode(obs_source_t *scene, obs_source_t *camera)
{
	bool whole = modeOf(camera) == MODE_WHOLE;
	if (whole) {
		obs_sceneitem_t *item = tallItem(scene, camera);
		if (item) {
			pl_vertical_fit_item(item, PL_FIT_WHOLE);
			obs_sceneitem_release(item);
		}
		applied.erase(std::string(obs_source_get_uuid(scene)));
	} else {
		placeNow(scene, camera, restOf(camera));
	}
	syncFundo(scene, camera, whole);
}

/* ------------------------------------------------------------------------- */
/* A guia em cima do No ar                                                    */

std::mutex guideMutex;
bool guideVisible = false;
float guideX = 0.0f, guideY = 0.0f, guideW = 0.0f, guideH = 0.0f;

void setGuide(bool visible, float x = 0.0f, float y = 0.0f, float w = 0.0f, float h = 0.0f)
{
	std::lock_guard<std::mutex> lock(guideMutex);
	guideVisible = visible;
	guideX = x;
	guideY = y;
	guideW = w;
	guideH = h;
}

/* Calcula a parte da imagem deitada que vai para o vertical. */
void updateGuide()
{
	obs_source_t *program = obs_frontend_get_current_scene();
	bool shown = false;
	if (program && !pl_vertical_pref(PREF_GUIDE_OFF) && pl_vertical_program_wanted()) {
		obs_sceneitem_t *wide = wideItem(program);
		obs_source_t *camera = wide ? obs_sceneitem_get_source(wide) : nullptr;
		if (wide && obs_sceneitem_visible(wide) && modeOf(camera) == MODE_FILL) {
			struct matrix4 box;
			obs_sceneitem_get_box_transform(wide, &box);
			struct vec3 corner0, corner1, p0, p1;
			vec3_set(&corner0, 0.0f, 0.0f, 0.0f);
			vec3_set(&corner1, 1.0f, 1.0f, 0.0f);
			vec3_transform(&p0, &corner0, &box);
			vec3_transform(&p1, &corner1, &box);
			float left = std::min(p0.x, p1.x), right = std::max(p0.x, p1.x);
			float top = std::min(p0.y, p1.y), bottom = std::max(p0.y, p1.y);
			float height = bottom - top;
			float window = height * 9.0f / 16.0f;
			if (right - left > window + 0.5f) {
				auto found = applied.find(obs_source_get_uuid(program));
				double position = found != applied.end() ? found->second : restOf(camera);
				setGuide(true, left + (float)position * (right - left - window), top, window, height);
				shown = true;
			}
		}
		obs_sceneitem_release(wide);
	}
	obs_source_release(program);
	if (!shown) {
		setGuide(false);
	}
}

/* ------------------------------------------------------------------------- */
/* Deslizar                                                                   */

struct Glide {
	double from = 0.0;
	double to = 0.0;
	qint64 startedAt = 0;
	bool active = false;
	int holdDirection = 0;
	qint64 pressedAt = 0;
	qint64 lastFrame = 0;
	std::string sceneUuid;
} glide;

QTimer *glideTimer = nullptr;
QTimer *houseTimer = nullptr;

double easeInOut(double t)
{
	return t < 0.5 ? 4.0 * t * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
}

/* A entrada no ar e a câmera dela, se der para deslizar (quem chama solta as duas). */
bool slidable(obs_source_t **scene, obs_source_t **camera)
{
	*scene = obs_frontend_get_current_scene();
	*camera = *scene ? cameraOf(*scene) : nullptr;
	if (!*camera || modeOf(*camera) != MODE_FILL) {
		obs_source_release(*camera);
		obs_source_release(*scene);
		*scene = nullptr;
		*camera = nullptr;
		return false;
	}
	return true;
}

double currentPosition(obs_source_t *scene, obs_source_t *camera)
{
	auto found = applied.find(obs_source_get_uuid(scene));
	return found != applied.end() ? found->second : restOf(camera);
}

void startGlide(obs_source_t *scene, obs_source_t *camera, double target)
{
	/* A primeira vez que mexe, a câmera passa a ser cuidada por aqui. */
	if (!isFramed(camera)) {
		setCameraRest(camera, restOf(camera));
	}
	glide.sceneUuid = obs_source_get_uuid(scene);
	glide.from = currentPosition(scene, camera);
	glide.to = std::clamp(target, 0.0, 1.0);
	glide.startedAt = nowMs();
	glide.lastFrame = glide.startedAt;
	glide.active = true;
	if (glideTimer && !glideTimer->isActive()) {
		glideTimer->start();
	}
}

void glideFrame()
{
	obs_source_t *scene = obs_frontend_get_current_scene();
	obs_source_t *camera = scene ? cameraOf(scene) : nullptr;
	bool sameScene = scene && glide.sceneUuid == obs_source_get_uuid(scene);
	if (!camera || !sameScene) {
		/* Cortou no meio: para de deslizar (a outra câmera já está no lugar dela). */
		glide.active = false;
		glide.holdDirection = 0;
		glideTimer->stop();
		obs_source_release(camera);
		obs_source_release(scene);
		return;
	}
	qint64 now = nowMs();
	double dt = (now - glide.lastFrame) / 1000.0;
	glide.lastFrame = now;
	if (glide.holdDirection != 0 && now - glide.pressedAt >= HOLD_AFTER_MS) {
		/* Segurando: anda em velocidade constante. */
		double position = currentPosition(scene, camera) + glide.holdDirection * HOLD_SPEED * dt;
		placeNow(scene, camera, position);
		glide.active = false;
	} else if (glide.active) {
		double t = std::clamp((now - glide.startedAt) / (double)GLIDE_MS, 0.0, 1.0);
		placeNow(scene, camera, glide.from + (glide.to - glide.from) * easeInOut(t));
		if (t >= 1.0) {
			glide.active = false;
		}
	}
	updateGuide();
	if (!glide.active && glide.holdDirection == 0) {
		glideTimer->stop();
	}
	obs_source_release(camera);
	obs_source_release(scene);
}

/* Apertou a seta: anda um pedacinho e, se continuar segurando, segue andando. */
void pressDirection(int direction)
{
	obs_source_t *scene = nullptr, *camera = nullptr;
	if (!slidable(&scene, &camera)) {
		return;
	}
	glide.holdDirection = direction;
	glide.pressedAt = nowMs();
	startGlide(scene, camera, currentPosition(scene, camera) + direction * CLICK_STEP);
	obs_source_release(camera);
	obs_source_release(scene);
}

void releaseDirection(int direction)
{
	if (glide.holdDirection == direction) {
		glide.holdDirection = 0;
	}
}

void backToRest()
{
	obs_source_t *scene = nullptr, *camera = nullptr;
	if (!slidable(&scene, &camera)) {
		return;
	}
	glide.holdDirection = 0;
	startGlide(scene, camera, restOf(camera));
	obs_source_release(camera);
	obs_source_release(scene);
}

/* De tempos em tempos: as câmeras fora do ar voltam ao descanso, e a guia acompanha. */
void houseKeeping()
{
	if (!pl_vertical_program_wanted()) {
		setGuide(false);
		return;
	}
	obs_source_t *program = obs_frontend_get_current_scene();
	obs_source_t *programCamera = program ? cameraOf(program) : nullptr;

	/* Durante a transição a câmera que está saindo ainda aparece: não mexe nela. */
	obs_source_t *transition = pl_vertical_get_transition();
	obs_source_t *incoming = transition ? obs_transition_get_source(transition, OBS_TRANSITION_SOURCE_B) : nullptr;
	bool transitioning = incoming != nullptr;
	obs_source_release(incoming);
	obs_source_release(transition);

	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_source_t *scene = scenes.sources.array[i];
		obs_source_t *camera = cameraOf(scene);
		if (!camera || !isFramed(camera)) {
			obs_source_release(camera);
			continue;
		}
		if (modeOf(camera) == MODE_WHOLE) {
			syncFundo(scene, camera, true);
		} else {
			bool onAir = scene == program;
			auto found = applied.find(obs_source_get_uuid(scene));
			if (found == applied.end()) {
				placeNow(scene, camera, restOf(camera));
			} else if (!onAir && !transitioning && camera != programCamera &&
				   std::abs(found->second - restOf(camera)) > 0.0001) {
				placeNow(scene, camera, restOf(camera));
			}
		}
		obs_source_release(camera);
	}
	obs_frontend_source_list_free(&scenes);
	obs_source_release(programCamera);
	obs_source_release(program);
	updateGuide();
}

/* ------------------------------------------------------------------------- */
/* Teclas de atalho                                                           */

obs_hotkey_id hotkeyLeft = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id hotkeyRight = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id hotkeyHome = OBS_INVALID_HOTKEY_ID;

void hotkeyDirection(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	int direction = (int)(intptr_t)data;
	QMetaObject::invokeMethod(
		qApp,
		[direction, pressed]() {
			if (pressed) {
				pressDirection(direction);
			} else {
				releaseDirection(direction);
			}
		},
		Qt::QueuedConnection);
}

void hotkeyBack(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed) {
		QMetaObject::invokeMethod(qApp, []() { backToRest(); }, Qt::QueuedConnection);
	}
}

void saveLoad(obs_data_t *data, bool saving, void *)
{
	const char *keys[3] = {"esquerda", "direita", "voltar"};
	obs_hotkey_id ids[3] = {hotkeyLeft, hotkeyRight, hotkeyHome};
	if (saving) {
		obs_data_t *all = obs_data_create();
		for (int i = 0; i < 3; i++) {
			obs_data_array_t *array = obs_hotkey_save(ids[i]);
			obs_data_set_array(all, keys[i], array);
			obs_data_array_release(array);
		}
		obs_data_set_obj(data, SAVE_HOTKEYS, all);
		obs_data_release(all);
		return;
	}
	applied.clear();
	obs_data_t *all = obs_data_get_obj(data, SAVE_HOTKEYS);
	if (all) {
		for (int i = 0; i < 3; i++) {
			obs_data_array_t *array = obs_data_get_array(all, keys[i]);
			if (array) {
				obs_hotkey_load(ids[i], array);
				obs_data_array_release(array);
			}
		}
		obs_data_release(all);
	}
}

void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		if (houseTimer) {
			houseTimer->start();
		}
	} else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP) {
		applied.clear();
		setGuide(false);
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		setGuide(false);
		if (houseTimer) {
			houseTimer->stop();
			houseTimer->deleteLater();
			houseTimer = nullptr;
		}
		if (glideTimer) {
			glideTimer->stop();
			glideTimer->deleteLater();
			glideTimer = nullptr;
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Botões embaixo da transição                                                */

class PanControls : public QWidget {
public:
	explicit PanControls(QWidget *parent) : QWidget(parent)
	{
		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(0, 8, 0, 0);
		outer->setSpacing(6);
		auto *title = new QLabel(msg("Pan.Title"), this);
		title->setAlignment(Qt::AlignCenter);
		title->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY));
		outer->addWidget(title);
		auto *arrows = new QHBoxLayout();
		arrows->setSpacing(6);
		left = arrowButton(QString::fromUtf8("◀"), -1);
		right = arrowButton(QString::fromUtf8("▶"), 1);
		arrows->addWidget(left);
		arrows->addWidget(right);
		outer->addLayout(arrows);
		home = new QPushButton(msg("Pan.Home"), this);
		home->setMinimumHeight(30);
		home->setFocusPolicy(Qt::NoFocus);
		home->setToolTip(msg("Pan.HomeHint"));
		connect(home, &QPushButton::clicked, this, []() { backToRest(); });
		outer->addWidget(home);
		guide = new QPushButton(msg("Pan.Guide"), this);
		guide->setMinimumHeight(26);
		guide->setCheckable(true);
		guide->setFocusPolicy(Qt::NoFocus);
		guide->setToolTip(msg("Pan.GuideHint"));
		guide->setStyleSheet(QString("QPushButton:checked { background: %1; color: %2; }")
					     .arg(QString(SOMUN_WHITE))
					     .arg(QString(SOMUN_BLACK)));
		guide->setChecked(!pl_vertical_pref(PREF_GUIDE_OFF));
		connect(guide, &QPushButton::toggled, this, [](bool on) {
			pl_vertical_set_pref(PREF_GUIDE_OFF, !on);
			updateGuide();
		});
		outer->addWidget(guide);

		auto *timer = new QTimer(this);
		timer->setInterval(300);
		connect(timer, &QTimer::timeout, this, [this]() { refresh(); });
		timer->start();
		refresh();
	}

private:
	QPushButton *arrowButton(const QString &text, int direction)
	{
		auto *button = new QPushButton(text, this);
		button->setMinimumSize(56, 40);
		button->setFocusPolicy(Qt::NoFocus);
		button->setToolTip(msg("Pan.ArrowHint"));
		connect(button, &QPushButton::pressed, this, [direction]() { pressDirection(direction); });
		connect(button, &QPushButton::released, this, [direction]() { releaseDirection(direction); });
		return button;
	}

	void refresh()
	{
		bool wanted = pl_vertical_program_wanted();
		if (isHidden() == wanted) {
			setVisible(wanted);
		}
		if (!wanted) {
			return;
		}
		obs_source_t *scene = nullptr, *camera = nullptr;
		bool can = slidable(&scene, &camera);
		obs_source_release(camera);
		obs_source_release(scene);
		left->setEnabled(can);
		right->setEnabled(can);
		home->setEnabled(can);
	}

	QPushButton *left = nullptr;
	QPushButton *right = nullptr;
	QPushButton *home = nullptr;
	QPushButton *guide = nullptr;
};

/* ------------------------------------------------------------------------- */
/* Janela "Enquadrar no vertical"                                             */

void drawRect(float x, float y, float w, float h, uint32_t rgba, bool filled)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	struct vec4 value;
	vec4_from_rgba(&value, rgba);
	gs_effect_set_vec4(color, &value);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(true);
		if (filled) {
			gs_vertex2f(x, y);
			gs_vertex2f(x + w, y);
			gs_vertex2f(x, y + h);
			gs_vertex2f(x + w, y + h);
			gs_render_stop(GS_TRISTRIP);
		} else {
			gs_vertex2f(x, y);
			gs_vertex2f(x + w, y);
			gs_vertex2f(x + w, y + h);
			gs_vertex2f(x, y + h);
			gs_vertex2f(x, y);
			gs_render_stop(GS_LINESTRIP);
		}
	}
}

/* A câmera inteira, com a janela 9:16 clara por cima; arrastar escolhe o lugar de descanso. */
class FrameView : public QWidget {
public:
	FrameView(QWidget *parent, obs_source_t *camera) : QWidget(parent)
	{
		weak = obs_source_get_weak_source(camera);
		position = restOf(camera);
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_StaticContents);
		setAttribute(Qt::WA_NoSystemBackground);
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_DontCreateNativeAncestors);
		setAttribute(Qt::WA_NativeWindow);
		setMinimumSize(480, 270);
		setCursor(Qt::SizeHorCursor);
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

	~FrameView() override
	{
		destroyDisplay();
		obs_weak_source_release(weak);
	}

	QPaintEngine *paintEngine() const override { return nullptr; }

	std::function<void(double)> moved;
	bool showWindow = true;

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

	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton) {
			moveTo(event->position().x());
		}
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (event->buttons() & Qt::LeftButton) {
			moveTo(event->position().x());
		}
	}

private:
	void moveTo(double mouseX)
	{
		if (!showWindow) {
			return;
		}
		obs_source_t *camera = obs_weak_source_get_source(weak);
		if (!camera) {
			return;
		}
		double sw = obs_source_get_width(camera);
		double sh = obs_source_get_height(camera);
		obs_source_release(camera);
		if (sw <= 0 || sh <= 0) {
			return;
		}
		double scale = std::min(width() / sw, height() / sh);
		double imageW = sw * scale;
		double imageX = (width() - imageW) / 2.0;
		double window = sh * scale * 9.0 / 16.0;
		if (imageW <= window) {
			return;
		}
		position = std::clamp((mouseX - window / 2.0 - imageX) / (imageW - window), 0.0, 1.0);
		if (moved) {
			moved(position);
		}
	}

	static void draw(void *data, uint32_t cx, uint32_t cy)
	{
		auto *self = static_cast<FrameView *>(data);
		obs_source_t *camera = obs_weak_source_get_source(self->weak);
		if (!camera) {
			return;
		}
		uint32_t sw = obs_source_get_width(camera);
		uint32_t sh = obs_source_get_height(camera);
		if (sw && sh && cx && cy) {
			float scale = std::min((float)cx / (float)sw, (float)cy / (float)sh);
			int w = std::max((int)std::lround(sw * scale), 1);
			int h = std::max((int)std::lround(sh * scale), 1);
			gs_projection_push();
			gs_viewport_push();
			gs_set_viewport(((int)cx - w) / 2, ((int)cy - h) / 2, w, h);
			gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
			obs_source_video_render(camera);
			float window = (float)sh * 9.0f / 16.0f;
			if (self->showWindow && (float)sw > window) {
				float x = (float)self->position * ((float)sw - window);
				gs_blend_state_push();
				gs_enable_blending(true);
				gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
				/* O que fica de fora escurece. */
				drawRect(0.0f, 0.0f, x, (float)sh, 0x99000000, true);
				drawRect(x + window, 0.0f, (float)sw - x - window, (float)sh, 0x99000000, true);
				gs_blend_state_pop();
				drawRect(x, 0.0f, window, (float)sh - 1.0f, 0xFFFFFFFF, false);
			}
			gs_viewport_pop();
			gs_projection_pop();
		}
		obs_source_release(camera);
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

	void destroyDisplay()
	{
		if (display) {
			obs_display_remove_draw_callback(display, draw, this);
			obs_display_destroy(display);
			display = nullptr;
		}
	}

	obs_weak_source_t *weak = nullptr;
	obs_display_t *display = nullptr;
	double position = 0.5;
};

class FrameDialog : public QDialog {
public:
	FrameDialog(QWidget *parent, obs_source_t *scene, obs_source_t *camera) : QDialog(parent)
	{
		weakScene = obs_source_get_weak_source(scene);
		weakCamera = obs_source_get_weak_source(camera);
		setAttribute(Qt::WA_DeleteOnClose);
		setObjectName("somunWindow");
		setAttribute(Qt::WA_StyledBackground, true);
		setStyleSheet(somun_window_style());
		setWindowTitle(msg("Pan.FrameTitle").arg(QString::fromUtf8(obs_source_get_name(scene))));
		resize(900, 680);

		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(14, 12, 14, 14);
		outer->setSpacing(10);

		auto *modes = new QHBoxLayout();
		fill = new QRadioButton(msg("Pan.ModeFill"), this);
		whole = new QRadioButton(msg("Pan.ModeWhole"), this);
		auto *group = new QButtonGroup(this);
		group->addButton(fill);
		group->addButton(whole);
		(modeOf(camera) == MODE_WHOLE ? whole : fill)->setChecked(true);
		modes->addWidget(fill);
		modes->addWidget(whole);
		modes->addStretch(1);
		outer->addLayout(modes);

		view = new FrameView(this, camera);
		outer->addWidget(view, 1);
		view->moved = [this](double position) {
			setRest(position);
		};

		hint = new QLabel(this);
		hint->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY2));
		hint->setWordWrap(true);
		outer->addWidget(hint);

		artBox = new QWidget(this);
		auto *art = new QHBoxLayout(artBox);
		art->setContentsMargins(0, 0, 0, 0);
		auto *generalTitle = new QLabel(msg("Pan.ArtGeneral"), artBox);
		generalTitle->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY2));
		art->addWidget(generalTitle);
		generalButton = new QPushButton(artBox);
		generalButton->setFocusPolicy(Qt::NoFocus);
		connect(generalButton, &QPushButton::clicked, this, [this]() { chooseArt(true); });
		art->addWidget(generalButton);
		art->addSpacing(16);
		auto *ownTitle = new QLabel(msg("Pan.ArtOwn"), artBox);
		ownTitle->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY2));
		art->addWidget(ownTitle);
		ownButton = new QPushButton(artBox);
		ownButton->setFocusPolicy(Qt::NoFocus);
		connect(ownButton, &QPushButton::clicked, this, [this]() { chooseArt(false); });
		art->addWidget(ownButton);
		clearOwn = new QPushButton(msg("Pan.ArtUseGeneral"), artBox);
		clearOwn->setFocusPolicy(Qt::NoFocus);
		connect(clearOwn, &QPushButton::clicked, this, [this]() { clearOwnArt(); });
		art->addWidget(clearOwn);
		art->addStretch(1);
		outer->addWidget(artBox);

		connect(fill, &QRadioButton::toggled, this, [this](bool on) { setMode(on ? MODE_FILL : MODE_WHOLE); });
		refreshTexts();
	}

	~FrameDialog() override
	{
		obs_weak_source_release(weakScene);
		obs_weak_source_release(weakCamera);
	}

private:
	void setRest(double position)
	{
		obs_source_t *scene = obs_weak_source_get_source(weakScene);
		obs_source_t *camera = obs_weak_source_get_source(weakCamera);
		if (scene && camera) {
			setCameraRest(camera, position);
			placeNow(scene, camera, position);
			updateGuide();
		}
		obs_source_release(camera);
		obs_source_release(scene);
	}

	void setMode(int mode)
	{
		obs_source_t *scene = obs_weak_source_get_source(weakScene);
		obs_source_t *camera = obs_weak_source_get_source(weakCamera);
		if (scene && camera) {
			setCameraValue(camera, KEY_MODE, mode);
			applyMode(scene, camera);
			updateGuide();
		}
		obs_source_release(camera);
		obs_source_release(scene);
		refreshTexts();
	}

	void chooseArt(bool general)
	{
		QString file =
			QFileDialog::getOpenFileName(this, msg("Pan.ChooseArt"), QString(), msg("Pan.ArtFilter"));
		if (file.isEmpty()) {
			return;
		}
		std::string path = file.toUtf8().constData();
		obs_source_t *scene = obs_weak_source_get_source(weakScene);
		obs_source_t *camera = obs_weak_source_get_source(weakCamera);
		if (general) {
			pl_vertical_set_pref_string(PREF_ART, path.c_str());
		} else if (camera) {
			setCameraArt(camera, path);
		}
		if (scene && camera) {
			applyMode(scene, camera);
		}
		obs_source_release(camera);
		obs_source_release(scene);
		refreshTexts();
	}

	void clearOwnArt()
	{
		obs_source_t *scene = obs_weak_source_get_source(weakScene);
		obs_source_t *camera = obs_weak_source_get_source(weakCamera);
		if (camera) {
			setCameraArt(camera, std::string());
		}
		if (scene && camera) {
			applyMode(scene, camera);
		}
		obs_source_release(camera);
		obs_source_release(scene);
		refreshTexts();
	}

	static QString shortName(const std::string &file, const QString &empty)
	{
		return file.empty() ? empty : QFileInfo(QString::fromStdString(file)).completeBaseName();
	}

	void refreshTexts()
	{
		obs_source_t *camera = obs_weak_source_get_source(weakCamera);
		bool isWhole = whole->isChecked();
		view->showWindow = !isWhole;
		artBox->setVisible(isWhole);
		view->setCursor(isWhole ? Qt::ArrowCursor : Qt::SizeHorCursor);
		if (camera) {
			uint32_t h = obs_source_get_height(camera);
			QString quality = msg(h >= 2000 ? "Pan.Quality4K" : "Pan.Quality1080");
			hint->setText(isWhole ? msg("Pan.WholeHint")
					      : QString("%1 %2").arg(msg("Pan.FillHint")).arg(quality));
			const char *generalArt = pl_vertical_pref_string(PREF_ART);
			generalButton->setText(shortName(generalArt ? generalArt : "", msg("Pan.ArtChoose")));
			std::string own = ownArt(camera);
			ownButton->setText(shortName(own, msg("Pan.ArtChoose")));
			clearOwn->setVisible(!own.empty());
		}
		obs_source_release(camera);
	}

	obs_weak_source_t *weakScene = nullptr;
	obs_weak_source_t *weakCamera = nullptr;
	QRadioButton *fill = nullptr;
	QRadioButton *whole = nullptr;
	FrameView *view = nullptr;
	QLabel *hint = nullptr;
	QWidget *artBox = nullptr;
	QPushButton *generalButton = nullptr;
	QPushButton *ownButton = nullptr;
	QPushButton *clearOwn = nullptr;
};

} // namespace

void pl_vertical_pan_load(void)
{
	struct obs_source_info info = {};
	info.id = FUNDO_ID;
	info.type = OBS_SOURCE_TYPE_INPUT;
	/* Não aparece em "Adicionar fonte": quem cria é o "Enquadrar no vertical". */
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_CAP_OBSOLETE;
	info.get_name = fundoName;
	info.create = fundoCreate;
	info.destroy = fundoDestroy;
	info.update = fundoUpdate;
	info.get_width = fundoWidth;
	info.get_height = fundoHeight;
	info.video_render = fundoRender;
	obs_register_source(&info);

	hotkeyLeft = obs_hotkey_register_frontend("somun.vertical.left", obs_module_text("Pan.HotkeyLeft"),
						  hotkeyDirection, (void *)(intptr_t)-1);
	hotkeyRight = obs_hotkey_register_frontend("somun.vertical.right", obs_module_text("Pan.HotkeyRight"),
						   hotkeyDirection, (void *)(intptr_t)1);
	hotkeyHome = obs_hotkey_register_frontend("somun.vertical.home", obs_module_text("Pan.HotkeyHome"), hotkeyBack,
						  nullptr);

	glideTimer = new QTimer();
	glideTimer->setInterval(16);
	QObject::connect(glideTimer, &QTimer::timeout, []() { glideFrame(); });
	houseTimer = new QTimer();
	houseTimer->setInterval(150);
	QObject::connect(houseTimer, &QTimer::timeout, []() { houseKeeping(); });
	obs_frontend_add_event_callback(frontendEvent, nullptr);
	obs_frontend_add_save_callback(saveLoad, nullptr);
}

void pl_vertical_pan_unload(void)
{
	obs_frontend_remove_save_callback(saveLoad, nullptr);
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
	obs_hotkey_unregister(hotkeyLeft);
	obs_hotkey_unregister(hotkeyRight);
	obs_hotkey_unregister(hotkeyHome);
}

QWidget *pl_vertical_pan_create(QWidget *parent)
{
	return new PanControls(parent);
}

bool pl_vertical_frame_available(obs_source_t *scene)
{
	obs_source_t *camera = scene ? cameraOf(scene) : nullptr;
	bool available = camera != nullptr;
	obs_source_release(camera);
	return available;
}

void pl_vertical_frame_open(obs_source_t *scene, QWidget *parent)
{
	obs_source_t *camera = scene ? cameraOf(scene) : nullptr;
	if (camera) {
		auto *dialog = new FrameDialog(parent, scene, camera);
		dialog->show();
		dialog->raise();
		dialog->activateWindow();
		obs_source_release(camera);
	}
}

void pl_vertical_pan_draw_guide(void)
{
	float x, y, w, h;
	{
		std::lock_guard<std::mutex> lock(guideMutex);
		if (!guideVisible) {
			return;
		}
		x = guideX;
		y = guideY;
		w = guideW;
		h = guideH;
	}
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	drawRect(x, y, w, h, 0x1FFFFFFF, true);
	gs_blend_state_pop();
	/* Um contorno escuro fininho atrás, para não sumir em imagem clara. */
	drawRect(x - 2.0f, y, w + 4.0f, h, 0xA0000000, false);
	drawRect(x, y, w, h, 0xE6FFFFFF, false);
}
