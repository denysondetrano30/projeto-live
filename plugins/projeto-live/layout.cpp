/*
 * Projeto Live: arranjos prontos dos painéis, no botão "Layout" da faixa "Situação".
 *
 * O OBS já deixa arrastar, soltar numa janela e fechar cada painel, e já guarda onde cada um
 * ficou. O que faltava era arrumar tudo de uma vez, porque numa igreja ninguém quer arrastar dez
 * painéis na mão:
 *
 * - "Clássico (OBS)": desliga os painéis novos e deixa a janela como o OBS de sempre.
 * - "Um monitor": a faixa Situação no alto, as câmeras numa fileira larga embaixo do preview, e o
 *   Diretor junto das fontes verticais, na direita.
 * - "Tela grande": o mesmo, com as câmeras mais altas (cabem duas fileiras) e a direita mais larga.
 * - "Dois monitores": as câmeras e o Diretor saem em janelas soltas, para arrastar para a outra
 *   tela.
 *
 * Antes do primeiro arranjo, o arranjo que a pessoa já tinha é guardado em layout.json, e volta
 * em "Voltar ao meu layout". Nada aqui é definitivo: depois de escolher, a pessoa continua
 * arrastando o que quiser, e o OBS guarda.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "layout.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <util/platform.h>

#include <QAction>
#include <QByteArray>
#include <QDockWidget>
#include <QList>
#include <QMainWindow>
#include <QMenu>
#include <QPoint>
#include <QString>
#include <QWidget>

#include <cstring>

namespace {

#define LAYOUT_FILE "layout.json"

const char *kStats = "projeto-live-situacao";
const char *kCameras = "projeto-live-cameras";
const char *kDirector = "projeto-live-diretor";
const char *kVerticalSources = "projeto-live-vertical-fontes";

QMainWindow *mainWindow()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

QDockWidget *dock(const char *id)
{
	QMainWindow *window = mainWindow();
	if (!window) {
		return nullptr;
	}
	/* O OBS dá ao painel o nome do id. */
	return window->findChild<QDockWidget *>(QString::fromUtf8(id));
}

/* Painéis do próprio OBS que a gente usa como referência para encaixar os nossos. */
QDockWidget *obsDock(const char *name)
{
	QMainWindow *window = mainWindow();
	return window ? window->findChild<QDockWidget *>(QString::fromUtf8(name)) : nullptr;
}

void show(QDockWidget *target, bool floating, Qt::DockWidgetArea area)
{
	if (!target) {
		return;
	}
	target->setFloating(floating);
	if (!floating) {
		QMainWindow *window = mainWindow();
		if (window) {
			window->addDockWidget(area, target);
		}
	}
	target->setVisible(true);
}

void hide(QDockWidget *target)
{
	if (target) {
		target->setVisible(false);
	}
}

/* Guarda o arranjo de agora, se ainda não tiver um guardado. */
void saveMineOnce()
{
	char *path = obs_module_config_path(LAYOUT_FILE);
	if (!path) {
		return;
	}
	if (os_file_exists(path)) {
		bfree(path);
		return;
	}
	QMainWindow *window = mainWindow();
	if (!window) {
		bfree(path);
		return;
	}
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "estado", window->saveState().toBase64().constData());
	obs_data_save_json_safe(data, path, "tmp", "bak");
	obs_data_release(data);
	bfree(path);
}

bool hasMine()
{
	char *path = obs_module_config_path(LAYOUT_FILE);
	if (!path) {
		return false;
	}
	bool exists = os_file_exists(path);
	bfree(path);
	return exists;
}

void restoreMine()
{
	char *path = obs_module_config_path(LAYOUT_FILE);
	if (!path) {
		return;
	}
	obs_data_t *data = obs_data_create_from_json_file_safe(path, "bak");
	bfree(path);
	if (!data) {
		return;
	}
	const char *encoded = obs_data_get_string(data, "estado");
	QMainWindow *window = mainWindow();
	if (window && encoded && *encoded) {
		window->restoreState(QByteArray::fromBase64(QByteArray(encoded)));
	}
	obs_data_release(data);
}

/* A fileira de câmeras ocupa a largura toda, acima dos painéis de baixo do OBS. */
void camerasAcrossTheBottom(int height)
{
	QMainWindow *window = mainWindow();
	QDockWidget *cameras = dock(kCameras);
	if (!window || !cameras) {
		return;
	}
	cameras->setFloating(false);
	window->addDockWidget(Qt::BottomDockWidgetArea, cameras);
	QDockWidget *scenes = obsDock("scenesDock");
	if (scenes && scenes->isVisible() && !scenes->isFloating()) {
		/* As cenas (e os painéis ao lado delas) descem para baixo das câmeras. */
		window->splitDockWidget(cameras, scenes, Qt::Vertical);
	}
	cameras->setVisible(true);
	window->resizeDocks({cameras}, {height}, Qt::Vertical);
}

void applyClassic()
{
	hide(dock(kStats));
	hide(dock(kCameras));
	hide(dock(kDirector));
	hide(dock(kVerticalSources));
}

void applyOneMonitor(int camerasHeight, int rightWidth)
{
	QMainWindow *window = mainWindow();
	if (!window) {
		return;
	}
	show(dock(kStats), false, Qt::TopDockWidgetArea);
	camerasAcrossTheBottom(camerasHeight);
	show(dock(kDirector), false, Qt::RightDockWidgetArea);
	QDockWidget *director = dock(kDirector);
	QDockWidget *vertical = dock(kVerticalSources);
	if (director && vertical) {
		vertical->setFloating(false);
		/* Diretor e fontes verticais dividem a direita, em abas. */
		window->tabifyDockWidget(director, vertical);
		vertical->setVisible(true);
		director->raise();
	}
	if (director) {
		window->resizeDocks({director}, {rightWidth}, Qt::Horizontal);
	}
}

void applyTwoMonitors()
{
	show(dock(kStats), false, Qt::TopDockWidgetArea);
	QDockWidget *cameras = dock(kCameras);
	if (cameras) {
		cameras->setFloating(true);
		cameras->resize(900, 320);
		cameras->setVisible(true);
	}
	QDockWidget *director = dock(kDirector);
	if (director) {
		director->setFloating(true);
		director->resize(420, 700);
		director->setVisible(true);
	}
}

void apply(const char *preset)
{
	saveMineOnce();
	QMainWindow *window = mainWindow();
	if (!window) {
		return;
	}
	if (strcmp(preset, "classico") == 0) {
		applyClassic();
	} else if (strcmp(preset, "um-monitor") == 0) {
		applyOneMonitor(220, 360);
	} else if (strcmp(preset, "tela-grande") == 0) {
		applyOneMonitor(360, 440);
	} else if (strcmp(preset, "dois-monitores") == 0) {
		applyTwoMonitors();
	} else if (strcmp(preset, "meu") == 0) {
		restoreMine();
	}
}

} // namespace

void pl_layout_show_menu(QWidget *button)
{
	if (!button) {
		return;
	}
	QMenu menu(button);
	struct {
		const char *key;
		const char *preset;
	} items[] = {
		{"Layout.OneMonitor", "um-monitor"},
		{"Layout.BigScreen", "tela-grande"},
		{"Layout.TwoMonitors", "dois-monitores"},
		{"Layout.Classic", "classico"},
	};
	for (const auto &entry : items) {
		const char *preset = entry.preset;
		QAction *action = menu.addAction(QString::fromUtf8(obs_module_text(entry.key)));
		QObject::connect(action, &QAction::triggered, &menu, [preset]() { apply(preset); });
	}
	if (hasMine()) {
		menu.addSeparator();
		QAction *action = menu.addAction(QString::fromUtf8(obs_module_text("Layout.Mine")));
		QObject::connect(action, &QAction::triggered, &menu, []() { apply("meu"); });
	}
	menu.exec(button->mapToGlobal(QPoint(0, button->height())));
}

void pl_layout_show_panels_menu(QWidget *button)
{
	QMainWindow *window = mainWindow();
	if (!button || !window) {
		return;
	}
	QMenu menu(button);
	const QList<QDockWidget *> docks = window->findChildren<QDockWidget *>();
	for (QDockWidget *target : docks) {
		if (target->windowTitle().isEmpty()) {
			continue;
		}
		QAction *action = menu.addAction(target->windowTitle());
		action->setCheckable(true);
		action->setChecked(target->isVisible());
		QObject::connect(action, &QAction::toggled, target, [target](bool on) { target->setVisible(on); });
	}
	if (menu.isEmpty()) {
		return;
	}
	menu.exec(button->mapToGlobal(QPoint(0, button->height())));
}
