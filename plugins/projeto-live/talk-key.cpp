/*
 * somun live: "segurar para falar com a equipe" por uma tecla do teclado.
 *
 * Segurou a tecla, o diretor fala com todos (o mesmo que segurar "Falar com todos" no painel Equipe);
 * soltou, para. Funciona em qualquer janela do programa (monitor 1 ou 2) enquanto a janela do somun
 * live está em uso. O operador escolhe a tecla no "⋯" (padrão: Espaço) e ela fica em tecla-falar.json.
 *
 * Cuidados:
 * - escrevendo num campo de texto, a tecla escreve normal;
 * - a tecla nunca chega aos botões (o Espaço apertaria o botão que estiver com o foco, por exemplo
 *   cortar sem querer);
 * - se a janela perder o foco com a tecla apertada, a fala para (senão ficaria falando para sempre).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "talk-key.h"

#include <obs-module.h>
#include <util/platform.h>

#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWidget>

#include "intercom.h"

namespace {

const char *const KEY_FILE = "tecla-falar.json";

QString msg(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

int loadKey()
{
	char *path = obs_module_config_path(KEY_FILE);
	obs_data_t *data = path ? obs_data_create_from_json_file_safe(path, "bak") : nullptr;
	bfree(path);
	int key = Qt::Key_Space;
	if (data) {
		obs_data_set_default_int(data, "tecla", Qt::Key_Space);
		key = (int)obs_data_get_int(data, "tecla");
		obs_data_release(data);
	}
	return key;
}

void saveKey(int key)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "tecla", key);
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path(KEY_FILE);
	if (path) {
		obs_data_save_json_safe(data, path, "tmp", "bak");
		bfree(path);
	}
	obs_data_release(data);
}

QString keyName(int key)
{
	if (key == 0) {
		return QString();
	}
	if (key == Qt::Key_Space) {
		return msg("TalkKey.Space");
	}
	return QKeySequence(key).toString(QKeySequence::NativeText);
}

/* Onde se escreve texto: ali a tecla escreve, não fala. */
bool isTextInput(QWidget *widget)
{
	if (!widget) {
		return false;
	}
	if (qobject_cast<QLineEdit *>(widget) || qobject_cast<QTextEdit *>(widget) ||
	    qobject_cast<QPlainTextEdit *>(widget) || qobject_cast<QAbstractSpinBox *>(widget)) {
		return true;
	}
	if (auto *combo = qobject_cast<QComboBox *>(widget)) {
		return combo->isEditable();
	}
	return false;
}

class TalkKeyFilter : public QObject {
public:
	int key = Qt::Key_Space;
	bool active = false;
	bool choosing = false;
	bool held = false;

	void release()
	{
		if (held) {
			held = false;
			pl_intercom_set_talk_all(false);
		}
	}

protected:
	bool eventFilter(QObject *object, QEvent *event) override
	{
		QEvent::Type type = event->type();
		if (type == QEvent::ApplicationDeactivate) {
			release();
			return QObject::eventFilter(object, event);
		}
		if (type != QEvent::KeyPress && type != QEvent::KeyRelease && type != QEvent::ShortcutOverride) {
			return QObject::eventFilter(object, event);
		}
		auto *keyEvent = static_cast<QKeyEvent *>(event);
		if (key == 0 || keyEvent->key() != key || choosing) {
			return QObject::eventFilter(object, event);
		}
		/* Soltar sempre solta, mesmo se a janela do somun live saiu de uso no meio. */
		if (type == QEvent::KeyRelease && held && !keyEvent->isAutoRepeat()) {
			release();
			return true;
		}
		if (!active || isTextInput(QApplication::focusWidget())) {
			return QObject::eventFilter(object, event);
		}
		if (type == QEvent::ShortcutOverride) {
			/* Não deixa nenhum atalho do OBS usar a tecla. */
			event->accept();
			return true;
		}
		if (type == QEvent::KeyPress && !keyEvent->isAutoRepeat() && !held) {
			held = true;
			pl_intercom_set_talk_all(true);
		}
		/* Engole a tecla (inclusive a repetição de segurar): nenhum botão recebe. */
		return true;
	}
};

TalkKeyFilter *filter = nullptr;

/* A janelinha "aperte a tecla". */
class ChooseDialog : public QDialog {
public:
	explicit ChooseDialog(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle(msg("TalkKey.Title"));
		auto *layout = new QVBoxLayout(this);
		auto *text = new QLabel(msg("TalkKey.Prompt"), this);
		text->setWordWrap(true);
		layout->addWidget(text);
		current = new QLabel(this);
		QFont big = current->font();
		big.setPointSizeF(big.pointSizeF() + 6.0);
		big.setBold(true);
		current->setFont(big);
		current->setAlignment(Qt::AlignCenter);
		current->setMinimumHeight(48);
		layout->addWidget(current);
		auto *buttons = new QHBoxLayout();
		auto *none = new QPushButton(msg("TalkKey.None"), this);
		auto *cancel = new QPushButton(msg("TalkKey.Cancel"), this);
		/* Os botões não pegam o foco: assim qualquer tecla (até o Espaço) vem para cá. */
		none->setFocusPolicy(Qt::NoFocus);
		cancel->setFocusPolicy(Qt::NoFocus);
		connect(none, &QPushButton::clicked, this, [this]() {
			chosen = 0;
			accept();
		});
		connect(cancel, &QPushButton::clicked, this, [this]() { reject(); });
		buttons->addStretch(1);
		buttons->addWidget(none);
		buttons->addWidget(cancel);
		layout->addLayout(buttons);
		QString name = keyName(filter ? filter->key : Qt::Key_Space);
		current->setText(name.isEmpty() ? msg("TalkKey.NoneShort") : name);
		setFocusPolicy(Qt::StrongFocus);
	}

	int chosen = -1;

protected:
	void keyPressEvent(QKeyEvent *event) override
	{
		if (event->isAutoRepeat()) {
			return;
		}
		if (event->key() == Qt::Key_Escape) {
			reject();
			return;
		}
		if (event->key() == 0 || event->key() == Qt::Key_unknown) {
			return;
		}
		chosen = event->key();
		accept();
	}
};

} // namespace

void pl_talk_key_install(void)
{
	if (filter) {
		return;
	}
	filter = new TalkKeyFilter();
	filter->key = loadKey();
	qApp->installEventFilter(filter);
}

void pl_talk_key_set_active(bool active)
{
	if (!filter) {
		return;
	}
	filter->active = active;
	if (!active) {
		filter->release();
	}
}

QString pl_talk_key_name(void)
{
	return keyName(filter ? filter->key : loadKey());
}

void pl_talk_key_choose(QWidget *parent)
{
	if (!filter) {
		pl_talk_key_install();
	}
	filter->release();
	/* Fica com o que já estava escolhido se a janelinha for fechada. */
	filter->choosing = true;
	ChooseDialog dialog(parent);
	dialog.setFocus();
	int result = dialog.exec();
	filter->choosing = false;
	if (result == QDialog::Accepted && dialog.chosen >= 0) {
		filter->key = dialog.chosen;
		saveKey(dialog.chosen);
	}
}

void pl_talk_key_release(void)
{
	if (filter) {
		filter->release();
		filter->active = false;
		/* O plugin vai sair da memória: a escuta do teclado não pode ficar no Qt. */
		qApp->removeEventFilter(filter);
		delete filter;
		filter = nullptr;
	}
}
