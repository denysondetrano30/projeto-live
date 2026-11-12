/*
 * Projeto Live: painel "Som", o mixer deitado com o "Ouvir no fone".
 *
 * O problema que isto resolve: a falha mais comum numa live de igreja é o som, e quem está no PC
 * não tem como ouvir o que está saindo, porque o som do salão está tocando. O OBS tem como
 * (o "Monitorar"), mas está escondido em dois lugares diferentes, com três opções de nome confuso.
 * Aqui fica tudo numa faixa só:
 *
 * - um canal por som (a mesa, o telão, cada celular, o som do PC), lado a lado;
 * - em cada um: o nome, o volume, o medidor, o mudo e o botão de fone;
 * - em cima, a escolha do fone e o botão "Ouvir tudo".
 *
 * O botão de fone NÃO muda a live. Ele liga o "monitorar e transmitir" do OBS
 * (OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT), nunca o "monitorar só", que tiraria aquele som da
 * transmissão — o engano clássico de quem mexe nisso na mão.
 *
 * Não existe no OBS um jeito de ouvir a mistura final pronta; "Ouvir tudo" liga o fone em todos os
 * canais de uma vez, que é o mais perto disso.
 *
 * Se o fone escolhido desaparecer (alguém puxou o cabo, o Bluetooth caiu), a escuta é desligada em
 * todos os canais e a faixa avisa, em vez de ficar mandando som para um aparelho que não existe.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "audio-dock.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <util/platform.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "phone-source.h"
#include "scene-audio.h"

namespace {

#define AUDIO_FILE "escuta.json"

const int CARD_WIDTH = 204;

class AudioDock;

QWidget *dock = nullptr;

/* ------------------------------------------------------------------------- */
/* Medidor                                                                   */

class Meter : public QWidget {
public:
	explicit Meter(QWidget *parent) : QWidget(parent)
	{
		setFixedHeight(7);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	}

	/* Pico em dB (0 é o máximo; -60 é o silêncio). */
	void setPeak(float db)
	{
		if (!std::isfinite(db)) {
			db = -60.0f;
		}
		db = std::max(-60.0f, std::min(0.0f, db));
		if (std::abs(db - peak) > 0.4f) {
			peak = db;
			update();
		}
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.fillRect(rect(), QColor("#2b2b33"));
		float part = (peak + 60.0f) / 60.0f;
		int filled = (int)(width() * part);
		if (filled <= 0) {
			return;
		}
		/* Verde até -12 dB, amarelo até -3, vermelho acima: o som alto demais se vê de longe. */
		QColor color("#6ddb8c");
		if (peak > -3.0f) {
			color = QColor("#e5352b");
		} else if (peak > -12.0f) {
			color = QColor("#e8a33d");
		}
		painter.fillRect(0, 0, filled, height(), color);
	}

private:
	float peak = -60.0f;
};

/* ------------------------------------------------------------------------- */
/* Um canal                                                                  */

class Card : public QFrame {
public:
	explicit Card(QWidget *parent) : QFrame(parent)
	{
		setFrameShape(QFrame::StyledPanel);
		setFixedWidth(CARD_WIDTH);

		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(8, 6, 8, 6);
		outer->setSpacing(5);

		auto *top = new QHBoxLayout();
		top->setContentsMargins(0, 0, 0, 0);
		top->setSpacing(6);
		name = new QLabel(this);
		name->setStyleSheet("font-weight: 600;");
		volume = new QLabel(this);
		volume->setStyleSheet("color: #8d8d98;");
		top->addWidget(name, 1);
		top->addWidget(volume);
		outer->addLayout(top);

		meter = new Meter(this);
		outer->addWidget(meter);

		tag = new QLabel(this);
		tag->setStyleSheet("color: #8d8d98;");
		outer->addWidget(tag);

		/* Só nos sons globais: este som toca na cena que está no ar? */
		scenePlay = new QCheckBox(QString::fromUtf8(obs_module_text("Audio.InScene")), this);
		scenePlay->setToolTip(QString::fromUtf8(obs_module_text("Audio.InScene.Tip")));
		scenePlay->setVisible(false);
		outer->addWidget(scenePlay);
		connect(scenePlay, &QCheckBox::clicked, this, [this](bool on) {
			obs_source_t *scene = obs_frontend_get_current_scene();
			obs_source_t *target = obs_weak_source_get_source(source);
			if (scene && target) {
				pl_scene_audio_set_silenced(scene, obs_source_get_name(target), !on);
			}
			obs_source_release(target);
			obs_source_release(scene);
		});

		auto *bottom = new QHBoxLayout();
		bottom->setContentsMargins(0, 0, 0, 0);
		bottom->setSpacing(5);
		mute = new QPushButton(QString::fromUtf8(obs_module_text("Audio.Mute")), this);
		mute->setCheckable(true);
		mute->setFixedHeight(24);
		phone = new QPushButton(QString::fromUtf8(obs_module_text("Audio.Listen")), this);
		phone->setCheckable(true);
		phone->setFixedHeight(24);
		phone->setToolTip(QString::fromUtf8(obs_module_text("Audio.Listen.Tip")));
		bottom->addWidget(mute, 1);
		bottom->addWidget(phone, 1);
		outer->addLayout(bottom);

		connect(mute, &QPushButton::clicked, this, [this](bool on) {
			obs_source_t *target = obs_weak_source_get_source(source);
			if (target) {
				obs_source_set_muted(target, on);
				obs_source_release(target);
			}
		});
		connect(phone, &QPushButton::clicked, this, [this](bool on) { setListening(on); });
	}

	~Card() override { detach(); }

	void attach(obs_source_t *target, bool global)
	{
		detach();
		source = obs_source_get_weak_source(target);
		peakDb.store(-60.0f);
		volmeter = obs_volmeter_create(OBS_FADER_LOG);
		obs_volmeter_attach_source(volmeter, target);
		obs_volmeter_add_callback(volmeter, levelCallback, this);
		name->setText(QString::fromUtf8(obs_source_get_name(target)));
		name->setToolTip(name->text());
		isGlobal = global;
		tag->setText(QString::fromUtf8(obs_module_text(global ? "Audio.Global" : "Audio.Scene")));
		scenePlay->setVisible(global);
	}

	void detach()
	{
		if (volmeter) {
			obs_volmeter_remove_callback(volmeter, levelCallback, this);
			obs_volmeter_detach_source(volmeter);
			obs_volmeter_destroy(volmeter);
			volmeter = nullptr;
		}
		if (source) {
			obs_weak_source_release(source);
			source = nullptr;
		}
	}

	bool holds(obs_source_t *target) const
	{
		return source && obs_weak_source_references_source(source, target);
	}

	/* Liga ou desliga o fone neste canal, sem mexer no que vai para a live. */
	void setListening(bool on)
	{
		obs_source_t *target = obs_weak_source_get_source(source);
		if (!target) {
			return;
		}
		/* Ligado = ouvir e continuar na saída (nunca só ouvir: isso tiraria o som da live). */
		obs_source_set_monitoring_enabled(target, on);
		obs_source_release(target);
		QSignalBlocker block(phone);
		phone->setChecked(on);
	}

	bool listening() const { return phone->isChecked(); }

	void refresh()
	{
		obs_source_t *target = obs_weak_source_get_source(source);
		if (!target) {
			return;
		}
		{
			QSignalBlocker block(mute);
			mute->setChecked(obs_source_muted(target));
		}
		{
			QSignalBlocker block(phone);
			phone->setChecked(obs_source_get_monitoring_enabled(target));
		}
		float db = obs_mul_to_db(obs_source_get_volume(target));
		if (!std::isfinite(db) || db < -95.0f) {
			volume->setText(QString::fromUtf8("−∞"));
		} else {
			volume->setText(QString("%1 dB").arg(QString::number(db, 'f', 1)));
		}
		obs_source_release(target);

		meter->setPeak(peakDb.load());

		if (isGlobal) {
			obs_source_t *scene = obs_frontend_get_current_scene();
			obs_source_t *held = obs_weak_source_get_source(source);
			if (scene && held) {
				QSignalBlocker block(scenePlay);
				scenePlay->setChecked(!pl_scene_audio_is_silenced(scene, obs_source_get_name(held)));
			}
			obs_source_release(held);
			obs_source_release(scene);
		}
	}

private:
	static void levelCallback(void *data, const float magnitude[MAX_AUDIO_CHANNELS],
				  const float peak[MAX_AUDIO_CHANNELS],
				  const float inputPeak[MAX_AUDIO_CHANNELS])
	{
		(void)magnitude;
		(void)inputPeak;
		/* Chamado numa thread do áudio: só guarda o número, a interface lê no relógio dela.
		 * O retorno de chamada sai antes de o canal ser destruído (detach), então `data` vale. */
		static_cast<Card *>(data)->peakDb.store(peak[0]);
	}

	obs_weak_source_t *source = nullptr;
	obs_volmeter_t *volmeter = nullptr;
	std::atomic<float> peakDb{-60.0f};
	bool isGlobal = false;
	QCheckBox *scenePlay = nullptr;
	QLabel *name = nullptr;
	QLabel *volume = nullptr;
	QLabel *tag = nullptr;
	Meter *meter = nullptr;
	QPushButton *mute = nullptr;
	QPushButton *phone = nullptr;
};

/* ------------------------------------------------------------------------- */
/* O painel                                                                  */

class AudioDock : public QWidget {
public:
	explicit AudioDock(QWidget *parent) : QWidget(parent)
	{
		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(8, 6, 8, 6);
		outer->setSpacing(6);

		auto *top = new QHBoxLayout();
		top->setContentsMargins(0, 0, 0, 0);
		top->setSpacing(6);
		top->addWidget(new QLabel(QString::fromUtf8(obs_module_text("Audio.Device")), this));
		devices = new QComboBox(this);
		devices->setMinimumWidth(180);
		top->addWidget(devices, 1);
		all = new QPushButton(QString::fromUtf8(obs_module_text("Audio.All")), this);
		all->setCheckable(true);
		top->addWidget(all);
		together = new QPushButton(QString::fromUtf8(obs_module_text("Audio.Together")), this);
		together->setToolTip(QString::fromUtf8(obs_module_text("Audio.Together.Tip")));
		together->setVisible(false);
		top->addWidget(together);
		connect(together, &QPushButton::clicked, this, []() { pl_scene_audio_bring_back(); });
		warning = new QLabel(this);
		warning->setStyleSheet("color: #e8a33d;");
		top->addWidget(warning, 2);
		outer->addLayout(top);

		area = new QScrollArea(this);
		area->setWidgetResizable(true);
		area->setFrameShape(QFrame::NoFrame);
		area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		holder = new QWidget(area);
		row = new QHBoxLayout(holder);
		row->setContentsMargins(0, 0, 0, 0);
		row->setSpacing(6);
		row->addStretch(1);
		area->setWidget(holder);
		outer->addWidget(area, 1);

		connect(devices, &QComboBox::currentIndexChanged, this, [this](int index) { chooseDevice(index); });
		connect(all, &QPushButton::clicked, this, [this](bool on) {
			for (auto &card : cards) {
				if (card->isVisible()) {
					card->setListening(on);
				}
			}
		});

		/* Só começa quando o OBS terminar de abrir: no meio do carregamento dos módulos as
		 * cenas ainda nem existem, e perguntar as coisas do OBS nessa hora derruba o
		 * programa. */
		timer = new QTimer(this);
		timer->setInterval(200);
		connect(timer, &QTimer::timeout, this, [this]() { tick(); });
	}

	/* O OBS acabou de abrir. */
	void start()
	{
		if (!timer->isActive()) {
			fillDevices();
			rebuild();
			timer->start();
		}
	}

private:
	/* Fones e caixas que o sistema oferece para a escuta. */
	void fillDevices()
	{
		QSignalBlocker block(devices);
		devices->clear();
		struct Collect {
			QComboBox *box;
		} collect{devices};
		obs_enum_audio_monitoring_devices(
			[](void *data, const char *name, const char *id) {
				auto *box = static_cast<Collect *>(data)->box;
				box->addItem(QString::fromUtf8(name), QString::fromUtf8(id));
				return true;
			},
			&collect);

		const char *currentName = nullptr;
		const char *currentId = nullptr;
		obs_get_audio_monitoring_device(&currentName, &currentId);
		if (currentId && *currentId) {
			int index = devices->findData(QString::fromUtf8(currentId));
			if (index >= 0) {
				devices->setCurrentIndex(index);
			}
		}
		if (devices->count() == 0) {
			devices->addItem(QString::fromUtf8(obs_module_text("Audio.NoDevice")), QString());
		}
	}

	void chooseDevice(int index)
	{
		if (index < 0) {
			return;
		}
		QString id = devices->itemData(index).toString();
		if (id.isEmpty()) {
			return;
		}
		obs_set_audio_monitoring_device(devices->itemText(index).toUtf8().constData(),
						id.toUtf8().constData());
		warning->clear();
		save(devices->itemText(index), id);
	}

	void save(const QString &name, const QString &id)
	{
		char *dir = obs_module_config_path("");
		if (dir) {
			os_mkdirs(dir);
			bfree(dir);
		}
		char *path = obs_module_config_path(AUDIO_FILE);
		if (!path) {
			return;
		}
		obs_data_t *data = obs_data_create();
		obs_data_set_string(data, "fone", name.toUtf8().constData());
		obs_data_set_string(data, "fone_id", id.toUtf8().constData());
		obs_data_save_json_safe(data, path, "tmp", "bak");
		obs_data_release(data);
		bfree(path);
	}

	/* O fone escolhido ainda existe? Se não, desliga a escuta e avisa. */
	void checkDevice()
	{
		QString id = devices->currentData().toString();
		if (id.isEmpty()) {
			return;
		}
		struct Found {
			QString id;
			bool found;
		} found{id, false};
		obs_enum_audio_monitoring_devices(
			[](void *data, const char *name, const char *deviceId) {
				(void)name;
				auto *state = static_cast<Found *>(data);
				if (state->id == QString::fromUtf8(deviceId)) {
					state->found = true;
					return false;
				}
				return true;
			},
			&found);
		if (found.found) {
			return;
		}
		bool wasListening = false;
		for (auto &card : cards) {
			if (card->listening()) {
				wasListening = true;
				card->setListening(false);
			}
		}
		{
			QSignalBlocker block(all);
			all->setChecked(false);
		}
		if (wasListening) {
			warning->setText(QString::fromUtf8(obs_module_text("Audio.DeviceGone")));
		}
		fillDevices();
	}

	Card *card(size_t index)
	{
		while (cards.size() <= index) {
			auto *created = new Card(holder);
			row->insertWidget((int)cards.size(), created);
			cards.push_back(created);
		}
		return cards[index];
	}

	/* Monta a lista: os canais globais (mesa, microfone, som do PC) e os sons da cena no ar. */
	void rebuild()
	{
		std::vector<std::pair<obs_source_t *, bool>> wanted;
		for (int channel = 1; channel <= 6; channel++) {
			obs_source_t *source = obs_get_output_source(channel);
			if (!source) {
				continue;
			}
			if (obs_source_audio_active(source)) {
				wanted.emplace_back(source, true);
			} else {
				obs_source_release(source);
			}
		}

		struct Walk {
			std::vector<std::pair<obs_source_t *, bool>> *list;
		} walk{&wanted};
		obs_enum_sources(
			[](void *data, obs_source_t *source) {
				auto *list = static_cast<Walk *>(data)->list;
				uint32_t flags = obs_source_get_output_flags(source);
				if ((flags & OBS_SOURCE_AUDIO) != 0 && obs_source_audio_active(source) &&
				    obs_source_active(source)) {
					bool already = false;
					for (auto &entry : *list) {
						if (entry.first == source) {
							already = true;
							break;
						}
					}
					if (!already) {
						obs_source_t *held = obs_source_get_ref(source);
						if (held) {
							list->emplace_back(held, false);
						}
					}
				}
				return true;
			},
			&walk);

		size_t used = 0;
		for (auto &entry : wanted) {
			Card *widget = card(used++);
			if (!widget->holds(entry.first)) {
				widget->attach(entry.first, entry.second);
			}
			widget->setVisible(true);
			obs_source_release(entry.first);
		}
		for (size_t i = used; i < cards.size(); i++) {
			cards[i]->detach();
			cards[i]->setVisible(false);
		}
	}

	void tick()
	{
		if (++ticks % 5 == 0) {
			rebuild();
			checkDevice();
		}
		for (auto &entry : cards) {
			if (entry->isVisible()) {
				entry->refresh();
			}
		}
		/* O botão só aparece quando a cena no ar está calando algum som. */
		together->setVisible(pl_scene_audio_has_silenced());
		together->setText(QString::fromUtf8(
			obs_module_text(pl_scene_audio_brought_back() ? "Audio.Together.Undo" : "Audio.Together")));
	}

	QComboBox *devices = nullptr;
	QPushButton *all = nullptr;
	QPushButton *together = nullptr;
	QLabel *warning = nullptr;
	QScrollArea *area = nullptr;
	QWidget *holder = nullptr;
	QHBoxLayout *row = nullptr;
	QTimer *timer = nullptr;
	std::vector<Card *> cards;
	int ticks = 0;
};

void frontendEvent(enum obs_frontend_event event, void *param)
{
	(void)param;
	(void)event;
	/* Qualquer aviso do OBS serve para saber que ele já abriu: start() só age na primeira vez. */
	if (dock) {
		static_cast<AudioDock *>(dock)->start();
	}
}

} // namespace

void pl_audio_dock_load(void)
{
	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (!mainWindow) {
		return;
	}

	auto *created = new AudioDock(mainWindow);
	if (!obs_frontend_add_dock_by_id("projeto-live-som", obs_module_text("Audio.Title"), created)) {
		delete created;
		return;
	}
	dock = created;
	QObject::connect(created, &QObject::destroyed, []() { dock = nullptr; });
	obs_frontend_add_event_callback(frontendEvent, nullptr);
}

void pl_audio_dock_unload(void)
{
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
}
