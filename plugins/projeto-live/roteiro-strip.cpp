/*
 * somun live: a faixa do roteiro do culto, embaixo da mesa (desenhada com ela em 24/11 e 25/11).
 *
 * Um bloco por item do roteiro publicado no Diretor, da esquerda para a direita, com o horário
 * previsto e o nome. O que já passou fica apagado; o item de agora fica aceso, com a borda vermelha
 * e "· agora"; os próximos ficam normais. No canto direito, o atraso em vermelho ("13 min
 * atrasado"); no horário, nada.
 *
 * - Horário previsto: a hora do culto mais os minutos dos itens de antes (campo "Duração" de cada
 *   item no Diretor). Item sem duração: os de depois ficam sem horário (só o nome).
 * - Quem decide o "agora": o item que o somun tela diz que está no telão (aoVivo/<culto>.agora). Sem
 *   o telão, o relógio, pelos horários previstos. O clique num bloco só corrige, até o telão mudar.
 * - Atraso: quanto o item de agora começou depois do previsto, ou quanto já passou do fim previsto
 *   dele (o maior dos dois).
 * - Sem culto: a faixa vira uma linha só e devolve o espaço.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "roteiro-strip.h"

#include <obs-module.h>

#include <QDateTime>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <functional>
#include <vector>

#include "pl-account.h"
#include "somun-style.h"

namespace {

QString msg(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

enum class When { Past, Now, Next };

/* Um bloco do roteiro (desenhado à mão para ficar igual ao desenho). */
class Block : public QWidget {
public:
	explicit Block(QWidget *parent) : QWidget(parent)
	{
		setCursor(Qt::PointingHandCursor);
		setMinimumWidth(130);
		setFixedHeight(58);
		setToolTip(msg("Roteiro.ClickHint"));
	}

	void set(const QString &newTime, const QString &newName, When newWhen)
	{
		if (newTime == time && newName == name && newWhen == when) {
			return;
		}
		time = newTime;
		name = newName;
		when = newWhen;
		update();
	}

	std::function<void()> onClick;

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		QRectF box = QRectF(rect()).adjusted(1, 1, -1, -1);
		QColor fill = when == When::Past ? QColor(SOMUN_PANEL) : QColor(SOMUN_PANEL2);
		QColor border = when == When::Now ? QColor(SOMUN_RED)
						  : QColor(when == When::Past ? SOMUN_LINE : SOMUN_LINE2);
		painter.setPen(QPen(border, when == When::Now ? 1.6 : 1.0));
		painter.setBrush(fill);
		painter.drawRoundedRect(box, 6, 6);

		int x = 14;
		QFont small = font();
		small.setPointSizeF(font().pointSizeF());
		QFont big = font();
		big.setPointSizeF(font().pointSizeF() + 1.5);
		QString top = time;
		if (when == When::Now) {
			painter.setPen(Qt::NoPen);
			painter.setBrush(QColor(SOMUN_RED));
			painter.drawEllipse(QRectF(14, 13, 8, 8));
			x = 30;
			top = time.isEmpty() ? msg("Roteiro.Now")
					     : time + QString::fromUtf8(" · ") + msg("Roteiro.Now");
		}
		QColor timeColor = when == When::Past ? QColor("#5C5C5C")
						      : QColor(when == When::Now ? SOMUN_TEXT : SOMUN_GRAY);
		QColor nameColor = when == When::Past ? QColor("#6E6E6E") : QColor(SOMUN_TEXT);
		painter.setFont(small);
		painter.setPen(timeColor);
		painter.drawText(QRect(x, 6, width() - x - 10, 22), Qt::AlignVCenter | Qt::AlignLeft, top);
		painter.setFont(big);
		painter.setPen(nameColor);
		QFontMetrics metrics(big);
		painter.drawText(QRect(14, 28, width() - 24, 24), Qt::AlignVCenter | Qt::AlignLeft,
				 metrics.elidedText(name, Qt::ElideRight, width() - 24));
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()) && onClick) {
			onClick();
		}
	}

private:
	QString time;
	QString name;
	When when = When::Next;
};

class RoteiroStrip : public QWidget {
public:
	explicit RoteiroStrip(QWidget *parent) : QWidget(parent)
	{
		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(0, 0, 0, 0);
		outer->setSpacing(8);

		/* A linha fina que separa a faixa do resto da mesa. */
		auto *line = new QWidget(this);
		line->setFixedHeight(1);
		line->setAttribute(Qt::WA_StyledBackground, true);
		line->setStyleSheet(QString("background: %1;").arg(SOMUN_LINE));
		outer->addWidget(line);

		auto *head = new QHBoxLayout();
		head->setContentsMargins(0, 0, 4, 0);
		title = new QLabel(this);
		title->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY2));
		head->addWidget(title, 1);
		late = new QLabel(this);
		late->setStyleSheet(QString("color: %1;").arg(SOMUN_RED));
		head->addWidget(late);
		outer->addLayout(head);

		area = new QScrollArea(this);
		area->setWidgetResizable(true);
		area->setFrameShape(QFrame::NoFrame);
		area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		area->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
		area->setFixedHeight(66);
		auto *holder = new QWidget(area);
		row = new QHBoxLayout(holder);
		row->setContentsMargins(0, 0, 0, 0);
		row->setSpacing(8);
		area->setWidget(holder);
		outer->addWidget(area);

		auto *timer = new QTimer(this);
		timer->setInterval(1000);
		connect(timer, &QTimer::timeout, this, [this]() { refresh(); });
		timer->start();
		refresh();
	}

private:
	Block *block(size_t index)
	{
		while (blocks.size() <= index) {
			auto *created = new Block(row->parentWidget());
			size_t mine = blocks.size();
			created->onClick = [this, mine]() {
				manualIndex = (int)mine;
				manualAgoraId = shownAgoraId;
				manualAt = QDateTime::currentDateTime();
				refresh();
			};
			row->addWidget(created, 1);
			blocks.push_back(created);
		}
		return blocks[index];
	}

	static QString clock(int minutes)
	{
		minutes = ((minutes % (24 * 60)) + 24 * 60) % (24 * 60);
		return QString("%1:%2").arg(minutes / 60, 2, 10, QChar('0')).arg(minutes % 60, 2, 10, QChar('0'));
	}

	void refresh()
	{
		PLRoteiro roteiro;
		bool has = pl_account_roteiro(roteiro) && !roteiro.itens.empty();
		if (!has) {
			title->setText(msg("Roteiro.None"));
			late->clear();
			area->setVisible(false);
			for (Block *each : blocks) {
				each->hide();
			}
			return;
		}
		area->setVisible(true);
		title->setText(roteiro.nome.isEmpty() ? msg("Roteiro.NoName") : roteiro.nome);

		/* Horário previsto de cada item (-1 = sem horário). */
		size_t count = roteiro.itens.size();
		std::vector<int> start(count, -1);
		QTime base = QTime::fromString(roteiro.hora, "HH:mm");
		if (base.isValid()) {
			int t = base.hour() * 60 + base.minute();
			for (size_t i = 0; i < count; i++) {
				start[i] = t;
				if (roteiro.itens[i].minutos <= 0) {
					break;
				}
				t += roteiro.itens[i].minutos;
			}
		}

		/* Quem é o agora: o clique (até o telão mudar), o telão, ou o relógio. */
		shownAgoraId = roteiro.agoraId;
		if (manualIndex >= 0 && (manualAgoraId != roteiro.agoraId || manualIndex >= (int)count)) {
			manualIndex = -1;
		}
		QDateTime now = QDateTime::currentDateTime();
		int nowMinutes = now.time().hour() * 60 + now.time().minute();
		int current = -1;
		QDateTime startedAt;
		if (manualIndex >= 0) {
			current = manualIndex;
			startedAt = manualAt;
		} else if (!roteiro.agoraId.isEmpty()) {
			for (size_t i = 0; i < count; i++) {
				if (roteiro.itens[i].id == roteiro.agoraId) {
					current = (int)i;
					startedAt = roteiro.agoraEm.toLocalTime();
				}
			}
		}
		if (current < 0 && roteiro.agoraId.isEmpty()) {
			for (size_t i = 0; i < count; i++) {
				if (start[i] >= 0 && start[i] <= nowMinutes) {
					current = (int)i;
				}
			}
		}

		/* Atraso: começou depois do previsto, ou já passou do fim previsto (o maior). */
		int delay = 0;
		if (current >= 0 && start[current] >= 0) {
			if (startedAt.isValid()) {
				int began = startedAt.time().hour() * 60 + startedAt.time().minute();
				delay = std::max(delay, began - start[current]);
			}
			if (roteiro.itens[current].minutos > 0) {
				delay = std::max(delay, nowMinutes - (start[current] + roteiro.itens[current].minutos));
			}
		}
		late->setText(delay >= 1 ? msg("Roteiro.Late").arg(delay) : QString());

		for (size_t i = 0; i < count; i++) {
			When when = current < 0         ? When::Next
				    : (int)i < current  ? When::Past
				    : (int)i == current ? When::Now
							: When::Next;
			const PLRoteiroItem &item = roteiro.itens[i];
			QString name = item.titulo.isEmpty() ? msg("Roteiro.Untitled") : item.titulo;
			Block *each = block(i);
			each->set(start[i] >= 0 ? clock(start[i]) : QString(), name, when);
			each->show();
			row->setStretch((int)i, when == When::Now ? 6 : 5);
		}
		for (size_t i = count; i < blocks.size(); i++) {
			blocks[i]->hide();
		}
		if (current >= 0 && current != shownCurrent) {
			area->ensureWidgetVisible(blocks[current], 40, 0);
		}
		shownCurrent = current;
	}

	QLabel *title = nullptr;
	QLabel *late = nullptr;
	QScrollArea *area = nullptr;
	QHBoxLayout *row = nullptr;
	std::vector<Block *> blocks;
	int manualIndex = -1;
	QString manualAgoraId;
	QDateTime manualAt;
	QString shownAgoraId;
	int shownCurrent = -2;
};

} // namespace

QWidget *pl_roteiro_strip_create(QWidget *parent)
{
	return new RoteiroStrip(parent);
}
