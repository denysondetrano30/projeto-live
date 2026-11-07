/*
 * Projeto Live: painéis da tela vertical.
 *
 * - "Programa vertical": mostra o que está saindo em pé, se o vertical está seguindo o Programa
 *   ou numa cena escolhida à mão, e o botão "Seguir o Programa".
 * - "Cenas verticais": as gêmeas (⇄, na ordem das cenas do OBS) e as cenas só verticais. Um clique
 *   escolhe a cena para mexer nas fontes; dois cliques (ou "Pôr no ar") põem a cena no vertical.
 * - "Fontes verticais": as fontes da cena escolhida, com a caixa de aparecer/sumir, a ordem e os
 *   enquadramentos rápidos (preencher, inteira, largura toda).
 *
 * O trabalho de verdade fica em vertical.c.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "vertical-dock.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QColor>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlatformSurfaceEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <vector>

#include "vertical.h"

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* ------------------------------------------------------------------------- */
/* Cena escolhida para mexer (compartilhada pelos painéis de cenas e fontes)   */

obs_weak_source_t *editing = nullptr;

obs_source_t *editingScene()
{
	return editing ? obs_weak_source_get_source(editing) : nullptr;
}

void setEditingScene(obs_source_t *scene)
{
	obs_weak_source_release(editing);
	editing = scene ? obs_source_get_weak_source(scene) : nullptr;
	pl_vertical_touch();
}

/* ------------------------------------------------------------------------- */
/* Janela de vídeo da tela vertical                                           */

void drawVertical(void *, uint32_t cx, uint32_t cy)
{
	obs_canvas_t *canvas = pl_vertical_get_canvas();
	if (!canvas) {
		return;
	}

	float base_w = PL_VERTICAL_WIDTH;
	float base_h = PL_VERTICAL_HEIGHT;
	obs_video_info ovi;
	if (obs_canvas_get_video_info(canvas, &ovi) && ovi.output_width && ovi.output_height) {
		base_w = (float)ovi.output_width;
		base_h = (float)ovi.output_height;
	}

	float scale = std::min((float)cx / base_w, (float)cy / base_h);
	int width = (int)(base_w * scale);
	int height = (int)(base_h * scale);
	int x = ((int)cx - width) / 2;
	int y = ((int)cy - height) / 2;

	gs_projection_push();
	gs_viewport_push();
	gs_set_viewport(x, y, width, height);
	gs_ortho(0.0f, base_w, 0.0f, base_h, -100.0f, 100.0f);
	obs_render_canvas_texture(canvas);
	gs_viewport_pop();
	gs_projection_pop();

	obs_canvas_release(canvas);
}

class VerticalDisplay : public QWidget {
public:
	explicit VerticalDisplay(QWidget *parent) : QWidget(parent)
	{
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_StaticContents);
		setAttribute(Qt::WA_NoSystemBackground);
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_DontCreateNativeAncestors);
		setAttribute(Qt::WA_NativeWindow);
		setMinimumSize(90, 160);

		if (QWindow *window = windowHandle()) {
			window->installEventFilter(this);
			connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
				if (visible) {
					createDisplay();
					resizeDisplay();
				}
			});
		}
	}

	~VerticalDisplay() override { destroyDisplay(); }

	QPaintEngine *paintEngine() const override { return nullptr; }

protected:
	void paintEvent(QPaintEvent *event) override
	{
		createDisplay();
		QWidget::paintEvent(event);
	}

	void resizeEvent(QResizeEvent *event) override
	{
		QWidget::resizeEvent(event);
		createDisplay();
		resizeDisplay();
	}

	bool eventFilter(QObject *object, QEvent *event) override
	{
		/* A janela nativa vai ser trocada (painel solto ou preso de novo): solta a tela antes. */
		if (event->type() == QEvent::PlatformSurface) {
			auto *surface = static_cast<QPlatformSurfaceEvent *>(event);
			if (surface->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
				destroyDisplay();
			}
		}
		return QWidget::eventFilter(object, event);
	}

private:
	QSize pixelSize() const
	{
		qreal ratio = devicePixelRatioF();
		return QSize((int)(width() * ratio), (int)(height() * ratio));
	}

	void createDisplay()
	{
		if (display || !windowHandle() || !windowHandle()->isExposed()) {
			return;
		}
		QSize size = pixelSize();
		gs_init_data info = {};
		info.cx = (uint32_t)std::max(size.width(), 1);
		info.cy = (uint32_t)std::max(size.height(), 1);
		info.format = GS_BGRA;
		info.zsformat = GS_ZS_NONE;
#ifdef _WIN32
		info.window.hwnd = (void *)windowHandle()->winId();
#else
		/* Mac e Linux: a janela de vídeo ainda não foi feita para lá (o Projeto Live é para Windows). */
		return;
#endif
		display = obs_display_create(&info, 0xFF202020);
		if (display) {
			obs_display_add_draw_callback(display, drawVertical, nullptr);
		}
	}

	void resizeDisplay()
	{
		if (display && isVisible()) {
			QSize size = pixelSize();
			obs_display_resize(display, (uint32_t)std::max(size.width(), 1),
					   (uint32_t)std::max(size.height(), 1));
		}
	}

	void destroyDisplay()
	{
		if (display) {
			obs_display_remove_draw_callback(display, drawVertical, nullptr);
			obs_display_destroy(display);
			display = nullptr;
		}
	}

	obs_display_t *display = nullptr;
};

/* ------------------------------------------------------------------------- */
/* Painel "Programa vertical"                                                 */

class ProgramDock : public QWidget {
public:
	explicit ProgramDock(QWidget *parent) : QWidget(parent)
	{
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(4, 4, 4, 4);

		layout->addWidget(new VerticalDisplay(this), 1);

		status = new QLabel(this);
		status->setWordWrap(true);
		status->setTextFormat(Qt::RichText);
		layout->addWidget(status);

		follow = new QPushButton(moduleText("Vertical.Follow"), this);
		connect(follow, &QPushButton::clicked, this, []() { pl_vertical_follow_program(); });
		layout->addWidget(follow);

		refresh();
	}

	void refresh()
	{
		obs_source_t *current = pl_vertical_current();
		QString name = current ? QString::fromUtf8(obs_source_get_name(current)).toHtmlEscaped()
				       : moduleText("Vertical.Nothing");
		obs_source_release(current);

		bool following = pl_vertical_following();
		status->setText(
			moduleText(following ? "Vertical.Status.Following" : "Vertical.Status.Manual").arg(name));
		follow->setEnabled(!following);
	}

private:
	QLabel *status = nullptr;
	QPushButton *follow = nullptr;
};

/* ------------------------------------------------------------------------- */
/* Painel "Cenas verticais"                                                   */

struct SceneEntry {
	obs_weak_source_t *weak = nullptr;
	bool twin = false;
};

class ScenesDock : public QWidget {
public:
	explicit ScenesDock(QWidget *parent) : QWidget(parent)
	{
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(4, 4, 4, 4);

		list = new QListWidget(this);
		layout->addWidget(list, 1);
		connect(list, &QListWidget::currentRowChanged, this, [this](int row) {
			if (!rebuilding) {
				chooseRow(row);
			}
		});
		connect(list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) { putOnAir(); });

		auto *help = new QLabel(moduleText("Vertical.Scenes.Help"), this);
		help->setWordWrap(true);
		help->setStyleSheet("color: #8a8a8a;");
		layout->addWidget(help);

		auto *row1 = new QHBoxLayout();
		auto *onAir = new QPushButton(moduleText("Vertical.OnAir"), this);
		auto *follow = new QPushButton(moduleText("Vertical.Follow"), this);
		connect(onAir, &QPushButton::clicked, this, [this]() { putOnAir(); });
		connect(follow, &QPushButton::clicked, this, []() { pl_vertical_follow_program(); });
		row1->addWidget(onAir);
		row1->addWidget(follow);
		layout->addLayout(row1);

		auto *row2 = new QHBoxLayout();
		auto *add = new QPushButton(moduleText("Vertical.AddExtra"), this);
		remove = new QPushButton(moduleText("Vertical.RemoveExtra"), this);
		connect(add, &QPushButton::clicked, this, [this]() { addExtra(); });
		connect(remove, &QPushButton::clicked, this, [this]() { removeExtra(); });
		row2->addWidget(add);
		row2->addWidget(remove);
		layout->addLayout(row2);
	}

	void rebuild()
	{
		rebuilding = true;
		obs_source_t *chosen = editingScene();
		obs_source_t *current = pl_vertical_current();

		clearEntries();
		list->clear();
		struct Collect {
			ScenesDock *self;
			obs_source_t *chosen;
			obs_source_t *current;
			int chosenRow;
		} collect = {this, chosen, current, -1};

		pl_vertical_enum_scenes(
			[](void *param, obs_source_t *scene, const char *twinOf) -> bool {
				auto *c = static_cast<Collect *>(param);
				QString text = twinOf ? QString::fromUtf8("⇄ ") + QString::fromUtf8(twinOf)
						      : QString::fromUtf8(obs_source_get_name(scene));
				auto *item = new QListWidgetItem(text);
				if (scene == c->current) {
					QFont font = item->font();
					font.setBold(true);
					item->setFont(font);
					item->setText(text + "  ●");
					item->setForeground(QColor(229, 72, 77));
				}
				c->self->list->addItem(item);
				SceneEntry entry;
				entry.weak = obs_source_get_weak_source(scene);
				entry.twin = twinOf != nullptr;
				c->self->entries.push_back(entry);
				if (scene == c->chosen) {
					c->chosenRow = (int)c->self->entries.size() - 1;
				}
				return true;
			},
			&collect);

		obs_source_release(current);
		obs_source_release(chosen);

		if (collect.chosenRow < 0 && !entries.empty()) {
			collect.chosenRow = 0;
			obs_source_t *first = obs_weak_source_get_source(entries[0].weak);
			obs_weak_source_release(editing);
			editing = first ? obs_source_get_weak_source(first) : nullptr;
			obs_source_release(first);
		}
		list->setCurrentRow(collect.chosenRow);
		updateButtons();
		rebuilding = false;
	}

	~ScenesDock() override { clearEntries(); }

private:
	void clearEntries()
	{
		for (SceneEntry &entry : entries) {
			obs_weak_source_release(entry.weak);
		}
		entries.clear();
	}

	obs_source_t *sceneAt(int row)
	{
		if (row < 0 || row >= (int)entries.size()) {
			return nullptr;
		}
		return obs_weak_source_get_source(entries[row].weak);
	}

	void chooseRow(int row)
	{
		obs_source_t *scene = sceneAt(row);
		setEditingScene(scene);
		obs_source_release(scene);
		updateButtons();
	}

	void updateButtons()
	{
		int row = list->currentRow();
		remove->setEnabled(row >= 0 && row < (int)entries.size() && !entries[row].twin);
	}

	void putOnAir()
	{
		obs_source_t *scene = sceneAt(list->currentRow());
		pl_vertical_select(scene);
		obs_source_release(scene);
	}

	void addExtra()
	{
		bool ok = false;
		QString name = QInputDialog::getText(this, moduleText("Vertical.AddExtra"),
						     moduleText("Vertical.AddExtra.Name"), QLineEdit::Normal,
						     moduleText("Vertical.AddExtra.Default"), &ok);
		if (!ok) {
			return;
		}
		QByteArray bytes = name.trimmed().toUtf8();
		obs_source_t *scene = pl_vertical_add_extra(bytes.constData());
		setEditingScene(scene);
		obs_source_release(scene);
	}

	void removeExtra()
	{
		obs_source_t *scene = sceneAt(list->currentRow());
		if (!scene) {
			return;
		}
		QString question =
			moduleText("Vertical.RemoveExtra.Question").arg(QString::fromUtf8(obs_source_get_name(scene)));
		if (QMessageBox::question(this, moduleText("Vertical.RemoveExtra"), question) == QMessageBox::Yes) {
			setEditingScene(nullptr);
			pl_vertical_remove_extra(scene);
		}
		obs_source_release(scene);
	}

	QListWidget *list = nullptr;
	QPushButton *remove = nullptr;
	std::vector<SceneEntry> entries;
	bool rebuilding = false;
};

/* ------------------------------------------------------------------------- */
/* Painel "Fontes verticais"                                                  */

class SourcesDock : public QWidget {
public:
	explicit SourcesDock(QWidget *parent) : QWidget(parent)
	{
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(4, 4, 4, 4);

		title = new QLabel(this);
		title->setWordWrap(true);
		title->setTextFormat(Qt::RichText);
		layout->addWidget(title);

		list = new QListWidget(this);
		layout->addWidget(list, 1);
		connect(list, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
			if (rebuilding) {
				return;
			}
			obs_sceneitem_t *sceneItem = itemAt(list->row(item));
			if (sceneItem) {
				obs_sceneitem_set_visible(sceneItem, item->checkState() == Qt::Checked);
				obs_sceneitem_release(sceneItem);
			}
		});

		auto *row1 = new QHBoxLayout();
		auto *add = new QPushButton(moduleText("Vertical.Sources.Add"), this);
		auto *remove = new QPushButton(moduleText("Vertical.Sources.Remove"), this);
		auto *up = new QPushButton(QString::fromUtf8("↑"), this);
		auto *down = new QPushButton(QString::fromUtf8("↓"), this);
		up->setToolTip(moduleText("Vertical.Sources.Up"));
		down->setToolTip(moduleText("Vertical.Sources.Down"));
		connect(add, &QPushButton::clicked, this, [this]() { addSource(); });
		connect(remove, &QPushButton::clicked, this, [this]() { removeSource(); });
		connect(up, &QPushButton::clicked, this, [this]() { move(OBS_ORDER_MOVE_UP); });
		connect(down, &QPushButton::clicked, this, [this]() { move(OBS_ORDER_MOVE_DOWN); });
		row1->addWidget(add);
		row1->addWidget(remove);
		row1->addWidget(up);
		row1->addWidget(down);
		layout->addLayout(row1);

		auto *row2 = new QHBoxLayout();
		auto *fill = new QPushButton(moduleText("Vertical.Fit.Fill"), this);
		auto *whole = new QPushButton(moduleText("Vertical.Fit.Whole"), this);
		auto *width = new QPushButton(moduleText("Vertical.Fit.Width"), this);
		connect(fill, &QPushButton::clicked, this, [this]() { fit(PL_FIT_FILL); });
		connect(whole, &QPushButton::clicked, this, [this]() { fit(PL_FIT_WHOLE); });
		connect(width, &QPushButton::clicked, this, [this]() { fit(PL_FIT_WIDTH); });
		row2->addWidget(fill);
		row2->addWidget(whole);
		row2->addWidget(width);
		layout->addLayout(row2);
	}

	void rebuild()
	{
		rebuilding = true;
		int previousRow = list->currentRow();
		list->clear();
		ids.clear();

		obs_source_t *sceneSource = editingScene();
		obs_scene_t *scene = obs_scene_from_source(sceneSource);
		if (!scene) {
			title->setText(moduleText("Vertical.Sources.NoScene"));
		} else {
			title->setText(
				moduleText("Vertical.Sources.Title")
					.arg(QString::fromUtf8(obs_source_get_name(sceneSource)).toHtmlEscaped()));

			/* O OBS lista de baixo para cima; o painel mostra o que fica por cima primeiro. */
			std::vector<obs_sceneitem_t *> items;
			obs_scene_enum_items(
				scene,
				[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
					static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
					return true;
				},
				&items);
			std::reverse(items.begin(), items.end());
			for (obs_sceneitem_t *item : items) {
				obs_source_t *source = obs_sceneitem_get_source(item);
				auto *row = new QListWidgetItem(QString::fromUtf8(obs_source_get_name(source)));
				row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
				row->setCheckState(obs_sceneitem_visible(item) ? Qt::Checked : Qt::Unchecked);
				list->addItem(row);
				ids.push_back(obs_sceneitem_get_id(item));
			}
		}
		obs_source_release(sceneSource);

		if (previousRow >= list->count()) {
			previousRow = list->count() - 1;
		}
		list->setCurrentRow(previousRow);
		rebuilding = false;
	}

private:
	/* Item da cena escolhida na linha `row` (nova referência). */
	obs_sceneitem_t *itemAt(int row)
	{
		if (row < 0 || row >= (int)ids.size()) {
			return nullptr;
		}
		obs_source_t *sceneSource = editingScene();
		obs_scene_t *scene = obs_scene_from_source(sceneSource);
		obs_sceneitem_t *item = scene ? obs_scene_find_sceneitem_by_id(scene, ids[row]) : nullptr;
		if (item) {
			obs_sceneitem_addref(item);
		}
		obs_source_release(sceneSource);
		return item;
	}

	void addSource()
	{
		obs_source_t *sceneSource = editingScene();
		if (!sceneSource) {
			return;
		}

		/* Fontes do OBS (as mesmas da tela horizontal) e as cenas horizontais. */
		QStringList names;
		obs_enum_sources(
			[](void *param, obs_source_t *source) -> bool {
				static_cast<QStringList *>(param)->append(
					QString::fromUtf8(obs_source_get_name(source)));
				return true;
			},
			&names);
		names.sort(Qt::CaseInsensitive);

		bool ok = false;
		QString chosen = QInputDialog::getItem(this, moduleText("Vertical.Sources.Add"),
						       moduleText("Vertical.Sources.Choose"), names, 0, false, &ok);
		if (ok && !chosen.isEmpty()) {
			QByteArray bytes = chosen.toUtf8();
			obs_source_t *source = obs_get_source_by_name(bytes.constData());
			pl_vertical_add_source(sceneSource, source);
			obs_source_release(source);
			pl_vertical_touch();
		}
		obs_source_release(sceneSource);
	}

	void removeSource()
	{
		obs_sceneitem_t *item = itemAt(list->currentRow());
		if (item) {
			obs_sceneitem_remove(item);
			obs_sceneitem_release(item);
			pl_vertical_touch();
		}
	}

	void move(enum obs_order_movement movement)
	{
		obs_sceneitem_t *item = itemAt(list->currentRow());
		if (item) {
			obs_sceneitem_set_order(item, movement);
			obs_sceneitem_release(item);
			int row = list->currentRow() + (movement == OBS_ORDER_MOVE_UP ? -1 : 1);
			pl_vertical_touch();
			rebuild();
			list->setCurrentRow(std::clamp(row, 0, std::max(list->count() - 1, 0)));
		}
	}

	void fit(enum pl_vertical_fit how)
	{
		obs_sceneitem_t *item = itemAt(list->currentRow());
		if (item) {
			pl_vertical_fit_item(item, how);
			obs_sceneitem_release(item);
		}
	}

	QLabel *title = nullptr;
	QListWidget *list = nullptr;
	std::vector<int64_t> ids;
	bool rebuilding = false;
};

/* ------------------------------------------------------------------------- */
/* Quem atualiza os painéis                                                   */

ProgramDock *programDock = nullptr;
ScenesDock *scenesDock = nullptr;
SourcesDock *sourcesDock = nullptr;
QTimer *timer = nullptr;
uint64_t seenVersion = (uint64_t)-1;

void refreshDocks()
{
	uint64_t now = pl_vertical_version();
	if (programDock) {
		programDock->refresh();
	}
	if (now == seenVersion) {
		return;
	}
	seenVersion = now;
	if (scenesDock) {
		scenesDock->rebuild();
	}
	if (sourcesDock) {
		sourcesDock->rebuild();
	}
}

void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP) {
		setEditingScene(nullptr);
	}
	if (event == OBS_FRONTEND_EVENT_EXIT && timer) {
		timer->stop();
	}
}

template<typename Dock> Dock *addDock(QWidget *mainWindow, const char *id, const char *titleKey)
{
	auto *dock = new Dock(mainWindow);
	if (!obs_frontend_add_dock_by_id(id, obs_module_text(titleKey), dock)) {
		delete dock;
		return nullptr;
	}
	return dock;
}

} // namespace

void pl_vertical_dock_load(void)
{
	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (!mainWindow) {
		return;
	}

	programDock = addDock<ProgramDock>(mainWindow, "projeto-live-vertical-programa", "Vertical.Program.Title");
	scenesDock = addDock<ScenesDock>(mainWindow, "projeto-live-vertical-cenas", "Vertical.Scenes.Title");
	sourcesDock = addDock<SourcesDock>(mainWindow, "projeto-live-vertical-fontes", "Vertical.Sources.Title.Dock");
	if (programDock) {
		QObject::connect(programDock, &QObject::destroyed, []() { programDock = nullptr; });
	}
	if (scenesDock) {
		QObject::connect(scenesDock, &QObject::destroyed, []() { scenesDock = nullptr; });
	}
	if (sourcesDock) {
		QObject::connect(sourcesDock, &QObject::destroyed, []() { sourcesDock = nullptr; });
	}

	timer = new QTimer(mainWindow);
	QObject::connect(timer, &QTimer::timeout, []() { refreshDocks(); });
	timer->start(300);
	obs_frontend_add_event_callback(frontendEvent, nullptr);
}
