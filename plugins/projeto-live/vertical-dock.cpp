/*
 * Projeto Live: painéis da tela vertical.
 *
 * - "Programa vertical": mostra o que está saindo em pé, se o vertical está seguindo o Programa
 *   ou numa cena escolhida à mão, e o botão "Seguir o Programa".
 * - "Cenas verticais": as gêmeas (⇄, na ordem das cenas do OBS) e as cenas só verticais. Um clique
 *   escolhe a cena para mexer nas fontes; dois cliques (ou "Pôr no ar") põem a cena no vertical.
 * - "Fontes verticais": as fontes da cena escolhida, com a caixa de aparecer/sumir, a ordem, os
 *   enquadramentos rápidos (preencher, inteira, largura toda) e "Casar fontes".
 * - Nas duas janelas de vídeo do vertical dá para mexer com o mouse, como no preview do OBS: um
 *   clique escolhe a fonte (moldura vermelha), arrastar move (gruda nas bordas e no meio) e os
 *   cantos redimensionam sem deformar. Mexe na cena que está no ar no vertical.
 * - Ao lado do preview do OBS (na mesma fileira, depois do Programa no modo estúdio): uma cópia do
 *   Programa vertical, que dá para desligar no painel "Programa vertical".
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

#include <QBoxLayout>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QListWidget>
#include <QMessageBox>
#include <QPlatformSurfaceEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <graphics/matrix4.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

#include "vertical-record.h"
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
/* Fonte escolhida com o mouse (a janela de vídeo desenha na thread de vídeo)  */

std::mutex selectionMutex;
obs_weak_source_t *selectedScene = nullptr;
int64_t selectedId = 0;

void setSelection(obs_source_t *scene, int64_t id)
{
	std::lock_guard<std::mutex> lock(selectionMutex);
	obs_weak_source_release(selectedScene);
	selectedScene = scene ? obs_source_get_weak_source(scene) : nullptr;
	selectedId = scene ? id : 0;
}

/* Item escolhido, se ele estiver na cena `scene` (nova referência ou nullptr). */
obs_sceneitem_t *selectedItemIn(obs_source_t *scene)
{
	int64_t id = 0;
	bool same = false;
	{
		std::lock_guard<std::mutex> lock(selectionMutex);
		obs_source_t *chosen = selectedScene ? obs_weak_source_get_source(selectedScene) : nullptr;
		same = chosen && chosen == scene;
		id = selectedId;
		obs_source_release(chosen);
	}
	obs_scene_t *sceneData = same ? obs_scene_from_source(scene) : nullptr;
	obs_sceneitem_t *item = sceneData ? obs_scene_find_sceneitem_by_id(sceneData, id) : nullptr;
	if (item) {
		obs_sceneitem_addref(item);
	}
	return item;
}

int64_t selectedItemId()
{
	std::lock_guard<std::mutex> lock(selectionMutex);
	return selectedId;
}

/* Cantos do item na tela vertical: 0 alto-esquerda, 1 alto-direita, 2 baixo-direita, 3 baixo-esquerda. */
void itemCorners(obs_sceneitem_t *item, vec3 corners[4])
{
	matrix4 transform;
	obs_sceneitem_get_box_transform(item, &transform);
	const float unit[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
	for (int i = 0; i < 4; i++) {
		vec3_set(&corners[i], unit[i][0], unit[i][1], 0.0f);
		vec3_transform(&corners[i], &corners[i], &transform);
	}
}

struct Box {
	float left, top, right, bottom;
	float width() const { return right - left; }
	float height() const { return bottom - top; }
};

Box itemBox(obs_sceneitem_t *item)
{
	vec3 corners[4];
	itemCorners(item, corners);
	Box box = {corners[0].x, corners[0].y, corners[0].x, corners[0].y};
	for (const vec3 &corner : corners) {
		box.left = std::min(box.left, corner.x);
		box.top = std::min(box.top, corner.y);
		box.right = std::max(box.right, corner.x);
		box.bottom = std::max(box.bottom, corner.y);
	}
	return box;
}

/* Moldura vermelha e quadradinhos nos cantos da fonte escolhida. */
void drawSelection(obs_canvas_t *canvas, float pixelsPerUnit)
{
	obs_source_t *current = obs_canvas_get_channel(canvas, 0);
	obs_sceneitem_t *item = current ? selectedItemIn(current) : nullptr;
	obs_source_release(current);
	if (!item) {
		return;
	}
	vec3 corners[4];
	itemCorners(item, corners);
	obs_sceneitem_release(item);

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	vec4 red;
	vec4_set(&red, 0.9f, 0.2f, 0.2f, 1.0f);
	gs_effect_set_vec4(color, &red);
	float half = 5.0f / pixelsPerUnit;

	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(true);
		for (int i = 0; i <= 4; i++) {
			gs_vertex2f(corners[i % 4].x, corners[i % 4].y);
		}
		gs_render_stop(GS_LINESTRIP);

		for (const vec3 &corner : corners) {
			gs_render_start(true);
			gs_vertex2f(corner.x - half, corner.y - half);
			gs_vertex2f(corner.x + half, corner.y - half);
			gs_vertex2f(corner.x - half, corner.y + half);
			gs_vertex2f(corner.x + half, corner.y + half);
			gs_render_stop(GS_TRISTRIP);
		}
	}
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
	drawSelection(canvas, scale);
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
		setMouseTracking(true);

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
	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() != Qt::LeftButton) {
			return;
		}
		float x, y;
		toCanvas(event->position(), x, y);
		obs_source_t *current = pl_vertical_current();
		obs_scene_t *scene = obs_scene_from_source(current);
		mode = Mode::None;

		/* Primeiro: um canto da fonte já escolhida? */
		obs_sceneitem_t *item = current ? selectedItemIn(current) : nullptr;
		int corner = item ? cornerAt(item, x, y) : -1;
		if (item && corner < 0) {
			obs_sceneitem_release(item);
			item = nullptr;
		}
		/* Senão: a fonte mais de cima embaixo do mouse. */
		if (!item && scene) {
			item = itemAt(scene, x, y);
		}

		if (item && !obs_sceneitem_locked(item)) {
			setSelection(current, obs_sceneitem_get_id(item));
			startBox = itemBox(item);
			startMouseX = x;
			startMouseY = y;
			obs_sceneitem_get_pos(item, &startPos);
			obs_sceneitem_get_scale(item, &startScale);
			activeCorner = corner;
			mode = corner >= 0 ? Mode::Resize : Mode::Move;
		} else {
			setSelection(item ? current : nullptr, item ? obs_sceneitem_get_id(item) : 0);
		}
		obs_sceneitem_release(item);

		obs_source_t *editingNow = editingScene();
		if (current && editingNow != current) {
			setEditingScene(current);
		}
		obs_source_release(editingNow);
		obs_source_release(current);
		pl_vertical_touch();
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		float x, y;
		toCanvas(event->position(), x, y);
		obs_source_t *current = pl_vertical_current();
		obs_sceneitem_t *item = current ? selectedItemIn(current) : nullptr;
		obs_source_release(current);

		if (!item) {
			unsetCursor();
			return;
		}
		if (mode == Mode::Move) {
			moveItem(item, x - startMouseX, y - startMouseY);
		} else if (mode == Mode::Resize) {
			resizeItem(item, x, y);
		} else {
			int corner = cornerAt(item, x, y);
			if (corner == 0 || corner == 2) {
				setCursor(Qt::SizeFDiagCursor);
			} else if (corner == 1 || corner == 3) {
				setCursor(Qt::SizeBDiagCursor);
			} else {
				unsetCursor();
			}
		}
		obs_sceneitem_release(item);
	}

	void mouseReleaseEvent(QMouseEvent *) override
	{
		if (mode != Mode::None) {
			mode = Mode::None;
			pl_vertical_touch();
		}
	}

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
	enum class Mode { None, Move, Resize };

	/* Quanto da tela vertical cabe em cada pixel da janela, e onde ela começa. */
	void viewMap(float &scale, float &offsetX, float &offsetY) const
	{
		QSize size = pixelSize();
		float baseW = PL_VERTICAL_WIDTH;
		float baseH = PL_VERTICAL_HEIGHT;
		scale = std::min((float)size.width() / baseW, (float)size.height() / baseH);
		if (scale <= 0.0f) {
			scale = 1.0f;
		}
		offsetX = ((float)size.width() - baseW * scale) / 2.0f;
		offsetY = ((float)size.height() - baseH * scale) / 2.0f;
	}

	void toCanvas(const QPointF &point, float &x, float &y) const
	{
		float scale, offsetX, offsetY;
		viewMap(scale, offsetX, offsetY);
		qreal ratio = devicePixelRatioF();
		x = ((float)(point.x() * ratio) - offsetX) / scale;
		y = ((float)(point.y() * ratio) - offsetY) / scale;
	}

	/* Distância (em unidades da tela vertical) que conta como "em cima" de um canto ou borda. */
	float grabDistance() const
	{
		float scale, offsetX, offsetY;
		viewMap(scale, offsetX, offsetY);
		return 9.0f * (float)devicePixelRatioF() / scale;
	}

	int cornerAt(obs_sceneitem_t *item, float x, float y) const
	{
		vec3 corners[4];
		itemCorners(item, corners);
		float grab = grabDistance();
		for (int i = 0; i < 4; i++) {
			if (std::fabs(corners[i].x - x) <= grab && std::fabs(corners[i].y - y) <= grab) {
				return i;
			}
		}
		return -1;
	}

	/* A fonte visível mais de cima no ponto (nova referência ou nullptr). */
	obs_sceneitem_t *itemAt(obs_scene_t *scene, float x, float y) const
	{
		struct Hit {
			float x, y;
			obs_sceneitem_t *found;
		} hit = {x, y, nullptr};
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
				auto *h = static_cast<Hit *>(param);
				if (!obs_sceneitem_visible(item)) {
					return true;
				}
				matrix4 transform, inverse;
				obs_sceneitem_get_box_transform(item, &transform);
				if (!matrix4_inv(&inverse, &transform)) {
					return true;
				}
				vec3 point;
				vec3_set(&point, h->x, h->y, 0.0f);
				vec3_transform(&point, &point, &inverse);
				if (point.x >= 0.0f && point.x <= 1.0f && point.y >= 0.0f && point.y <= 1.0f) {
					/* A lista vai de baixo para cima: o último que acertar é o de cima. */
					h->found = item;
				}
				return true;
			},
			&hit);
		if (hit.found) {
			obs_sceneitem_addref(hit.found);
		}
		return hit.found;
	}

	/* Move, grudando as bordas e o meio da fonte nas bordas e no meio da tela. */
	void moveItem(obs_sceneitem_t *item, float dx, float dy)
	{
		float grab = grabDistance() * 1.5f;
		auto snap = [grab](float start, float size, float delta, float limit) {
			const float targets[] = {0.0f, limit / 2.0f, limit};
			const float edges[] = {0.0f, size / 2.0f, size};
			float best = delta;
			float bestDistance = grab;
			for (float edge : edges) {
				for (float target : targets) {
					float wanted = target - (start + edge);
					if (std::fabs(wanted - delta) < bestDistance) {
						bestDistance = std::fabs(wanted - delta);
						best = wanted;
					}
				}
			}
			return best;
		};
		dx = snap(startBox.left, startBox.width(), dx, (float)PL_VERTICAL_WIDTH);
		dy = snap(startBox.top, startBox.height(), dy, (float)PL_VERTICAL_HEIGHT);

		vec2 pos;
		vec2_set(&pos, startPos.x + dx, startPos.y + dy);
		obs_sceneitem_set_pos(item, &pos);
	}

	/* Redimensiona pelo canto, sem deformar; o canto oposto fica parado. */
	void resizeItem(obs_sceneitem_t *item, float x, float y)
	{
		bool right = activeCorner == 1 || activeCorner == 2;
		bool bottom = activeCorner == 2 || activeCorner == 3;
		float anchorX = right ? startBox.left : startBox.right;
		float anchorY = bottom ? startBox.top : startBox.bottom;
		float w0 = std::max(startBox.width(), 1.0f);
		float h0 = std::max(startBox.height(), 1.0f);

		float factor = std::max(std::fabs(x - anchorX) / w0, std::fabs(y - anchorY) / h0);
		factor = std::max(factor, 16.0f / std::min(w0, h0));
		float w = w0 * factor;
		float h = h0 * factor;
		float left = right ? anchorX : anchorX - w;
		float top = bottom ? anchorY : anchorY - h;

		obs_sceneitem_defer_update_begin(item);
		obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
		vec2 pos;
		vec2_set(&pos, left, top);
		obs_sceneitem_set_pos(item, &pos);
		if (obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE) {
			vec2 bounds;
			vec2_set(&bounds, w, h);
			obs_sceneitem_set_bounds(item, &bounds);
		} else {
			vec2 scale;
			vec2_set(&scale, startScale.x * factor, startScale.y * factor);
			obs_sceneitem_set_scale(item, &scale);
		}
		obs_sceneitem_defer_update_end(item);
	}

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
	Mode mode = Mode::None;
	Box startBox = {0.0f, 0.0f, 0.0f, 0.0f};
	vec2 startPos = {};
	vec2 startScale = {};
	float startMouseX = 0.0f;
	float startMouseY = 0.0f;
	int activeCorner = -1;
};

void placeEmbedded();

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

		auto *embed = new QCheckBox(moduleText("Vertical.Embed"), this);
		embed->setChecked(pl_vertical_pref("embed"));
		connect(embed, &QCheckBox::toggled, this, [](bool on) {
			pl_vertical_set_pref("embed", on);
			placeEmbedded();
		});
		layout->addWidget(embed);

		auto *record = new QCheckBox(moduleText("Vertical.Record"), this);
		record->setChecked(pl_vertical_pref("record"));
		connect(record, &QCheckBox::toggled, this, [](bool on) { pl_vertical_set_pref("record", on); });
		layout->addWidget(record);

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
/* Janela "Casar fontes"                                                      */

QStringList allSourceNames()
{
	QStringList names;
	obs_enum_sources(
		[](void *param, obs_source_t *source) -> bool {
			static_cast<QStringList *>(param)->append(QString::fromUtf8(obs_source_get_name(source)));
			return true;
		},
		&names);
	names.sort(Qt::CaseInsensitive);
	return names;
}

class PairDialog : public QDialog {
public:
	PairDialog(QWidget *parent, const QString &start) : QDialog(parent)
	{
		setWindowTitle(moduleText("Vertical.Pair.Title"));
		setMinimumWidth(420);
		auto *layout = new QVBoxLayout(this);

		auto *help = new QLabel(moduleText("Vertical.Pair.Help"), this);
		help->setWordWrap(true);
		layout->addWidget(help);

		QStringList names = allSourceNames();
		auto *form = new QFormLayout();
		source = new QComboBox(this);
		source->addItems(names);
		pair = new QComboBox(this);
		pair->addItem(moduleText("Vertical.Pair.Same"), QString());
		pair->addItem(moduleText("Vertical.Pair.Hide"), QString::fromUtf8("\x01"));
		for (const QString &name : names) {
			pair->addItem(name, name);
		}
		form->addRow(moduleText("Vertical.Pair.Source"), source);
		form->addRow(moduleText("Vertical.Pair.Shows"), pair);
		layout->addLayout(form);

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		layout->addWidget(buttons);

		connect(source, &QComboBox::currentIndexChanged, this, [this](int) { showCurrentPair(); });
		int index = source->findText(start);
		source->setCurrentIndex(index < 0 ? 0 : index);
		showCurrentPair();
	}

	void apply()
	{
		QByteArray name = source->currentText().toUtf8();
		obs_source_t *chosen = obs_get_source_by_name(name.constData());
		if (!chosen) {
			return;
		}
		QString data = pair->currentData().toString();
		if (data.isEmpty()) {
			pl_vertical_set_pair(chosen, PL_PAIR_SAME, nullptr);
		} else if (data == QString::fromUtf8("\x01")) {
			pl_vertical_set_pair(chosen, PL_PAIR_HIDE, nullptr);
		} else {
			QByteArray otherName = data.toUtf8();
			obs_source_t *other = obs_get_source_by_name(otherName.constData());
			if (other && other != chosen) {
				pl_vertical_set_pair(chosen, PL_PAIR_OTHER, other);
			} else {
				pl_vertical_set_pair(chosen, PL_PAIR_SAME, nullptr);
			}
			obs_source_release(other);
		}
		obs_source_release(chosen);
	}

private:
	void showCurrentPair()
	{
		QByteArray name = source->currentText().toUtf8();
		obs_source_t *chosen = obs_get_source_by_name(name.constData());
		obs_source_t *other = nullptr;
		enum pl_vertical_pair_mode mode = pl_vertical_get_pair(chosen, &other);
		int index = 0;
		if (mode == PL_PAIR_HIDE) {
			index = 1;
		} else if (mode == PL_PAIR_OTHER && other) {
			index = pair->findData(QString::fromUtf8(obs_source_get_name(other)));
		}
		pair->setCurrentIndex(index < 0 ? 0 : index);
		obs_source_release(other);
		obs_source_release(chosen);
	}

	QComboBox *source = nullptr;
	QComboBox *pair = nullptr;
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
		connect(list, &QListWidget::currentRowChanged, this, [this](int row) {
			if (rebuilding || row < 0 || row >= (int)ids.size()) {
				return;
			}
			/* Escolher na lista também mostra a moldura vermelha no vídeo. */
			obs_source_t *sceneSource = editingScene();
			setSelection(sceneSource, ids[row]);
			obs_source_release(sceneSource);
		});
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

		auto *pairButton = new QPushButton(moduleText("Vertical.Pair.Button"), this);
		connect(pairButton, &QPushButton::clicked, this, [this]() { pairSources(); });
		layout->addWidget(pairButton);
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
		/* A fonte escolhida com o mouse fica marcada na lista. */
		obs_sceneitem_t *selected = sceneSource ? selectedItemIn(sceneSource) : nullptr;
		if (selected) {
			auto found = std::find(ids.begin(), ids.end(), obs_sceneitem_get_id(selected));
			if (found != ids.end()) {
				previousRow = (int)(found - ids.begin());
			}
			obs_sceneitem_release(selected);
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

	void pairSources()
	{
		QString start;
		obs_sceneitem_t *item = itemAt(list->currentRow());
		if (item) {
			start = QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(item)));
			obs_sceneitem_release(item);
		}
		PairDialog dialog(this, start);
		if (dialog.exec() == QDialog::Accepted) {
			dialog.apply();
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

/* ------------------------------------------------------------------------- */
/* Programa vertical ao lado do preview                                       */

class EmbeddedVertical : public QWidget {
public:
	explicit EmbeddedVertical(QWidget *parent) : QWidget(parent)
	{
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(4, 0, 0, 0);
		layout->setSpacing(2);
		label = new QLabel(moduleText("Vertical.Embedded.Label"), this);
		label->setStyleSheet("color: #8a8a8a;");
		layout->addWidget(label);
		layout->addWidget(new VerticalDisplay(this), 1);
		parent->installEventFilter(this);
		fitWidth();
	}

	/* Largura certa para a tela em pé caber na altura da área do preview. */
	void fitWidth()
	{
		QWidget *area = parentWidget();
		int height = (area ? area->height() : 400) - label->sizeHint().height() - 2;
		setFixedWidth(std::max(90, height * 9 / 16 + 4));
	}

protected:
	bool eventFilter(QObject *object, QEvent *event) override
	{
		if (object == parentWidget() && event->type() == QEvent::Resize) {
			fitWidth();
		}
		return QWidget::eventFilter(object, event);
	}

private:
	QLabel *label = nullptr;
};

EmbeddedVertical *embedded = nullptr;

QBoxLayout *previewRow()
{
	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	QWidget *area = mainWindow ? mainWindow->findChild<QWidget *>("canvasEditor") : nullptr;
	return area ? qobject_cast<QBoxLayout *>(area->layout()) : nullptr;
}

void placeEmbedded()
{
	QBoxLayout *row = previewRow();
	bool wanted = pl_vertical_pref("embed") && row;

	if (!wanted && embedded) {
		if (row) {
			row->removeWidget(embedded);
		}
		delete embedded;
		embedded = nullptr;
		return;
	}
	if (!wanted) {
		return;
	}
	if (!embedded) {
		embedded = new EmbeddedVertical(row->parentWidget());
		QObject::connect(embedded, &QObject::destroyed, []() { embedded = nullptr; });
	} else {
		row->removeWidget(embedded);
	}
	/* Sempre o último da fileira (no modo estúdio o OBS acrescenta o Programa depois). */
	row->addWidget(embedded);
	embedded->show();
}

void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING || event == OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED ||
	    event == OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED) {
		QTimer::singleShot(0, []() { placeEmbedded(); });
	}
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
