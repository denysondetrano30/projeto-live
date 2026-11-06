/*
 * Projeto Live: painel "Diretor".
 *
 * Mostra cada fonte "Celular" com o celular conectado e se está no ar, e tem um botão para cada
 * aviso pronto ("Abre", "Fecha", "Foca no pregador", "Você está no ar") e um campo para escrever
 * um aviso livre. Embaixo de cada câmera aparece o último aviso e se o operador já tocou em
 * "Entendi". Os mesmos avisos prontos também podem ir por atalhos de teclado (veja phone-source.c).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "director-dock.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "phone-source.h"

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

struct FoundSource {
	obs_weak_source_t *weak;
	std::string name;
};

bool collectPhoneSource(void *param, obs_source_t *source)
{
	auto *found = static_cast<std::vector<FoundSource> *>(param);
	if (pl_phone_is_source(source)) {
		found->push_back({obs_source_get_weak_source(source), obs_source_get_name(source)});
	}
	return true;
}

struct CameraRow {
	obs_weak_source_t *weak = nullptr;
	std::string name;
	QLabel *status = nullptr;
	QLabel *last = nullptr;
	std::vector<QPushButton *> buttons;
	QLineEdit *message = nullptr;
	QPushButton *send = nullptr;
	uint32_t seenSent = 0;
	QString sentAt;
	QDateTime notSentUntil;
};

class DirectorDock : public QWidget {
public:
	explicit DirectorDock(QWidget *parent) : QWidget(parent)
	{
		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(4, 4, 4, 4);

		auto *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		outer->addWidget(scroll);

		auto *content = new QWidget(scroll);
		rowsLayout = new QVBoxLayout(content);
		rowsLayout->setContentsMargins(0, 0, 0, 0);
		rowsLayout->setSpacing(8);

		empty = new QLabel(moduleText("Director.Empty"), content);
		empty->setWordWrap(true);
		rowsLayout->addWidget(empty);
		rowsLayout->addStretch(1);
		scroll->setWidget(content);

		connect(&timer, &QTimer::timeout, this, [this]() { refresh(); });
		timer.start(500);
	}

	~DirectorDock() override { stop(); }

	/* O OBS está fechando: para de olhar as fontes. */
	void stop()
	{
		timer.stop();
		clearRows();
	}

private:
	void clearRows()
	{
		for (auto &row : rows) {
			obs_weak_source_release(row->weak);
			row->weak = nullptr;
		}
		rows.clear();
		while (rowsLayout->count() > 2) {
			QLayoutItem *item = rowsLayout->takeAt(1);
			if (item->widget()) {
				/* Escondido, nenhum clique chega mais nos botões antes de ele ser apagado. */
				item->widget()->hide();
				item->widget()->deleteLater();
			}
			delete item;
		}
	}

	void refresh()
	{
		std::vector<FoundSource> found;
		obs_enum_sources(collectPhoneSource, &found);

		bool same = found.size() == rows.size();
		for (size_t i = 0; same && i < found.size(); i++) {
			same = found[i].weak == rows[i]->weak && found[i].name == rows[i]->name;
		}

		if (same) {
			for (auto &item : found) {
				obs_weak_source_release(item.weak);
			}
		} else {
			clearRows();
			for (auto &item : found) {
				addRow(item);
			}
		}

		empty->setVisible(rows.empty());
		for (auto &row : rows) {
			updateRow(row.get());
		}
	}

	/* Fica com a referência fraca de `item`. */
	void addRow(const FoundSource &item)
	{
		auto row = std::make_unique<CameraRow>();
		row->weak = item.weak;
		row->name = item.name;
		CameraRow *rowPtr = row.get();

		auto *frame = new QFrame();
		frame->setFrameShape(QFrame::StyledPanel);
		auto *layout = new QVBoxLayout(frame);

		auto *title = new QLabel(QString::fromStdString(item.name), frame);
		title->setTextFormat(Qt::PlainText);
		QFont bold = title->font();
		bold.setBold(true);
		title->setFont(bold);
		layout->addWidget(title);

		row->status = new QLabel(frame);
		row->status->setTextFormat(Qt::RichText);
		layout->addWidget(row->status);

		auto *grid = new QGridLayout();
		for (size_t i = 0; i < pl_phone_command_count(); i++) {
			QString command = QString::fromUtf8(pl_phone_command_text(i));
			auto *button = new QPushButton(command, frame);
			button->setMinimumHeight(32);
			connect(button, &QPushButton::clicked, this,
				[this, rowPtr, command]() { sendCommand(rowPtr, command); });
			grid->addWidget(button, (int)(i / 2), (int)(i % 2));
			row->buttons.push_back(button);
		}
		layout->addLayout(grid);

		auto *messageLine = new QHBoxLayout();
		row->message = new QLineEdit(frame);
		row->message->setPlaceholderText(moduleText("Director.MessagePlaceholder"));
		row->message->setMaxLength(PL_COMMAND_MAX);
		row->send = new QPushButton(moduleText("Director.Send"), frame);
		auto sendTyped = [this, rowPtr]() {
			QString typed = rowPtr->message->text().trimmed();
			if (!typed.isEmpty() && sendCommand(rowPtr, typed)) {
				rowPtr->message->clear();
			}
		};
		connect(row->send, &QPushButton::clicked, this, sendTyped);
		connect(row->message, &QLineEdit::returnPressed, this, sendTyped);
		messageLine->addWidget(row->message, 1);
		messageLine->addWidget(row->send);
		layout->addLayout(messageLine);

		row->last = new QLabel(frame);
		row->last->setWordWrap(true);
		row->last->setTextFormat(Qt::RichText);
		layout->addWidget(row->last);

		rowsLayout->insertWidget(rowsLayout->count() - 1, frame);
		rows.push_back(std::move(row));
	}

	bool sendCommand(CameraRow *row, const QString &command)
	{
		obs_source_t *source = obs_weak_source_get_source(row->weak);
		if (!source) {
			return false;
		}
		uint32_t id = pl_phone_send_command(source, command.toUtf8().constData());
		obs_source_release(source);

		if (!id) {
			row->notSentUntil = QDateTime::currentDateTime().addSecs(4);
		}
		updateRow(row);
		return id != 0;
	}

	void updateRow(CameraRow *row)
	{
		obs_source_t *source = obs_weak_source_get_source(row->weak);
		if (!source) {
			return;
		}
		struct pl_phone_status status;
		bool ok = pl_phone_get_status(source, &status);
		obs_source_release(source);
		if (!ok) {
			return;
		}

		QString line;
		if (status.connected) {
			line = QString::fromUtf8(status.camera).toHtmlEscaped() + " · " +
			       moduleText(status.usb ? "Director.ViaUsb" : "Director.ViaWifi");
		} else {
			line = "<span style='color:#8a8a8a'>" + moduleText("Director.NotConnected") + "</span>";
		}
		if (status.on_air) {
			line += " &nbsp;<span style='color:#ffffff;background-color:#d62828;font-weight:bold'>&nbsp;" +
				moduleText("Director.OnAir") + "&nbsp;</span>";
		}
		row->status->setText(line);

		for (QPushButton *button : row->buttons) {
			button->setEnabled(status.connected);
		}
		row->message->setEnabled(status.connected);
		row->send->setEnabled(status.connected);

		if (status.last_sent != row->seenSent) {
			row->seenSent = status.last_sent;
			row->sentAt = QTime::currentTime().toString("HH:mm:ss");
		}

		if (row->notSentUntil.isValid() && QDateTime::currentDateTime() < row->notSentUntil) {
			row->last->setText("<span style='color:#e5484d'>" + moduleText("Director.NotSent") + "</span>");
		} else if (status.last_sent) {
			QString text = moduleText("Director.Last")
					       .arg(QString::fromUtf8(status.last_text).toHtmlEscaped(), row->sentAt);
			bool understood = status.last_acked >= status.last_sent;
			if (understood) {
				text += " · <span style='color:#3fb950;font-weight:bold'>&#10003; " +
					moduleText("Director.Understood") + "</span>";
			} else {
				text += " · " + moduleText("Director.Waiting");
			}
			row->last->setText(text);
		} else {
			row->last->clear();
		}
	}

	QVBoxLayout *rowsLayout = nullptr;
	QLabel *empty = nullptr;
	std::vector<std::unique_ptr<CameraRow>> rows;
	QTimer timer;
};

DirectorDock *dock = nullptr;

void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT && dock) {
		dock->stop();
	}
}

} // namespace

void pl_director_dock_load(void)
{
	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (!mainWindow) {
		return;
	}

	dock = new DirectorDock(mainWindow);
	if (!obs_frontend_add_dock_by_id("projeto-live-diretor", obs_module_text("Director.Title"), dock)) {
		delete dock;
		dock = nullptr;
		return;
	}
	QObject::connect(dock, &QObject::destroyed, []() { dock = nullptr; });
	obs_frontend_add_event_callback(frontendEvent, nullptr);
}
