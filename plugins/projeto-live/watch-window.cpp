/*
 * somun live: a tela "Acompanhar", no monitor 2.
 *
 * Só imagem, como um multiview: em cima a Prévia (onde se mexe com o mouse) e o No ar grandes;
 * embaixo as entradas em grade, cada uma com o nome e o estado (no ar, prévia ou travou).
 * No alto, só o botão "Cor" (o modo Cor chega na próxima parte). Equipe e Situação ficam na tela
 * Operar, no monitor 1, para as câmeras ganharem espaço aqui.
 *
 * Abre sozinha, em tela cheia, no monitor que não é o da tela Operar, quando o PC tem dois monitores.
 * O "⋯" da tela Operar liga e desliga, e a escolha fica guardada em acompanhar.json. Com um monitor
 * só, nada muda: as entradas da tela Operar ficam no tamanho normal e servem de multiview.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "watch-window.h"

#include <obs-module.h>
#include <util/platform.h>

#include <QAction>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include "camera-dock.h"
#include "color-mode.h"
#include "somun-style.h"
#include "studio-window.h"

namespace {

const char *const WATCH_FILE = "acompanhar.json";

QString msg(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

bool loadWanted()
{
	char *path = obs_module_config_path(WATCH_FILE);
	obs_data_t *data = path ? obs_data_create_from_json_file_safe(path, "bak") : nullptr;
	bfree(path);
	bool wanted = true;
	if (data) {
		obs_data_set_default_bool(data, "ligado", true);
		wanted = obs_data_get_bool(data, "ligado");
		obs_data_release(data);
	}
	return wanted;
}

void saveWanted(bool wanted)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, "ligado", wanted);
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path(WATCH_FILE);
	if (path) {
		obs_data_save_json_safe(data, path, "tmp", "bak");
		bfree(path);
	}
	obs_data_release(data);
}

bool wantedLoaded = false;
bool wanted = true;

class WatchWindow : public QWidget {
public:
	WatchWindow() : QWidget(nullptr)
	{
		setObjectName("somunWindow");
		setStyleSheet(somun_window_style());
		setWindowTitle(msg("Watch.Title"));
		setAttribute(Qt::WA_StyledBackground, true);
		/* Abrir no monitor 2 não tira o foco da tela Operar. */
		setAttribute(Qt::WA_ShowWithoutActivating, true);

		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(14, 10, 14, 14);
		outer->setSpacing(8);

		/* No alto, só o botão "Cor". */
		auto *top = new QHBoxLayout();
		top->addStretch(1);
		colorButton = new QPushButton(msg("Watch.Color"), this);
		colorButton->setCheckable(true);
		colorButton->setMinimumHeight(30);
		colorButton->setFocusPolicy(Qt::NoFocus);
		colorButton->setToolTip(msg("Watch.ColorHint"));
		colorButton->setStyleSheet(QString("QPushButton:checked { background: %1; color: %2; }")
						   .arg(QString(SOMUN_WHITE))
						   .arg(QString(SOMUN_BLACK)));
		top->addWidget(colorButton);
		outer->addLayout(top);

		/* Em cima, a Prévia e o No ar grandes; embaixo, as entradas em grade (um multiview). */
		screens = pl_studio_screens_create(this);
		outer->addWidget(screens, 5);

		grid = pl_camera_grid_create(this);
		outer->addWidget(grid, 4);

		/* O modo Cor toma o monitor inteiro; Prévia, No ar e grade voltam ao sair. */
		colorPanel = pl_color_panel_create(this, nullptr);
		colorPanel->setVisible(false);
		outer->addWidget(colorPanel, 9);
		connect(colorButton, &QPushButton::toggled, this, [this](bool on) {
			screens->setVisible(!on);
			grid->setVisible(!on);
			colorPanel->setVisible(on);
		});
	}

	/* Vai para a tela (monitor) pedida e ocupa ela inteira. */
	void showOn(QScreen *screen)
	{
		if (screen != shownOn || !isVisible()) {
			shownOn = screen;
			if (isVisible()) {
				hide();
			}
			setGeometry(screen->geometry());
			if (QWindow *window = windowHandle()) {
				window->setScreen(screen);
			}
			showFullScreen();
		}
	}

	QScreen *screenShown() const { return shownOn; }

protected:
	/* Fechar (Alt+F4) é o mesmo que desligar no "⋯". */
	void closeEvent(QCloseEvent *event) override
	{
		event->ignore();
		pl_watch_set_wanted(false);
		hide();
	}

	void contextMenuEvent(QContextMenuEvent *event) override
	{
		QMenu menu(this);
		QAction *close = menu.addAction(msg("Watch.Close"));
		if (menu.exec(event->globalPos()) == close) {
			pl_watch_set_wanted(false);
			hide();
		}
		event->accept();
	}

private:
	QPushButton *colorButton = nullptr;
	QWidget *screens = nullptr;
	QWidget *colorPanel = nullptr;
	QWidget *grid = nullptr;
	QScreen *shownOn = nullptr;
};

QPointer<WatchWindow> window;

/* O monitor onde o Acompanhar vai: o primeiro que não é o da tela Operar. */
QScreen *otherScreen(QWidget *operate)
{
	QList<QScreen *> screens = QGuiApplication::screens();
	if (screens.size() < 2) {
		return nullptr;
	}
	QScreen *mine = operate ? operate->screen() : QGuiApplication::primaryScreen();
	for (QScreen *screen : screens) {
		if (screen != mine) {
			return screen;
		}
	}
	return nullptr;
}

} // namespace

bool pl_watch_wanted(void)
{
	if (!wantedLoaded) {
		wantedLoaded = true;
		wanted = loadWanted();
	}
	return wanted;
}

void pl_watch_set_wanted(bool on)
{
	wantedLoaded = true;
	wanted = on;
	saveWanted(on);
}

bool pl_watch_available(void)
{
	return QGuiApplication::screens().size() >= 2;
}

bool pl_watch_update(QWidget *operate)
{
	bool inUse = operate && operate->isVisible() && !operate->isMinimized();
	QScreen *target = inUse && pl_watch_wanted() ? otherScreen(operate) : nullptr;
	if (!target) {
		if (window && window->isVisible()) {
			window->hide();
		}
		return false;
	}
	if (!window) {
		window = new WatchWindow();
	}
	window->showOn(target);
	return true;
}

void pl_watch_release(void)
{
	if (window) {
		delete window.data();
	}
}
