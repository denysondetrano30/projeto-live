/*
 * Projeto Live: parte de tela da ligação com o app de projeção (veja projection-link.h).
 *
 * - Pergunta "Permitir?" na primeira vez que uma projeção pede para conectar.
 * - Pontinho "Projeção" na barra de baixo enquanto houver projeção conectada.
 * - Ferramentas > "Projeção: esquecer computadores pareados".
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "projection-link.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QStatusBar>
#include <QTimer>

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QLabel *statusLabel = nullptr;
QTimer *statusTimer = nullptr;
bool restartShown = false;

QWidget *mainWindow()
{
	return static_cast<QWidget *>(obs_frontend_get_main_window());
}

void refreshStatus()
{
	if (!statusLabel) {
		return;
	}
	char names[512];
	int count = pl_projection_status(names, sizeof(names));
	if (!count) {
		statusLabel->hide();
		return;
	}
	statusLabel->setText(QStringLiteral("<span style=\"color:#3ec46d\">&#9679;</span> ") +
			     moduleText("Projection.Status"));
	statusLabel->setToolTip(moduleText("Projection.Connected").arg(QString::fromUtf8(names)));
	statusLabel->show();
}

void forgetAll(void *)
{
	auto answer = QMessageBox::question(mainWindow(), moduleText("Projection.Forget"),
					    moduleText("Projection.ForgetConfirm"));
	if (answer == QMessageBox::Yes) {
		pl_projection_forget_all();
	}
}

void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT && statusTimer) {
		statusTimer->stop();
	}
}

} // namespace

void pl_projection_ui_load(void)
{
	auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!window) {
		return;
	}
	if (QStatusBar *bar = window->statusBar()) {
		statusLabel = new QLabel(bar);
		statusLabel->setTextFormat(Qt::RichText);
		statusLabel->setContentsMargins(6, 0, 6, 0);
		statusLabel->hide();
		bar->insertPermanentWidget(0, statusLabel);
		QObject::connect(statusLabel, &QObject::destroyed, []() { statusLabel = nullptr; });
	}
	statusTimer = new QTimer(window);
	QObject::connect(statusTimer, &QTimer::timeout, []() { refreshStatus(); });
	statusTimer->start(1000);

	QByteArray forget = moduleText("Projection.Forget").toUtf8();
	obs_frontend_add_tools_menu_item(forget.constData(), forgetAll, nullptr);
	obs_frontend_add_event_callback(frontendEvent, nullptr);
}

void pl_projection_ui_ask(unsigned int ticket, const char *client_name)
{
	QWidget *window = mainWindow();
	auto *box = new QMessageBox(QMessageBox::Question, moduleText("Projection.AskTitle"),
				    moduleText("Projection.Ask").arg(QString::fromUtf8(client_name).toHtmlEscaped()),
				    QMessageBox::Yes | QMessageBox::No, window);
	box->setTextFormat(Qt::RichText);
	box->setDefaultButton(QMessageBox::No);
	box->setAttribute(Qt::WA_DeleteOnClose);
	/* Sem travar o OBS: a resposta chega quando a pessoa clicar. */
	QObject::connect(box, &QMessageBox::finished,
			 [ticket](int result) { pl_projection_answer(ticket, result == QMessageBox::Yes); });
	/* A projeção desiste depois de 1 minuto; a pergunta some junto. */
	QTimer::singleShot(60000, box, [box]() { box->done(QMessageBox::No); });
	box->show();
	box->raise();
	box->activateWindow();
}

void pl_projection_ui_restart_notice(void)
{
	if (restartShown) {
		return;
	}
	restartShown = true;
	auto *box = new QMessageBox(QMessageBox::Information, moduleText("Projection.AskTitle"),
				    moduleText("Projection.Restart"), QMessageBox::Ok, mainWindow());
	box->setAttribute(Qt::WA_DeleteOnClose);
	box->show();
}
