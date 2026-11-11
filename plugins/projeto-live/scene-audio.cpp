/*
 * Projeto Live: o som que segue a cena (o "audio follow video" das mesas de transmissão).
 *
 * A dor: para o som da mesa valer em todas as cenas, sem o nosso assistente, a pessoa teria de
 * repetir a mesma fonte de áudio dentro de cada cena. E quando entra um vídeo da projeção, ela
 * precisa correr no mixer para calar a mesa, senão o salão e o vídeo tocam juntos na live.
 *
 * Como funciona aqui:
 *
 * - O som da mesa já é global (o assistente põe num canal do OBS), então toca em toda cena.
 * - Cada cena guarda quais desses sons globais ficam **calados** enquanto ela está no ar. Ao
 *   cortar, o OBS cala e devolve sozinho, sem ninguém tocar no mixer.
 * - Regra automática: a cena que tem o "Telão (projeção)" dentro nasce calando a mesa, porque o
 *   vídeo já traz o som dele. A pessoa pode mudar depois, e a escolha dela manda.
 * - "Mesa junto": devolve os sons calados **só naquele momento**, para alguém narrar por cima do
 *   vídeo. No próximo corte tudo volta ao que a cena manda.
 *
 * A escolha mora nas "configurações privadas" da cena, então ela anda junto com a coleção de
 * cenas: quem leva a coleção para outro PC leva o som junto.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "scene-audio.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QCheckBox>
#include <QDialog>
#include <QGridLayout>
#include <QLabel>
#include <QPointer>
#include <QScrollArea>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include <cstring>
#include <string>
#include <vector>

namespace {

/* Nome da lista guardada em cada cena. */
const char *const LIST_KEY = "projeto_live_sem_som";
/* Marca que a regra automática já rodou nesta cena, para não desfazer a escolha da pessoa. */
const char *const DONE_KEY = "projeto_live_som_definido";
const char *const MIRROR_ID = "projeto_live_mirror";

bool loaded = false;
bool broughtBack = false;
/* Sons que nós calamos, para devolver depois sem mexer no que a pessoa calou na mão. */
std::vector<std::string> silencedByUs;
QPointer<QDialog> window;

/* Os sons globais do OBS (canais 1 a 6): a mesa, o microfone, o som do PC. */
std::vector<std::string> globalNames()
{
	std::vector<std::string> names;
	for (int channel = 1; channel <= 6; channel++) {
		obs_source_t *source = obs_get_output_source(channel);
		if (!source) {
			continue;
		}
		if (obs_source_audio_active(source)) {
			names.emplace_back(obs_source_get_name(source));
		}
		obs_source_release(source);
	}
	return names;
}

obs_source_t *findGlobal(const char *name)
{
	for (int channel = 1; channel <= 6; channel++) {
		obs_source_t *source = obs_get_output_source(channel);
		if (!source) {
			continue;
		}
		const char *found = obs_source_get_name(source);
		if (found && strcmp(found, name) == 0) {
			return source;
		}
		obs_source_release(source);
	}
	return nullptr;
}

bool sceneHasMirror(obs_source_t *scene)
{
	obs_scene_t *as_scene = obs_scene_from_source(scene);
	if (!as_scene) {
		return false;
	}
	bool found = false;
	obs_scene_enum_items(
		as_scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *data) {
			obs_source_t *source = obs_sceneitem_get_source(item);
			const char *id = source ? obs_source_get_id(source) : nullptr;
			if (id && strcmp(id, MIRROR_ID) == 0) {
				*static_cast<bool *>(data) = true;
				return false;
			}
			return true;
		},
		&found);
	return found;
}

/* A lista de calados desta cena. Na primeira vez, aplica a regra automática. */
std::vector<std::string> listFor(obs_source_t *scene)
{
	std::vector<std::string> names;
	if (!scene) {
		return names;
	}
	obs_data_t *priv = obs_source_get_private_settings(scene);
	if (!obs_data_get_bool(priv, DONE_KEY)) {
		/* Primeira vez: a cena com o telão dentro nasce calando os sons globais. */
		obs_data_set_bool(priv, DONE_KEY, true);
		if (sceneHasMirror(scene)) {
			obs_data_array_t *array = obs_data_array_create();
			for (const std::string &name : globalNames()) {
				obs_data_t *entry = obs_data_create();
				obs_data_set_string(entry, "nome", name.c_str());
				obs_data_array_push_back(array, entry);
				obs_data_release(entry);
			}
			obs_data_set_array(priv, LIST_KEY, array);
			obs_data_array_release(array);
		}
	}
	obs_data_array_t *array = obs_data_get_array(priv, LIST_KEY);
	size_t count = array ? obs_data_array_count(array) : 0;
	for (size_t i = 0; i < count; i++) {
		obs_data_t *entry = obs_data_array_item(array, i);
		const char *name = obs_data_get_string(entry, "nome");
		if (name && *name) {
			names.emplace_back(name);
		}
		obs_data_release(entry);
	}
	obs_data_array_release(array);
	obs_data_release(priv);
	return names;
}

void saveList(obs_source_t *scene, const std::vector<std::string> &names)
{
	if (!scene) {
		return;
	}
	obs_data_t *priv = obs_source_get_private_settings(scene);
	obs_data_array_t *array = obs_data_array_create();
	for (const std::string &name : names) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "nome", name.c_str());
		obs_data_array_push_back(array, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(priv, LIST_KEY, array);
	obs_data_set_bool(priv, DONE_KEY, true);
	obs_data_array_release(array);
	obs_data_release(priv);
}

/* Devolve o som de tudo o que nós calamos. */
void bringEverythingBack()
{
	for (const std::string &name : silencedByUs) {
		obs_source_t *source = findGlobal(name.c_str());
		if (source) {
			obs_source_set_muted(source, false);
			obs_source_release(source);
		}
	}
	silencedByUs.clear();
}

/* Aplica o que a cena no ar manda. */
void applyCurrent()
{
	bringEverythingBack();
	broughtBack = false;

	obs_source_t *scene = obs_frontend_get_current_scene();
	if (!scene) {
		return;
	}
	for (const std::string &name : listFor(scene)) {
		obs_source_t *source = findGlobal(name.c_str());
		if (!source) {
			continue;
		}
		if (!obs_source_muted(source)) {
			obs_source_set_muted(source, true);
			silencedByUs.push_back(name);
		}
		obs_source_release(source);
	}
	obs_source_release(scene);
}

void frontendEvent(enum obs_frontend_event event, void *param)
{
	(void)param;
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		applyCurrent();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		bringEverythingBack();
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------------- */
/* Janela "Som das cenas"                                                    */

class Window : public QDialog {
public:
	explicit Window(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle(QString::fromUtf8(obs_module_text("SceneAudio.Window")));
		resize(640, 420);

		auto *outer = new QVBoxLayout(this);
		auto *help = new QLabel(QString::fromUtf8(obs_module_text("SceneAudio.Help")), this);
		help->setWordWrap(true);
		outer->addWidget(help);

		auto *area = new QScrollArea(this);
		area->setWidgetResizable(true);
		auto *holder = new QWidget(area);
		auto *grid = new QGridLayout(holder);
		grid->setHorizontalSpacing(18);

		std::vector<std::string> globals = globalNames();
		grid->addWidget(new QLabel(QString::fromUtf8(obs_module_text("SceneAudio.Scene")), holder), 0, 0);
		for (size_t column = 0; column < globals.size(); column++) {
			grid->addWidget(new QLabel(QString::fromUtf8(globals[column].c_str()), holder), 0,
					(int)column + 1);
		}

		obs_frontend_source_list scenes = {};
		obs_frontend_get_scenes(&scenes);
		for (size_t i = 0; i < scenes.sources.num; i++) {
			obs_source_t *scene = scenes.sources.array[i];
			int line = (int)i + 1;
			grid->addWidget(new QLabel(QString::fromUtf8(obs_source_get_name(scene)), holder), line, 0);
			for (size_t column = 0; column < globals.size(); column++) {
				std::string name = globals[column];
				auto *box = new QCheckBox(holder);
				box->setChecked(!pl_scene_audio_is_silenced(scene, name.c_str()));
				box->setToolTip(QString::fromUtf8(obs_module_text("SceneAudio.Tip")));
				obs_weak_source_t *weak = obs_source_get_weak_source(scene);
				connect(box, &QCheckBox::toggled, this, [weak, name](bool on) {
					obs_source_t *target = obs_weak_source_get_source(weak);
					if (target) {
						pl_scene_audio_set_silenced(target, name.c_str(), !on);
						obs_source_release(target);
					}
				});
				connect(box, &QObject::destroyed, [weak]() { obs_weak_source_release(weak); });
				grid->addWidget(box, line, (int)column + 1);
			}
		}
		obs_frontend_source_list_free(&scenes);

		grid->setRowStretch(grid->rowCount(), 1);
		area->setWidget(holder);
		outer->addWidget(area, 1);
	}
};

} // namespace

bool pl_scene_audio_is_silenced(obs_source_t *scene, const char *source_name)
{
	if (!scene || !source_name) {
		return false;
	}
	for (const std::string &name : listFor(scene)) {
		if (name == source_name) {
			return true;
		}
	}
	return false;
}

void pl_scene_audio_set_silenced(obs_source_t *scene, const char *source_name, bool silenced)
{
	if (!scene || !source_name) {
		return;
	}
	std::vector<std::string> names = listFor(scene);
	std::vector<std::string> updated;
	for (const std::string &name : names) {
		if (name != source_name) {
			updated.push_back(name);
		}
	}
	if (silenced) {
		updated.emplace_back(source_name);
	}
	saveList(scene, updated);

	obs_source_t *current = obs_frontend_get_current_scene();
	bool isCurrent = current == scene;
	obs_source_release(current);
	if (isCurrent) {
		applyCurrent();
	}
}

bool pl_scene_audio_has_silenced(void)
{
	return !silencedByUs.empty() || broughtBack;
}

void pl_scene_audio_bring_back(void)
{
	if (broughtBack) {
		/* Apertou de novo: cala outra vez, como a cena manda. */
		applyCurrent();
		return;
	}
	if (silencedByUs.empty()) {
		return;
	}
	bringEverythingBack();
	broughtBack = true;
}

bool pl_scene_audio_brought_back(void)
{
	return broughtBack;
}

void pl_scene_audio_show_window(void)
{
	if (window) {
		window->raise();
		window->activateWindow();
		return;
	}
	auto *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (!parent) {
		return;
	}
	window = new Window(parent);
	window->setAttribute(Qt::WA_DeleteOnClose);
	window->show();
}

void pl_scene_audio_load(void)
{
	if (loaded) {
		return;
	}
	obs_frontend_add_event_callback(frontendEvent, nullptr);
	obs_frontend_add_tools_menu_item(obs_module_text("SceneAudio.Window"),
					 [](void *) { pl_scene_audio_show_window(); }, nullptr);
	loaded = true;
}

void pl_scene_audio_unload(void)
{
	if (!loaded) {
		return;
	}
	bringEverythingBack();
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
	loaded = false;
}
