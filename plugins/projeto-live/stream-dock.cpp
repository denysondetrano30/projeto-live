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
#include <QAbstractItemModel>
#include <QAction>
#include <QBrush>
#include <QColor>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QBoxLayout>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QStringList>
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

#include "intercom-audio.h"
#include "intercom.h"
#include "multistream.h"
#include "somun-style.h"
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
			platform->addItem(moduleText(item.textKey), QString::fromUtf8(item.id));
		}
		/* Horizontal recebe o No ar; vertical, o No ar vertical. */
		format = new QComboBox(this);
		format->addItem(moduleText("Stream.Format.Horizontal"), false);
		format->addItem(moduleText("Stream.Format.Vertical"), true);
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
		form->addRow(moduleText("Stream.Format"), format);
		form->addRow(moduleText("Stream.Name"), name);
		form->addRow(moduleText("Stream.Server"), server);
		form->addRow(moduleText("Stream.Key"), key);
		form->addRow(QString(), showKey);
		layout->addLayout(form);

		/* A explicação de onde achar a chave fica na dica do mouse, no campo da chave. */
		help = new QLabel(this);
		help->hide();

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		connect(buttons, &QDialogButtonBox::accepted, this, [this]() { tryAccept(); });
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		layout->addWidget(buttons);

		/* Preenche com o destino atual. */
		int index = platform->findData(QString::fromUtf8(start.platform));
		platform->setCurrentIndex(index < 0 ? 0 : index);
		format->setCurrentIndex(start.vertical ? 1 : 0);
		lastPlatform = platform->currentData().toString();
		name->setText(QString::fromUtf8(start.name));
		server->setText(QString::fromUtf8(start.server));
		key->setText(QString::fromUtf8(start.key));
		if (!editing) {
			applyPlatformDefaults(QString());
		}
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
		info.vertical = format->currentData().toBool();
		return info;
	}

private:
	/* Ao trocar de plataforma, troca o nome, o endereço e o formato se ainda eram os da anterior. */
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
		/* YouTube e Facebook são horizontais; Instagram e TikTok, verticais; "outro" fica como está. */
		if (now.horizontal != now.vertical) {
			format->setCurrentIndex(now.vertical ? 1 : 0);
		}
	}

	void updateHelp()
	{
		QByteArray id = platform->currentData().toString().toUtf8();
		help->setText(moduleText(findPlatform(id.constData()).helpKey));
		key->setToolTip(help->text());
		server->setToolTip(help->text());
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
	QComboBox *format = nullptr;
	QLineEdit *name = nullptr;
	QLineEdit *server = nullptr;
	QLineEdit *key = nullptr;
	QLabel *help = nullptr;
	QString lastPlatform;
};

/* ------------------------------------------------------------------------- */
/* A lista de destinos (uma só, horizontais e verticais juntos)               */

/* "Qualidade: 6,0 Mb/s (a da aba Saída das Configurações)". */
QString qualityText()
{
	QString value = QString::number(pl_multistream_quality_kbps() / 1000.0, 'f', 1).replace('.', ',');
	return moduleText("Multistream.QualityFromObs").arg(value);
}

/*
 * Mexe numa cópia de trabalho: nada vale até quem a usa chamar pl_multistream_set_all (o Salvar da
 * janela Destinos ou o Aplicar das Configurações). Arrastar muda a ordem; botão direito na linha:
 * Mudar e Remover; dois cliques: Mudar. Com `status`, cada linha mostra o estado do destino no ar.
 */
class DestinationsList : public QWidget {
public:
	DestinationsList(QWidget *parent, std::vector<pl_dest_info> &list, std::function<void()> onChange, bool status)
		: QWidget(parent),
		  dests(list),
		  changed(std::move(onChange))
	{
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		view = new QListWidget(this);
		view->setMinimumHeight(140);
		view->setDragDropMode(QAbstractItemView::InternalMove);
		view->setDefaultDropAction(Qt::MoveAction);
		view->setContextMenuPolicy(Qt::CustomContextMenu);
		layout->addWidget(view);

		connect(view, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
			if (rebuilding) {
				return;
			}
			int index = item->data(Qt::UserRole).toInt();
			if (index >= 0 && index < (int)dests.size()) {
				dests[index].enabled = item->checkState() == Qt::Checked;
				changed();
			}
		});
		connect(view, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) { edit(item); });
		connect(view->model(), &QAbstractItemModel::rowsMoved, this, [this]() { syncOrder(); });
		connect(view, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
			QListWidgetItem *item = view->itemAt(pos);
			QMenu menu(this);
			QAction *editAction = nullptr;
			QAction *removeAction = nullptr;
			if (item && item->data(Qt::UserRole).toInt() >= 0) {
				editAction = menu.addAction(moduleText("Stream.Edit"));
				removeAction = menu.addAction(moduleText("Stream.Remove"));
				menu.addSeparator();
			}
			QAction *addAction = menu.addAction(moduleText("Stream.Add"));
			QAction *chosen = menu.exec(view->viewport()->mapToGlobal(pos));
			if (!chosen) {
				return;
			}
			if (chosen == editAction) {
				edit(item);
			} else if (chosen == removeAction) {
				removeDestination(item);
			} else if (chosen == addAction) {
				addDestination();
			}
		});

		auto *add = new QPushButton(moduleText("Stream.Add"), this);
		connect(add, &QPushButton::clicked, this, [this]() { addDestination(); });
		auto *buttons = new QHBoxLayout();
		buttons->addWidget(add);
		buttons->addStretch(1);
		layout->addLayout(buttons);
		rebuild();

		if (status) {
			auto *timer = new QTimer(this);
			connect(timer, &QTimer::timeout, this, [this]() { refreshStatus(); });
			timer->start(1000);
		}
	}

	void rebuild()
	{
		rebuilding = true;
		view->clear();
		for (size_t i = 0; i < dests.size(); i++) {
			auto *item = new QListWidgetItem(rowText(dests[i], QString()));
			item->setFlags((item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled) &
				       ~Qt::ItemIsDropEnabled);
			item->setCheckState(dests[i].enabled ? Qt::Checked : Qt::Unchecked);
			item->setData(Qt::UserRole, (int)i);
			view->addItem(item);
		}
		if (dests.empty()) {
			auto *item = new QListWidgetItem(moduleText("Multistream.EmptyList"));
			item->setFlags(Qt::NoItemFlags);
			item->setData(Qt::UserRole, -1);
			view->addItem(item);
		}
		rebuilding = false;
		refreshStatus();
	}

private:
	static QString rowText(const pl_dest_info &info, const QString &state)
	{
		QString platformName = moduleText(findPlatform(info.platform).textKey);
		QString text = QString::fromUtf8(info.name) + QStringLiteral("   ·   ") +
			       moduleText(info.vertical ? "Stream.Format.VerticalTag" : "Stream.Format.HorizontalTag");
		if (strcmp(info.platform, "custom") != 0 && QString::fromUtf8(info.name) != platformName) {
			text += QStringLiteral("   ·   ") + platformName;
		}
		if (!state.isEmpty()) {
			text += QStringLiteral("   ·   ") + state;
		}
		return text;
	}

	/* O estado de cada destino já salvo (os novos só ganham estado depois do Salvar). */
	QString stateOf(const pl_dest_info &info, bool &red) const
	{
		red = false;
		if (!pl_multistream_active() || !*info.id) {
			return QString();
		}
		for (size_t i = 0; i < pl_multistream_count(); i++) {
			pl_dest_info saved;
			if (!pl_multistream_get(i, &saved) || strcmp(saved.id, info.id) != 0) {
				continue;
			}
			pl_dest_status status;
			pl_multistream_get_status(i, &status);
			switch (status.state) {
			case PL_DEST_LIVE:
				red = true;
				return moduleText("Stream.State.Live");
			case PL_DEST_CONNECTING:
				return moduleText("Stream.State.Connecting");
			case PL_DEST_RECONNECTING:
				red = true;
				return moduleText("Stream.State.Reconnecting");
			case PL_DEST_ERROR:
				red = true;
				return QString::fromUtf8(status.error);
			case PL_DEST_PAUSED:
				return moduleText("Stream.State.Paused");
			default:
				return QString();
			}
		}
		return QString();
	}

	void refreshStatus()
	{
		for (int row = 0; row < view->count(); row++) {
			QListWidgetItem *item = view->item(row);
			int index = item->data(Qt::UserRole).toInt();
			if (index < 0 || index >= (int)dests.size()) {
				continue;
			}
			bool red = false;
			QString state = stateOf(dests[index], red);
			QString text = rowText(dests[index], state);
			if (item->text() != text) {
				QSignalBlocker block(view);
				item->setText(text);
				item->setToolTip(state);
				item->setForeground(red ? QBrush(QColor(SOMUN_RED)) : QBrush());
			}
		}
	}

	/* Depois de arrastar: a ordem da lista na tela vira a ordem da cópia de trabalho. */
	void syncOrder()
	{
		std::vector<pl_dest_info> ordered;
		for (int row = 0; row < view->count(); row++) {
			int index = view->item(row)->data(Qt::UserRole).toInt();
			if (index >= 0 && index < (int)dests.size()) {
				ordered.push_back(dests[index]);
			}
		}
		if (ordered.size() != dests.size()) {
			return;
		}
		dests = ordered;
		QTimer::singleShot(0, this, [this]() { rebuild(); });
		changed();
	}

	void addDestination()
	{
		pl_dest_info start = {};
		snprintf(start.platform, sizeof(start.platform), "%s", "youtube");
		start.vertical = false;
		start.enabled = true;
		DestinationDialog dialog(this, start, false);
		if (dialog.exec() == QDialog::Accepted) {
			dests.push_back(dialog.result());
			rebuild();
			changed();
		}
	}

	void edit(QListWidgetItem *item)
	{
		int index = item ? item->data(Qt::UserRole).toInt() : -1;
		if (index < 0 || index >= (int)dests.size()) {
			return;
		}
		DestinationDialog dialog(this, dests[index], true);
		if (dialog.exec() == QDialog::Accepted) {
			dests[index] = dialog.result();
			rebuild();
			changed();
		}
	}

	void removeDestination(QListWidgetItem *item)
	{
		int index = item ? item->data(Qt::UserRole).toInt() : -1;
		if (index < 0 || index >= (int)dests.size()) {
			return;
		}
		QString question = moduleText("Stream.RemoveQuestion").arg(QString::fromUtf8(dests[index].name));
		if (QMessageBox::question(this, moduleText("Stream.Remove"), question) == QMessageBox::Yes) {
			dests.erase(dests.begin() + index);
			rebuild();
			changed();
		}
	}

	std::vector<pl_dest_info> &dests;
	std::function<void()> changed;
	QListWidget *view = nullptr;
	bool rebuilding = false;
};

std::vector<pl_dest_info> savedDestinations()
{
	std::vector<pl_dest_info> list;
	for (size_t i = 0; i < pl_multistream_count(); i++) {
		pl_dest_info info;
		if (pl_multistream_get(i, &info)) {
			list.push_back(info);
		}
	}
	return list;
}

/*
 * A transmissão do somun live, no alto da aba Transmissão (parte G): a qualidade (a da aba Saída,
 * só mostrada aqui) e a lista de destinos. O "Transmitir" da tela Operar liga todos os marcados.
 * A transmissão do próprio OBS fica embaixo, valendo só no Modo OBS.
 */
class DestinationsSection : public QWidget {
public:
	DestinationsSection(QWidget *parent, std::function<void()> onChange) : QWidget(parent), changed(onChange)
	{
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 8);
		dests = savedDestinations();

		auto *title = new QLabel(moduleText("Multistream.Title"), this);
		title->setTextFormat(Qt::RichText);
		title->setWordWrap(true);
		layout->addWidget(title);

		quality = new QLabel(qualityText(), this);
		quality->setWordWrap(true);
		quality->setStyleSheet(QStringLiteral("color: #8C8C8C;"));
		layout->addWidget(quality);

		setToolTip(moduleText("Multistream.Help"));
		list = new DestinationsList(this, dests, changed, false);
		layout->addWidget(list);

		auto *obsTitle = new QLabel(moduleText("Multistream.ObsOnly"), this);
		obsTitle->setTextFormat(Qt::RichText);
		obsTitle->setWordWrap(true);
		obsTitle->setContentsMargins(0, 12, 0, 0);
		layout->addWidget(obsTitle);
	}

	void commit()
	{
		pl_multistream_set_all(dests.data(), dests.size());
		/* Os destinos novos ganharam identificador ao salvar: relê para o próximo "Aplicar". */
		dests = savedDestinations();
		list->rebuild();
		/* A taxa de bits pode ter mudado na aba Saída no mesmo "Aplicar" (o OBS salva depois). */
		QPointer<QLabel> label(quality);
		QTimer::singleShot(500, this, [label]() {
			if (label) {
				label->setText(qualityText());
			}
		});
	}

private:
	std::function<void()> changed;
	std::vector<pl_dest_info> dests;
	QLabel *quality = nullptr;
	DestinationsList *list = nullptr;
};

/*
 * A janela "Destinos" do somun live (botão direito no Transmitir, ou "⋯ › Destinos..."): a mesma
 * lista, com o estado de cada destino no ar. As mudanças só valem no Salvar.
 */
class DestinationsWindow : public QDialog {
public:
	explicit DestinationsWindow(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle(moduleText("Destinations.Title"));
		setAttribute(Qt::WA_DeleteOnClose);
		setMinimumSize(560, 400);
		dests = savedDestinations();

		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(16, 16, 16, 16);
		layout->setSpacing(10);

		auto *help = new QLabel(moduleText("Destinations.Help"), this);
		help->setWordWrap(true);
		layout->addWidget(help);

		auto *quality = new QLabel(qualityText(), this);
		quality->setWordWrap(true);
		quality->setStyleSheet(QStringLiteral("color: #8C8C8C;"));
		layout->addWidget(quality);

		list = new DestinationsList(this, dests, [this]() { dirty = true; }, true);
		layout->addWidget(list, 1);

		auto *buttons = new QHBoxLayout();
		buttons->addStretch(1);
		auto *cancel = new QPushButton(moduleText("Destinations.Cancel"), this);
		auto *save = new QPushButton(moduleText("Destinations.Save"), this);
		somun_make_primary(save);
		buttons->addWidget(cancel);
		buttons->addWidget(save);
		layout->addLayout(buttons);

		connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
		connect(save, &QPushButton::clicked, this, [this]() {
			pl_multistream_set_all(dests.data(), dests.size());
			dirty = false;
			accept();
		});
	}

	void reject() override
	{
		if (dirty && QMessageBox::question(this, windowTitle(), moduleText("Destinations.Discard")) !=
				     QMessageBox::Yes) {
			return;
		}
		QDialog::reject();
	}

private:
	std::vector<pl_dest_info> dests;
	DestinationsList *list = nullptr;
	bool dirty = false;
};

QPointer<DestinationsWindow> destinationsWindow;

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

/* Configurações › Áudio › Intercom: o fone, o microfone e o volume do diretor (antes ficavam no
 * painel Equipe; a Equipe da tela Operar ficou só com a lista de nomes). */
class IntercomSection : public QGroupBox {
public:
	IntercomSection(QWidget *parent, std::function<void()> onChange)
		: QGroupBox(moduleText("Intercom.SettingsTitle"), parent),
		  changed(onChange)
	{
		auto *form = new QFormLayout(this);
		microphone = new QComboBox(this);
		headset = new QComboBox(this);
		volume = new QSlider(Qt::Horizontal, this);
		volume->setRange(0, 200);
		form->addRow(moduleText("Intercom.Microphone"), microphone);
		form->addRow(moduleText("Intercom.Headset"), headset);
		form->addRow(moduleText("Intercom.Volume"), volume);
		setToolTip(moduleText("Intercom.DevicesHelp"));

		char capture[256];
		char playback[256];
		float level = 1.0f;
		pl_intercom_get_devices(capture, sizeof(capture), playback, sizeof(playback), &level);
		fill(microphone, true, capture);
		fill(headset, false, playback);
		volume->setValue((int)(level * 100.0f + 0.5f));

		connect(microphone, &QComboBox::activated, this, [this](int) { changed(); });
		connect(headset, &QComboBox::activated, this, [this](int) { changed(); });
		connect(volume, &QSlider::valueChanged, this, [this](int) { changed(); });
	}

	void commit()
	{
		QByteArray capture = microphone->currentData().toString().toUtf8();
		QByteArray playback = headset->currentData().toString().toUtf8();
		pl_intercom_set_devices(capture.constData(), playback.constData(), (float)volume->value() / 100.0f);
	}

private:
	static void addName(void *param, const char *name, bool)
	{
		static_cast<QStringList *>(param)->append(QString::fromUtf8(name));
	}

	/* Lista os aparelhos de som e marca o escolhido ("" = padrão do sistema). */
	static void fill(QComboBox *combo, bool capture, const char *current)
	{
		QStringList names;
		pl_audio_enum_devices(capture, addName, &names);
		combo->addItem(moduleText("Intercom.SystemDefault"), QString());
		for (const QString &name : names) {
			combo->addItem(name, name);
		}
		QString chosen = QString::fromUtf8(current);
		int index = 0;
		if (!chosen.isEmpty()) {
			index = combo->findData(chosen);
			if (index < 0) {
				/* Escolhido antes, mas desligado agora: continua na lista para não se perder. */
				combo->addItem(chosen + " (?)", chosen);
				index = combo->count() - 1;
			}
		}
		combo->setCurrentIndex(index);
	}

	std::function<void()> changed;
	QComboBox *microphone = nullptr;
	QComboBox *headset = nullptr;
	QSlider *volume = nullptr;
};

/* Põe a seção Intercom na aba Áudio, logo depois dos dispositivos de áudio globais. */
IntercomSection *addIntercomSection(QDialog *dialog, std::function<void()> onChange)
{
	auto *devices = dialog->findChild<QWidget *>("audioDevicesGroupBox");
	QWidget *holder = devices ? devices->parentWidget() : nullptr;
	auto *layout = holder ? qobject_cast<QBoxLayout *>(holder->layout()) : nullptr;
	if (!layout) {
		blog(LOG_WARNING, "[projeto-live] não achei a aba Áudio das Configurações");
		return nullptr;
	}
	auto *section = new IntercomSection(holder, onChange);
	layout->insertWidget(layout->indexOf(devices) + 1, section);
	return section;
}

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
		/* A nossa transmissão vem primeiro; a do OBS (só Modo OBS) fica embaixo. */
		extra->setParent(content);
		layout->addWidget(extra);
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

		QPointer<IntercomSection> intercom(addIntercomSection(dialog, enableApply));

		QObject::connect(buttons, &QDialogButtonBox::clicked, destinations,
				 [buttons, destinations, record, intercom](QAbstractButton *button) {
					 QDialogButtonBox::ButtonRole role = buttons->buttonRole(button);
					 if (role == QDialogButtonBox::ApplyRole ||
					     role == QDialogButtonBox::AcceptRole) {
						 destinations->commit();
						 record->commit();
						 if (intercom) {
							 intercom->commit();
						 }
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
		bool live = pl_multistream_active();
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

void pl_destinations_window_open(QWidget *parent)
{
	if (destinationsWindow) {
		destinationsWindow->raise();
		destinationsWindow->activateWindow();
		return;
	}
	destinationsWindow = new DestinationsWindow(parent);
	destinationsWindow->show();
}
