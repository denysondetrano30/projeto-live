/*
 * Projeto Live: a linha de uma fonte nas listas de fontes (horizontal e vertical) e os botões de ícone,
 * com a mesma cara do painel Fontes do OBS: o nome, o olho (mostrar/esconder) e o cadeado (travar).
 * Os ícones vêm do tema do OBS (as mesmas classes que o painel Fontes usa).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <QCheckBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QSizePolicy>
#include <QString>
#include <QStyle>
#include <QToolButton>
#include <QWidget>

#include <functional>
#include <utility>

class PLSourceRow : public QWidget {
public:
	PLSourceRow(QWidget *parent, const QString &name, bool visible, bool locked,
		    std::function<void(bool)> onVisible, std::function<void(bool)> onLock)
		: QWidget(parent),
		  visibleChanged(std::move(onVisible)),
		  lockChanged(std::move(onLock))
	{
		auto *layout = new QHBoxLayout(this);
		layout->setContentsMargins(4, 0, 4, 0);
		layout->setSpacing(6);

		label = new QLabel(name, this);
		label->setAttribute(Qt::WA_TransparentForMouseEvents);
		layout->addWidget(label, 1);

		eye = new QCheckBox(this);
		eye->setProperty("class", "checkbox-icon indicator-visibility");
		eye->setChecked(visible);
		eye->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
		layout->addWidget(eye);

		lock = new QCheckBox(this);
		lock->setProperty("class", "checkbox-icon indicator-lock");
		lock->setChecked(locked);
		lock->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
		layout->addWidget(lock);

		setMuted(!visible);
		connect(eye, &QCheckBox::toggled, this, [this](bool on) {
			setMuted(!on);
			if (visibleChanged) {
				visibleChanged(on);
			}
		});
		connect(lock, &QCheckBox::toggled, this, [this](bool on) {
			if (lockChanged) {
				lockChanged(on);
			}
		});
	}

private:
	/* Fonte escondida: nome apagado, como no OBS. */
	void setMuted(bool muted) { label->setEnabled(!muted); }

	std::function<void(bool)> visibleChanged;
	std::function<void(bool)> lockChanged;
	QLabel *label = nullptr;
	QCheckBox *eye = nullptr;
	QCheckBox *lock = nullptr;
};

/* Botão só de ícone, como os da barra do painel Fontes do OBS ("icon-plus", "icon-gear"...). */
inline QToolButton *plIconButton(QWidget *parent, const char *themeClass, const char *resource, const QString &tip)
{
	auto *button = new QToolButton(parent);
	button->setProperty("class", themeClass);
	button->setIcon(QIcon(QString::fromUtf8(resource)));
	button->setToolTip(tip);
	button->setAutoRaise(true);
	return button;
}
