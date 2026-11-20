/*
 * somun live: a cara do somun nas nossas janelas.
 *
 * A fonte Outfit (licença livre SIL OFL 1.1, arquivo OFL.txt ao lado) vem na pasta data/fonts do
 * plugin e é carregada só para o programa, sem instalar nada no Windows.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "somun-style.h"

#include <obs-module.h>

#include <QFont>
#include <QFontDatabase>
#include <QLabel>
#include <QStringList>
#include <QWidget>

QString somun_font_family()
{
	static bool tried = false;
	static QString family;
	if (tried) {
		return family;
	}
	tried = true;

	const char *files[] = {"fonts/Outfit-ExtraLight.ttf", "fonts/Outfit-Regular.ttf", "fonts/Outfit-Medium.ttf",
			       "fonts/Outfit-SemiBold.ttf", "fonts/Outfit-ExtraBold.ttf"};
	for (const char *file : files) {
		char *path = obs_module_file(file);
		if (!path) {
			continue;
		}
		int id = QFontDatabase::addApplicationFont(QString::fromUtf8(path));
		bfree(path);
		if (id >= 0 && family.isEmpty()) {
			QStringList names = QFontDatabase::applicationFontFamilies(id);
			if (!names.isEmpty()) {
				family = names.first();
			}
		}
	}
	if (family.isEmpty()) {
		family = QFont().family();
	}
	return family;
}

QString somun_window_style()
{
	QString font = somun_font_family();
	/* Só as nossas janelas: o tema do OBS continua valendo no Modo OBS. */
	return QString(
		       "QWidget#somunWindow { background: %1; }"
		       "QWidget#somunWindow, QWidget#somunWindow QWidget { font-family: '%2'; color: %3; }"
		       "QWidget#somunWindow QLabel { background: transparent; }"
		       "QWidget#somunWindow QPushButton, QWidget#somunWindow QToolButton {"
		       "  background: %4; color: %3; border: 1px solid %5; border-radius: 6px; padding: 4px 10px; }"
		       "QWidget#somunWindow QPushButton:hover, QWidget#somunWindow QToolButton:hover {"
		       "  background: %6; }"
		       "QWidget#somunWindow QPushButton:disabled { color: %7; }"
		       "QWidget#somunWindow QToolButton::menu-indicator { image: none; width: 0; }"
		       "QWidget#somunWindow QScrollArea { background: transparent; }"
		       "QWidget#somunWindow QScrollArea > QWidget > QWidget { background: transparent; }"
		       "QFrame#somunPanel { background: %8; border: 1px solid %9; border-radius: 8px; }"
		       "QLabel#somunPanelTitle { color: %10; font-weight: 600; letter-spacing: 1px; }")
		.arg(QString(SOMUN_BLACK))
		.arg(font)
		.arg(QString(SOMUN_TEXT))
		.arg(QString(SOMUN_PANEL2))
		.arg(QString(SOMUN_LINE2))
		.arg(QString("#2C2C2C"))
		.arg(QString(SOMUN_GRAY))
		.arg(QString(SOMUN_PANEL))
		.arg(QString(SOMUN_LINE))
		.arg(QString(SOMUN_GRAY2));
}

QLabel *somun_brand_label(QWidget *parent, const QString &word, int pixelSize)
{
	auto *label = new QLabel(parent);
	label->setTextFormat(Qt::RichText);
	label->setText(QString("<span style=\"font-family:'%1'; font-weight:800; font-size:%2px; color:%3;\">%4"
			       "<span style=\"color:%5;\">.</span></span>")
			       .arg(somun_font_family())
			       .arg(pixelSize)
			       .arg(QString(SOMUN_WHITE))
			       .arg(word.toHtmlEscaped())
			       .arg(QString(SOMUN_RED)));
	return label;
}

void somun_make_primary(QWidget *button)
{
	button->setStyleSheet(QString("QPushButton { background: %1; color: %2; border: none; border-radius: 6px;"
				      " font-weight: 600; padding: 6px 14px; }"
				      "QPushButton:hover { background: #E6E6E6; }"
				      "QPushButton:pressed { background: #CFCFCF; }")
				      .arg(QString(SOMUN_WHITE))
				      .arg(QString(SOMUN_BLACK)));
}
