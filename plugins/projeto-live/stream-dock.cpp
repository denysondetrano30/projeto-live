/*
 * Projeto Live: painel "Transmissão".
 *
 * Lista os destinos extras (YouTube, Facebook, Instagram, TikTok ou outro endereço RTMP), cada um
 * com uma caixa para ligar ou desligar e o estado (esperando, ligando, no ar, reconectando, erro).
 * Os destinos marcados entram no ar junto com o botão "Iniciar transmissão" do OBS. No alto fica
 * a soma do upload que a transmissão está usando. O trabalho de verdade fica em multistream.c.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "stream-dock.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QCheckBox>
#include <QDateTime>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "multistream.h"

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

struct Platform {
	const char *id;
	const char *textKey;
	const char *helpKey;
};

const Platform platforms[] = {
	{"youtube", "Stream.Platform.YouTube", "Stream.Help.YouTube"},
	{"facebook", "Stream.Platform.Facebook", "Stream.Help.Facebook"},
	{"instagram", "Stream.Platform.Instagram", "Stream.Help.Instagram"},
	{"tiktok", "Stream.Platform.TikTok", "Stream.Help.TikTok"},
	{"custom", "Stream.Platform.Custom", "Stream.Help.Custom"},
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

QString mbps(double kbps)
{
	return QString::number(kbps / 1000.0, 'f', 1);
}

/* Janela para adicionar ou mudar um destino. */
class DestinationDialog : public QDialog {
public:
	DestinationDialog(QWidget *parent, const pl_dest_info &start, bool editing) : QDialog(parent)
	{
		setWindowTitle(moduleText(editing ? "Stream.EditTitle" : "Stream.AddTitle"));
		setMinimumWidth(460);
		auto *layout = new QVBoxLayout(this);
		auto *form = new QFormLayout();

		platform = new QComboBox(this);
		for (const Platform &item : platforms) {
			platform->addItem(moduleText(item.textKey), QString::fromUtf8(item.id));
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

		auto *qualityFrame = new QFrame(this);
		qualityFrame->setFrameShape(QFrame::StyledPanel);
		auto *qualityLayout = new QVBoxLayout(qualityFrame);
		qualityLayout->addWidget(new QLabel(moduleText("Stream.Quality"), qualityFrame));
		sameQuality = new QRadioButton(moduleText("Stream.Quality.Same"), qualityFrame);
		ownQuality = new QRadioButton(moduleText("Stream.Quality.Own"), qualityFrame);
		bitrate = new QDoubleSpinBox(qualityFrame);
		bitrate->setRange(0.5, 50.0);
		bitrate->setSingleStep(0.5);
		bitrate->setDecimals(1);
		bitrate->setSuffix(" Mbps");
		auto *ownLine = new QHBoxLayout();
		ownLine->addWidget(ownQuality);
		ownLine->addWidget(bitrate);
		ownLine->addStretch(1);
		qualityLayout->addWidget(sameQuality);
		qualityLayout->addLayout(ownLine);
		auto *qualityHelp = new QLabel(moduleText("Stream.Quality.Help"), qualityFrame);
		qualityHelp->setWordWrap(true);
		qualityLayout->addWidget(qualityHelp);
		layout->addWidget(qualityFrame);
		connect(ownQuality, &QRadioButton::toggled, bitrate, &QWidget::setEnabled);

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
		bitrate->setValue(start.bitrate_kbps > 0 ? start.bitrate_kbps / 1000.0 : 4.5);
		bitrate->setEnabled(start.own_quality);
		updateHelp();

		connect(platform, &QComboBox::currentIndexChanged, this, [this](int) {
			applyPlatformDefaults(lastPlatform);
			lastPlatform = platform->currentData().toString();
			updateHelp();
		});
	}

	pl_dest_info result() const
	{
		pl_dest_info info = {};
		copy(info.platform, sizeof(info.platform), platform->currentData().toString());
		copy(info.name, sizeof(info.name), name->text().trimmed());
		copy(info.server, sizeof(info.server), server->text().trimmed());
		copy(info.key, sizeof(info.key), key->text().trimmed());
		info.enabled = true;
		info.own_quality = ownQuality->isChecked();
		info.bitrate_kbps = (int)(bitrate->value() * 1000.0 + 0.5);
		return info;
	}

private:
	static void copy(char *out, size_t size, const QString &text)
	{
		QByteArray bytes = text.toUtf8();
		size_t length = std::min((size_t)bytes.size(), size - 1);
		memcpy(out, bytes.constData(), length);
		out[length] = 0;
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
		QString text = moduleText(findPlatform(id.constData()).helpKey);
		if (pl_multistream_platform_vertical(id.constData())) {
			text += "\n\n" + moduleText("Stream.Help.VerticalSoon");
		}
		help->setText(text);
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

	QComboBox *platform = nullptr;
	QLineEdit *name = nullptr;
	QLineEdit *server = nullptr;
	QLineEdit *key = nullptr;
	QLabel *help = nullptr;
	QRadioButton *sameQuality = nullptr;
	QRadioButton *ownQuality = nullptr;
	QDoubleSpinBox *bitrate = nullptr;
	QString lastPlatform;
};

struct DestinationRow {
	QCheckBox *enabled = nullptr;
	QLabel *details = nullptr;
	QLabel *status = nullptr;
	uint64_t lastBytes = 0;
	qint64 lastMs = 0;
	double kbps = 0.0;
};

class StreamDock : public QWidget {
public:
	explicit StreamDock(QWidget *parent) : QWidget(parent)
	{
		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(4, 4, 4, 4);

		mainStatus = new QLabel(this);
		mainStatus->setWordWrap(true);
		mainStatus->setTextFormat(Qt::RichText);
		outer->addWidget(mainStatus);

		total = new QLabel(this);
		total->setWordWrap(true);
		total->setTextFormat(Qt::RichText);
		outer->addWidget(total);

		auto *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		outer->addWidget(scroll, 1);

		auto *content = new QWidget(scroll);
		rowsLayout = new QVBoxLayout(content);
		rowsLayout->setContentsMargins(0, 0, 0, 0);
		rowsLayout->setSpacing(6);
		scroll->setWidget(content);

		auto *add = new QPushButton(moduleText("Stream.Add"), this);
		add->setMinimumHeight(32);
		connect(add, &QPushButton::clicked, this, [this]() { addDestination(); });
		outer->addWidget(add);

		auto *help = new QLabel(moduleText("Stream.DockHelp"), this);
		help->setWordWrap(true);
		help->setStyleSheet("color: #8a8a8a;");
		outer->addWidget(help);

		rebuild();
		connect(&timer, &QTimer::timeout, this, [this]() { refresh(); });
		timer.start(1000);
	}

	void stop() { timer.stop(); }

private:
	void clearRows()
	{
		rows.clear();
		while (QLayoutItem *item = rowsLayout->takeAt(0)) {
			if (item->widget()) {
				item->widget()->hide();
				item->widget()->deleteLater();
			}
			delete item;
		}
	}

	void rebuild()
	{
		clearRows();
		size_t count = pl_multistream_count();
		if (count == 0) {
			auto *empty = new QLabel(moduleText("Stream.Empty"));
			empty->setWordWrap(true);
			rowsLayout->addWidget(empty);
		}
		for (size_t i = 0; i < count; i++) {
			addRow(i);
		}
		rowsLayout->addStretch(1);
		refresh();
	}

	void addRow(size_t index)
	{
		pl_dest_info info;
		if (!pl_multistream_get(index, &info)) {
			return;
		}

		auto *frame = new QFrame();
		frame->setFrameShape(QFrame::StyledPanel);
		auto *layout = new QVBoxLayout(frame);

		DestinationRow row;
		row.enabled = new QCheckBox(QString::fromUtf8(info.name), frame);
		QFont bold = row.enabled->font();
		bold.setBold(true);
		row.enabled->setFont(bold);
		row.enabled->setChecked(info.enabled);
		connect(row.enabled, &QCheckBox::toggled, this, [this, index](bool on) {
			pl_multistream_set_enabled(index, on);
			refresh();
		});
		layout->addWidget(row.enabled);

		row.details = new QLabel(frame);
		row.details->setWordWrap(true);
		row.details->setStyleSheet("color: #8a8a8a;");
		QString details = moduleText(findPlatform(info.platform).textKey) + " · ";
		details += info.own_quality ? moduleText("Stream.Row.Own").arg(mbps(info.bitrate_kbps))
					    : moduleText("Stream.Row.Same");
		if (pl_multistream_platform_vertical(info.platform)) {
			details += " · " + moduleText("Stream.Row.VerticalSoon");
		}
		row.details->setText(details);
		layout->addWidget(row.details);

		row.status = new QLabel(frame);
		row.status->setWordWrap(true);
		row.status->setTextFormat(Qt::RichText);
		layout->addWidget(row.status);

		auto *buttons = new QHBoxLayout();
		auto *edit = new QPushButton(moduleText("Stream.Edit"), frame);
		auto *remove = new QPushButton(moduleText("Stream.Remove"), frame);
		connect(edit, &QPushButton::clicked, this, [this, index]() { editDestination(index); });
		connect(remove, &QPushButton::clicked, this, [this, index]() { removeDestination(index); });
		buttons->addWidget(edit);
		buttons->addWidget(remove);
		buttons->addStretch(1);
		layout->addLayout(buttons);

		rowsLayout->addWidget(frame);
		rows.push_back(row);
	}

	void addDestination()
	{
		pl_dest_info start = {};
		snprintf(start.platform, sizeof(start.platform), "youtube");
		start.bitrate_kbps = 4500;
		DestinationDialog dialog(this, start, false);
		if (dialog.exec() == QDialog::Accepted) {
			pl_dest_info info = dialog.result();
			pl_multistream_add(&info);
			rebuild();
		}
	}

	void editDestination(size_t index)
	{
		pl_dest_info start;
		if (!pl_multistream_get(index, &start)) {
			return;
		}
		DestinationDialog dialog(this, start, true);
		if (dialog.exec() == QDialog::Accepted) {
			pl_dest_info info = dialog.result();
			info.enabled = start.enabled;
			pl_multistream_update(index, &info);
			rebuild();
		}
	}

	void removeDestination(size_t index)
	{
		pl_dest_info info;
		if (!pl_multistream_get(index, &info)) {
			return;
		}
		QString question = moduleText("Stream.RemoveQuestion").arg(QString::fromUtf8(info.name));
		if (QMessageBox::question(this, moduleText("Stream.Remove"), question) == QMessageBox::Yes) {
			pl_multistream_remove(index);
			rebuild();
		}
	}

	QString stateText(const pl_dest_status &status, double kbps)
	{
		switch (status.state) {
		case PL_DEST_OFF:
			return "<span style='color:#8a8a8a'>" + moduleText("Stream.State.Off") + "</span>";
		case PL_DEST_WAITING:
			return "<span style='color:#8a8a8a'>" + moduleText("Stream.State.Waiting") + "</span>";
		case PL_DEST_CONNECTING:
			return "<span style='color:#e0a800;font-weight:bold'>" + moduleText("Stream.State.Connecting") +
			       "</span>";
		case PL_DEST_LIVE:
			return "<span style='color:#ffffff;background-color:#d62828;font-weight:bold'>&nbsp;" +
			       moduleText("Director.OnAir") + "&nbsp;</span> " + mbps(kbps) + " Mbps";
		case PL_DEST_RECONNECTING:
			return "<span style='color:#e0a800;font-weight:bold'>" +
			       moduleText("Stream.State.Reconnecting") + "</span>";
		case PL_DEST_ERROR:
			return "<span style='color:#e5484d'>" + QString::fromUtf8(status.error).toHtmlEscaped() +
			       "</span>";
		}
		return QString();
	}

	void refresh()
	{
		bool live = pl_multistream_main_live();
		int mainKbps = pl_multistream_main_kbps();

		mainStatus->setText(moduleText(live ? "Stream.Main.Live" : "Stream.Main.Off"));

		double extras = 0.0;
		int sameCount = 0;
		int ownCount = 0;
		for (size_t i = 0; i < rows.size(); i++) {
			pl_dest_info info;
			pl_dest_status status;
			if (!pl_multistream_get(i, &info)) {
				continue;
			}
			pl_multistream_get_status(i, &status);

			/* Velocidade real: bytes enviados desde a última olhada. */
			DestinationRow &row = rows[i];
			qint64 now = QDateTime::currentMSecsSinceEpoch();
			if (status.total_bytes < row.lastBytes || !status.total_bytes) {
				row.kbps = 0.0;
			} else if (row.lastMs && now - row.lastMs >= 500) {
				double kbps =
					(double)(status.total_bytes - row.lastBytes) * 8.0 / (double)(now - row.lastMs);
				row.kbps = row.kbps > 0.0 ? row.kbps * 0.6 + kbps * 0.4 : kbps;
			}
			if (!row.lastMs || now - row.lastMs >= 500 || status.total_bytes < row.lastBytes) {
				row.lastBytes = status.total_bytes;
				row.lastMs = now;
			}
			row.status->setText(stateText(status, row.kbps));

			if (info.enabled) {
				if (info.own_quality) {
					extras += info.bitrate_kbps;
					ownCount++;
				} else {
					sameCount++;
				}
			}
		}

		/* Soma: a principal, os destinos com o mesmo vídeo dela, os de qualidade própria e o som
		 * (160 kbps) de cada um. */
		int destinations = 1 + sameCount + ownCount;
		double totalKbps = (double)mainKbps * (1 + sameCount) + extras + 160.0 * destinations;

		if (mainKbps > 0) {
			total->setText(moduleText("Stream.Total").arg(mbps(totalKbps)).arg(destinations));
		} else {
			total->setText(moduleText("Stream.TotalUnknown").arg(destinations));
		}
	}

	QLabel *mainStatus = nullptr;
	QLabel *total = nullptr;
	QVBoxLayout *rowsLayout = nullptr;
	std::vector<DestinationRow> rows;
	QTimer timer;
};

StreamDock *dock = nullptr;

void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT && dock) {
		dock->stop();
	}
}

} // namespace

void pl_stream_dock_load(void)
{
	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (!mainWindow) {
		return;
	}

	dock = new StreamDock(mainWindow);
	if (!obs_frontend_add_dock_by_id("projeto-live-transmissao", obs_module_text("Stream.Title"), dock)) {
		delete dock;
		dock = nullptr;
		return;
	}
	QObject::connect(dock, &QObject::destroyed, []() { dock = nullptr; });
	obs_frontend_add_event_callback(frontendEvent, nullptr);
}
