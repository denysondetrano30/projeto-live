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
#include <QAction>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
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
#include "somun-style.h"
#include "studio-window.h"
#include "stream-dock.h"
#include "team-panel.h"

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

	/*
	 * hollow: bolinha vazada (só o contorno), usada no destino que caiu. textColor: cor do texto
	 * (nullptr = a normal). destination: o destino desta linha (-1 = não é destino), para o botão
	 * direito saber em quem mexer.
	 */
	void set(const QString &value, const char *color, bool hollow = false, const char *textColor = nullptr,
		 int destination = -1)
	{
		destIndex = destination;
		if (text->text() != value) {
			text->setText(value);
		}
		std::string wantedText = textColor ? textColor : "";
		if (shownTextColor != wantedText) {
			shownTextColor = wantedText;
			text->setStyleSheet(textColor ? QString("color: %1;").arg(textColor) : QString());
		}
		if (!color) {
			dot->setVisible(false);
			return;
		}
		dot->setVisible(true);
		std::string wanted = std::string(color) + (hollow ? "/o" : "");
		if (shownColor != wanted) {
			shownColor = wanted;
			dot->setStyleSheet(hollow ? QString("background: transparent; border: 2px solid %1;"
							    " border-radius: 4px;")
							    .arg(color)
						  : QString("background: %1; border-radius: 4px;").arg(color));
		}
	}

	int destination() const { return destIndex; }

private:
	QLabel *dot = nullptr;
	QLabel *text = nullptr;
	std::string shownColor;
	std::string shownTextColor;
	int destIndex = -1;
};

class StatsDock;
void removeInstance(StatsDock *gone);

class StatsDock : public QWidget {
public:
	/* withButtons: na janela do Projeto Live a faixa vem sem os botões Projeto Live, Painéis e Layout. */
	explicit StatsDock(QWidget *parent, bool withButtons = true, bool column = false) : QWidget(parent)
	{
		cpuInfo = os_cpu_usage_info_start();

		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(0, 0, 0, 0);
		outer->setSpacing(0);

		timer = new QTimer(this);
		timer->setInterval(1000);
		connect(timer, &QTimer::timeout, this, [this]() { refresh(); });

		/* Coluna "Situação completa" da tela Operar: um item por linha, com o título em cima. */
		if (column) {
			auto *title = new QLabel(QString::fromUtf8(obs_module_text("Stats.Full")), this);
			title->setObjectName("somunPanelTitle");
			outer->addWidget(title);
			outer->addSpacing(6);
			row = new QVBoxLayout();
			row->setContentsMargins(0, 0, 0, 0);
			row->setSpacing(6);
			row->addStretch(1);
			outer->addLayout(row, 1);
			return;
		}

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
			created->setContextMenuPolicy(Qt::CustomContextMenu);
			connect(created, &QWidget::customContextMenuRequested, this,
				[this, created](const QPoint &pos) { destinationMenu(created, pos); });
			/* Antes do espaçador, para a faixa encher da esquerda para a direita. */
			row->insertWidget((int)items.size(), created);
			items.push_back(created);
		}
		return items[index];
	}

	/*
	 * Botão direito num destino da Situação, com a live no ar (parte G3): tentar de novo agora,
	 * tirar só este do ar (pergunta antes; os outros seguem) e os quadros perdidos dele.
	 */
	void destinationMenu(Item *widget, const QPoint &pos)
	{
		int index = widget->destination();
		if (index < 0 || !pl_multistream_active()) {
			return;
		}
		struct pl_dest_info info;
		struct pl_dest_status status;
		if (!pl_multistream_get((size_t)index, &info)) {
			return;
		}
		pl_multistream_get_status((size_t)index, &status);
		bool paused = status.state == PL_DEST_PAUSED;

		QMenu menu(this);
		QAction *retry = menu.addAction(
			QString::fromUtf8(obs_module_text(paused ? "Stats.DestBackOnAir" : "Stats.DestRetry")));
		QAction *takeOff = nullptr;
		if (!paused) {
			takeOff = menu.addAction(QString::fromUtf8(obs_module_text("Stats.DestTakeOff")));
		}
		menu.addSeparator();
		QString dropped = QString::fromUtf8(obs_module_text("Stats.DestDropped")).arg(status.frames_dropped);
		if (status.frames_total > 0 && status.frames_dropped > 0) {
			dropped += QString(" (%1%)").arg(
				QString::number(status.frames_dropped * 100.0 / status.frames_total, 'f', 1)
					.replace('.', ','));
		}
		menu.addAction(dropped)->setEnabled(false);

		QAction *chosen = menu.exec(widget->mapToGlobal(pos));
		if (!chosen) {
			return;
		}
		if (chosen == retry) {
			pl_multistream_retry((size_t)index);
		} else if (takeOff && chosen == takeOff) {
			QString question = QString::fromUtf8(obs_module_text("Stats.DestTakeOffConfirm"))
						   .arg(QString::fromUtf8(info.name));
			if (QMessageBox::question(this, QString::fromUtf8(obs_module_text("Stats.DestTakeOff")),
						  question) == QMessageBox::Yes) {
				pl_multistream_take_off((size_t)index);
			}
		}
		refresh();
	}

	void refresh()
	{
		size_t used = 0;

		/* A transmissão é a nossa lista de destinos (parte G): parada, só "Fora do ar". */
		if (!pl_multistream_active()) {
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

			/* Velocidade: o que saiu desde a última volta (1 s). */
			if (lastBytes.size() <= i) {
				lastBytes.resize(i + 1, 0);
			}
			QString speed;
			if (status.state == PL_DEST_LIVE && lastBytes[i] > 0 && status.total_bytes > lastBytes[i]) {
				double mbps = (double)(status.total_bytes - lastBytes[i]) * 8.0 / 1000000.0 /
					      (timer->interval() / 1000.0);
				speed = QString::number(mbps, 'f', 1).replace('.', ',');
			}
			lastBytes[i] = status.total_bytes;

			/*
			 * Parte G3: vermelho só no ar e no erro. No ar = bolinha vermelha cheia + Mb/s; ligando,
			 * esperando ou tirado do ar = bolinha cinza; caiu = bolinha vermelha vazada e texto
			 * vermelho. O botão direito mexe só neste destino.
			 */
			QString name = QString::fromUtf8(info.name);
			QString text = name;
			const char *color = "#6d6d78";
			bool hollow = false;
			const char *textColor = nullptr;
			QString tip;
			switch (status.state) {
			case PL_DEST_LIVE:
				color = SOMUN_RED;
				if (!speed.isEmpty()) {
					text = QString::fromUtf8(obs_module_text("Stats.DestLive")).arg(name, speed);
				}
				break;
			case PL_DEST_CONNECTING:
				text = QString::fromUtf8(obs_module_text("Stats.DestConnecting")).arg(name);
				break;
			case PL_DEST_RECONNECTING:
				color = SOMUN_RED;
				hollow = true;
				textColor = SOMUN_RED;
				text = QString::fromUtf8(obs_module_text("Stats.DestRetrying"))
					       .arg(name)
					       .arg(std::max(status.tries, 1))
					       .arg(status.max_tries);
				tip = QString::fromUtf8(obs_module_text("Stats.DestRightClick"));
				break;
			case PL_DEST_ERROR:
				color = SOMUN_RED;
				hollow = true;
				textColor = SOMUN_RED;
				text = QString::fromUtf8(obs_module_text("Stats.DestDown")).arg(name);
				tip = QString::fromUtf8(status.error) + "\n" +
				      QString::fromUtf8(obs_module_text("Stats.DestRightClick"));
				break;
			case PL_DEST_PAUSED:
				text = QString::fromUtf8(obs_module_text("Stats.DestPaused")).arg(name);
				break;
			default:
				break;
			}
			Item *widget = item(used++);
			widget->set(text, color, hollow, textColor, (int)i);
			widget->setToolTip(tip);
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

		/* Quadros perdidos na transmissão do OBS (só no Modo OBS; os destinos do somun entram na G3). */
		obs_output_t *stream = obs_frontend_get_streaming_output();
		if (stream && obs_output_active(stream)) {
			int dropped = obs_output_get_frames_dropped(stream);
			int total = obs_output_get_total_frames(stream);
			QString text = QString::fromUtf8(obs_module_text("Stats.Dropped")).arg(dropped);
			if (total > 0 && dropped > 0) {
				text += QString(" (%1%)").arg(QString::number(dropped * 100.0 / total, 'f', 1));
			}
			item(used++)->set(text, dropped > 0 ? "#e8a33d" : nullptr);
		}
		obs_output_release(stream);

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
			/* Aviso discreto: o telão caiu no modo antigo (sem NDI, com mais atraso). */
			if (pl_mirror_mode() == 2) {
				Item *old = item(used++);
				old->set(QString::fromUtf8(obs_module_text("Stats.MirrorOld")), "#d9a21b");
				old->setToolTip(QString::fromUtf8(obs_module_text("Stats.MirrorOldTip")));
			}
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
	QBoxLayout *row = nullptr;
	/* Bytes enviados por destino na última volta do relógio (para a velocidade em Mb/s). */
	std::vector<uint64_t> lastBytes;
	QTimer *timer = nullptr;
	std::vector<Item *> items;
	os_cpu_usage_info_t *cpuInfo = nullptr;
	int recordedMs = 0;
};

/* ------------------------------------------------------------------------- */
/* A coluna "Transmissão" da mesa (desenho de 24/11; engoliu a antiga Situação) */

/* Uma linha: bolinha (opcional), nome à esquerda e o valor à direita. */
class Line : public QWidget {
public:
	explicit Line(QWidget *parent, bool withDot) : QWidget(parent)
	{
		auto *layout = new QHBoxLayout(this);
		layout->setContentsMargins(0, 2, 2, 2);
		layout->setSpacing(10);
		if (withDot) {
			dot = new QLabel(this);
			dot->setFixedSize(8, 8);
			layout->addWidget(dot);
		}
		left = new QLabel(this);
		layout->addWidget(left, 1);
		right = new QLabel(this);
		right->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		layout->addWidget(right);
	}

	/* dotColor nullptr = sem bolinha nesta vez; hollow = só o contorno (destino que caiu). */
	void set(const QString &name, const QString &value, const char *dotColor, bool hollow, const char *nameColor,
		 const char *valueColor)
	{
		if (left->text() != name) {
			left->setText(name);
		}
		if (right->text() != value) {
			right->setText(value);
		}
		QString look = QString("%1|%2|%3|%4")
				       .arg(dotColor ? dotColor : "")
				       .arg(hollow)
				       .arg(nameColor ? nameColor : "")
				       .arg(valueColor ? valueColor : "");
		if (look == shownLook) {
			return;
		}
		shownLook = look;
		left->setStyleSheet(QString("color: %1;").arg(nameColor ? nameColor : SOMUN_TEXT));
		right->setStyleSheet(QString("color: %1;").arg(valueColor ? valueColor : SOMUN_TEXT));
		if (dot) {
			dot->setVisible(dotColor != nullptr);
			if (dotColor) {
				dot->setStyleSheet(
					hollow ? QString("background: transparent; border: 2px solid %1;"
							 " border-radius: 4px;")
							 .arg(dotColor)
					       : QString("background: %1; border-radius: 4px;").arg(dotColor));
			}
		}
	}

	int destination = -1;

private:
	QLabel *dot = nullptr;
	QLabel *left = nullptr;
	QLabel *right = nullptr;
	QString shownLook;
};

QWidget *separator(QWidget *parent)
{
	auto *line = new QWidget(parent);
	line->setFixedHeight(1);
	line->setAttribute(Qt::WA_StyledBackground, true);
	line->setStyleSheet(QString("background: %1;").arg(SOMUN_LINE));
	return line;
}

class TransmissionPanel : public QWidget {
public:
	explicit TransmissionPanel(QWidget *parent) : QWidget(parent)
	{
		cpuInfo = os_cpu_usage_info_start();
		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(0, 0, 0, 0);
		outer->setSpacing(6);

		auto *head = new QHBoxLayout();
		head->setContentsMargins(0, 0, 0, 0);
		auto *title = new QLabel(QString::fromUtf8(obs_module_text("Transmission.Title")), this);
		title->setObjectName("somunPanelTitle");
		head->addWidget(title, 1);
		auto *check = new QPushButton(QString::fromUtf8(obs_module_text("Transmission.Check")), this);
		check->setFocusPolicy(Qt::NoFocus);
		check->setToolTip(QString::fromUtf8(obs_module_text("Check.ButtonHelp")));
		connect(check, &QPushButton::clicked, this, [this]() { pl_destinations_check(window()); });
		head->addWidget(check);
		outer->addLayout(head);
		outer->addSpacing(4);

		destLayout = new QVBoxLayout();
		destLayout->setContentsMargins(0, 0, 0, 0);
		destLayout->setSpacing(4);
		outer->addLayout(destLayout);

		outer->addSpacing(6);
		outer->addWidget(separator(this));
		outer->addSpacing(6);

		recordLine = new Line(this, false);
		diskLine = new Line(this, false);
		cpuLine = new Line(this, false);
		mirrorLine = new Line(this, false);
		mirrorLine->setToolTip(QString::fromUtf8(obs_module_text("Stats.MirrorOldTip")));
		for (Line *each : {recordLine, diskLine, cpuLine, mirrorLine}) {
			outer->addWidget(each);
		}
		mirrorLine->hide();

		outer->addSpacing(6);
		outer->addWidget(separator(this));
		outer->addSpacing(6);
		outer->addWidget(pl_team_panel_create(this));
		outer->addStretch(1);

		timer = new QTimer(this);
		timer->setInterval(1000);
		connect(timer, &QTimer::timeout, this, [this]() { refresh(); });
		timer->start();
		refresh();
	}

	~TransmissionPanel() override
	{
		if (cpuInfo) {
			os_cpu_usage_info_destroy(cpuInfo);
		}
	}

private:
	Line *destLine(size_t index)
	{
		while (destLines.size() <= index) {
			auto *created = new Line(this, true);
			created->setContextMenuPolicy(Qt::CustomContextMenu);
			connect(created, &QWidget::customContextMenuRequested, this,
				[this, created](const QPoint &pos) { destinationMenu(created, pos); });
			destLayout->addWidget(created);
			destLines.push_back(created);
		}
		return destLines[index];
	}

	/* Botão direito num destino no ar (parte G3): tentar de novo, tirar só este do ar, quadros perdidos. */
	void destinationMenu(Line *widget, const QPoint &pos)
	{
		int index = widget->destination;
		if (index < 0 || !pl_multistream_active()) {
			return;
		}
		struct pl_dest_info info;
		struct pl_dest_status status;
		if (!pl_multistream_get((size_t)index, &info) || !info.enabled) {
			return;
		}
		pl_multistream_get_status((size_t)index, &status);
		bool paused = status.state == PL_DEST_PAUSED;
		QMenu menu(this);
		QAction *retry = menu.addAction(
			QString::fromUtf8(obs_module_text(paused ? "Stats.DestBackOnAir" : "Stats.DestRetry")));
		QAction *takeOff = nullptr;
		if (!paused) {
			takeOff = menu.addAction(QString::fromUtf8(obs_module_text("Stats.DestTakeOff")));
		}
		menu.addSeparator();
		QString dropped = QString::fromUtf8(obs_module_text("Stats.DestDropped")).arg(status.frames_dropped);
		if (status.frames_total > 0 && status.frames_dropped > 0) {
			dropped += QString(" (%1%)").arg(
				QString::number(status.frames_dropped * 100.0 / status.frames_total, 'f', 1)
					.replace('.', ','));
		}
		menu.addAction(dropped)->setEnabled(false);
		QAction *chosen = menu.exec(widget->mapToGlobal(pos));
		if (chosen == retry) {
			pl_multistream_retry((size_t)index);
		} else if (takeOff && chosen == takeOff) {
			QString question = QString::fromUtf8(obs_module_text("Stats.DestTakeOffConfirm"))
						   .arg(QString::fromUtf8(info.name));
			if (QMessageBox::question(this, QString::fromUtf8(obs_module_text("Stats.DestTakeOff")),
						  question) == QMessageBox::Yes) {
				pl_multistream_take_off((size_t)index);
			}
		}
		refresh();
	}

	static QString clock(uint64_t seconds)
	{
		return QString("%1:%2:%3")
			.arg(seconds / 3600, 2, 10, QChar('0'))
			.arg(seconds / 60 % 60, 2, 10, QChar('0'))
			.arg(seconds % 60, 2, 10, QChar('0'));
	}

	void refresh()
	{
		const char *off = SOMUN_GRAY;
		QString offText = QString::fromUtf8(obs_module_text("Transmission.Off"));
		bool streaming = pl_multistream_active();
		size_t used = 0;
		size_t count = pl_multistream_count();
		for (size_t i = 0; i < count; i++) {
			struct pl_dest_info info;
			if (!pl_multistream_get(i, &info)) {
				continue;
			}
			QString name = QString::fromUtf8(info.name);
			Line *line = destLine(used++);
			line->destination = (int)i;
			line->setToolTip(QString());
			if (!streaming || !info.enabled) {
				line->set(name, offText, SOMUN_LINE2, false, SOMUN_GRAY2, off);
				continue;
			}
			struct pl_dest_status status;
			pl_multistream_get_status(i, &status);
			if (lastBytes.size() <= i) {
				lastBytes.resize(i + 1, 0);
			}
			QString speed;
			if (status.state == PL_DEST_LIVE && lastBytes[i] > 0 && status.total_bytes > lastBytes[i]) {
				double mbps = (double)(status.total_bytes - lastBytes[i]) * 8.0 / 1000000.0 /
					      (timer->interval() / 1000.0);
				speed = QString::number(mbps, 'f', 1).replace('.', ',');
			}
			lastBytes[i] = status.total_bytes;
			switch (status.state) {
			case PL_DEST_LIVE:
				line->set(name, speed, SOMUN_RED, false, nullptr, nullptr);
				break;
			case PL_DEST_RECONNECTING:
				line->set(name,
					  QString::fromUtf8(obs_module_text("Transmission.Retrying"))
						  .arg(std::max(status.tries, 1))
						  .arg(status.max_tries),
					  SOMUN_RED, true, SOMUN_RED, SOMUN_RED);
				line->setToolTip(QString::fromUtf8(obs_module_text("Stats.DestRightClick")));
				break;
			case PL_DEST_ERROR:
				line->set(name, QString::fromUtf8(obs_module_text("Transmission.Down")), SOMUN_RED,
					  true, SOMUN_RED, SOMUN_RED);
				line->setToolTip(QString::fromUtf8(status.error) + "\n" +
						 QString::fromUtf8(obs_module_text("Stats.DestRightClick")));
				break;
			case PL_DEST_PAUSED:
				line->set(name, QString::fromUtf8(obs_module_text("Transmission.Paused")), SOMUN_LINE2,
					  false, SOMUN_GRAY2, off);
				break;
			default:
				line->set(name, QString::fromUtf8(obs_module_text("Transmission.Connecting")),
					  SOMUN_GRAY, false, nullptr, off);
				break;
			}
		}
		/* Sem destino nenhum: "Fora do ar". */
		if (used == 0) {
			Line *line = destLine(used++);
			line->destination = -1;
			line->set(QString::fromUtf8(obs_module_text("Stats.Offline")), QString(), SOMUN_LINE2, false,
				  SOMUN_GRAY2, off);
		}
		for (size_t i = 0; i < destLines.size(); i++) {
			destLines[i]->setVisible(i < used);
		}

		/* Gravando (tempo do OBS, pelos quadros), Disco, Processador. */
		if (obs_frontend_recording_active()) {
			obs_output_t *output = obs_frontend_get_recording_output();
			struct obs_video_info ovi;
			uint64_t seconds = 0;
			if (output && obs_get_video_info(&ovi) && ovi.fps_num) {
				seconds = (uint64_t)obs_output_get_total_frames(output) * ovi.fps_den / ovi.fps_num;
			}
			obs_output_release(output);
			const char *key = obs_frontend_recording_paused() ? "Transmission.RecordPaused"
									  : "Transmission.Recording";
			recordLine->set(QString::fromUtf8(obs_module_text(key)), clock(seconds), nullptr, false,
					nullptr, nullptr);
		} else {
			recordLine->set(QString::fromUtf8(obs_module_text("Transmission.Record")), offText, nullptr,
					false, SOMUN_GRAY2, off);
		}

		char *path = obs_frontend_get_current_record_output_path();
		if (path) {
			uint64_t free = os_get_free_disk_space(path);
			bfree(path);
			double gb = (double)free / (1024.0 * 1024.0 * 1024.0);
			/* Pouco disco é erro (a gravação vai parar): vermelho. */
			const char *color = gb < 5.0 ? SOMUN_RED : nullptr;
			diskLine->set(QString::fromUtf8(obs_module_text("Transmission.Disk")),
				      QString("%1 GB").arg(QString::number(gb, 'f', 0)), nullptr, false, color, color);
			diskLine->setVisible(free > 0);
		} else {
			diskLine->hide();
		}

		if (cpuInfo) {
			double usage = os_cpu_usage_info_query(cpuInfo);
			cpuLine->set(QString::fromUtf8(obs_module_text("Transmission.Cpu")),
				     QString("%1%").arg(QString::number(usage, 'f', 0)), nullptr, false, nullptr,
				     nullptr);
		}

		/* Aviso discreto: o telão caiu no modo antigo (sem NDI, com mais atraso). */
		char names[256];
		bool mirrorOld = pl_projection_status(names, sizeof(names)) > 0 && pl_mirror_mode() == 2;
		if (mirrorOld) {
			mirrorLine->set(QString::fromUtf8(obs_module_text("Stats.MirrorOld")), QString(), nullptr,
					false, SOMUN_GRAY2, nullptr);
		}
		if (mirrorLine->isHidden() == mirrorOld) {
			mirrorLine->setVisible(mirrorOld);
		}
	}

	QVBoxLayout *destLayout = nullptr;
	std::vector<Line *> destLines;
	std::vector<uint64_t> lastBytes;
	Line *recordLine = nullptr;
	Line *diskLine = nullptr;
	Line *cpuLine = nullptr;
	Line *mirrorLine = nullptr;
	QTimer *timer = nullptr;
	os_cpu_usage_info_t *cpuInfo = nullptr;
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

QWidget *pl_stats_column_create(QWidget *parent)
{
	auto *created = new StatsDock(parent, false, true);
	instances.push_back(created);
	created->start();
	return created;
}

QWidget *pl_transmission_panel_create(QWidget *parent)
{
	return new TransmissionPanel(parent);
}
