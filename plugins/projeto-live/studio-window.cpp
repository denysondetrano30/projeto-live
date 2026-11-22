/*
 * Projeto Live: a janela própria do Projeto Live.
 *
 * Dois modos, um clique:
 * - modo somun live: esta janela, a tela "Operar" do monitor 1, com a cara do somun. No topo, o
 *   logo "live.", o culto, Gravar/Gravando, Transmitir/No ar · tempo e o "⋯" (Acompanhar, tecla de
 *   falar, transições, modo estúdio, configurações, Modo OBS e os menus do OBS); no meio, Prévia
 *   (moldura branca), cortar e a transição, No ar (moldura vermelha) e as duas telas em pé
 *   pequenas; embaixo, Entradas e Som; à direita, Equipe e Situação completa. As fontes da entrada
 *   abrem numa janelinha pelo botão direito. No monitor 2 fica o "Acompanhar" (watch-window.cpp).
 *   A janela do OBS fica escondida. Os painéis são cópias dos mesmos painéis do OBS (mesmo código);
 * - modo OBS: a janela original do OBS, como sempre foi. Esta fica escondida.
 *
 * O OBS lembra o último modo usado (modo.json) e abre nele.
 *
 * Prévia e Programa são desenhados aqui mesmo (o OBS deixa desenhar uma cena em qualquer janela):
 * a Prévia desenha a cena da prévia; o Programa desenha a saída do OBS, a mesma que vai para a live,
 * com a transição no meio.
 *
 * O modo Projeto Live liga o modo estúdio do OBS (prévia antes de cortar).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "studio-window.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

#include <util/platform.h>
#include <graphics/matrix4.h>
#include <graphics/vec3.h>

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QCloseEvent>
#include <QColor>
#include <QDialog>
#include <QIcon>
#include <QPainter>
#include <QPen>
#include <QRect>
#include <QRectF>
#include <QPixmap>
#include <QVariant>
#include <QContextMenuEvent>
#include <QListWidget>
#include <QMainWindow>
#include <QMenuBar>
#include <QStackedWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QStringList>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMetaObject>
#include <QPaintEvent>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "artes.h"
#include "audio-dock.h"
#include "camera-dock.h"
#include "color-mode.h"
#include "director-dock.h"
#include "multistream.h"
#include "stream-dock.h"
#include "pl-account.h"
#include "source-row.hpp"
#include "somun-style.h"
#include "stats-dock.h"
#include "talk-key.h"
#include "team-panel.h"
#include "transitions-dialog.h"
#include "vertical-dock.h"
#include "vertical-pan.h"
#include "watch-window.h"

namespace {

const char *const MODE_FILE = "modo.json";
const char *const MODE_LIVE = "projeto-live";
const char *const MODE_OBS = "obs";

QString msg(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QWidget *obsWindow()
{
	return static_cast<QWidget *>(obs_frontend_get_main_window());
}

/* Mostra ou esconde a janela do OBS do mesmo jeito que o ícone da bandeja faz (guarda a posição,
 * esconde as janelinhas filhas e desliga a prévia do OBS enquanto ela está escondida). */
void setObsShowing(bool showing)
{
	if (QWidget *main = obsWindow()) {
		QMetaObject::invokeMethod(main, "SetShowing", Q_ARG(bool, showing));
	}
}

std::string loadMode()
{
	char *path = obs_module_config_path(MODE_FILE);
	obs_data_t *data = path ? obs_data_create_from_json_file_safe(path, "bak") : nullptr;
	bfree(path);
	std::string mode = MODE_OBS;
	if (data) {
		const char *saved = obs_data_get_string(data, "modo");
		if (saved && *saved) {
			mode = saved;
		}
		obs_data_release(data);
	}
	return mode;
}

void saveMode(const char *mode)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "modo", mode);
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path(MODE_FILE);
	if (path) {
		obs_data_save_json_safe(data, path, "tmp", "bak");
		bfree(path);
	}
	obs_data_release(data);
}

/* ------------------------------------------------------------------------- */
/* Uma tela do OBS dentro da nossa janela (igual às miniaturas das Entradas)  */

class PLScreen : public QWidget {
public:
	using DrawFn = void (*)(void *owner, uint32_t cx, uint32_t cy);

	PLScreen(QWidget *parent, DrawFn fn, void *owner) : QWidget(parent), drawFn(fn), owner(owner)
	{
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_StaticContents);
		setAttribute(Qt::WA_NoSystemBackground);
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_DontCreateNativeAncestors);
		setAttribute(Qt::WA_NativeWindow);
		setMinimumSize(160, 90);

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

	~PLScreen() override { destroyDisplay(); }

	QPaintEngine *paintEngine() const override { return nullptr; }

	/* Confere se a tela do OBS tem o tamanho da janela (ao maximizar, ela pode ficar com o tamanho
	 * antigo e a imagem sai torta e encostada num canto). */
	void syncSize()
	{
		if (!display || !isVisible()) {
			return;
		}
		uint32_t cx = 0;
		uint32_t cy = 0;
		obs_display_size(display, &cx, &cy);
		QSize size = pixelSize();
		uint32_t wantX = (uint32_t)std::max(size.width(), 1);
		uint32_t wantY = (uint32_t)std::max(size.height(), 1);
		if (cx != wantX || cy != wantY) {
			obs_display_resize(display, wantX, wantY);
		}
	}

	/* Solta a tela do OBS já (antes de o OBS fechar a parte gráfica). */
	void destroyDisplay()
	{
		if (display) {
			obs_display_remove_draw_callback(display, draw, this);
			obs_display_destroy(display);
			display = nullptr;
		}
	}

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

	void showEvent(QShowEvent *event) override
	{
		QWidget::showEvent(event);
		if (display) {
			obs_display_set_enabled(display, true);
		}
	}

	void hideEvent(QHideEvent *event) override
	{
		QWidget::hideEvent(event);
		if (display) {
			obs_display_set_enabled(display, false);
		}
	}

	bool eventFilter(QObject *object, QEvent *event) override
	{
		if (event->type() == QEvent::PlatformSurface) {
			auto *surface = static_cast<QPlatformSurfaceEvent *>(event);
			if (surface->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
				destroyDisplay();
			}
		}
		return QWidget::eventFilter(object, event);
	}

private:
	static void draw(void *data, uint32_t cx, uint32_t cy)
	{
		auto *self = static_cast<PLScreen *>(data);
		self->drawFn(self->owner, cx, cy);
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
#elif defined(__APPLE__)
		info.window.view = (id)windowHandle()->winId();
#else
		if (obs_get_nix_platform() != OBS_NIX_PLATFORM_X11_EGL) {
			return;
		}
		info.window.id = (uint32_t)windowHandle()->winId();
		info.window.display = obs_get_nix_platform_display();
#endif
		display = obs_display_create(&info, 0xFF141414);
		if (display) {
			obs_display_add_draw_callback(display, draw, this);
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

	DrawFn drawFn = nullptr;
	void *owner = nullptr;
	obs_display_t *display = nullptr;
};

/* Encaixa uma imagem de sw x sh numa tela de cx x cy sem deformar e prepara o desenho. */
bool beginFit(uint32_t sw, uint32_t sh, uint32_t cx, uint32_t cy)
{
	if (!sw || !sh || !cx || !cy) {
		return false;
	}
	float scale = std::min((float)cx / (float)sw, (float)cy / (float)sh);
	int w = std::max((int)std::lround(sw * scale), 1);
	int h = std::max((int)std::lround(sh * scale), 1);
	int x = ((int)cx - w) / 2;
	int y = ((int)cy - h) / 2;

	gs_projection_push();
	gs_viewport_push();
	gs_set_viewport(x, y, w, h);
	gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
	return true;
}

void endFit()
{
	gs_viewport_pop();
	gs_projection_pop();
}

/* ------------------------------------------------------------------------- */

/* A moldura que abraça a imagem: a tela fica sempre na proporção da saída do OBS, centralizada,
 * e a borda colorida é desenhada só em volta dela (sem tarjas cinzas em cima e embaixo). */
class AspectFrame : public QWidget {
public:
	explicit AspectFrame(QWidget *parent) : QWidget(parent) {}

	void setChild(QWidget *widget)
	{
		child = widget;
		relayout();
	}

	void setBorder(const char *color)
	{
		border = QColor(color);
		update();
	}

	/* A saída do OBS pode mudar de tamanho: confere de tempos em tempos (barato). */
	void refit()
	{
		if (std::abs(currentAspect() - aspect) > 0.001) {
			relayout();
		}
	}

protected:
	void resizeEvent(QResizeEvent *event) override
	{
		QWidget::resizeEvent(event);
		relayout();
	}

	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(QPen(border, 2));
		painter.setBrush(Qt::NoBrush);
		painter.drawRoundedRect(QRectF(box).adjusted(1, 1, -1, -1), 6, 6);
	}

private:
	static double currentAspect()
	{
		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi) && ovi.base_width && ovi.base_height) {
			return (double)ovi.base_width / (double)ovi.base_height;
		}
		return 16.0 / 9.0;
	}

	void relayout()
	{
		aspect = currentAspect();
		int w = width();
		int h = height();
		int bw = w;
		int bh = (int)std::lround(w / aspect);
		if (bh > h) {
			bh = h;
			bw = (int)std::lround(h * aspect);
		}
		box = QRect((w - bw) / 2, (h - bh) / 2, std::max(bw, 1), std::max(bh, 1));
		if (child) {
			child->setGeometry(box.adjusted(3, 3, -3, -3));
		}
		update();
	}

	QWidget *child = nullptr;
	QColor border = QColor(SOMUN_LINE2);
	QRect box;
	double aspect = 16.0 / 9.0;
};

/* Uma das duas telas grandes (Prévia ou No ar), com a moldura colorida e o título. */
class Monitor : public QWidget {
public:
	Monitor(QWidget *parent, const QString &title, const char *color, PLScreen::DrawFn fn, void *owner)
		: Monitor(parent, title, color, nullptr, fn, owner)
	{
	}

	/* Com uma tela feita por fora (a Prévia, onde se mexe com o mouse). Ela nasce já dentro da
	 * moldura: trocar de pai uma janela nativa troca a janela e a tela do OBS se perde. */
	using MakeFn = QWidget *(*)(QWidget * parent);
	Monitor(QWidget *parent, const QString &title, const char *color, MakeFn custom, PLScreen::DrawFn fn = nullptr,
		void *owner = nullptr)
		: QWidget(parent)
	{
		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(0, 0, 0, 0);
		outer->setSpacing(4);

		auto *head = new QHBoxLayout();
		label = new QLabel(title, this);
		QFont bold = label->font();
		bold.setBold(true);
		label->setFont(bold);
		head->addWidget(label);
		head->addStretch(1);
		sceneName = new QLabel(this);
		sceneName->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY));
		head->addWidget(sceneName);
		outer->addLayout(head);

		frame = new AspectFrame(this);
		if (custom) {
			view = custom(frame);
		} else {
			screen = new PLScreen(frame, fn, owner);
			view = screen;
		}
		frame->setChild(view);
		setLook(title, color);
		outer->addWidget(frame, 1);
	}

	/* Título e cor da moldura (a Prévia vira "No ar" vermelha no modo normal). */
	void setLook(const QString &title, const char *color)
	{
		frame->refit();
		if (look == title) {
			return;
		}
		look = title;
		label->setText(title);
		label->setStyleSheet(QString("color: %1;").arg(color));
		frame->setBorder(color);
	}

	void setSceneName(const QString &text)
	{
		if (sceneName->text() != text) {
			sceneName->setText(text);
		}
	}

	QWidget *view = nullptr;
	PLScreen *screen = nullptr; /* nullptr quando a tela veio pronta */

private:
	QLabel *sceneName = nullptr;
	QLabel *label = nullptr;
	AspectFrame *frame = nullptr;
	QString look;
};

/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* Coluna "Fontes da entrada": as fontes da entrada que está na prévia.       */

using ItemList = std::vector<obs_sceneitem_t *>;

ItemList sceneItems(obs_source_t *sceneSource)
{
	ItemList items;
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (scene) {
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				obs_sceneitem_addref(item);
				static_cast<ItemList *>(param)->push_back(item);
				return true;
			},
			&items);
	}
	return items;
}

void releaseItems(ItemList &items)
{
	for (obs_sceneitem_t *item : items) {
		obs_sceneitem_release(item);
	}
	items.clear();
}

/* A cena que o painel Fontes do OBS mostraria: a da prévia (modo estúdio) ou a do ar. */
obs_source_t *workingScene()
{
	obs_source_t *scene = obs_frontend_get_current_preview_scene();
	if (!scene) {
		scene = obs_frontend_get_current_scene();
	}
	return scene;
}

class SourcesPanel : public QFrame {
public:
	explicit SourcesPanel(QWidget *parent) : QFrame(parent)
	{
		setObjectName("plSources");
		setStyleSheet(
			"QFrame#plSources { background: #1C1C1C; border: 1px solid #2A2A2A; border-radius: 8px; }");
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(8, 6, 8, 8);
		layout->setSpacing(4);

		auto *head = new QHBoxLayout();
		auto *title = new QLabel(msg("Studio.Sources"), this);
		QFont bold = title->font();
		bold.setBold(true);
		title->setFont(bold);
		head->addWidget(title, 1);
		addButton = plIconButton(this, "icon-plus", ":/res/images/plus.svg", msg("Entries.AddSource"));
		connect(addButton, &QToolButton::clicked, this, [this]() {
			if (pages->currentIndex() == 1) {
				pl_vertical_sources_add(verticalPage,
							addButton->mapToGlobal(QPoint(0, addButton->height())));
			} else {
				addSource();
			}
		});
		head->addWidget(addButton);
		gearButton = plIconButton(this, "icon-gear", ":/settings/images/settings/general.svg",
					  msg("Studio.Properties"));
		connect(gearButton, &QToolButton::clicked, this, [this]() {
			if (pages->currentIndex() == 1) {
				pl_vertical_sources_properties(verticalPage);
			} else {
				openProperties();
			}
		});
		head->addWidget(gearButton);
		layout->addLayout(head);

		/* Duas páginas: as fontes da entrada (horizontal) e as fontes verticais. */
		auto *tabs = new QHBoxLayout();
		tabs->setSpacing(4);
		horizontalTab = new QPushButton(msg("Studio.Horizontal"), this);
		verticalTab = new QPushButton(msg("Studio.Vertical"), this);
		for (QPushButton *tab : {horizontalTab, verticalTab}) {
			tab->setCheckable(true);
			tab->setAutoExclusive(true);
			tabs->addWidget(tab, 1);
		}
		horizontalTab->setChecked(true);
		connect(horizontalTab, &QPushButton::clicked, this, [this]() { showPage(0); });
		connect(verticalTab, &QPushButton::clicked, this, [this]() { showPage(1); });
		layout->addLayout(tabs);

		pages = new QStackedWidget(this);
		layout->addWidget(pages, 1);

		auto *horizontalPage = new QWidget(pages);
		auto *pageLayout = new QVBoxLayout(horizontalPage);
		pageLayout->setContentsMargins(0, 0, 0, 0);
		pageLayout->setSpacing(4);
		pages->addWidget(horizontalPage);
		verticalPage = pl_vertical_sources_create(pages);
		pages->addWidget(verticalPage);

		entryName = new QLabel(horizontalPage);
		entryName->setEnabled(false);
		pageLayout->addWidget(entryName);

		list = new QListWidget(horizontalPage);
		list->setDragDropMode(QAbstractItemView::InternalMove);
		list->setDefaultDropAction(Qt::MoveAction);
		list->setSelectionMode(QAbstractItemView::SingleSelection);
		list->setContextMenuPolicy(Qt::CustomContextMenu);
		list->setToolTip(msg("Studio.SourcesHint"));
		pageLayout->addWidget(list, 1);

		connect(list->model(), &QAbstractItemModel::rowsMoved, this, [this]() { applyOrder(); });
		connect(list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) { openProperties(); });
		connect(list, &QWidget::customContextMenuRequested, this,
			[this](const QPoint &pos) { showMenu(list->viewport()->mapToGlobal(pos)); });
	}

	~SourcesPanel() override { obs_weak_source_release(weak); }

	/* Chamado pelo relógio da janela: segue a entrada da prévia e refaz a lista se algo mudou. */
	void refresh()
	{
		obs_source_t *scene = workingScene();
		obs_source_t *mine = obs_weak_source_get_source(weak);
		if (scene != mine) {
			obs_weak_source_release(weak);
			weak = scene ? obs_source_get_weak_source(scene) : nullptr;
		}
		obs_source_release(mine);

		QString name = scene ? QString::fromUtf8(obs_source_get_name(scene)) : QString();
		if (entryName->text() != name) {
			entryName->setText(name);
		}

		ItemList items = scene ? sceneItems(scene) : ItemList();
		obs_source_release(scene);

		QStringList signature;
		signature << name;
		for (obs_sceneitem_t *item : items) {
			obs_source_t *src = obs_sceneitem_get_source(item);
			signature << QString("%1|%2%3|%4")
					     .arg((qlonglong)obs_sceneitem_get_id(item))
					     .arg(obs_sceneitem_visible(item) ? "1" : "0")
					     .arg(obs_sceneitem_locked(item) ? "1" : "0")
					     .arg(QString::fromUtf8(src ? obs_source_get_name(src) : ""));
		}
		if (signature == shown) {
			releaseItems(items);
			return;
		}
		shown = signature;

		qlonglong current = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toLongLong() : -1;
		QSignalBlocker block(list);
		list->clear();
		/* O OBS guarda do fundo para a frente; a lista mostra a frente em cima. */
		for (auto it = items.rbegin(); it != items.rend(); ++it) {
			obs_sceneitem_t *item = *it;
			obs_source_t *src = obs_sceneitem_get_source(item);
			auto *row = new QListWidgetItem(list);
			qlonglong id = (qlonglong)obs_sceneitem_get_id(item);
			row->setData(Qt::UserRole, id);
			row->setFlags((row->flags() | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled);
			/* Nome, olho e cadeado, como no painel Fontes do OBS. */
			QString name = QString::fromUtf8(src ? obs_source_get_name(src) : "");
			auto onVisible = [this, id](bool on) {
				setItemVisible(id, on);
			};
			auto onLock = [this, id](bool on) {
				setItemLocked(id, on);
			};
			bool shownNow = obs_sceneitem_visible(item);
			bool lockedNow = obs_sceneitem_locked(item);
			auto *widget = new PLSourceRow(list, name, shownNow, lockedNow, onVisible, onLock);
			row->setSizeHint(QSize(0, std::max(widget->sizeHint().height(), 26)));
			list->setItemWidget(row, widget);
			if (row->data(Qt::UserRole).toLongLong() == current) {
				list->setCurrentItem(row);
			}
		}
		releaseItems(items);
	}

private:
	/* As duas páginas iguais: o "+" e a engrenagem de cima valem para a página aberta. */
	void showPage(int index) { pages->setCurrentIndex(index); }

	/* O item da cena que a linha representa (sem referência; usar na hora). */
	obs_sceneitem_t *itemFor(QListWidgetItem *row) const
	{
		if (!row) {
			return nullptr;
		}
		obs_source_t *src = obs_weak_source_get_source(weak);
		obs_scene_t *scene = obs_scene_from_source(src);
		obs_sceneitem_t *item =
			scene ? obs_scene_find_sceneitem_by_id(scene, row->data(Qt::UserRole).toLongLong()) : nullptr;
		obs_source_release(src);
		return item;
	}

	void addSource()
	{
		obs_source_t *src = obs_weak_source_get_source(weak);
		if (src) {
			pl_entries_add_source(src);
			obs_source_release(src);
		}
	}

	void applyOrder()
	{
		obs_source_t *src = obs_weak_source_get_source(weak);
		obs_scene_t *scene = obs_scene_from_source(src);
		if (scene) {
			std::vector<obs_sceneitem_t *> order;
			for (int i = list->count() - 1; i >= 0; i--) {
				obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(
					scene, list->item(i)->data(Qt::UserRole).toLongLong());
				if (item) {
					order.push_back(item);
				}
			}
			obs_scene_reorder_items(scene, order.data(), order.size());
		}
		obs_source_release(src);
		shown.clear();
	}

	obs_sceneitem_t *itemById(qlonglong id) const
	{
		obs_source_t *src = obs_weak_source_get_source(weak);
		obs_scene_t *scene = obs_scene_from_source(src);
		obs_sceneitem_t *item = scene ? obs_scene_find_sceneitem_by_id(scene, id) : nullptr;
		obs_source_release(src);
		return item;
	}

	void setItemVisible(qlonglong id, bool on)
	{
		if (obs_sceneitem_t *item = itemById(id)) {
			obs_sceneitem_set_visible(item, on);
		}
	}

	void setItemLocked(qlonglong id, bool on)
	{
		if (obs_sceneitem_t *item = itemById(id)) {
			obs_sceneitem_set_locked(item, on);
		}
	}

	void openProperties()
	{
		if (obs_sceneitem_t *item = itemFor(list->currentItem())) {
			obs_frontend_open_source_properties(obs_sceneitem_get_source(item));
		}
	}

	void showMenu(const QPoint &where)
	{
		QListWidgetItem *row = list->itemAt(list->viewport()->mapFromGlobal(where));
		if (row) {
			list->setCurrentItem(row);
		}

		QMenu menu;
		QAction *add = menu.addAction(msg("Entries.AddSource"));
		QAction *properties = nullptr;
		QAction *filters = nullptr;
		QAction *remove = nullptr;
		if (row) {
			menu.addSeparator();
			properties = menu.addAction(msg("Studio.Properties"));
			filters = menu.addAction(msg("Studio.Filters"));
			menu.addSeparator();
			remove = menu.addAction(msg("Studio.RemoveSource"));
		}

		QAction *chosen = menu.exec(where);
		if (!chosen) {
			return;
		}
		if (chosen == add) {
			addSource();
			return;
		}
		obs_sceneitem_t *item = itemFor(row);
		if (!item) {
			return;
		}
		if (chosen == properties) {
			obs_frontend_open_source_properties(obs_sceneitem_get_source(item));
		} else if (chosen == filters) {
			obs_frontend_open_source_filters(obs_sceneitem_get_source(item));
		} else if (chosen == remove) {
			QString text = msg("Studio.RemoveConfirm").arg(row->text());
			if (QMessageBox::question(this, msg("Studio.RemoveSource"), text) == QMessageBox::Yes) {
				/* A pergunta pode ter demorado: procura o item de novo. */
				if (obs_sceneitem_t *again = itemFor(row)) {
					obs_sceneitem_remove(again);
				}
			}
		}
		shown.clear();
	}

	obs_weak_source_t *weak = nullptr;
	QToolButton *addButton = nullptr;
	QToolButton *gearButton = nullptr;
	QPushButton *horizontalTab = nullptr;
	QPushButton *verticalTab = nullptr;
	QStackedWidget *pages = nullptr;
	QWidget *verticalPage = nullptr;
	QLabel *entryName = nullptr;
	QListWidget *list = nullptr;
	QStringList shown;
};

/* ------------------------------------------------------------------------- */
/* Coisas que a tela Operar e o Acompanhar (monitor 2) usam igual            */

void openSourcesDialog(); /* abre a janelinha "Fontes desta entrada" (mais embaixo) */

/* O item "Fontes desta entrada..." que entra no alto do menu do OBS na Prévia. */
QAction *sourcesMenuAction = nullptr;

/* Seleciona (no OBS) a fonte que está debaixo do mouse e abre o menu da prévia do OBS, que age
 * sobre a fonte selecionada (Transformar, Modo de mescla, Filtros...). Fora de qualquer fonte,
 * nada fica selecionado e o OBS mostra o menu da cena (Adicionar fonte...). */
void previewMenuAt(QWidget *view, const QPoint &pos)
{
	QWidget *main = obsWindow();
	if (!main) {
		return;
	}
	obs_source_t *sceneSource = workingScene();
	double sw = sceneSource ? obs_source_get_width(sceneSource) : 0;
	double sh = sceneSource ? obs_source_get_height(sceneSource) : 0;
	if (obs_scene_from_source(sceneSource) && sw > 0 && sh > 0 && view->width() > 0 && view->height() > 0) {
		double scale = std::min(view->width() / sw, view->height() / sh);
		double x0 = (view->width() - sw * scale) / 2.0;
		double y0 = (view->height() - sh * scale) / 2.0;
		struct vec3 point;
		vec3_set(&point, (float)((pos.x() - x0) / scale), (float)((pos.y() - y0) / scale), 0.0f);

		ItemList items = sceneItems(sceneSource);
		obs_sceneitem_t *hit = nullptr;
		/* A frente fica no fim da lista do OBS. */
		for (auto it = items.rbegin(); it != items.rend() && !hit; ++it) {
			if (!obs_sceneitem_visible(*it)) {
				continue;
			}
			struct matrix4 box, inverse;
			obs_sceneitem_get_box_transform(*it, &box);
			matrix4_inv(&inverse, &box);
			struct vec3 local;
			vec3_transform(&local, &point, &inverse);
			if (local.x >= 0.0f && local.x <= 1.0f && local.y >= 0.0f && local.y <= 1.0f) {
				hit = *it;
			}
		}
		for (obs_sceneitem_t *item : items) {
			obs_sceneitem_select(item, item == hit);
		}
		releaseItems(items);
	}
	obs_source_release(sceneSource);
	/* O menu do OBS ganha, só desta vez, o "Fontes desta entrada..." no alto (veja
	 * OBSBasic::CreateSourcePopupMenu, que lê estas duas propriedades). */
	if (!sourcesMenuAction) {
		sourcesMenuAction = new QAction(msg("Entries.Sources"), main);
		QObject::connect(sourcesMenuAction, &QAction::triggered, main, []() { openSourcesDialog(); });
		QObject::connect(sourcesMenuAction, &QObject::destroyed, []() { sourcesMenuAction = nullptr; });
	}
	main->setProperty("plPreviewExtra", QVariant::fromValue<QObject *>(sourcesMenuAction));
	main->setProperty("plPreviewExtraOnce", true);
	/* Na fila: o OBS atualiza a seleção dele antes de montar o menu. */
	QMetaObject::invokeMethod(main, "on_preview_customContextMenuRequested", Qt::QueuedConnection);
}

/* A saída do OBS: o que vai para a live, com a transição acontecendo. */
void drawProgramOutput(void *, uint32_t cx, uint32_t cy)
{
	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		return;
	}
	if (beginFit(ovi.base_width, ovi.base_height, cx, cy)) {
		obs_render_main_texture();
		/* A guia: a parte do 16:9 que está indo para o vertical (só no No ar). */
		pl_vertical_pan_draw_guide();
		endFit();
	}
}

/* Botão direito na Prévia e no No ar: os mesmos menus da janela original do OBS. */
void attachScreenMenus(QObject *owner, QWidget *previewView, QWidget *programView)
{
	previewView->setContextMenuPolicy(Qt::CustomContextMenu);
	QObject::connect(previewView, &QWidget::customContextMenuRequested, owner,
			 [previewView](const QPoint &pos) { previewMenuAt(previewView, pos); });
	programView->setContextMenuPolicy(Qt::CustomContextMenu);
	QObject::connect(programView, &QWidget::customContextMenuRequested, owner, []() {
		if (QWidget *main = obsWindow()) {
			QMetaObject::invokeMethod(main, "ProgramViewContextMenuRequested", Qt::QueuedConnection);
		}
	});
}

void cutFromAnywhere();        /* cortar (mais embaixo, junto da tela Operar) */
void transitionFromAnywhere(); /* trocar com a transição escolhida */

/* O "cortar" e a transição, como numa mesa de corte: cortar (branco), o nome da transição (faz a
 * troca com ela; botão direito abre a janelinha das transições) e a duração embaixo. */
class CutColumn : public QWidget {
public:
	explicit CutColumn(QWidget *parent) : QWidget(parent)
	{
		auto *center = new QVBoxLayout(this);
		center->setContentsMargins(0, 0, 0, 0);
		center->setSpacing(6);
		center->addStretch(1);
		cut = new QPushButton(msg("Studio.Cut"), this);
		cut->setMinimumSize(120, 52);
		cut->setFocusPolicy(Qt::NoFocus);
		somun_make_primary(cut);
		QFont cutFont = cut->font();
		cutFont.setPointSizeF(cutFont.pointSizeF() + 2.0);
		cut->setFont(cutFont);
		cut->setToolTip(msg("Studio.CutHint"));
		connect(cut, &QPushButton::clicked, this, []() { cutFromAnywhere(); });
		center->addWidget(cut);
		transition = new QPushButton(msg("Studio.Transition"), this);
		transition->setMinimumSize(120, 34);
		transition->setFocusPolicy(Qt::NoFocus);
		transition->setToolTip(msg("Studio.TransitionHint"));
		transition->setContextMenuPolicy(Qt::CustomContextMenu);
		connect(transition, &QPushButton::clicked, this, []() { transitionFromAnywhere(); });
		connect(transition, &QWidget::customContextMenuRequested, this,
			[this]() { pl_transitions_dialog_show(window()); });
		center->addWidget(transition);
		info = new QLabel(this);
		info->setAlignment(Qt::AlignCenter);
		info->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY));
		center->addWidget(info);
		/* Deslizar a janela do vertical, logo abaixo da transição. */
		center->addWidget(pl_vertical_pan_create(this));
		center->addStretch(1);
	}

	/* Sem modo estúdio não tem cortar nem transição, mas o deslizar do vertical continua. */
	void setCutVisible(bool on)
	{
		cut->setVisible(on);
		transition->setVisible(on);
		info->setVisible(on);
	}

	/* Nome e duração da transição de agora. */
	void refresh()
	{
		obs_source_t *current = obs_frontend_get_current_transition();
		QString name = msg("Studio.Transition");
		QString ms;
		if (current) {
			name = QString::fromUtf8(obs_source_get_name(current));
			ms = QString("%1 ms").arg(obs_frontend_get_transition_duration());
			obs_source_release(current);
		}
		if (transition->text() != name) {
			transition->setText(name);
		}
		if (info->text() != ms) {
			info->setText(ms);
		}
	}

private:
	QPushButton *cut = nullptr;
	QPushButton *transition = nullptr;
	QLabel *info = nullptr;
};

/* O topo do Acompanhar (monitor 2): Prévia | cortar e transição | No ar, como numa mesa de corte.
 * Assim o corte inteiro acontece aqui (clica na entrada → vê na Prévia → corta). No modo normal
 * (sem modo estúdio) fica só a tela do ar, onde se mexe. */
class ScreensPair : public QWidget {
public:
	explicit ScreensPair(QWidget *parent) : QWidget(parent)
	{
		auto *row = new QHBoxLayout(this);
		row->setContentsMargins(0, 0, 0, 0);
		row->setSpacing(12);
		preview = new Monitor(this, msg("Studio.Preview"), SOMUN_WHITE, pl_horizontal_preview_create);
		row->addWidget(preview, 1);
		/* A Prévia em pé ao lado da Prévia (onde se ajusta o vertical com o mouse). */
		verticalPreview = new QWidget(this);
		auto *verticalPreviewLayout = new QVBoxLayout(verticalPreview);
		verticalPreviewLayout->setContentsMargins(0, 0, 0, 0);
		verticalPreviewLayout->addWidget(pl_vertical_preview_create(verticalPreview));
		row->addWidget(verticalPreview);
		cutColumn = new CutColumn(this);
		row->addWidget(cutColumn);
		program = new Monitor(this, msg("Studio.Program"), SOMUN_RED, drawProgramOutput, nullptr);
		row->addWidget(program, 1);
		/* O No ar em pé ao lado do No ar. */
		verticalProgram = new QWidget(this);
		auto *verticalProgramLayout = new QVBoxLayout(verticalProgram);
		verticalProgramLayout->setContentsMargins(0, 0, 0, 0);
		verticalProgramLayout->addWidget(pl_vertical_program_create(verticalProgram));
		row->addWidget(verticalProgram);
		attachScreenMenus(this, preview->view, program->view);

		auto *timer = new QTimer(this);
		timer->setInterval(200);
		connect(timer, &QTimer::timeout, this, [this]() { refresh(); });
		timer->start();
		refresh();
	}

	~ScreensPair() override { release(); }

	void release()
	{
		if (preview) {
			pl_horizontal_preview_release(preview->view);
		}
		if (program && program->screen) {
			program->screen->destroyDisplay();
		}
	}

private:
	void refresh()
	{
		if (!isVisible()) {
			return;
		}
		pl_horizontal_preview_sync(preview->view);
		program->screen->syncSize();
		bool studio = obs_frontend_preview_program_mode_active();
		if (program->isHidden() == studio) {
			program->setVisible(studio);
			cutColumn->setCutVisible(studio);
		}
		/* Telas em pé: o No ar em pé quando o vertical está ligado; a Prévia em pé só no modo estúdio. */
		bool vertical = pl_vertical_program_wanted();
		if (verticalProgram->isHidden() == vertical) {
			verticalProgram->setVisible(vertical);
		}
		bool verticalPreviewShown = vertical && studio;
		if (verticalPreview->isHidden() == verticalPreviewShown) {
			verticalPreview->setVisible(verticalPreviewShown);
		}
		if (studio) {
			cutColumn->refresh();
			preview->setLook(msg("Studio.Preview"), SOMUN_WHITE);
		} else {
			preview->setLook(msg("Studio.Program"), SOMUN_RED);
		}
		program->setLook(msg("Studio.Program"), SOMUN_RED);
		obs_source_t *previewSource = obs_frontend_get_current_preview_scene();
		obs_source_t *programSource = obs_frontend_get_current_scene();
		obs_source_t *shownFirst = previewSource ? previewSource : programSource;
		preview->setSceneName(shownFirst ? QString::fromUtf8(obs_source_get_name(shownFirst)) : QString());
		program->setSceneName(programSource ? QString::fromUtf8(obs_source_get_name(programSource))
						    : QString());
		obs_source_release(previewSource);
		obs_source_release(programSource);
	}

	Monitor *preview = nullptr;
	QWidget *verticalPreview = nullptr;
	CutColumn *cutColumn = nullptr;
	Monitor *program = nullptr;
	QWidget *verticalProgram = nullptr;
};

/* ------------------------------------------------------------------------- */

class StudioWindow : public QWidget {
public:
	StudioWindow() : QWidget(nullptr)
	{
		setWindowTitle(msg("Studio.Title"));
		if (QWidget *main = obsWindow()) {
			setWindowIcon(main->windowIcon());
		}
		resize(1400, 860);

		/* A cara do somun (cores e fonte Outfit) só nesta janela; o Modo OBS segue com o tema dele. */
		setObjectName("somunWindow");
		setAttribute(Qt::WA_StyledBackground, true);
		setStyleSheet(somun_window_style());

		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(12, 8, 12, 10);
		outer->setSpacing(10);

		/* Topo: logo, culto, Gravando, No ar · tempo e "⋯" (o resto do OBS mora no "⋯"). */
		auto *top = new QHBoxLayout();
		top->setSpacing(10);
		top->addWidget(somun_brand_label(this, "live", 24));
		top->addSpacing(8);
		if (QWidget *badge = pl_account_badge_create(this)) {
			top->addWidget(badge);
		}
		top->addStretch(1);
		recordButton = new QPushButton(this);
		recordButton->setMinimumHeight(32);
		recordButton->setFocusPolicy(Qt::NoFocus);
		connect(recordButton, &QPushButton::clicked, this, []() { toggleRecord(); });
		top->addWidget(recordButton);
		streamButton = new QPushButton(this);
		streamButton->setMinimumHeight(32);
		streamButton->setFocusPolicy(Qt::NoFocus);
		connect(streamButton, &QPushButton::clicked, this, [this]() { toggleStream(); });
		/* Botão direito no Transmitir: a janela Destinos (parte G2). */
		streamButton->setContextMenuPolicy(Qt::CustomContextMenu);
		connect(streamButton, &QPushButton::customContextMenuRequested, this, [this](const QPoint &pos) {
			QMenu menu(this);
			QAction *open = menu.addAction(msg("Studio.Destinations"));
			if (menu.exec(streamButton->mapToGlobal(pos)) == open) {
				pl_destinations_window_open(this);
			}
		});
		top->addWidget(streamButton);
		moreButton = new QPushButton(QString::fromUtf8("⋯"), this);
		moreButton->setMinimumSize(40, 32);
		moreButton->setFocusPolicy(Qt::NoFocus);
		moreButton->setToolTip(msg("Studio.More"));
		connect(moreButton, &QPushButton::clicked, this, [this]() { showMoreMenu(); });
		top->addWidget(moreButton);
		outer->addLayout(top);

		auto *body = new QHBoxLayout();
		body->setSpacing(12);

		/* Coluna das Fontes (as peças da entrada que está na Prévia): aparece na mesa quando a Prévia e o
		 * No ar estão no monitor 2. Com um monitor só ela fica no botão direito ("Fontes desta entrada..."). */
		sourcesColumn = new SourcesPanel(this);
		sourcesColumn->setFixedWidth(280);
		sourcesColumn->setVisible(false);
		body->addWidget(sourcesColumn);

		auto *right = new QVBoxLayout();
		right->setSpacing(10);

		/* Prévia | cortar e transição | No ar */
		auto *middle = new QHBoxLayout();
		middle->setSpacing(12);
		/* Sem Prévia e No ar (eles foram para o monitor 2), o que sobra fica no meio. */
		middle->addStretch(0);

		/* A Prévia aceita o mouse: escolher, mover, aumentar e diminuir pelos quadradinhos, Alt corta. */
		preview = new Monitor(this, msg("Studio.Preview"), SOMUN_WHITE, pl_horizontal_preview_create);
		middle->addWidget(preview, 1);

		/* A Prévia vertical, em pé, ao lado da Prévia: é nela que se ajusta o vertical. */
		verticalPreviewHolder = new QWidget(this);
		auto *verticalPreviewLayout = new QVBoxLayout(verticalPreviewHolder);
		verticalPreviewLayout->setContentsMargins(0, 0, 0, 0);
		verticalPreviewLayout->addWidget(pl_vertical_preview_create(verticalPreviewHolder));
		middle->addWidget(verticalPreviewHolder);

		auto *center = new QVBoxLayout();
		center->setSpacing(6);
		center->addStretch(1);
		cutButton = new QPushButton(msg("Studio.Cut"), this);
		cutButton->setMinimumSize(120, 52);
		cutButton->setFocusPolicy(Qt::NoFocus);
		somun_make_primary(cutButton);
		QFont cutFont = cutButton->font();
		cutFont.setPointSizeF(cutFont.pointSizeF() + 2.0);
		cutButton->setFont(cutFont);
		cutButton->setToolTip(msg("Studio.CutHint"));
		connect(cutButton, &QPushButton::clicked, this, []() { cutNow(); });
		center->addWidget(cutButton);
		/* A transição: o nome dela é o botão (faz a troca com a transição); o botão direito abre a
		 * janelinha das transições. */
		transitionButton = new QPushButton(msg("Studio.Transition"), this);
		transitionButton->setMinimumSize(120, 34);
		transitionButton->setFocusPolicy(Qt::NoFocus);
		transitionButton->setToolTip(msg("Studio.TransitionHint"));
		transitionButton->setContextMenuPolicy(Qt::CustomContextMenu);
		connect(transitionButton, &QPushButton::clicked, this, []() { transitionNow(); });
		connect(transitionButton, &QWidget::customContextMenuRequested, this,
			[this]() { pl_transitions_dialog_show(this); });
		center->addWidget(transitionButton);
		transitionInfo = new QLabel(this);
		transitionInfo->setAlignment(Qt::AlignCenter);
		transitionInfo->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY));
		center->addWidget(transitionInfo);
		/* Com um monitor só, o deslizar do vertical fica aqui; com o Acompanhar, no monitor 2. */
		panHolder = new QWidget(this);
		auto *panLayout = new QVBoxLayout(panHolder);
		panLayout->setContentsMargins(0, 0, 0, 0);
		panLayout->addWidget(pl_vertical_pan_create(panHolder));
		center->addWidget(panHolder);
		center->addStretch(1);
		middle->addLayout(center);

		program = new Monitor(this, msg("Studio.Program"), SOMUN_RED, drawProgramOutput, nullptr);

		/* Botão direito: os mesmos menus da janela original do OBS. */
		attachScreenMenus(this, preview->view, program->view);
		middle->addWidget(program, 1);

		/* O vertical do ar, em pé, ao lado do No ar (some quando está desligado em Painéis). */
		verticalHolder = new QWidget(this);
		auto *verticalLayout = new QVBoxLayout(verticalHolder);
		verticalLayout->setContentsMargins(0, 0, 0, 0);
		verticalLayout->addWidget(pl_vertical_program_create(verticalHolder));
		middle->addWidget(verticalHolder);
		middle->addStretch(0);

		right->addLayout(middle, 5);
		rightLayout = right;
		middleLayout = middle;

		/* Modo Cor com um monitor só: cobre o meio da mesa até clicar em "sair". */
		colorPanel = pl_color_panel_create(this, [this]() {
			colorHere = false;
			refresh();
		});
		colorPanel->setVisible(false);
		right->addWidget(colorPanel, 8);

		/* Entradas: a mesma lista do painel do OBS (outra cópia, que mostra as mesmas cenas). Com o
		 * Acompanhar ligado no monitor 2, elas ficam só lá. */
		/* Artes: a barra das peças "No programa" (logo, QR code, letterbox), por cima de tudo que vai
		 * ao ar. Um clique liga, outro desliga; "+" cria; botão direito edita. */
		right->addWidget(pl_artes_bar_create(this));

		entries = pl_camera_dock_create(this);
		right->addWidget(entries, 3);

		/* Som: o mesmo painel Som do OBS (outra cópia), numa faixa fina. */
		auto *audio = pl_audio_dock_create(this);
		audio->setMaximumHeight(130);
		right->addWidget(audio, 1);

		body->addLayout(right, 1);

		/* Coluna da direita: Equipe (lista pequena: nome, bolinha de quem fala, clique = recado,
		 * segurar = falar só com a pessoa) e a Situação completa. */
		auto *side = new QVBoxLayout();
		side->setSpacing(10);
		auto *team = new QFrame(this);
		team->setObjectName("somunPanel");
		auto *teamLayout = new QVBoxLayout(team);
		teamLayout->setContentsMargins(10, 8, 10, 8);
		teamLayout->addWidget(pl_team_panel_create(team));
		side->addWidget(team);
		auto *status = new QFrame(this);
		status->setObjectName("somunPanel");
		auto *statusLayout = new QVBoxLayout(status);
		statusLayout->setContentsMargins(10, 8, 10, 10);
		statusLayout->addWidget(pl_stats_column_create(status));
		side->addWidget(status);
		side->addStretch(1);
		auto *sideHolder = new QWidget(this);
		sideHolder->setLayout(side);
		side->setContentsMargins(0, 0, 0, 0);
		sideHolder->setFixedWidth(260);
		body->addWidget(sideHolder);

		outer->addLayout(body, 1);

		/* Os recados do diretor aparecem no canto de cima, à direita. */
		pl_account_attach_toasts(this);

		timer = new QTimer(this);
		timer->setInterval(200);
		connect(timer, &QTimer::timeout, this, [this]() { refresh(); });
		timer->start();
		refresh();
	}

	~StudioWindow() override
	{
		releaseScreens();
		std::lock_guard<std::mutex> lock(previewMutex);
		obs_weak_source_release(previewScene);
		previewScene = nullptr;
	}

	/* Solta as telas do OBS (chamado antes de o OBS desligar a parte gráfica). */
	void releaseScreens()
	{
		if (preview) {
			pl_horizontal_preview_release(preview->view);
		}
		if (program) {
			program->screen->destroyDisplay();
		}
		pl_watch_release();
	}

	/* Fechar a nossa janela é fechar o OBS (com as perguntas de sempre, se estiver ao vivo). */
	static void enterObs();
	static void cutNow();
	static void transitionNow() { obs_frontend_preview_program_trigger_transition(); }

protected:
	void closeEvent(QCloseEvent *event) override
	{
		event->ignore();
		if (QWidget *main = obsWindow()) {
			QMetaObject::invokeMethod(main, "close", Qt::QueuedConnection);
		}
	}

public:
	/* A janelinha "Fontes desta entrada" (segue a entrada que está na prévia). */
	void showSources()
	{
		if (!sourcesDialog) {
			sourcesDialog = new QDialog(this);
			sourcesDialog->setWindowTitle(msg("Studio.SourcesTitle"));
			sourcesDialog->resize(360, 460);
			auto *layout = new QVBoxLayout(sourcesDialog);
			layout->setContentsMargins(8, 8, 8, 8);
			sources = new SourcesPanel(sourcesDialog);
			layout->addWidget(sources);
		}
		sources->refresh();
		sourcesDialog->show();
		sourcesDialog->raise();
		sourcesDialog->activateWindow();
	}

private:
	/* O "⋯": tudo do OBS que não precisa ficar na tela. */
	void showMoreMenu()
	{
		QMenu menu(this);

		QAction *watch = menu.addAction(msg("Studio.Watch"));
		watch->setCheckable(true);
		watch->setChecked(pl_watch_wanted());
		if (!pl_watch_available()) {
			watch->setText(msg("Studio.WatchNeedsTwo"));
			watch->setEnabled(false);
		}
		QString keyName = pl_talk_key_name();
		QAction *talkKey = menu.addAction(
			msg("Studio.TalkKey").arg(keyName.isEmpty() ? msg("TalkKey.NoneShort") : keyName));
		menu.addSeparator();

		QAction *artes = menu.addAction(msg("Studio.ArtesMenu"));
		/* Com o Acompanhar, o modo Cor é o botão "Cor" do monitor 2. */
		QAction *color = nullptr;
		if (!watchingNow) {
			color = menu.addAction(msg("Studio.ColorMenu"));
			color->setCheckable(true);
			color->setChecked(colorHere);
		}
		QAction *transitions = menu.addAction(msg("Studio.TransitionsMenu"));
		QAction *studio = menu.addAction(msg("Studio.StudioMode"));
		studio->setCheckable(true);
		studio->setChecked(obs_frontend_preview_program_mode_active());
		studio->setToolTip(msg("Studio.StudioModeHint"));
		menu.addSeparator();

		QAction *destinations = menu.addAction(msg("Studio.Destinations"));
		QAction *settings = menu.addAction(msg("Studio.Settings"));
		QAction *toObs = menu.addAction(msg("Studio.ToObs"));
		/* Os menus do OBS (Arquivo, Editar, Visualizar...): os mesmos, não uma cópia. */
		if (auto *main = qobject_cast<QMainWindow *>(obsWindow())) {
			QMenu *obsMenus = menu.addMenu(msg("Studio.ObsMenus"));
			for (QAction *action : main->menuBar()->actions()) {
				obsMenus->addAction(action);
			}
		}

		QAction *chosen = menu.exec(moreButton->mapToGlobal(QPoint(0, moreButton->height())));
		if (!chosen) {
			return;
		}
		if (chosen == watch) {
			pl_watch_set_wanted(watch->isChecked());
			refresh();
		} else if (chosen == talkKey) {
			pl_talk_key_choose(this);
		} else if (chosen == artes) {
			pl_artes_open_editor(this);
		} else if (color && chosen == color) {
			colorHere = color->isChecked();
			refresh();
		} else if (chosen == transitions) {
			pl_transitions_dialog_show(this);
		} else if (chosen == studio) {
			obs_frontend_set_preview_program_mode(studio->isChecked());
		} else if (chosen == destinations) {
			pl_destinations_window_open(this);
		} else if (chosen == settings) {
			openSettings();
		} else if (chosen == toObs) {
			enterObs();
		}
	}

	void refresh()
	{
		pl_horizontal_preview_sync(preview->view);
		program->screen->syncSize();
		if (sources && sourcesDialog && sourcesDialog->isVisible()) {
			sources->refresh();
		}
		updateTop();

		/* Monitor 2: o Acompanhar aparece quando há dois monitores e ele está ligado. */
		bool watching = pl_watch_update(this);
		watchingNow = watching;
		if (watching) {
			colorHere = false; /* com dois monitores, o modo Cor mora no monitor 2 */
		}
		if (colorPanel->isHidden() == colorHere) {
			colorPanel->setVisible(colorHere);
		}
		/* Com o Acompanhar, a imagem fica só no monitor 2; com o modo Cor, ele cobre o meio da mesa. */
		bool imagesAway = watching || colorHere;
		/* No modo Cor o meio (vazio) não guarda lugar: o painel ocupa a altura toda. */
		rightLayout->setStretchFactor(middleLayout, colorHere ? 0 : 5);
		/* Com o Acompanhar, as entradas ficam só no monitor 2 (as duas telas não repetem nada). */
		if (entries->isHidden() == !imagesAway) {
			entries->setVisible(!imagesAway);
		}
		pl_talk_key_set_active(isVisible());

		bool vertical = pl_vertical_program_wanted();
		/* Com o Acompanhar, as telas em pé vão para o monitor 2 (cada uma ao lado da sua). */
		bool verticalHere = vertical && !imagesAway;
		if (verticalHolder->isHidden() == verticalHere) {
			verticalHolder->setVisible(verticalHere);
		}
		/* Modo normal: uma tela só (a do ar, onde se mexe), sem cortar e transição. Com o
		 * Acompanhar no monitor 2, a Prévia e o No ar ficam só lá (aqui sobra espaço para o resto). */
		bool studio = obs_frontend_preview_program_mode_active();
		bool programShown = studio && !imagesAway;
		if (program->isHidden() == programShown) {
			program->setVisible(programShown);
		}
		if (preview->isHidden() == !imagesAway) {
			preview->setVisible(!imagesAway);
		}
		/* Com o Acompanhar, o cortar e a transição ficam só no monitor 2 (entre a Prévia e o No ar). */
		bool cutHere = studio && !imagesAway;
		if (panHolder->isHidden() == !imagesAway) {
			panHolder->setVisible(!imagesAway);
		}
		if (cutButton->isHidden() == cutHere) {
			cutButton->setVisible(cutHere);
			transitionButton->setVisible(cutHere);
			transitionInfo->setVisible(cutHere);
		}
		/* As Fontes voltam para a mesa quando sobra espaço (Prévia e No ar foram para o monitor 2). */
		if (sourcesColumn->isHidden() == watching) {
			sourcesColumn->setVisible(watching);
		}
		if (watching) {
			sourcesColumn->refresh();
		}
		if (studio) {
			preview->setLook(msg("Studio.Preview"), SOMUN_WHITE);
		} else {
			preview->setLook(msg("Studio.Program"), SOMUN_RED);
		}
		bool verticalPreview = verticalHere && studio;
		if (verticalPreviewHolder->isHidden() == verticalPreview) {
			verticalPreviewHolder->setVisible(verticalPreview);
		}

		obs_source_t *previewSource = obs_frontend_get_current_preview_scene();
		obs_source_t *programSource = obs_frontend_get_current_scene();

		{
			std::lock_guard<std::mutex> lock(previewMutex);
			obs_weak_source_release(previewScene);
			previewScene = previewSource ? obs_source_get_weak_source(previewSource) : nullptr;
		}

		obs_source_t *shownFirst = previewSource ? previewSource : programSource;
		preview->setSceneName(shownFirst ? QString::fromUtf8(obs_source_get_name(shownFirst)) : QString());
		program->setSceneName(programSource ? QString::fromUtf8(obs_source_get_name(programSource))
						    : QString());
		obs_source_release(previewSource);
		obs_source_release(programSource);

		/* O nome da transição é o próprio botão; a duração fica embaixo, em cinza. */
		obs_source_t *transition = obs_frontend_get_current_transition();
		QString name = msg("Studio.Transition");
		QString info;
		if (transition) {
			name = QString::fromUtf8(obs_source_get_name(transition));
			info = QString("%1 ms").arg(obs_frontend_get_transition_duration());
			obs_source_release(transition);
		}
		if (transitionButton->text() != name) {
			transitionButton->setText(name);
		}
		if (transitionInfo->text() != info) {
			transitionInfo->setText(info);
		}
	}

	/* Tempo de uma saída (transmissão ou gravação) pelos quadros que ela já mandou. */
	static QString outputClock(obs_output_t *output)
	{
		struct obs_video_info ovi;
		if (!output || !obs_get_video_info(&ovi) || !ovi.fps_num) {
			return QString();
		}
		uint64_t frames = (uint64_t)obs_output_get_total_frames(output);
		uint64_t seconds = frames * ovi.fps_den / ovi.fps_num;
		return QString("%1:%2:%3")
			.arg(seconds / 3600, 2, 10, QChar('0'))
			.arg(seconds / 60 % 60, 2, 10, QChar('0'))
			.arg(seconds % 60, 2, 10, QChar('0'));
	}

	/* Bolinha vermelha do "No ar" (ícone desenhado aqui, para não depender do tema). Feito na hora,
	 * só quando entra no ar: um ícone guardado para sempre seria apagado depois do Qt, ao fechar. */
	static QIcon redDot()
	{
		QPixmap pixmap(20, 20);
		pixmap.fill(Qt::transparent);
		QPainter painter(&pixmap);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(SOMUN_RED));
		painter.drawEllipse(4, 4, 12, 12);
		painter.end();
		return QIcon(pixmap);
	}

	/* Topo: "Transmitir" (botão principal branco) vira "● No ar · tempo"; "Gravar" vira "Gravando · tempo". */
	void updateTop()
	{
		/* A transmissão é a nossa lista de destinos (parte G), não a do OBS. */
		bool streaming = pl_multistream_active();
		/* Parte G3: com destino caído, o topo diz quantos ("· 1 caiu") e a letra fica vermelha. */
		int down = streaming ? pl_multistream_down_count() : 0;
		QString streamText;
		if (streaming) {
			uint64_t seconds = pl_multistream_seconds();
			QString clock = QString("%1:%2:%3")
						.arg(seconds / 3600, 2, 10, QChar('0'))
						.arg(seconds / 60 % 60, 2, 10, QChar('0'))
						.arg(seconds % 60, 2, 10, QChar('0'));
			streamText = msg("Studio.OnAirClock").arg(clock);
			if (down > 0) {
				streamText += msg(down == 1 ? "Studio.OnAirDownOne" : "Studio.OnAirDownMany").arg(down);
			}
		} else {
			streamText = msg("Studio.StreamStart");
		}
		if (streamButton->text() != streamText) {
			streamButton->setText(streamText);
		}
		int shown = streaming ? (down > 0 ? 2 : 1) : 0;
		if (streamShownLive != shown) {
			streamShownLive = shown;
			if (streaming) {
				streamButton->setIcon(redDot());
				streamButton->setToolTip(
					msg(down > 0 ? "Studio.StreamDownHint" : "Studio.StreamStopHint"));
				streamButton->setStyleSheet(
					QString("QPushButton { background: %1; color: %2; border: 1px"
						" solid %3; border-radius: 6px; padding: 4px 12px;"
						" font-weight: 600; }")
						.arg(QString(SOMUN_PANEL2))
						.arg(QString(down > 0 ? SOMUN_RED : SOMUN_TEXT))
						.arg(QString(SOMUN_RED)));
			} else {
				streamButton->setIcon(QIcon());
				streamButton->setToolTip(QString());
				somun_make_primary(streamButton);
			}
		}

		bool recording = obs_frontend_recording_active();
		QString recordText;
		if (recording) {
			obs_output_t *output = obs_frontend_get_recording_output();
			QString clock = outputClock(output);
			obs_output_release(output);
			recordText = clock.isEmpty() ? msg("Studio.Recording")
						     : msg("Studio.RecordingClock").arg(clock);
		} else {
			recordText = msg("Studio.RecordStart");
		}
		if (recordButton->text() != recordText) {
			recordButton->setText(recordText);
		}
		if (recordShown != (int)recording) {
			recordShown = (int)recording;
			recordButton->setToolTip(recording ? msg("Studio.RecordStopHint") : QString());
			recordButton->setStyleSheet(
				recording ? QString("QPushButton { border: 1px solid %1; }").arg(QString(SOMUN_WHITE))
					  : QString());
		}
	}

	/* Parar a transmissão pergunta antes (um clique sem querer derrubaria a live). */
	void toggleStream()
	{
		if (!pl_multistream_active()) {
			/* Sem destino marcado não há para onde transmitir: oferece abrir a janela Destinos. */
			if (pl_multistream_start() == 0) {
				QMessageBox box(QMessageBox::Information, msg("Studio.StreamStart"),
						msg("Studio.NoDestinations"), QMessageBox::NoButton, this);
				QPushButton *open =
					box.addButton(msg("Studio.OpenDestinations"), QMessageBox::AcceptRole);
				box.addButton(QMessageBox::Cancel);
				box.exec();
				if (box.clickedButton() == open) {
					pl_destinations_window_open(this);
				}
			}
			return;
		}
		if (QMessageBox::question(this, msg("Studio.StreamStop"), msg("Studio.StreamStopConfirm")) ==
		    QMessageBox::Yes) {
			pl_multistream_stop();
		}
	}

	static void toggleRecord()
	{
		if (obs_frontend_recording_active()) {
			obs_frontend_recording_stop();
		} else {
			obs_frontend_recording_start();
		}
	}

	/* As configurações do OBS (a mesma janela do menu Arquivo > Configurações). */
	static void openSettings()
	{
		QWidget *main = obsWindow();
		QAction *action = main ? main->findChild<QAction *>("action_Settings") : nullptr;
		if (action) {
			action->trigger();
		}
	}

	/* Seleciona (no OBS) a fonte que está debaixo do mouse e abre o menu da prévia do OBS, que age
	 * sobre a fonte selecionada (Transformar, Modo de mescla, Filtros...). Fora de qualquer fonte,
	 * nada fica selecionado e o OBS mostra o menu da cena (Adicionar fonte...). */
	void previewMenu(const QPoint &pos) { previewMenuAt(preview->view, pos); }

	static void drawPreview(void *data, uint32_t cx, uint32_t cy)
	{
		auto *self = static_cast<StudioWindow *>(data);
		obs_source_t *scene = nullptr;
		{
			std::lock_guard<std::mutex> lock(self->previewMutex);
			scene = obs_weak_source_get_source(self->previewScene);
		}
		if (!scene) {
			return;
		}
		if (beginFit(obs_source_get_width(scene), obs_source_get_height(scene), cx, cy)) {
			obs_source_video_render(scene);
			endFit();
		}
		obs_source_release(scene);
	}

	/* A saída do OBS: o que vai para a live, com a transição acontecendo. */
	static void drawProgram(void *, uint32_t cx, uint32_t cy)
	{
		struct obs_video_info ovi;
		if (!obs_get_video_info(&ovi)) {
			return;
		}
		if (beginFit(ovi.base_width, ovi.base_height, cx, cy)) {
			obs_render_main_texture();
			endFit();
		}
	}

	Monitor *preview = nullptr;
	Monitor *program = nullptr;
	QPushButton *cutButton = nullptr;
	QPushButton *transitionButton = nullptr;
	QPushButton *moreButton = nullptr;
	QLabel *transitionInfo = nullptr;
	QWidget *entries = nullptr;
	QDialog *sourcesDialog = nullptr;
	QAction *sourcesAction = nullptr;
	SourcesPanel *sources = nullptr;
	SourcesPanel *sourcesColumn = nullptr;
	QWidget *colorPanel = nullptr;
	QWidget *panHolder = nullptr;
	QVBoxLayout *rightLayout = nullptr;
	QHBoxLayout *middleLayout = nullptr;
	bool colorHere = false;
	bool watchingNow = false;
	int streamShownLive = -1;
	int recordShown = -1;
	QWidget *verticalHolder = nullptr;
	QWidget *verticalPreviewHolder = nullptr;
	QPushButton *streamButton = nullptr;
	QPushButton *recordButton = nullptr;
	QTimer *timer = nullptr;

	std::mutex previewMutex;
	obs_weak_source_t *previewScene = nullptr;
};

QPointer<StudioWindow> studioWindow;

void openSourcesDialog()
{
	if (studioWindow) {
		studioWindow->showSources();
	}
}

void cutFromAnywhere()
{
	StudioWindow::cutNow();
}

void transitionFromAnywhere()
{
	StudioWindow::transitionNow();
}
bool started = false;

void StudioWindow::enterObs()
{
	saveMode(MODE_OBS);
	setObsShowing(true);
	pl_talk_key_set_active(false);
	if (studioWindow) {
		studioWindow->hide();
	}
	/* O Acompanhar some junto (é parte da tela de operar). */
	pl_watch_update(nullptr);
}

/* CORTAR: troca na hora, sem a transição. Usa a transição "Cortar" do OBS só para este corte e
 * volta para a transição escolhida logo depois (no OBS isso tudo acontece de uma vez). */
void StudioWindow::cutNow()
{
	obs_source_t *current = obs_frontend_get_current_transition();
	obs_source_t *cut = nullptr;

	struct obs_frontend_source_list transitions = {};
	obs_frontend_get_transitions(&transitions);
	for (size_t i = 0; i < transitions.sources.num; i++) {
		obs_source_t *each = transitions.sources.array[i];
		const char *id = obs_source_get_unversioned_id(each);
		if (id && strcmp(id, "cut_transition") == 0) {
			cut = obs_source_get_ref(each);
			break;
		}
	}
	obs_frontend_source_list_free(&transitions);

	if (cut && current && cut != current) {
		obs_frontend_set_current_transition(cut);
		obs_frontend_preview_program_trigger_transition();
		obs_frontend_set_current_transition(current);
	} else {
		obs_frontend_preview_program_trigger_transition();
	}

	obs_source_release(cut);
	obs_source_release(current);
}

void enterLive()
{
	saveMode(MODE_LIVE);
	if (!obs_frontend_preview_program_mode_active()) {
		obs_frontend_set_preview_program_mode(true);
	}
	if (!studioWindow) {
		pl_talk_key_install();
		/* As fontes da entrada saíram da tela: abrem numa janelinha pelo botão direito. */
		pl_entries_set_sources_opener([]() {
			if (studioWindow) {
				studioWindow->showSources();
			}
		});
		studioWindow = new StudioWindow();
		/* Abre na mesma tela em que estava a janela do OBS. */
		if (QWidget *main = obsWindow()) {
			if (QScreen *screen = main->screen()) {
				studioWindow->move(screen->availableGeometry().topLeft());
			}
		}
	}
	studioWindow->showMaximized();
	studioWindow->raise();
	studioWindow->activateWindow();
	setObsShowing(false);
}

void toolsMenuClicked(void *)
{
	if (studioWindow && studioWindow->isVisible()) {
		StudioWindow::enterObs();
	} else {
		enterLive();
	}
}

void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING && !started) {
		started = true;
		if (loadMode() == MODE_LIVE) {
			/* Depois de o OBS terminar de mostrar a janela dele. */
			QTimer::singleShot(0, []() { enterLive(); });
		}
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		pl_talk_key_release();
		pl_entries_set_sources_opener(nullptr);
		if (studioWindow) {
			studioWindow->releaseScreens();
			delete studioWindow.data();
		}
	}
}

} // namespace

void pl_studio_window_enter_live(void)
{
	enterLive();
}

void pl_studio_window_load(void)
{
	obs_frontend_add_event_callback(frontendEvent, nullptr);
	obs_frontend_add_tools_menu_item(obs_module_text("Studio.ToolsMenu"), toolsMenuClicked, nullptr);
}

void pl_studio_window_unload(void)
{
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
	pl_talk_key_release();
	pl_entries_set_sources_opener(nullptr);
	if (studioWindow) {
		studioWindow->releaseScreens();
		delete studioWindow.data();
	}
	pl_watch_release();
}

QWidget *pl_studio_screens_create(QWidget *parent)
{
	return new ScreensPair(parent);
}
