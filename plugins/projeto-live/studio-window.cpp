/*
 * Projeto Live: a janela própria do Projeto Live.
 *
 * Dois modos, um clique:
 * - modo Projeto Live: esta janela, com a cara do produto. Em cima, a faixa Situação; à esquerda, as
 *   fontes da entrada da prévia e o Diretor; no meio, Prévia e Programa lado a lado com CORTAR e
 *   Transição; embaixo, Entradas e Som; no rodapé, Transmitir, Gravar e Configurações. A janela do
 *   OBS fica escondida. Os painéis são cópias dos mesmos painéis do OBS (mesmo código);
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

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QCloseEvent>
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

#include "audio-dock.h"
#include "camera-dock.h"
#include "director-dock.h"
#include "source-row.hpp"
#include "stats-dock.h"
#include "vertical-dock.h"

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
		display = obs_display_create(&info, 0xFF101014);
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

/* Uma das duas telas grandes (Prévia ou Programa), com a moldura colorida e o título. */
class Monitor : public QFrame {
public:
	Monitor(QWidget *parent, const QString &title, const char *color, PLScreen::DrawFn fn, void *owner)
		: QFrame(parent)
	{
		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(0, 0, 0, 0);
		outer->setSpacing(4);

		auto *head = new QHBoxLayout();
		auto *label = new QLabel(title, this);
		QFont bold = label->font();
		bold.setBold(true);
		label->setFont(bold);
		label->setStyleSheet(QString("color: %1;").arg(color));
		head->addWidget(label);
		head->addStretch(1);
		sceneName = new QLabel(this);
		head->addWidget(sceneName);
		outer->addLayout(head);

		auto *frame = new QFrame(this);
		frame->setObjectName("plMonitor");
		frame->setStyleSheet(
			QString("QFrame#plMonitor { border: 2px solid %1; border-radius: 6px; }").arg(color));
		auto *inner = new QVBoxLayout(frame);
		inner->setContentsMargins(2, 2, 2, 2);
		view = new PLScreen(frame, fn, owner);
		inner->addWidget(view);
		outer->addWidget(frame, 1);
	}

	void setSceneName(const QString &text)
	{
		if (sceneName->text() != text) {
			sceneName->setText(text);
		}
	}

	PLScreen *view = nullptr;

private:
	QLabel *sceneName = nullptr;
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
		setStyleSheet("QFrame#plSources { border: 1px solid #33333d; border-radius: 6px; }");
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
		connect(addButton, &QToolButton::clicked, this, [this]() { addSource(); });
		head->addWidget(addButton);
		gearButton = plIconButton(this, "icon-gear", ":/settings/images/settings/general.svg",
					  msg("Studio.Properties"));
		connect(gearButton, &QToolButton::clicked, this, [this]() { openProperties(); });
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
		pages->addWidget(pl_vertical_sources_create(pages));

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
	/* A página vertical tem o próprio "+"; o "+ Fonte" de cima é só da horizontal. */
	void showPage(int index)
	{
		pages->setCurrentIndex(index);
		addButton->setVisible(index == 0);
		gearButton->setVisible(index == 0);
	}

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
	QLabel *entryName = nullptr;
	QListWidget *list = nullptr;
	QStringList shown;
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

		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(10, 6, 10, 8);
		outer->setSpacing(8);

		/* Os menus do OBS (Arquivo, Editar, Visualizar...): os mesmos, não uma cópia. */
		auto *bar = new QMenuBar(this);
		/* Dentro da janela também no Mac (lá o OBS já usa a barra de cima da tela). */
		bar->setNativeMenuBar(false);
		if (auto *main = qobject_cast<QMainWindow *>(obsWindow())) {
			for (QAction *action : main->menuBar()->actions()) {
				bar->addAction(action);
			}
		}
		outer->setMenuBar(bar);

		/* Faixa de cima: nome do produto, a faixa Situação e o botão de voltar ao OBS. */
		auto *top = new QHBoxLayout();
		auto *brand = new QLabel(msg("Studio.Brand"), this);
		QFont brandFont = brand->font();
		brandFont.setBold(true);
		brandFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
		brand->setFont(brandFont);
		top->addWidget(brand);
		top->addSpacing(12);
		top->addWidget(pl_stats_dock_create(this), 1);
		auto *toObs = new QPushButton(msg("Studio.ToObs"), this);
		toObs->setToolTip(msg("Studio.ToObsHint"));
		connect(toObs, &QPushButton::clicked, this, []() { enterObs(); });
		top->addWidget(toObs);
		outer->addLayout(top);

		auto *body = new QHBoxLayout();
		body->setSpacing(10);

		/* Coluna da esquerda: as fontes da entrada da prévia e o Diretor. */
		auto *left = new QVBoxLayout();
		left->setSpacing(10);
		sources = new SourcesPanel(this);
		left->addWidget(sources, 2);
		auto *director = pl_director_dock_create(this);
		left->addWidget(director, 3);
		auto *leftHolder = new QWidget(this);
		leftHolder->setLayout(left);
		left->setContentsMargins(0, 0, 0, 0);
		leftHolder->setFixedWidth(330);
		body->addWidget(leftHolder);

		auto *right = new QVBoxLayout();
		right->setSpacing(8);

		/* Prévia | CORTAR e Transição | Programa */
		auto *middle = new QHBoxLayout();
		middle->setSpacing(12);

		preview = new Monitor(this, msg("Studio.Preview"), "#2fa84f", drawPreview, this);
		middle->addWidget(preview, 1);

		auto *center = new QVBoxLayout();
		center->addStretch(1);
		auto *cut = new QPushButton(msg("Studio.Cut"), this);
		cut->setMinimumSize(110, 48);
		QFont cutFont = cut->font();
		cutFont.setBold(true);
		cut->setFont(cutFont);
		cut->setStyleSheet("QPushButton { background: #e5352b; color: white; border-radius: 6px; }"
				   "QPushButton:hover { background: #f0473d; }"
				   "QPushButton:pressed { background: #c22a22; }");
		cut->setToolTip(msg("Studio.CutHint"));
		connect(cut, &QPushButton::clicked, this, []() { cutNow(); });
		center->addWidget(cut);
		auto *transition = new QPushButton(msg("Studio.Transition"), this);
		transition->setMinimumSize(110, 34);
		transition->setToolTip(msg("Studio.TransitionHint"));
		connect(transition, &QPushButton::clicked, this, []() { transitionNow(); });
		center->addWidget(transition);
		transitionInfo = new QLabel(this);
		transitionInfo->setAlignment(Qt::AlignCenter);
		transitionInfo->setEnabled(false);
		center->addWidget(transitionInfo);
		center->addStretch(1);
		middle->addLayout(center);

		program = new Monitor(this, msg("Studio.Program"), "#e5352b", drawProgram, this);
		middle->addWidget(program, 1);

		/* O vertical, em pé, ao lado do Programa (some quando está desligado em Painéis). */
		verticalHolder = new QWidget(this);
		auto *verticalLayout = new QVBoxLayout(verticalHolder);
		verticalLayout->setContentsMargins(0, 0, 0, 0);
		verticalLayout->addWidget(pl_vertical_program_create(verticalHolder));
		middle->addWidget(verticalHolder);

		right->addLayout(middle, 5);

		/* Entradas: a mesma lista do painel do OBS (outra cópia, que mostra as mesmas cenas). */
		right->addWidget(pl_camera_dock_create(this), 3);

		/* Som: o mesmo painel Som do OBS (outra cópia). */
		auto *audio = pl_audio_dock_create(this);
		audio->setMaximumHeight(170);
		right->addWidget(audio, 2);

		body->addLayout(right, 1);
		outer->addLayout(body, 1);

		/* Rodapé: transmitir, gravar e as configurações do OBS. */
		auto *footer = new QHBoxLayout();
		streamButton = new QPushButton(this);
		streamButton->setMinimumHeight(34);
		connect(streamButton, &QPushButton::clicked, this, [this]() { toggleStream(); });
		footer->addWidget(streamButton);
		recordButton = new QPushButton(this);
		recordButton->setMinimumHeight(34);
		connect(recordButton, &QPushButton::clicked, this, []() { toggleRecord(); });
		footer->addWidget(recordButton);
		footer->addStretch(1);
		auto *studioNote = new QLabel(msg("Studio.StudioNote"), this);
		studioNote->setEnabled(false);
		footer->addWidget(studioNote);
		auto *settings = new QPushButton(msg("Studio.Settings"), this);
		settings->setMinimumHeight(34);
		connect(settings, &QPushButton::clicked, this, []() { openSettings(); });
		footer->addWidget(settings);
		outer->addLayout(footer);

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
			preview->view->destroyDisplay();
		}
		if (program) {
			program->view->destroyDisplay();
		}
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

private:
	void refresh()
	{
		preview->view->syncSize();
		program->view->syncSize();
		sources->refresh();
		updateFooter();
		bool vertical = pl_vertical_program_wanted();
		if (verticalHolder->isHidden() == vertical) {
			verticalHolder->setVisible(vertical);
		}

		obs_source_t *previewSource = obs_frontend_get_current_preview_scene();
		obs_source_t *programSource = obs_frontend_get_current_scene();

		{
			std::lock_guard<std::mutex> lock(previewMutex);
			obs_weak_source_release(previewScene);
			previewScene = previewSource ? obs_source_get_weak_source(previewSource) : nullptr;
		}

		preview->setSceneName(previewSource ? QString::fromUtf8(obs_source_get_name(previewSource))
						    : QString());
		program->setSceneName(programSource ? QString::fromUtf8(obs_source_get_name(programSource))
						    : QString());
		obs_source_release(previewSource);
		obs_source_release(programSource);

		obs_source_t *transition = obs_frontend_get_current_transition();
		QString info;
		if (transition) {
			info = QString("%1 · %2 ms")
				       .arg(QString::fromUtf8(obs_source_get_name(transition)))
				       .arg(obs_frontend_get_transition_duration());
			obs_source_release(transition);
		}
		if (transitionInfo->text() != info) {
			transitionInfo->setText(info);
		}
	}

	void updateFooter()
	{
		bool streaming = obs_frontend_streaming_active();
		QString streamText = msg(streaming ? "Studio.StreamStop" : "Studio.StreamStart");
		if (streamButton->text() != streamText) {
			streamButton->setText(streamText);
			streamButton->setStyleSheet(streaming ? "QPushButton { background: #1f4fa8; color: white; }"
							      : QString());
		}
		bool recording = obs_frontend_recording_active();
		QString recordText = msg(recording ? "Studio.RecordStop" : "Studio.RecordStart");
		if (recordButton->text() != recordText) {
			recordButton->setText(recordText);
			recordButton->setStyleSheet(recording ? "QPushButton { background: #8c2a24; color: white; }"
							      : QString());
		}
	}

	/* Parar a transmissão pergunta antes (um clique sem querer derrubaria a live). */
	void toggleStream()
	{
		if (!obs_frontend_streaming_active()) {
			obs_frontend_streaming_start();
			return;
		}
		if (QMessageBox::question(this, msg("Studio.StreamStop"), msg("Studio.StreamStopConfirm")) ==
		    QMessageBox::Yes) {
			obs_frontend_streaming_stop();
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
	QLabel *transitionInfo = nullptr;
	SourcesPanel *sources = nullptr;
	QWidget *verticalHolder = nullptr;
	QPushButton *streamButton = nullptr;
	QPushButton *recordButton = nullptr;
	QTimer *timer = nullptr;

	std::mutex previewMutex;
	obs_weak_source_t *previewScene = nullptr;
};

QPointer<StudioWindow> studioWindow;
bool started = false;

void StudioWindow::enterObs()
{
	saveMode(MODE_OBS);
	setObsShowing(true);
	if (studioWindow) {
		studioWindow->hide();
	}
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
	if (studioWindow) {
		studioWindow->releaseScreens();
		delete studioWindow.data();
	}
}
