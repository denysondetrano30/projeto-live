/*
 * Projeto Live: painel "Diretor".
 *
 * Mostra cada fonte "Celular" com o celular conectado e se está no ar, e tem um botão para cada
 * aviso pronto ("Abre", "Fecha", "Foca no pregador", "Você está no ar") e um campo para escrever
 * um aviso livre. Embaixo de cada câmera aparece o último aviso e se o operador já tocou em
 * "Entendi". Os mesmos avisos prontos também podem ir por atalhos de teclado (veja phone-source.c).
 *
 * No alto fica o intercom: "Falar com todos" (segurar), "Travar microfone", quem está falando e
 * a escolha do fone e do microfone do diretor. Cada câmera tem "Falar só com esta câmera".
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "director-dock.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStringList>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "intercom-audio.h"
#include "intercom.h"
#include "phone-source.h"

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

const char *redButton = "QPushButton { background-color: #d62828; color: white; font-weight: bold; }";

void addDeviceName(void *param, const char *name, bool)
{
	static_cast<QStringList *>(param)->append(QString::fromUtf8(name));
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
	QPushButton *talk = nullptr;
	bool talking = false; /* o diretor está segurando "Falar só com esta câmera" */
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
		outer->addWidget(buildIntercom());

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
		timer.start(250);
	}

	~DirectorDock() override { stop(); }

	/* O OBS está fechando: para de olhar as fontes. */
	void stop()
	{
		timer.stop();
		clearRows();
	}

private:
	QWidget *buildIntercom()
	{
		auto *frame = new QFrame(this);
		frame->setFrameShape(QFrame::StyledPanel);
		auto *layout = new QVBoxLayout(frame);

		auto *title = new QLabel(moduleText("Intercom.Title"), frame);
		QFont bold = title->font();
		bold.setBold(true);
		title->setFont(bold);
		layout->addWidget(title);

		auto *buttons = new QHBoxLayout();
		talkAll = new QPushButton(moduleText("Intercom.TalkAll"), frame);
		talkAll->setToolTip(moduleText("Intercom.TalkAll.ToolTip"));
		talkAll->setMinimumHeight(40);
		connect(talkAll, &QPushButton::pressed, this, []() { pl_intercom_set_talk_all(true); });
		connect(talkAll, &QPushButton::released, this, []() { pl_intercom_set_talk_all(false); });
		latch = new QPushButton(moduleText("Intercom.Latch"), frame);
		latch->setToolTip(moduleText("Intercom.Latch.ToolTip"));
		latch->setCheckable(true);
		latch->setMinimumHeight(40);
		connect(latch, &QPushButton::toggled, this, [](bool on) { pl_intercom_set_latched(on); });
		buttons->addWidget(talkAll, 1);
		buttons->addWidget(latch, 1);
		layout->addLayout(buttons);

		intercomStatus = new QLabel(frame);
		intercomStatus->setWordWrap(true);
		intercomStatus->setTextFormat(Qt::RichText);
		layout->addWidget(intercomStatus);

		devicesToggle = new QPushButton(frame);
		devicesToggle->setFlat(true);
		devicesToggle->setCheckable(true);
		devicesToggle->setStyleSheet("text-align: left;");
		layout->addWidget(devicesToggle);

		devicesBox = new QWidget(frame);
		auto *form = new QGridLayout(devicesBox);
		form->setContentsMargins(0, 0, 0, 0);
		microphone = new QComboBox(devicesBox);
		headset = new QComboBox(devicesBox);
		volume = new QSlider(Qt::Horizontal, devicesBox);
		volume->setRange(0, 200);
		form->addWidget(new QLabel(moduleText("Intercom.Microphone"), devicesBox), 0, 0);
		form->addWidget(microphone, 0, 1);
		form->addWidget(new QLabel(moduleText("Intercom.Headset"), devicesBox), 1, 0);
		form->addWidget(headset, 1, 1);
		form->addWidget(new QLabel(moduleText("Intercom.Volume"), devicesBox), 2, 0);
		form->addWidget(volume, 2, 1);
		auto *help = new QLabel(moduleText("Intercom.DevicesHelp"), devicesBox);
		help->setWordWrap(true);
		form->addWidget(help, 3, 0, 1, 2);
		form->setColumnStretch(1, 1);
		devicesBox->setVisible(false);
		layout->addWidget(devicesBox);
		setDevicesTitle(false);

		connect(devicesToggle, &QPushButton::toggled, this, [this](bool open) {
			setDevicesTitle(open);
			if (open) {
				loadDevices();
			}
			devicesBox->setVisible(open);
		});
		connect(microphone, &QComboBox::activated, this, [this](int) { saveDevices(); });
		connect(headset, &QComboBox::activated, this, [this](int) { saveDevices(); });
		connect(volume, &QSlider::sliderReleased, this, [this]() { saveDevices(); });
		connect(volume, &QSlider::valueChanged, this, [this](int) {
			if (!volume->isSliderDown()) {
				saveDevices();
			}
		});
		return frame;
	}

	void setDevicesTitle(bool open)
	{
		devicesToggle->setText(QString::fromUtf8(open ? "\xE2\x96\xBE " : "\xE2\x96\xB8 ") +
				       moduleText("Intercom.Devices"));
	}

	/* Lista os aparelhos de som e marca os escolhidos ("" = padrão do sistema). */
	void fillDevices(QComboBox *combo, bool capture, const char *current)
	{
		QStringList names;
		pl_audio_enum_devices(capture, addDeviceName, &names);

		QSignalBlocker block(combo);
		combo->clear();
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

	void loadDevices()
	{
		char capture[256];
		char playback[256];
		float level = 1.0f;
		pl_intercom_get_devices(capture, sizeof(capture), playback, sizeof(playback), &level);
		fillDevices(microphone, true, capture);
		fillDevices(headset, false, playback);
		QSignalBlocker block(volume);
		volume->setValue((int)(level * 100.0f + 0.5f));
	}

	void saveDevices()
	{
		QByteArray capture = microphone->currentData().toString().toUtf8();
		QByteArray playback = headset->currentData().toString().toUtf8();
		pl_intercom_set_devices(capture.constData(), playback.constData(), (float)volume->value() / 100.0f);
	}

	void setPrivate(CameraRow *row, bool on)
	{
		row->talking = on;
		obs_source_t *source = obs_weak_source_get_source(row->weak);
		if (source) {
			pl_phone_set_private_talk(source, on);
			obs_source_release(source);
		}
		row->talk->setStyleSheet(on ? redButton : "");
	}

	void updateIntercom()
	{
		struct pl_intercom_state state;
		pl_intercom_get_state(&state);

		{
			QSignalBlocker block(latch);
			latch->setChecked(state.latched);
		}
		latch->setStyleSheet(state.latched ? redButton : "");
		talkAll->setStyleSheet(state.talk_all && !state.latched ? redButton : "");

		QStringList privateNames;
		for (auto &row : rows) {
			if (row->talking) {
				privateNames.append(QString::fromStdString(row->name).toHtmlEscaped());
			}
		}

		QString text;
		if (state.error[0]) {
			text = "<span style='color:#e5484d'>" + QString::fromUtf8(state.error).toHtmlEscaped() +
			       "</span>";
		} else if (!state.any_phone) {
			text = "<span style='color:#8a8a8a'>" + moduleText("Intercom.NoPhones") + "</span>";
		} else {
			if (state.talk_all) {
				text = "<b>" + moduleText("Intercom.YouTalking") + "</b>";
			} else if (!privateNames.isEmpty()) {
				text = "<b>" + moduleText("Intercom.YouTalkingPrivate").arg(privateNames.join(", ")) +
				       "</b>";
			}
			if (state.speaking[0]) {
				if (!text.isEmpty()) {
					text += "<br>";
				}
				text += "<span style='color:#3fb950;font-weight:bold'>" +
					moduleText("Intercom.Speaking")
						.arg(QString::fromUtf8(state.speaking).toHtmlEscaped()) +
					"</span>";
			}
			if (text.isEmpty()) {
				text = "<span style='color:#8a8a8a'>" + moduleText("Intercom.Quiet") + "</span>";
			}
		}
		intercomStatus->setText(text);
	}

	void clearRows()
	{
		for (auto &row : rows) {
			if (row->talking) {
				setPrivate(row.get(), false);
			}
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
		updateIntercom();
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

		row->talk = new QPushButton(moduleText("Intercom.TalkPrivate"), frame);
		row->talk->setToolTip(moduleText("Intercom.TalkPrivate.ToolTip"));
		row->talk->setMinimumHeight(32);
		connect(row->talk, &QPushButton::pressed, this, [this, rowPtr]() { setPrivate(rowPtr, true); });
		connect(row->talk, &QPushButton::released, this, [this, rowPtr]() { setPrivate(rowPtr, false); });
		layout->addWidget(row->talk);

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
		if (status.talking) {
			line += " &nbsp;<span style='color:#3fb950;font-weight:bold'>&#9679; " +
				moduleText("Intercom.PhoneTalking") + "</span>";
		}
		row->status->setText(line);
		row->talk->setEnabled(status.intercom || row->talking);

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

	QPushButton *talkAll = nullptr;
	QPushButton *latch = nullptr;
	QLabel *intercomStatus = nullptr;
	QPushButton *devicesToggle = nullptr;
	QWidget *devicesBox = nullptr;
	QComboBox *microphone = nullptr;
	QComboBox *headset = nullptr;
	QSlider *volume = nullptr;

	QVBoxLayout *rowsLayout = nullptr;
	QLabel *empty = nullptr;
	std::vector<std::unique_ptr<CameraRow>> rows;
	QTimer timer;
};

DirectorDock *dock = nullptr;
obs_hotkey_id talkAllHotkey = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id latchHotkey = OBS_INVALID_HOTKEY_ID;

void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		pl_intercom_set_talk_all(false);
		pl_intercom_set_latched(false);
		if (dock) {
			dock->stop();
		}
	}
}

/* Atalhos do intercom (thread dos atalhos; as funções do intercom servem para qualquer thread). */
void talkAllPressed(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	pl_intercom_set_talk_all(pressed);
}

void latchPressed(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed) {
		pl_intercom_set_latched(!pl_intercom_get_latched());
	}
}

/* As teclas escolhidas ficam guardadas junto com a coleção de cenas. */
void saveHotkeys(obs_data_t *data, bool saving, void *)
{
	const char *keys[] = {"projeto_live_intercom_talk_all", "projeto_live_intercom_latch"};
	obs_hotkey_id ids[] = {talkAllHotkey, latchHotkey};
	for (size_t i = 0; i < 2; i++) {
		if (saving) {
			obs_data_array_t *array = obs_hotkey_save(ids[i]);
			obs_data_set_array(data, keys[i], array);
			obs_data_array_release(array);
		} else {
			obs_data_array_t *array = obs_data_get_array(data, keys[i]);
			if (array) {
				obs_hotkey_load(ids[i], array);
				obs_data_array_release(array);
			}
		}
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

	talkAllHotkey = obs_hotkey_register_frontend("projeto_live.intercom.talk_all",
						     obs_module_text("Hotkey.TalkAll"), talkAllPressed, nullptr);
	latchHotkey = obs_hotkey_register_frontend("projeto_live.intercom.latch", obs_module_text("Hotkey.Latch"),
						   latchPressed, nullptr);
	obs_frontend_add_save_callback(saveHotkeys, nullptr);
}
