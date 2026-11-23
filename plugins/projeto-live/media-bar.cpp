/*
 * somun live: controles do vídeo que está no ar (a faixa fininha embaixo do No ar) e os mesmos
 * controles no botão direito da entrada.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "media-bar.h"
#include "somun-style.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QSize>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QToolButton>

#include <memory>
#include <vector>

namespace {

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

using WeakPtr = std::shared_ptr<obs_weak_source_t>;

WeakPtr weakOf(obs_source_t *source)
{
	return WeakPtr(obs_source_get_weak_source(source), obs_weak_source_release);
}

bool isMedia(obs_source_t *source)
{
	return source && (obs_source_get_output_flags(source) & OBS_SOURCE_CONTROLLABLE_MEDIA) != 0;
}

/* Os vídeos visíveis dentro da cena (e dos grupos e cenas dentro dela). Cada um com referência:
 * soltar com releaseAll. */
void collectMedia(obs_source_t *sceneSource, std::vector<obs_source_t *> &out, int depth = 0)
{
	if (!sceneSource || depth > 8) {
		return;
	}
	obs_scene_t *scene = obs_group_or_scene_from_source(sceneSource);
	if (!scene) {
		return;
	}
	struct Walk {
		std::vector<obs_source_t *> *out;
		int depth;
	} walk = {&out, depth};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *w = static_cast<Walk *>(param);
			if (!obs_sceneitem_visible(item)) {
				return true;
			}
			obs_source_t *source = obs_sceneitem_get_source(item);
			if (isMedia(source)) {
				for (obs_source_t *already : *w->out) {
					if (already == source) {
						return true;
					}
				}
				w->out->push_back(obs_source_get_ref(source));
			} else if (obs_group_or_scene_from_source(source)) {
				collectMedia(source, *w->out, w->depth + 1);
			}
			return true;
		},
		&walk);
}

void releaseAll(std::vector<obs_source_t *> &list)
{
	for (obs_source_t *source : list) {
		obs_source_release(source);
	}
	list.clear();
}

bool isPlaying(obs_source_t *source)
{
	return obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING;
}

/* Tocar se está parado/pausado/acabou; pausar se está tocando. */
void togglePlay(obs_source_t *source)
{
	enum obs_media_state state = obs_source_media_get_state(source);
	if (state == OBS_MEDIA_STATE_PLAYING) {
		obs_source_media_play_pause(source, true);
	} else if (state == OBS_MEDIA_STATE_PAUSED) {
		obs_source_media_play_pause(source, false);
	} else {
		obs_source_media_restart(source);
	}
}

/* A barra da linha do tempo: clicar em qualquer ponto pula direto para lá (como no OBS), e
 * arrastar a bolinha vai mostrando o ponto. */
class TimeSlider : public QSlider {
public:
	explicit TimeSlider(QWidget *parent) : QSlider(Qt::Horizontal, parent)
	{
		setRange(0, 1000);
		setFocusPolicy(Qt::NoFocus);
		setStyleSheet(QString("QSlider::groove:horizontal { height: 4px; background: %1; border-radius: 2px; }"
				      "QSlider::sub-page:horizontal { background: %2; border-radius: 2px; }"
				      "QSlider::handle:horizontal { background: %2; width: 14px; margin: -5px 0;"
				      " border-radius: 7px; }")
				      .arg(SOMUN_LINE2, SOMUN_WHITE));
	}

protected:
	int valueAt(QMouseEvent *event) const
	{
		return QStyle::sliderValueFromPosition(minimum(), maximum(), (int)event->position().x(), width());
	}

	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton) {
			int value = valueAt(event);
			setSliderDown(true);
			setValue(value);
			emit sliderMoved(value);
			event->accept();
			return;
		}
		QSlider::mousePressEvent(event);
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (isSliderDown()) {
			int value = valueAt(event);
			setValue(value);
			emit sliderMoved(value);
			event->accept();
			return;
		}
		QSlider::mouseMoveEvent(event);
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (isSliderDown()) {
			setSliderDown(false);
			emit sliderReleased();
			event->accept();
			return;
		}
		QSlider::mouseReleaseEvent(event);
	}
};

/* "00:00:38", como o OBS mostra. */
QString longClock(int64_t ms)
{
	if (ms < 0) {
		ms = 0;
	}
	int64_t total = ms / 1000;
	return QString("%1:%2:%3")
		.arg(total / 3600, 2, 10, QChar('0'))
		.arg((total / 60) % 60, 2, 10, QChar('0'))
		.arg(total % 60, 2, 10, QChar('0'));
}

class MediaBar : public QWidget {
public:
	explicit MediaBar(QWidget *parent) : QWidget(parent)
	{
		setObjectName("plMediaBar");
		auto *row = new QHBoxLayout(this);
		row->setContentsMargins(2, 2, 2, 2);
		row->setSpacing(6);

		play = new QToolButton(this);
		play->setFocusPolicy(Qt::NoFocus);
		play->setIconSize(QSize(18, 18));
		connect(play, &QToolButton::clicked, this, [this]() { act(togglePlay); });
		row->addWidget(play);

		stop = new QToolButton(this);
		stop->setFocusPolicy(Qt::NoFocus);
		stop->setIconSize(QSize(18, 18));
		stop->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
		stop->setToolTip(text("Media.Stop"));
		connect(stop, &QToolButton::clicked, this,
			[this]() { act([](obs_source_t *source) { obs_source_media_stop(source); }); });
		row->addWidget(stop);

		name = new QLabel(this);
		name->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY));
		name->setVisible(false);
		row->addWidget(name);

		slider = new TimeSlider(this);
		connect(slider, &QSlider::sliderMoved, this, [this](int value) { seek(value); });
		connect(slider, &QSlider::sliderReleased, this, [this]() { seek(slider->value()); });
		row->addWidget(slider, 1);

		times = new QLabel(this);
		row->addWidget(times);

		setVisible(false);
		auto *timer = new QTimer(this);
		timer->setInterval(200);
		connect(timer, &QTimer::timeout, this, [this]() { refresh(); });
		timer->start();
	}

	void setAllowed(bool on)
	{
		if (allowed != on) {
			allowed = on;
			refresh();
		}
	}

private:
	/* O vídeo que a barra mostra: o primeiro tocando; se nenhum, o primeiro pausado; se nenhum, o
	 * primeiro. Volta com referência (ou nullptr). */
	obs_source_t *pick()
	{
		obs_source_t *program = obs_frontend_get_current_scene();
		std::vector<obs_source_t *> list;
		collectMedia(program, list);
		obs_source_release(program);
		count = (int)list.size();
		obs_source_t *best = nullptr;
		for (obs_source_t *source : list) {
			if (isPlaying(source)) {
				best = source;
				break;
			}
		}
		if (!best) {
			for (obs_source_t *source : list) {
				if (obs_source_media_get_state(source) == OBS_MEDIA_STATE_PAUSED) {
					best = source;
					break;
				}
			}
		}
		if (!best && !list.empty()) {
			best = list.front();
		}
		obs_source_t *result = best ? obs_source_get_ref(best) : nullptr;
		releaseAll(list);
		return result;
	}

	template<typename Fn> void act(Fn fn)
	{
		obs_source_t *source = obs_weak_source_get_source(current.get());
		if (source) {
			fn(source);
			obs_source_release(source);
		}
		refresh();
	}

	/* Pula para o ponto da barra (0 a 1000). */
	void seek(int value)
	{
		obs_source_t *source = obs_weak_source_get_source(current.get());
		if (!source) {
			return;
		}
		int64_t duration = obs_source_media_get_duration(source);
		if (duration > 0) {
			int64_t ms = duration * value / 1000;
			obs_source_media_set_time(source, ms);
			showTimes(ms, duration);
		}
		obs_source_release(source);
	}

	void showTimes(int64_t time, int64_t duration)
	{
		QString label = duration > 0 ? QString("%1 / -%2").arg(longClock(time), longClock(duration - time))
					     : longClock(time);
		if (times->text() != label) {
			times->setText(label);
		}
	}

	void refresh()
	{
		/* Tela escondida (ex.: as telas foram para o monitor 2): nem procura. */
		bool shown = allowed && parentWidget() && parentWidget()->isVisible();
		obs_source_t *source = shown ? pick() : nullptr;
		if (!source) {
			current.reset();
			if (isVisible()) {
				setVisible(false);
			}
			return;
		}
		current = weakOf(source);

		enum obs_media_state state = obs_source_media_get_state(source);
		bool playing = state == OBS_MEDIA_STATE_PLAYING;
		if (playing != shownPlaying || play->icon().isNull()) {
			shownPlaying = playing;
			play->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
			play->setToolTip(text(playing ? "Media.Pause" : "Media.Play"));
		}

		/* Com mais de um vídeo no ar, mostra de qual é a barra. */
		QString title;
		if (count > 1) {
			title = text("Media.OneOfMany").arg(QString::fromUtf8(obs_source_get_name(source))).arg(count);
		}
		if (name->text() != title) {
			name->setText(title);
			name->setVisible(!title.isEmpty());
		}
		slider->setToolTip(QString::fromUtf8(obs_source_get_name(source)));

		/* Enquanto a pessoa arrasta, a barra é dela. */
		if (!slider->isSliderDown()) {
			int64_t duration = obs_source_media_get_duration(source);
			bool stopped = state == OBS_MEDIA_STATE_STOPPED || state == OBS_MEDIA_STATE_NONE;
			int64_t time = stopped ? 0 : obs_source_media_get_time(source);
			if (state == OBS_MEDIA_STATE_ENDED) {
				time = duration;
			}
			int value = duration > 0 ? (int)(time * 1000 / duration) : 0;
			slider->setEnabled(duration > 0);
			if (slider->value() != value) {
				QSignalBlocker block(slider);
				slider->setValue(value);
			}
			showTimes(time, duration);
		}

		obs_source_release(source);
		if (!isVisible()) {
			setVisible(true);
		}
	}

	QToolButton *play = nullptr;
	QToolButton *stop = nullptr;
	QLabel *name = nullptr;
	TimeSlider *slider = nullptr;
	QLabel *times = nullptr;
	WeakPtr current;
	int count = 0;
	bool allowed = true;
	bool shownPlaying = false;
};

void addControls(QMenu *menu, obs_source_t *source, const QString &suffix)
{
	WeakPtr weak = weakOf(source);
	bool playing = isPlaying(source);
	QAction *toggle = menu->addAction(text(playing ? "Media.PauseMenu" : "Media.PlayMenu") + suffix);
	QObject::connect(toggle, &QAction::triggered, menu, [weak]() {
		obs_source_t *s = obs_weak_source_get_source(weak.get());
		if (s) {
			togglePlay(s);
			obs_source_release(s);
		}
	});
	QAction *again = menu->addAction(text("Media.RestartMenu") + suffix);
	QObject::connect(again, &QAction::triggered, menu, [weak]() {
		obs_source_t *s = obs_weak_source_get_source(weak.get());
		if (s) {
			obs_source_media_restart(s);
			obs_source_release(s);
		}
	});
}

} // namespace

QWidget *pl_media_bar_create(QWidget *parent)
{
	return new MediaBar(parent);
}

void pl_media_bar_set_allowed(QWidget *bar, bool allowed)
{
	auto *media = dynamic_cast<MediaBar *>(bar);
	if (media) {
		media->setAllowed(allowed);
	}
}

void pl_media_menu(QMenu *menu, obs_source_t *scene)
{
	if (!menu || !scene) {
		return;
	}
	std::vector<obs_source_t *> list;
	collectMedia(scene, list);
	if (list.size() == 1) {
		addControls(menu, list.front(), QString());
		menu->addSeparator();
	} else if (list.size() > 1) {
		/* Mais de um vídeo na entrada: um par de opções para cada, com o nome. */
		for (obs_source_t *source : list) {
			addControls(menu, source, QString(" (%1)").arg(QString::fromUtf8(obs_source_get_name(source))));
		}
		menu->addSeparator();
	}
	releaseAll(list);
}
