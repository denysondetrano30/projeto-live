/*
 * Projeto Live: painel "Situação" — a faixa fininha de estatísticas.
 *
 * Numa linha só, o que o operador precisa olhar de canto de olho sem abrir outra janela:
 * cada destino no ar, a gravação com o tempo, o uso do processador, os quadros perdidos, o espaço
 * que falta no disco, se a projeção está conectada e quantas câmeras estão em sincronia.
 *
 * Tudo vem de quem já sabe: multistream.c (destinos), o OBS (gravação, quadros, disco),
 * projection-link.c (projeção) e phone-video.c com pl-sync.c (câmeras e atraso).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "stats-dock.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <util/platform.h>

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <string>
#include <vector>

#include "layout.h"
#include "multistream.h"
#include "phone-video.h"
#include "pl-sync.h"
#include "projection-link.h"
#include "studio-window.h"

namespace {

void openLive(QWidget *)
{
	pl_studio_window_enter_live();
}

void openPanels(QWidget *from)
{
	pl_layout_show_panels_menu(from);
}

void openLayout(QWidget *from)
{
	pl_layout_show_menu(from);
}

/* Um item da faixa: a bolinha colorida e o texto. */
class Item : public QWidget {
public:
	explicit Item(QWidget *parent) : QWidget(parent)
	{
		auto *layout = new QHBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(5);

		dot = new QLabel(this);
		dot->setFixedSize(8, 8);
		layout->addWidget(dot);

		text = new QLabel(this);
		layout->addWidget(text);
	}

	void set(const QString &value, const char *color)
	{
		if (text->text() != value) {
			text->setText(value);
		}
		if (!color) {
			dot->setVisible(false);
			return;
		}
		dot->setVisible(true);
		if (shownColor != color) {
			shownColor = color;
			dot->setStyleSheet(QString("background: %1; border-radius: 4px;").arg(color));
		}
	}

private:
	QLabel *dot = nullptr;
	QLabel *text = nullptr;
	std::string shownColor;
};

class StatsDock;
void removeInstance(StatsDock *gone);

class StatsDock : public QWidget {
public:
	/* withButtons: na janela do Projeto Live a faixa vem sem os botões Projeto Live, Painéis e Layout. */
	explicit StatsDock(QWidget *parent, bool withButtons = true) : QWidget(parent)
	{
		cpuInfo = os_cpu_usage_info_start();

		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(0, 0, 0, 0);
		outer->setSpacing(0);

		area = new QScrollArea(this);
		area->setWidgetResizable(true);
		area->setFrameShape(QFrame::NoFrame);
		area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

		auto *holder = new QWidget(area);
		row = new QHBoxLayout(holder);
		row->setContentsMargins(8, 4, 8, 4);
		row->setSpacing(16);
		row->addStretch(1);
		area->setWidget(holder);

		/* A faixa rola para o lado quando falta espaço; os dois botões ficam parados na direita. */
		auto *line = new QHBoxLayout();
		line->setContentsMargins(0, 0, 8, 0);
		line->setSpacing(6);
		line->addWidget(area, 1);
		if (withButtons) {
			line->addWidget(button(obs_module_text("Studio.ToLive"), openLive));
			line->addWidget(button(obs_module_text("Layout.Panels"), openPanels));
			line->addWidget(button(obs_module_text("Layout.Menu"), openLayout));
		}
		outer->addLayout(line);

		setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
		setMaximumHeight(34);

		/* O relógio só começa quando o OBS terminar de abrir (veja start()). Durante o
		 * carregamento dos módulos, perguntas como obs_frontend_get_streaming_output() ainda
		 * não têm o que responder e derrubam o programa. */
		timer = new QTimer(this);
		timer->setInterval(1000);
		connect(timer, &QTimer::timeout, this, [this]() { refresh(); });
	}

	/* O OBS acabou de abrir: agora dá para perguntar as coisas dele. */
	void start()
	{
		if (!timer->isActive()) {
			timer->start();
			refresh();
		}
	}

	~StatsDock() override
	{
		removeInstance(this);
		if (cpuInfo) {
			os_cpu_usage_info_destroy(cpuInfo);
		}
	}

	/* Zera o relógio da gravação (o OBS avisou que começou). */
	void recordingStarted() { recordedMs = 0; }

private:
	/* Botão pequeno e sem moldura, do tamanho da faixa. */
	QPushButton *button(const char *text, void (*onClick)(QWidget *))
	{
		auto *created = new QPushButton(QString::fromUtf8(text), this);
		created->setFlat(true);
		created->setCursor(Qt::PointingHandCursor);
		created->setMaximumHeight(26);
		connect(created, &QPushButton::clicked, this, [created, onClick]() { onClick(created); });
		return created;
	}

	Item *item(size_t index)
	{
		while (items.size() <= index) {
			auto *created = new Item(this);
			/* Antes do espaçador, para a faixa encher da esquerda para a direita. */
			row->insertWidget((int)items.size(), created);
			items.push_back(created);
		}
		return items[index];
	}

	void refresh()
	{
		size_t used = 0;

		/* Transmissão principal do OBS e os destinos extras. */
		if (obs_frontend_streaming_active()) {
			const char *color = "#2fa84f";
			int kbps = pl_multistream_main_kbps();
			QString text = QString::fromUtf8(obs_module_text("Stats.Main"));
			if (kbps > 0) {
				text += QString(" · %1 kb/s").arg(kbps);
			}
			item(used++)->set(text, color);
		} else {
			item(used++)->set(QString::fromUtf8(obs_module_text("Stats.Offline")), "#6d6d78");
		}

		size_t count = pl_multistream_count();
		for (size_t i = 0; i < count; i++) {
			struct pl_dest_info info;
			if (!pl_multistream_get(i, &info) || !info.enabled) {
				continue;
			}
			struct pl_dest_status status;
			pl_multistream_get_status(i, &status);
			const char *color = "#6d6d78";
			if (status.state == PL_DEST_LIVE) {
				color = "#2fa84f";
			} else if (status.state == PL_DEST_CONNECTING || status.state == PL_DEST_RECONNECTING) {
				color = "#e8a33d";
			} else if (status.state == PL_DEST_ERROR) {
				color = "#e5352b";
			}
			Item *widget = item(used++);
			widget->set(QString::fromUtf8(info.name), color);
			widget->setToolTip(QString::fromUtf8(status.error));
		}

		/* Gravação. */
		if (obs_frontend_recording_active()) {
			if (!obs_frontend_recording_paused()) {
				recordedMs += timer->interval();
			}
			int seconds = recordedMs / 1000;
			QString clock = QString("%1:%2:%3")
						.arg(seconds / 3600, 2, 10, QChar('0'))
						.arg(seconds / 60 % 60, 2, 10, QChar('0'))
						.arg(seconds % 60, 2, 10, QChar('0'));
			const char *key = obs_frontend_recording_paused() ? "Stats.RecordingPaused" : "Stats.Recording";
			item(used++)->set(QString::fromUtf8(obs_module_text(key)) + " " + clock, "#e5352b");
		}

		/* Processador. */
		if (cpuInfo) {
			double usage = os_cpu_usage_info_query(cpuInfo);
			item(used++)->set(
				QString::fromUtf8(obs_module_text("Stats.Cpu")).arg(QString::number(usage, 'f', 0)),
				nullptr);
		}

		/* Quadros perdidos na transmissão. */
		obs_output_t *stream = obs_frontend_get_streaming_output();
		if (stream) {
			int dropped = obs_output_get_frames_dropped(stream);
			int total = obs_output_get_total_frames(stream);
			QString text = QString::fromUtf8(obs_module_text("Stats.Dropped")).arg(dropped);
			if (total > 0 && dropped > 0) {
				text += QString(" (%1%)").arg(QString::number(dropped * 100.0 / total, 'f', 1));
			}
			item(used++)->set(text, dropped > 0 ? "#e8a33d" : nullptr);
			obs_output_release(stream);
		}

		/* Espaço no disco onde a gravação é salva. */
		char *path = obs_frontend_get_current_record_output_path();
		if (path) {
			uint64_t free = os_get_free_disk_space(path);
			bfree(path);
			if (free > 0) {
				double gb = (double)free / (1024.0 * 1024.0 * 1024.0);
				const char *color = nullptr;
				if (gb < 5.0) {
					color = "#e5352b";
				} else if (gb < 20.0) {
					color = "#e8a33d";
				}
				item(used++)->set(QString::fromUtf8(obs_module_text("Stats.Disk"))
							  .arg(QString::number(gb, 'f', 0)),
						  color);
			}
		}

		/* Projeção. */
		char names[256];
		int projection = pl_projection_status(names, sizeof(names));
		if (projection > 0) {
			Item *widget = item(used++);
			widget->set(QString::fromUtf8(obs_module_text("Stats.Projection")), "#2fa84f");
			widget->setToolTip(QString::fromUtf8(names));
		}

		/* Câmeras em sincronia e o atraso que todas usam. */
		int cameras = pl_phone_video_synced_count();
		if (cameras > 0) {
			QString text = QString::fromUtf8(obs_module_text("Stats.Cameras")).arg(cameras);
			text += QString(" · %1 s").arg(QString::number(pl_sync_current_ms() / 1000.0, 'f', 2));
			item(used++)->set(text, "#2fa84f");
		}

		/* Esconde o que sobrou desta vez. */
		for (size_t i = 0; i < items.size(); i++) {
			items[i]->setVisible(i < used);
		}
	}

	QScrollArea *area = nullptr;
	QHBoxLayout *row = nullptr;
	QTimer *timer = nullptr;
	std::vector<Item *> items;
	os_cpu_usage_info_t *cpuInfo = nullptr;
	int recordedMs = 0;
};

StatsDock *dock = nullptr;
std::vector<StatsDock *> instances;

void removeInstance(StatsDock *gone)
{
	instances.erase(std::remove(instances.begin(), instances.end(), gone), instances.end());
}

void frontendEvent(enum obs_frontend_event event, void *param)
{
	(void)param;
	/* Qualquer aviso do OBS serve para saber que ele já abriu: start() só age na primeira vez.
	 * Assim a faixa não fica parada para sempre se o aviso de "terminei de abrir" não vier. */
	for (StatsDock *each : instances) {
		each->start();
		if (event == OBS_FRONTEND_EVENT_RECORDING_STARTING) {
			each->recordingStarted();
		}
	}
}

} // namespace

void pl_stats_dock_load(void)
{
	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (!mainWindow) {
		return;
	}

	dock = new StatsDock(mainWindow);
	if (!obs_frontend_add_dock_by_id("projeto-live-situacao", obs_module_text("Stats.Title"), dock)) {
		delete dock;
		dock = nullptr;
		return;
	}
	instances.push_back(dock);
	QObject::connect(dock, &QObject::destroyed, []() { dock = nullptr; });
	obs_frontend_add_event_callback(frontendEvent, nullptr);
}

void pl_stats_dock_unload(void)
{
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
}

QWidget *pl_stats_dock_create(QWidget *parent)
{
	auto *created = new StatsDock(parent, false);
	instances.push_back(created);
	created->start();
	return created;
}
