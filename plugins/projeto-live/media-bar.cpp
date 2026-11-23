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

/* "0:42" ou "1:02:03". */
QString clockText(int64_t ms)
{
	if (ms < 0) {
		ms = 0;
	}
	int64_t total = (ms + 999) / 1000;
	int64_t h = total / 3600;
	int64_t m = (total / 60) % 60;
	int64_t s = total % 60;
	if (h > 0) {
		return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
	}
	return QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
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

class MediaBar : public QWidget {
public:
	explicit MediaBar(QWidget *parent) : QWidget(parent)
	{
		setObjectName("plMediaBar");
		auto *row = new QHBoxLayout(this);
		row->setContentsMargins(2, 0, 2, 0);
		row->setSpacing(6);

		play = new QToolButton(this);
		play->setAutoRaise(true);
		play->setFocusPolicy(Qt::NoFocus);
		connect(play, &QToolButton::clicked, this, [this]() { act(togglePlay); });
		row->addWidget(play);

		restart = new QToolButton(this);
		restart->setAutoRaise(true);
		restart->setFocusPolicy(Qt::NoFocus);
		restart->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));
		restart->setToolTip(text("Media.Restart"));
		connect(restart, &QToolButton::clicked, this,
			[this]() { act([](obs_source_t *source) { obs_source_media_restart(source); }); });
		row->addWidget(restart);

		name = new QLabel(this);
		name->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY));
		name->setMinimumWidth(20);
		row->addWidget(name, 1);

		left = new QLabel(this);
		row->addWidget(left);

		setVisible(false);
		auto *timer = new QTimer(this);
		timer->setInterval(250);
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
	/* O vídeo que a faixa mostra: o primeiro tocando; se nenhum, o primeiro pausado; se nenhum, o
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

		QString title = QString::fromUtf8(obs_source_get_name(source));
		if (count > 1) {
			title = text("Media.OneOfMany").arg(title).arg(count);
		}
		if (name->text() != title) {
			name->setText(title);
		}

		QString status;
		int64_t duration = obs_source_media_get_duration(source);
		int64_t time = obs_source_media_get_time(source);
		if (state == OBS_MEDIA_STATE_ENDED || state == OBS_MEDIA_STATE_STOPPED) {
			status = text("Media.Ended");
		} else if (duration > 0) {
			status = text("Media.Left").arg(clockText(duration - time));
			if (state == OBS_MEDIA_STATE_PAUSED) {
				status = text("Media.PausedLeft").arg(status);
			}
		} else if (state == OBS_MEDIA_STATE_PAUSED) {
			status = text("Media.Paused");
		}
		if (left->text() != status) {
			left->setText(status);
		}

		obs_source_release(source);
		if (!isVisible()) {
			setVisible(true);
		}
	}

	QToolButton *play = nullptr;
	QToolButton *restart = nullptr;
	QLabel *name = nullptr;
	QLabel *left = nullptr;
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
