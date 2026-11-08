/*
 * Projeto Live: destinos extras e gravação vertical dentro das Configurações do OBS, e bolinhas
 * na barra de baixo.
 *
 * (O nome do arquivo ficou da época do painel "Transmissão", que saiu em 07/11.)
 *
 * - Aba Transmissão do OBS: no alto continua a transmissão principal do próprio OBS (com o login
 *   do YouTube etc.); embaixo, na mesma aba, entram os destinos horizontais (YouTube, Facebook ou
 *   outro endereço) e os verticais (Instagram, TikTok ou outro endereço). Tudo fica numa área com
 *   rolagem.
 * - Aba Saída do OBS: no fim, a caixa "Tela vertical": gravação vertical (ligar, pasta e qualidade)
 *   e a transição do vertical (acompanha a do Programa ou uma só dele).
 * - As mudanças só valem no "Aplicar" ou "OK", como o resto das Configurações.
 * - Barra de baixo do OBS: uma bolinha por destino marcado enquanto a transmissão está no ar
 *   (verde no ar, amarelo ligando ou reconectando, vermelho com erro); passar o mouse mostra o
 *   detalhe e a velocidade.
 *
 * O trabalho de verdade fica em multistream.c e vertical-record.c.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "stream-dock.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "multistream.h"
#include "vertical-record.h"
#include "vertical.h"

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString mbps(double kbps)
{
	return QString::number(kbps / 1000.0, 'f', 1);
}

void copyText(char *out, size_t size, const QString &text)
{
	QByteArray bytes = text.toUtf8();
	size_t length = std::min((size_t)bytes.size(), size - 1);
	memcpy(out, bytes.constData(), length);
	out[length] = 0;
}

/* ------------------------------------------------------------------------- */
/* Plataformas                                                                */

struct Platform {
	const char *id;
	const char *textKey;
	const char *helpKey;
	bool horizontal;
	bool vertical;
};

const Platform platforms[] = {
	{"youtube", "Stream.Platform.YouTube", "Stream.Help.YouTube", true, false},
	{"facebook", "Stream.Platform.Facebook", "Stream.Help.Facebook", true, false},
	{"instagram", "Stream.Platform.Instagram", "Stream.Help.Instagram", false, true},
	{"tiktok", "Stream.Platform.TikTok", "Stream.Help.TikTok", false, true},
	{"custom", "Stream.Platform.Custom", "Stream.Help.Custom", true, true},
};

const Platform &findPlatform(const char *id)
{
	for (const Platform &platform : platforms) {
		if (strcmp(platform.id, id) == 0) {
			return platform;
		}
	}
	return platforms[4];
}

/* ------------------------------------------------------------------------- */
/* Janela para adicionar ou mudar um destino                                  */

class DestinationDialog : public QDialog {
public:
	DestinationDialog(QWidget *parent, const pl_dest_info &start, bool editing) : QDialog(parent), original(start)
	{
		setWindowTitle(moduleText(editing ? "Stream.EditTitle" : "Stream.AddTitle"));
		setMinimumWidth(480);
		auto *layout = new QVBoxLayout(this);
		auto *form = new QFormLayout();

		platform = new QComboBox(this);
		for (const Platform &item : platforms) {
			if (start.vertical ? item.vertical : item.horizontal) {
				platform->addItem(moduleText(item.textKey), QString::fromUtf8(item.id));
			}
		}
		name = new QLineEdit(this);
		name->setMaxLength(PL_DEST_NAME_MAX - 1);
		server = new QLineEdit(this);
		server->setMaxLength(PL_DEST_TEXT_MAX - 1);
		key = new QLineEdit(this);
		key->setMaxLength(PL_DEST_TEXT_MAX - 1);
		key->setEchoMode(QLineEdit::Password);
		auto *showKey = new QCheckBox(moduleText("Stream.ShowKey"), this);
		connect(showKey, &QCheckBox::toggled, this,
			[this](bool on) { key->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password); });

		form->addRow(moduleText("Stream.Platform"), platform);
		form->addRow(moduleText("Stream.Name"), name);
		form->addRow(moduleText("Stream.Server"), server);
		form->addRow(moduleText("Stream.Key"), key);
		form->addRow(QString(), showKey);
		layout->addLayout(form);

		help = new QLabel(this);
		help->setWordWrap(true);
		help->setTextFormat(Qt::PlainText);
		layout->addWidget(help);

		/* Qualidade: a mesma da principal ou própria (codificador e Mbps). */
		auto *qualityBox = new QGroupBox(moduleText("Stream.Quality"), this);
		auto *qualityLayout = new QVBoxLayout(qualityBox);
		sameQuality = new QRadioButton(moduleText(start.vertical ? "Multistream.Quality.SameVertical"
									 : "Stream.Quality.Same"),
					       qualityBox);
		ownQuality = new QRadioButton(moduleText("Stream.Quality.Own"), qualityBox);
		qualityLayout->addWidget(sameQuality);
		qualityLayout->addWidget(ownQuality);

		auto *ownForm = new QFormLayout();
		bitrate = new QDoubleSpinBox(qualityBox);
		bitrate->setRange(0.5, 50.0);
		bitrate->setSingleStep(0.5);
		bitrate->setDecimals(1);
		bitrate->setSuffix(" Mbps");
		encoder = new QComboBox(qualityBox);
		encoder->addItem(moduleText("Multistream.Encoder.Auto"), QString());
		pl_multistream_enum_encoders(
			[](void *param, const char *id, const char *label) {
				static_cast<QComboBox *>(param)->addItem(QString::fromUtf8(label),
									 QString::fromUtf8(id));
			},
			encoder);
		ownForm->addRow(moduleText("Multistream.Bitrate"), bitrate);
		ownForm->addRow(moduleText("Multistream.Encoder"), encoder);
		qualityLayout->addLayout(ownForm);

		size = new QComboBox(qualityBox);
		size->addItem(moduleText("Multistream.Size.Full"), 1080);
		size->addItem(moduleText("Multistream.Size.Small"), 720);
		if (start.vertical) {
			auto *sizeForm = new QFormLayout();
			sizeForm->addRow(moduleText("Multistream.Size"), size);
			qualityLayout->addLayout(sizeForm);
		} else {
			size->hide();
		}

		auto *qualityHelp = new QLabel(moduleText("Stream.Quality.Help"), qualityBox);
		qualityHelp->setWordWrap(true);
		qualityLayout->addWidget(qualityHelp);
		layout->addWidget(qualityBox);
		connect(ownQuality, &QRadioButton::toggled, this, [this](bool) { updateQuality(); });

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		connect(buttons, &QDialogButtonBox::accepted, this, [this]() { tryAccept(); });
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		layout->addWidget(buttons);

		/* Preenche com o destino atual. */
		int index = platform->findData(QString::fromUtf8(start.platform));
		platform->setCurrentIndex(index < 0 ? 0 : index);
		lastPlatform = platform->currentData().toString();
		name->setText(QString::fromUtf8(start.name));
		server->setText(QString::fromUtf8(start.server));
		key->setText(QString::fromUtf8(start.key));
		if (!editing) {
			applyPlatformDefaults(QString());
		}
		sameQuality->setChecked(!start.own_quality);
		ownQuality->setChecked(start.own_quality);
		bitrate->setValue(start.bitrate_kbps > 0 ? start.bitrate_kbps / 1000.0 : (start.vertical ? 4.0 : 4.5));
		int encoderIndex = encoder->findData(QString::fromUtf8(start.encoder));
		encoder->setCurrentIndex(encoderIndex < 0 ? 0 : encoderIndex);
		size->setCurrentIndex(start.width > 0 && start.width < 1080 ? 1 : 0);
		updateQuality();
		updateHelp();

		connect(platform, &QComboBox::currentIndexChanged, this, [this](int) {
			applyPlatformDefaults(lastPlatform);
			lastPlatform = platform->currentData().toString();
			updateHelp();
		});
	}

	pl_dest_info result() const
	{
		pl_dest_info info = original;
		copyText(info.platform, sizeof(info.platform), platform->currentData().toString());
		copyText(info.name, sizeof(info.name), name->text().trimmed());
		copyText(info.server, sizeof(info.server), server->text().trimmed());
		copyText(info.key, sizeof(info.key), key->text().trimmed());
		info.own_quality = ownQuality->isChecked();
		info.bitrate_kbps = (int)(bitrate->value() * 1000.0 + 0.5);
		copyText(info.encoder, sizeof(info.encoder), encoder->currentData().toString());
		if (info.vertical && size->currentData().toInt() == 720) {
			info.width = 720;
			info.height = 1280;
		} else {
			info.width = 0;
			info.height = 0;
		}
		return info;
	}

private:
	void updateQuality()
	{
		/* O vertical sempre tem codificador próprio: escolher o codificador vale nos dois casos. */
		bool own = ownQuality->isChecked();
		bitrate->setEnabled(own);
		encoder->setEnabled(own || original.vertical);
	}

	/* Ao trocar de plataforma, troca o nome e o endereço se ainda eram os padrões da anterior. */
	void applyPlatformDefaults(const QString &previous)
	{
		QByteArray id = platform->currentData().toString().toUtf8();
		const Platform &now = findPlatform(id.constData());

		QString previousName;
		QString previousServer;
		if (!previous.isEmpty()) {
			QByteArray previousId = previous.toUtf8();
			previousName = moduleText(findPlatform(previousId.constData()).textKey);
			previousServer = QString::fromUtf8(pl_multistream_default_server(previousId.constData()));
		}
		if (name->text().trimmed().isEmpty() || name->text() == previousName) {
			name->setText(strcmp(now.id, "custom") == 0 ? QString() : moduleText(now.textKey));
		}
		if (server->text().trimmed().isEmpty() || server->text() == previousServer) {
			server->setText(QString::fromUtf8(pl_multistream_default_server(now.id)));
		}
	}

	void updateHelp()
	{
		QByteArray id = platform->currentData().toString().toUtf8();
		help->setText(moduleText(findPlatform(id.constData()).helpKey));
	}

	void tryAccept()
	{
		if (name->text().trimmed().isEmpty() || server->text().trimmed().isEmpty() ||
		    key->text().trimmed().isEmpty()) {
			QMessageBox::warning(this, windowTitle(), moduleText("Stream.FillAll"));
			return;
		}
		accept();
	}

	pl_dest_info original;
	QComboBox *platform = nullptr;
	QLineEdit *name = nullptr;
	QLineEdit *server = nullptr;
	QLineEdit *key = nullptr;
	QLabel *help = nullptr;
	QRadioButton *sameQuality = nullptr;
	QRadioButton *ownQuality = nullptr;
	QDoubleSpinBox *bitrate = nullptr;
	QComboBox *encoder = nullptr;
	QComboBox *size = nullptr;
	QString lastPlatform;
};

/* ------------------------------------------------------------------------- */
/* Aba "Multistream"                                                          */

/* Uma das duas listas (horizontais ou verticais), mexendo na cópia de trabalho. */
class DestinationsBox : public QGroupBox {
public:
	DestinationsBox(QWidget *parent, const QString &title, bool isVertical, std::vector<pl_dest_info> &list,
			std::function<void()> onChange)
		: QGroupBox(title, parent),
		  vertical(isVertical),
		  dests(list),
		  changed(std::move(onChange))
	{
		auto *layout = new QVBoxLayout(this);
		view = new QListWidget(this);
		view->setMinimumHeight(110);
		layout->addWidget(view);
		connect(view, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
			if (rebuilding) {
				return;
			}
			int index = indexOfRow(view->row(item));
			if (index >= 0) {
				dests[index].enabled = item->checkState() == Qt::Checked;
				changed();
			}
		});
		connect(view, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) { edit(); });

		auto *buttons = new QHBoxLayout();
		auto *add = new QPushButton(moduleText("Stream.Add"), this);
		auto *editButton = new QPushButton(moduleText("Stream.Edit"), this);
		auto *remove = new QPushButton(moduleText("Stream.Remove"), this);
		connect(add, &QPushButton::clicked, this, [this]() { addDestination(); });
		connect(editButton, &QPushButton::clicked, this, [this]() { edit(); });
		connect(remove, &QPushButton::clicked, this, [this]() { removeDestination(); });
		buttons->addWidget(add);
		buttons->addWidget(editButton);
		buttons->addWidget(remove);
		buttons->addStretch(1);
		layout->addLayout(buttons);
		rebuild();
	}

	void rebuild()
	{
		rebuilding = true;
		view->clear();
		rows.clear();
		for (size_t i = 0; i < dests.size(); i++) {
			const pl_dest_info &info = dests[i];
			if (info.vertical != vertical) {
				continue;
			}
			QString text = QString::fromUtf8(info.name) + QString::fromUtf8("  —  ") +
				       moduleText(findPlatform(info.platform).textKey);
			if (info.own_quality) {
				text += QString::fromUtf8(" · ") +
					moduleText("Stream.Row.Own").arg(mbps(info.bitrate_kbps));
			} else if (!vertical) {
				text += QString::fromUtf8(" · ") + moduleText("Stream.Row.Same");
			}
			if (vertical && info.width > 0 && info.width < 1080) {
				text += QString::fromUtf8(" · 720x1280");
			}
			auto *item = new QListWidgetItem(text);
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			item->setCheckState(info.enabled ? Qt::Checked : Qt::Unchecked);
			view->addItem(item);
			rows.push_back((int)i);
		}
		if (rows.empty()) {
			auto *item = new QListWidgetItem(moduleText("Multistream.EmptyList"));
			item->setFlags(Qt::NoItemFlags);
			view->addItem(item);
		}
		rebuilding = false;
	}

private:
	int indexOfRow(int row) const { return row >= 0 && row < (int)rows.size() ? rows[row] : -1; }

	void addDestination()
	{
		pl_dest_info start = {};
		snprintf(start.platform, sizeof(start.platform), "%s", vertical ? "instagram" : "youtube");
		start.vertical = vertical;
		start.enabled = true;
		start.bitrate_kbps = vertical ? 4000 : 4500;
		DestinationDialog dialog(this, start, false);
		if (dialog.exec() == QDialog::Accepted) {
			dests.push_back(dialog.result());
			rebuild();
			changed();
		}
	}

	void edit()
	{
		int index = indexOfRow(view->currentRow());
		if (index < 0) {
			return;
		}
		DestinationDialog dialog(this, dests[index], true);
		if (dialog.exec() == QDialog::Accepted) {
			dests[index] = dialog.result();
			rebuild();
			changed();
		}
	}

	void removeDestination()
	{
		int index = indexOfRow(view->currentRow());
		if (index < 0) {
			return;
		}
		QString question = moduleText("Stream.RemoveQuestion").arg(QString::fromUtf8(dests[index].name));
		if (QMessageBox::question(this, moduleText("Stream.Remove"), question) == QMessageBox::Yes) {
			dests.erase(dests.begin() + index);
			rebuild();
			changed();
		}
	}

	bool vertical;
	std::vector<pl_dest_info> &dests;
	std::function<void()> changed;
	QListWidget *view = nullptr;
	std::vector<int> rows;
	bool rebuilding = false;
};

/* Os destinos extras, embaixo da transmissão principal na aba Transmissão. */
class DestinationsSection : public QWidget {
public:
	DestinationsSection(QWidget *parent, std::function<void()> onChange) : QWidget(parent), changed(onChange)
	{
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 8, 0, 0);
		reload();

		auto *help = new QLabel(moduleText("Multistream.Help"), this);
		help->setWordWrap(true);
		layout->addWidget(help);

		horizontal = new DestinationsBox(this, moduleText("Multistream.Horizontal"), false, dests, changed);
		vertical = new DestinationsBox(this, moduleText("Multistream.Vertical"), true, dests, changed);
		layout->addWidget(horizontal);
		layout->addWidget(vertical);
	}

	void commit()
	{
		pl_multistream_set_all(dests.data(), dests.size());
		/* Os destinos novos ganharam identificador ao salvar: relê para o próximo "Aplicar". */
		reload();
		horizontal->rebuild();
		vertical->rebuild();
	}

private:
	void reload()
	{
		dests.clear();
		for (size_t i = 0; i < pl_multistream_count(); i++) {
			pl_dest_info info;
			if (pl_multistream_get(i, &info)) {
				dests.push_back(info);
			}
		}
	}

	std::function<void()> changed;
	std::vector<pl_dest_info> dests;
	DestinationsBox *horizontal = nullptr;
	DestinationsBox *vertical = nullptr;
};

/* A caixa "Tela vertical" (gravação e transição do vertical), no fim da aba Saída. */
class RecordSection : public QGroupBox {
public:
	RecordSection(QWidget *parent, std::function<void()> onChange)
		: QGroupBox(moduleText("Multistream.Record"), parent),
		  changed(onChange)
	{
		auto *form = new QFormLayout(this);
		recordOn = new QCheckBox(moduleText("Multistream.Record.Enable"), this);
		recordOn->setChecked(pl_vertical_pref("record"));
		folder = new QLineEdit(QString::fromUtf8(pl_vertical_pref_string("record_folder")), this);
		folder->setPlaceholderText(moduleText("Multistream.Record.FolderHint"));
		auto *browse = new QPushButton(moduleText("Multistream.Record.Browse"), this);
		auto *folderLine = new QHBoxLayout();
		folderLine->addWidget(folder, 1);
		folderLine->addWidget(browse);
		recordKbps = new QDoubleSpinBox(this);
		recordKbps->setRange(1.0, 50.0);
		recordKbps->setSingleStep(1.0);
		recordKbps->setDecimals(1);
		recordKbps->setSuffix(" Mbps");
		int kbps = pl_vertical_pref_int("record_kbps");
		recordKbps->setValue(kbps > 0 ? kbps / 1000.0 : 10.0);
		form->addRow(recordOn);
		form->addRow(moduleText("Multistream.Record.Folder"), folderLine);
		form->addRow(moduleText("Multistream.Record.Quality"), recordKbps);

		/* Transição do vertical: acompanha a do Programa (padrão) ou uma só dele. */
		transitionType = new QComboBox(this);
		transitionType->addItem(moduleText("Vertical.Transition.Follow"), QString());
		const char *id;
		for (size_t i = 0; obs_enum_transition_types(i, &id); i++) {
			if (obs_get_source_output_flags(id) & (OBS_SOURCE_DEPRECATED | OBS_SOURCE_CAP_DISABLED)) {
				continue;
			}
			const char *label = obs_source_get_display_name(id);
			transitionType->addItem(QString::fromUtf8(label ? label : id), QString::fromUtf8(id));
		}
		int chosen = 0;
		if (pl_vertical_pref("transition_own")) {
			chosen = transitionType->findData(QString::fromUtf8(pl_vertical_pref_string("transition_id")));
		}
		transitionType->setCurrentIndex(chosen < 0 ? 0 : chosen);
		transitionMs = new QSpinBox(this);
		transitionMs->setRange(50, 20000);
		transitionMs->setSingleStep(50);
		transitionMs->setSuffix(" ms");
		transitionMs->setValue(pl_vertical_pref_int("transition_ms"));
		auto *transitionProps = new QPushButton(QString::fromUtf8("⚙"), this);
		transitionProps->setToolTip(moduleText("Vertical.Transition.Properties"));
		auto *transitionLine = new QHBoxLayout();
		transitionLine->addWidget(transitionType, 1);
		transitionLine->addWidget(transitionMs);
		transitionLine->addWidget(transitionProps);
		form->addRow(moduleText("Vertical.Transition"), transitionLine);
		auto updateTransition = [this, transitionProps]() {
			bool own = transitionType->currentIndex() > 0;
			transitionMs->setEnabled(own);
			/* As propriedades são da transição que já está valendo (depois do Aplicar). */
			transitionProps->setEnabled(own && pl_vertical_pref("transition_own"));
		};
		updateTransition();
		connect(transitionType, &QComboBox::currentIndexChanged, this, [this, updateTransition](int) {
			updateTransition();
			changed();
		});
		connect(transitionMs, &QSpinBox::valueChanged, this, [this](int) { changed(); });
		connect(transitionProps, &QPushButton::clicked, this, []() {
			obs_source_t *current = pl_vertical_get_transition();
			if (current) {
				obs_frontend_open_source_properties(current);
			}
			obs_source_release(current);
		});

		connect(browse, &QPushButton::clicked, this, [this]() {
			QString dir = QFileDialog::getExistingDirectory(this, moduleText("Multistream.Record.Browse"),
									folder->text());
			if (!dir.isEmpty()) {
				folder->setText(dir);
				changed();
			}
		});
		connect(recordOn, &QCheckBox::toggled, this, [this](bool) { changed(); });
		connect(folder, &QLineEdit::textEdited, this, [this](const QString &) { changed(); });
		connect(recordKbps, &QDoubleSpinBox::valueChanged, this, [this](double) { changed(); });
	}

	void commit()
	{
		pl_vertical_set_pref("record", recordOn->isChecked());
		QByteArray path = folder->text().trimmed().toUtf8();
		pl_vertical_set_pref_string("record_folder", path.constData());
		pl_vertical_set_pref_int("record_kbps", (int)(recordKbps->value() * 1000.0 + 0.5));

		QByteArray transitionId = transitionType->currentData().toString().toUtf8();
		bool own = !transitionId.isEmpty();
		pl_vertical_set_pref("transition_own", own);
		if (own) {
			pl_vertical_set_pref_string("transition_id", transitionId.constData());
		}
		pl_vertical_set_pref_int("transition_ms", transitionMs->value());
		pl_vertical_transition_changed();
	}

private:
	std::function<void()> changed;
	QCheckBox *recordOn = nullptr;
	QLineEdit *folder = nullptr;
	QDoubleSpinBox *recordKbps = nullptr;
	QComboBox *transitionType = nullptr;
	QSpinBox *transitionMs = nullptr;
};

/* Põe as partes do Projeto Live nas abas Transmissão e Saída quando as Configurações abrem. */
class SettingsInjector : public QObject {
public:
	using QObject::QObject;

protected:
	bool eventFilter(QObject *object, QEvent *event) override
	{
		if (event->type() == QEvent::Show && object->objectName() == QStringLiteral("OBSBasicSettings") &&
		    !object->property("projetoLive").toBool()) {
			if (auto *dialog = qobject_cast<QDialog *>(object)) {
				dialog->setProperty("projetoLive", true);
				inject(dialog);
			}
		}
		return QObject::eventFilter(object, event);
	}

private:
	/*
	 * A aba Transmissão não tem rolagem: o que já está nela vai para dentro de uma área com
	 * rolagem, e os destinos entram embaixo.
	 */
	static void wrapInScroll(QWidget *page, QWidget *extra)
	{
		QLayout *old = page->layout();
		if (!old) {
			return;
		}
		auto *scroll = new QScrollArea(page);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		auto *content = new QWidget(scroll);
		auto *layout = new QVBoxLayout(content);
		layout->setContentsMargins(old->contentsMargins());
		while (QLayoutItem *item = old->takeAt(0)) {
			if (item->spacerItem()) {
				/* As molas do fim empurravam tudo para cima: aqui a rolagem cuida disso. */
				delete item;
				continue;
			}
			if (QWidget *widget = item->widget()) {
				layout->addWidget(widget);
			} else if (QLayout *inner = item->layout()) {
				layout->addLayout(inner);
			}
			delete item;
		}
		extra->setParent(content);
		layout->addWidget(extra);
		layout->addStretch(1);
		scroll->setWidget(content);
		old->setContentsMargins(0, 0, 0, 0);
		old->addWidget(scroll);
	}

	static void inject(QDialog *dialog)
	{
		auto *streamPage = dialog->findChild<QWidget *>("streamPage");
		auto *outputPage = dialog->findChild<QWidget *>("outputPage");
		auto *buttons = dialog->findChild<QDialogButtonBox *>("buttonBox");
		if (!streamPage || !outputPage || !buttons || !outputPage->layout()) {
			blog(LOG_WARNING, "[projeto-live] não achei as partes da janela de Configurações");
			return;
		}

		auto enableApply = [buttons]() {
			if (QAbstractButton *apply = buttons->button(QDialogButtonBox::Apply)) {
				apply->setEnabled(true);
			}
		};
		auto *destinations = new DestinationsSection(streamPage, enableApply);
		wrapInScroll(streamPage, destinations);

		auto *record = new RecordSection(outputPage, enableApply);
		outputPage->layout()->addWidget(record);

		QObject::connect(buttons, &QDialogButtonBox::clicked, destinations,
				 [buttons, destinations, record](QAbstractButton *button) {
					 QDialogButtonBox::ButtonRole role = buttons->buttonRole(button);
					 if (role == QDialogButtonBox::ApplyRole ||
					     role == QDialogButtonBox::AcceptRole) {
						 destinations->commit();
						 record->commit();
					 }
				 });
	}
};

/* ------------------------------------------------------------------------- */
/* Bolinhas na barra de baixo                                                 */

class StatusDots : public QWidget {
public:
	explicit StatusDots(QWidget *parent) : QWidget(parent)
	{
		label = new QLabel(this);
		label->setTextFormat(Qt::RichText);
		auto *layout = new QHBoxLayout(this);
		layout->setContentsMargins(6, 0, 6, 0);
		layout->addWidget(label);
		hide();
	}

	void refresh()
	{
		bool live = pl_multistream_main_live();
		QStringList parts;
		QStringList tips;
		qint64 now = QDateTime::currentMSecsSinceEpoch();

		for (size_t i = 0; i < pl_multistream_count(); i++) {
			pl_dest_info info;
			pl_dest_status status;
			if (!pl_multistream_get(i, &info) || !info.enabled) {
				continue;
			}
			pl_multistream_get_status(i, &status);
			if (!live && (status.state == PL_DEST_OFF || status.state == PL_DEST_WAITING)) {
				continue;
			}

			/* Velocidade real: bytes enviados desde a última olhada. */
			Speed &speed = speeds[std::string(info.id)];
			if (status.total_bytes < speed.bytes || !status.total_bytes) {
				speed.kbps = 0.0;
			} else if (speed.ms && now - speed.ms >= 500) {
				double kbps =
					(double)(status.total_bytes - speed.bytes) * 8.0 / (double)(now - speed.ms);
				speed.kbps = speed.kbps > 0.0 ? speed.kbps * 0.6 + kbps * 0.4 : kbps;
			}
			if (!speed.ms || now - speed.ms >= 500 || status.total_bytes < speed.bytes) {
				speed.bytes = status.total_bytes;
				speed.ms = now;
			}

			QString color = QStringLiteral("#8a8a8a");
			QString state = moduleText("Stream.State.Waiting");
			switch (status.state) {
			case PL_DEST_LIVE:
				color = QStringLiteral("#3fb950");
				state = moduleText("Director.OnAir") + QString::fromUtf8(" · ") + mbps(speed.kbps) +
					QStringLiteral(" Mbps");
				break;
			case PL_DEST_CONNECTING:
				color = QStringLiteral("#e0a800");
				state = moduleText("Stream.State.Connecting");
				break;
			case PL_DEST_RECONNECTING:
				color = QStringLiteral("#e0a800");
				state = moduleText("Stream.State.Reconnecting");
				break;
			case PL_DEST_ERROR:
				color = QStringLiteral("#e5484d");
				state = QString::fromUtf8(status.error);
				break;
			default:
				break;
			}
			QString name = QString::fromUtf8(info.name).toHtmlEscaped();
			parts.append(QStringLiteral("<span style='color:%1'>&#9679;</span> %2").arg(color, name));
			tips.append(QString::fromUtf8(info.name) + QStringLiteral(": ") + state);
		}

		if (parts.isEmpty()) {
			hide();
			return;
		}
		label->setText(parts.join(QStringLiteral("&nbsp;&nbsp;")));
		setToolTip(tips.join(QStringLiteral("\n")));
		show();
	}

private:
	struct Speed {
		uint64_t bytes = 0;
		qint64 ms = 0;
		double kbps = 0.0;
	};

	QLabel *label = nullptr;
	std::map<std::string, Speed> speeds;
};

SettingsInjector *injector = nullptr;
StatusDots *dots = nullptr;
QTimer *timer = nullptr;

void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		if (timer) {
			timer->stop();
		}
		if (injector) {
			qApp->removeEventFilter(injector);
		}
	}
}

} // namespace

void pl_multistream_ui_load(void)
{
	auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!mainWindow) {
		return;
	}

	injector = new SettingsInjector(mainWindow);
	qApp->installEventFilter(injector);

	if (QStatusBar *bar = mainWindow->statusBar()) {
		dots = new StatusDots(bar);
		bar->insertPermanentWidget(0, dots);
		QObject::connect(dots, &QObject::destroyed, []() { dots = nullptr; });
	}

	timer = new QTimer(mainWindow);
	QObject::connect(timer, &QTimer::timeout, []() {
		if (dots) {
			dots->refresh();
		}
	});
	timer->start(1000);
	obs_frontend_add_event_callback(frontendEvent, nullptr);
}
