/*
 * Projeto Live: painel "Entradas".
 *
 * Cada entrada é uma cena do OBS. Dentro dela ficam as fontes que a pessoa quiser (câmera, legenda,
 * letterbox, logo em PNG, vídeo, telão), e a entrada mostra a cena inteira. Uma fonte que está em
 * duas cenas é a mesma fonte (o OBS não duplica), por isso o botão direito reaproveita o que já
 * existe no OBS.
 *
 * Cada miniatura mostra:
 * - a cena ao vivo (uma tela do OBS por miniatura, igual à janela de vídeo do vertical);
 * - a moldura: vermelha no ar, verde na prévia, amarela quando a câmera dentro dela travou,
 *   vermelha escura sem sinal, cinza no resto;
 * - o nome, o atraso da câmera (se a entrada tiver uma) e uma barrinha com o som da entrada;
 * - a bolinha de sinal.
 *
 * Um clique manda a entrada para a prévia e dois cliques cortam para o ar.
 *
 * Botão direito:
 * - no espaço vazio: "Nova entrada" (cria uma cena e abre a janela "Adicionar fonte" do OBS);
 * - numa entrada: "Adicionar fonte" (a janela original do OBS, com todos os tipos de fonte e as que
 *   já existem), "Organizar fontes" (abre o painel Fontes do OBS já mostrando esta entrada),
 *   renomear ou remover a entrada.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "camera-dock.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

#include <util/platform.h>

#include <QAction>
#include <QColor>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QEvent>
#include <QFont>
#include <QApplication>
#include <QFrame>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QList>
#include <QMargins>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPlatformSurfaceEvent>
#include <QPoint>
#include <QDockWidget>
#include <QPointer>
#include <QRect>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShowEvent>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "phone-source.h"

namespace {

const char *const SIZE_FILE = "cameras.json";

const float SILENCE_DB = -60.0f;

/* Larguras da miniatura: pequeno, médio, grande. */
const int CARD_WIDTHS[3] = {170, 230, 310};

enum class Tally { Off, Preview, Program };

enum class Signal { None, Stalled, Live };

/* Texto do módulo (pt-BR ou en-US, conforme o OBS). */
QString msg(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* Barrinha com o som que vem daquela entrada. */
class Level : public QWidget {
public:
	explicit Level(QWidget *parent) : QWidget(parent) { setFixedHeight(3); }

	void setValue(int percent)
	{
		percent = std::clamp(percent, 0, 100);
		if (percent != value) {
			value = percent;
			update();
		}
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.fillRect(rect(), QColor(0x2b, 0x2b, 0x33));
		int filled = width() * value / 100;
		if (filled > 0) {
			painter.fillRect(0, 0, filled, height(), QColor(0x6d, 0xdb, 0x8c));
		}
	}

private:
	int value = 0;
};

/* ------------------------------------------------------------------------- */
/* Layout que quebra linha sozinho (as miniaturas viram duas fileiras na tela grande) */

class FlowLayout : public QLayout {
public:
	explicit FlowLayout(QWidget *parent) : QLayout(parent) { setContentsMargins(0, 0, 0, 0); }

	~FlowLayout() override
	{
		while (QLayoutItem *item = takeAt(0)) {
			delete item;
		}
	}

	void addItem(QLayoutItem *item) override { items.append(item); }
	int count() const override { return items.size(); }
	QLayoutItem *itemAt(int index) const override { return items.value(index); }

	QLayoutItem *takeAt(int index) override
	{
		if (index < 0 || index >= items.size()) {
			return nullptr;
		}
		return items.takeAt(index);
	}

	Qt::Orientations expandingDirections() const override { return {}; }
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int width) const override { return layoutRows(QRect(0, 0, width, 0), false); }

	void setGeometry(const QRect &rect) override
	{
		QLayout::setGeometry(rect);
		layoutRows(rect, true);
	}

	QSize sizeHint() const override { return minimumSize(); }

	QSize minimumSize() const override
	{
		QSize size;
		for (QLayoutItem *item : items) {
			size = size.expandedTo(item->minimumSize());
		}
		QMargins margins = contentsMargins();
		return size + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
	}

private:
	int layoutRows(const QRect &rect, bool place) const
	{
		QMargins margins = contentsMargins();
		QRect area = rect.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
		int x = area.x();
		int y = area.y();
		int rowHeight = 0;
		for (QLayoutItem *item : items) {
			QSize hint = item->sizeHint();
			int next = x + hint.width() + GAP;
			if (rowHeight > 0 && next - GAP > area.right() + 1) {
				x = area.x();
				y += rowHeight + GAP;
				rowHeight = 0;
			}
			if (place) {
				item->setGeometry(QRect(QPoint(x, y), hint));
			}
			x += hint.width() + GAP;
			rowHeight = std::max(rowHeight, hint.height());
		}
		return y + rowHeight - rect.y() + margins.bottom();
	}

	static const int GAP = 8;
	QList<QLayoutItem *> items;
};

/* ------------------------------------------------------------------------- */
/* Fontes dentro de uma cena                                                 */

using ItemList = std::vector<obs_sceneitem_t *>;

/* Os itens de uma cena, cada um com referência. Quem chama solta com releaseItems. */
ItemList sceneItems(obs_source_t *sceneSource)
{
	ItemList items;
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (scene) {
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				auto *list = static_cast<ItemList *>(param);
				obs_sceneitem_addref(item);
				list->push_back(item);
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

/* A primeira câmera ("Celular") dentro desta cena, com referência; nulo se não houver. */
obs_source_t *firstPhone(const ItemList &items)
{
	for (obs_sceneitem_t *item : items) {
		obs_source_t *src = obs_sceneitem_get_source(item);
		if (src && pl_phone_is_source(src)) {
			return obs_source_get_ref(src);
		}
	}
	return nullptr;
}

/* Nome que ainda não existe no OBS: "Palco", "Palco 2", "Palco 3"... */
QString freeName(const QString &base)
{
	QString name = base;
	for (int n = 2;; n++) {
		obs_source_t *taken = obs_get_source_by_name(name.toUtf8().constData());
		if (!taken) {
			return name;
		}
		obs_source_release(taken);
		name = QString("%1 %2").arg(base).arg(n);
	}
}

bool nameTaken(const QString &name)
{
	obs_source_t *taken = obs_get_source_by_name(name.toUtf8().constData());
	obs_source_release(taken);
	return taken != nullptr;
}

/* ------------------------------------------------------------------------- */

class Card;

/* Decide o que o botão direito de uma entrada (ou do espaço vazio) mostra. */
void showCardMenu(Card *card, const QPoint &where);
void showEmptyMenu(const QPoint &where);
void newEntry();

/* A imagem ao vivo de uma miniatura: uma tela do OBS, como a janela de vídeo do vertical. */
class Thumb : public QWidget {
public:
	Thumb(Card *card, obs_source_t *source);
	~Thumb() override;

	QPaintEngine *paintEngine() const override { return nullptr; }

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void contextMenuEvent(QContextMenuEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	static void draw(void *data, uint32_t cx, uint32_t cy);

	QSize pixelSize() const
	{
		qreal ratio = devicePixelRatioF();
		return QSize((int)(width() * ratio), (int)(height() * ratio));
	}

	void createDisplay();
	void resizeDisplay();
	void destroyDisplay();

	Card *card = nullptr;
	obs_weak_source_t *weak = nullptr;
	obs_display_t *display = nullptr;
};

/* ------------------------------------------------------------------------- */

/* Uma miniatura inteira (uma entrada = uma cena): imagem, moldura, nome, atraso e barrinha. */
class Card : public QFrame {
public:
	Card(QWidget *parent, obs_source_t *scene) : QFrame(parent)
	{
		weak = obs_source_get_weak_source(scene);

		setObjectName("plCard");
		setFrameShape(QFrame::NoFrame);
		setCursor(Qt::PointingHandCursor);
		setToolTip(msg("Cameras.Hint"));

		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(3, 3, 3, 3);
		layout->setSpacing(4);

		thumb = new Thumb(this, scene);
		layout->addWidget(thumb);

		auto *row = new QHBoxLayout();
		row->setContentsMargins(0, 0, 0, 0);
		row->setSpacing(5);

		dot = new QLabel(this);
		dot->setFixedSize(8, 8);
		row->addWidget(dot);

		badge = new QLabel(this);
		QFont badgeFont = badge->font();
		badgeFont.setBold(true);
		badgeFont.setPointSizeF(std::max(badgeFont.pointSizeF() - 2.0, 6.0));
		badge->setFont(badgeFont);
		badge->setVisible(false);
		row->addWidget(badge);

		name = new QLabel(this);
		name->setTextInteractionFlags(Qt::NoTextInteraction);
		row->addWidget(name, 1);

		detail = new QLabel(this);
		detail->setEnabled(false);
		row->addWidget(detail);

		layout->addLayout(row);

		level = new Level(this);
		layout->addWidget(level);

		setSize(1);
	}

	~Card() override
	{
		releaseMeter();
		obs_weak_source_release(weak);
	}

	/* A cena desta entrada, com referência (quem chama solta). */
	obs_source_t *source() const { return obs_weak_source_get_source(weak); }

	bool isSource(obs_source_t *other) const
	{
		obs_source_t *mine = source();
		bool same = mine == other;
		obs_source_release(mine);
		return same;
	}

	void setSize(int index)
	{
		int cardWidth = CARD_WIDTHS[std::clamp(index, 0, 2)];
		thumb->setFixedSize(cardWidth, cardWidth * 9 / 16);
		setFixedWidth(cardWidth + 6);
	}

	/* Um clique: manda para a prévia (só funciona com o modo estúdio ligado). */
	void sendToPreview()
	{
		obs_source_t *scene = source();
		if (!scene) {
			return;
		}
		if (obs_frontend_preview_program_mode_active()) {
			obs_frontend_set_current_preview_scene(scene);
		}
		obs_source_release(scene);
	}

	/* Dois cliques: corta para o ar. */
	void sendToProgram()
	{
		obs_source_t *scene = source();
		if (!scene) {
			return;
		}
		obs_frontend_set_current_scene(scene);
		obs_source_release(scene);
	}

	/* Chamado pelo relógio do painel. */
	void refresh()
	{
		obs_source_t *src = source();
		if (!src) {
			return;
		}

		name->setText(QString::fromUtf8(obs_source_get_name(src)));

		Tally tally = Tally::Off;
		if (obs_source_active(src)) {
			tally = Tally::Program;
		} else if (obs_source_showing(src)) {
			tally = Tally::Preview;
		}

		ItemList items = sceneItems(src);
		bool blank = items.empty();
		obs_source_t *cam = firstPhone(items);
		releaseItems(items);

		Signal signal = Signal::Live;
		QString info;
		if (cam) {
			struct pl_phone_status status;
			if (pl_phone_get_status(cam, &status)) {
				if (status.stalled_ms < 0) {
					signal = Signal::None;
				} else if (status.stalled_ms >= PL_PHONE_STALL_MS) {
					signal = Signal::Stalled;
				}
				if (signal == Signal::Live && status.latency_ms >= 0) {
					info = QString::number(status.latency_ms / 1000.0, 'f', 2) + " s";
				} else if (signal == Signal::Stalled) {
					info = msg("Cameras.Stalled");
				} else {
					info = msg("Cameras.NoSignal");
				}
				if (signal == Signal::Live && status.usb) {
					info = msg("Cameras.Cable");
				}
			}
			obs_source_release(cam);
		} else if (blank) {
			signal = Signal::None;
			info = msg("Entries.Blank");
		}

		detail->setText(info);
		applyColors(tally, signal);
		updateLevel(src);

		obs_source_release(src);
	}

protected:
	void contextMenuEvent(QContextMenuEvent *event) override
	{
		showCardMenu(this, event->globalPos());
		event->accept();
	}

private:
	void applyColors(Tally tally, Signal signal)
	{
		const char *border = "#3a3a44";
		const char *badgeText = nullptr;
		const char *badgeColor = nullptr;
		if (tally == Tally::Program) {
			border = "#e5352b";
			badgeText = "Cameras.OnAir";
			badgeColor = "#ff8b84";
		} else if (tally == Tally::Preview) {
			border = "#2fa84f";
			badgeText = "Cameras.Preview";
			badgeColor = "#6ddb8c";
		}
		/* A imagem travada ou sem sinal avisa com a moldura amarela, mesmo fora do ar. */
		if (signal != Signal::Live) {
			if (signal == Signal::Stalled) {
				border = "#e8a33d";
			} else {
				border = "#8c3a33";
			}
		}

		if (badgeText) {
			if (shownBadge != badgeText) {
				shownBadge = badgeText;
				badge->setText(QString::fromUtf8(obs_module_text(badgeText)));
				badge->setStyleSheet(QString("color: %1;").arg(badgeColor));
			}
			badge->setVisible(true);
		} else {
			shownBadge.clear();
			badge->setVisible(false);
		}

		const char *dotColor = "#e5352b";
		if (signal == Signal::Live) {
			dotColor = "#2fa84f";
		} else if (signal == Signal::Stalled) {
			dotColor = "#e8a33d";
		}
		if (shownDot != dotColor) {
			shownDot = dotColor;
			dot->setStyleSheet(QString("background: %1; border-radius: 4px;").arg(dotColor));
		}
		if (shownBorder != border) {
			shownBorder = border;
			setStyleSheet(
				QString("QFrame#plCard { border: 2px solid %1; border-radius: 6px; }").arg(border));
		}
	}

	void updateLevel(obs_source_t *src)
	{
		if (!meter) {
			if (!(obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO)) {
				level->setVisible(false);
				return;
			}
			meter = obs_volmeter_create(OBS_FADER_LOG);
			obs_volmeter_attach_source(meter, src);
			obs_volmeter_add_callback(meter, levelCallback, this);
		}
		float db = peak.exchange(SILENCE_DB);
		if (db < SILENCE_DB) {
			db = SILENCE_DB;
		}
		int percent = (int)std::lround((db - SILENCE_DB) / -SILENCE_DB * 100.0f);
		percent = std::clamp(percent, 0, 100);
		/* Sobe na hora e desce devagar, para o olho acompanhar. */
		shown = std::max(percent, shown - 7);
		level->setValue(shown);
	}

	static void levelCallback(void *param, const float magnitude[MAX_AUDIO_CHANNELS],
				  const float peakValues[MAX_AUDIO_CHANNELS], const float inputPeak[MAX_AUDIO_CHANNELS])
	{
		(void)magnitude;
		(void)inputPeak;
		auto *card = static_cast<Card *>(param);
		float value = std::isfinite(peakValues[0]) ? peakValues[0] : SILENCE_DB;
		float old = card->peak.load();
		while (value > old && !card->peak.compare_exchange_weak(old, value)) {
		}
	}

	obs_weak_source_t *weak = nullptr;
	std::string shownBorder;
	std::string shownBadge;
	std::string shownDot;
	Thumb *thumb = nullptr;
	QLabel *dot = nullptr;
	QLabel *badge = nullptr;
	QLabel *name = nullptr;
	QLabel *detail = nullptr;
	Level *level = nullptr;
	obs_volmeter_t *meter = nullptr;
	std::atomic<float> peak{SILENCE_DB};
	int shown = 0;

public:
	void releaseMeter()
	{
		if (meter) {
			obs_volmeter_remove_callback(meter, levelCallback, this);
			obs_volmeter_detach_source(meter);
			obs_volmeter_destroy(meter);
			meter = nullptr;
		}
	}
};

/* ------------------------------------------------------------------------- */
/* Thumb (depois de Card, para poder chamar os métodos dele)                 */

Thumb::Thumb(Card *card, obs_source_t *source) : QWidget(card), card(card)
{
	weak = obs_source_get_weak_source(source);

	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);

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

Thumb::~Thumb()
{
	destroyDisplay();
	obs_weak_source_release(weak);
}

void Thumb::paintEvent(QPaintEvent *event)
{
	createDisplay();
	QWidget::paintEvent(event);
}

void Thumb::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	createDisplay();
	resizeDisplay();
}

void Thumb::mousePressEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton && card) {
		card->sendToPreview();
	}
	QWidget::mousePressEvent(event);
}

void Thumb::mouseDoubleClickEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton && card) {
		card->sendToProgram();
	}
	QWidget::mouseDoubleClickEvent(event);
}

void Thumb::contextMenuEvent(QContextMenuEvent *event)
{
	if (card) {
		showCardMenu(card, event->globalPos());
	}
	event->accept();
}

/* Escondida (a janela do OBS ou a do Projeto Live fora de uso): para de desenhar, para não gastar a placa. */
void Thumb::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	if (display) {
		obs_display_set_enabled(display, true);
	}
}

void Thumb::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);
	if (display) {
		obs_display_set_enabled(display, false);
	}
}

bool Thumb::eventFilter(QObject *object, QEvent *event)
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

void Thumb::draw(void *data, uint32_t cx, uint32_t cy)
{
	auto *thumb = static_cast<Thumb *>(data);
	obs_source_t *src = obs_weak_source_get_source(thumb->weak);
	if (!src) {
		return;
	}
	uint32_t sw = obs_source_get_width(src);
	uint32_t sh = obs_source_get_height(src);
	if (!sw || !sh || !cx || !cy) {
		obs_source_release(src);
		return;
	}

	/* Encaixa a cena inteira na miniatura, sem deformar. */
	float scale = std::min((float)cx / (float)sw, (float)cy / (float)sh);
	int w = std::max((int)std::lround(sw * scale), 1);
	int h = std::max((int)std::lround(sh * scale), 1);
	int x = ((int)cx - w) / 2;
	int y = ((int)cy - h) / 2;

	gs_projection_push();
	gs_viewport_push();
	gs_set_viewport(x, y, w, h);
	gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
	obs_source_video_render(src);
	gs_viewport_pop();
	gs_projection_pop();

	obs_source_release(src);
}

void Thumb::createDisplay()
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
	/* Igual ao OBS (OBSQTDisplay): cada sistema entrega a janela de um jeito. */
#ifdef _WIN32
	info.window.hwnd = (void *)windowHandle()->winId();
#elif defined(__APPLE__)
	info.window.view = (id)windowHandle()->winId();
#else
	if (obs_get_nix_platform() != OBS_NIX_PLATFORM_X11_EGL) {
		/* Wayland: as miniaturas ainda não foram feitas para lá. */
		return;
	}
	info.window.id = (uint32_t)windowHandle()->winId();
	info.window.display = obs_get_nix_platform_display();
#endif
	display = obs_display_create(&info, 0xFF17171C);
	if (display) {
		obs_display_add_draw_callback(display, draw, this);
	}
}

void Thumb::resizeDisplay()
{
	if (display && isVisible()) {
		QSize size = pixelSize();
		obs_display_resize(display, (uint32_t)std::max(size.width(), 1), (uint32_t)std::max(size.height(), 1));
	}
}

void Thumb::destroyDisplay()
{
	if (display) {
		obs_display_remove_draw_callback(display, draw, this);
		obs_display_destroy(display);
		display = nullptr;
	}
}

/* ------------------------------------------------------------------------- */

/* O cartão pontilhado "+ Entrada", sempre o último da lista: cria uma entrada nova. */
class AddCard : public QFrame {
public:
	explicit AddCard(QWidget *parent) : QFrame(parent)
	{
		setObjectName("plAddCard");
		setCursor(Qt::PointingHandCursor);
		setToolTip(msg("Entries.New"));
		setStyleSheet("QFrame#plAddCard { border: 2px dashed #4a4a55; border-radius: 6px; }");
		auto *layout = new QVBoxLayout(this);
		auto *label = new QLabel(msg("Entries.AddCard"), this);
		label->setAlignment(Qt::AlignCenter);
		label->setEnabled(false);
		layout->addWidget(label);
	}

	void setSize(int index)
	{
		int cardWidth = CARD_WIDTHS[std::clamp(index, 0, 2)];
		/* Mesma altura do cartão de entrada: imagem 16:9 mais a linha do nome. */
		setFixedSize(cardWidth + 6, cardWidth * 9 / 16 + 34);
	}

protected:
	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton && rect().contains(event->pos())) {
			newEntry();
		}
		QFrame::mouseReleaseEvent(event);
	}
};

class CameraDock;
std::vector<CameraDock *> instances;

class CameraDock : public QWidget {
public:
	explicit CameraDock(QWidget *parent) : QWidget(parent)
	{
		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(6, 6, 6, 6);
		outer->setSpacing(6);

		auto *top = new QHBoxLayout();
		top->setContentsMargins(0, 0, 0, 0);
		top->setSpacing(6);

		hint = new QLabel(msg("Cameras.Hint"), this);
		hint->setEnabled(false);
		top->addWidget(hint, 1);

		sizeBox = new QComboBox(this);
		sizeBox->addItem(msg("Cameras.Size.Small"));
		sizeBox->addItem(msg("Cameras.Size.Medium"));
		sizeBox->addItem(msg("Cameras.Size.Large"));
		sizeBox->setCurrentIndex(loadSize());
		connect(sizeBox, &QComboBox::currentIndexChanged, this, [this](int index) {
			for (Card *card : cards) {
				card->setSize(index);
			}
			if (addCard) {
				addCard->setSize(index);
			}
			saveSize(index);
		});
		top->addWidget(sizeBox);

		outer->addLayout(top);

		/* Sem entradas, o aviso também aceita o botão direito (é daqui que se cria a primeira). */
		empty = new QLabel(msg("Cameras.Empty"), this);
		empty->setWordWrap(true);
		empty->setContextMenuPolicy(Qt::CustomContextMenu);
		connect(empty, &QWidget::customContextMenuRequested, this,
			[this](const QPoint &pos) { showEmptyMenu(empty->mapToGlobal(pos)); });
		outer->addWidget(empty);

		area = new QScrollArea(this);
		area->setWidgetResizable(true);
		area->setFrameShape(QFrame::NoFrame);
		holder = new QWidget(area);
		holder->setContextMenuPolicy(Qt::CustomContextMenu);
		connect(holder, &QWidget::customContextMenuRequested, this,
			[this](const QPoint &pos) { showEmptyMenu(holder->mapToGlobal(pos)); });
		flow = new FlowLayout(holder);
		addCard = new AddCard(holder);
		addCard->setSize(sizeBox->currentIndex());
		flow->addWidget(addCard);
		area->setWidget(holder);
		outer->addWidget(area, 1);

		timer = new QTimer(this);
		timer->setInterval(200);
		connect(timer, &QTimer::timeout, this, [this]() {
			bool deep = ++ticks % 5 == 0;
			if (deep) {
				rebuild();
			}
			for (Card *card : cards) {
				card->refresh();
			}
		});
		timer->start();

		instances.push_back(this);
		rebuild();
	}

	~CameraDock() override
	{
		instances.erase(std::remove(instances.begin(), instances.end(), this), instances.end());
	}

	/* Refaz a lista agora (depois de criar ou remover uma entrada). */
	void rebuildNow() { rebuild(); }

	/* A miniatura desta cena, se ela já existir no painel. */
	Card *cardFor(obs_source_t *scene) const
	{
		for (Card *card : cards) {
			if (card->isSource(scene)) {
				return card;
			}
		}
		return nullptr;
	}

private:
	/* Uma miniatura por cena do OBS, na mesma ordem da lista de cenas. */
	void rebuild()
	{
		std::vector<obs_source_t *> found;
		struct obs_frontend_source_list scenes = {};
		obs_frontend_get_scenes(&scenes);
		for (size_t i = 0; i < scenes.sources.num; i++) {
			found.push_back(obs_source_get_ref(scenes.sources.array[i]));
		}
		obs_frontend_source_list_free(&scenes);

		/* Tira as cenas que não existem mais. */
		for (int i = (int)cards.size() - 1; i >= 0; i--) {
			Card *card = cards[(size_t)i];
			bool keep = false;
			for (obs_source_t *scene : found) {
				if (card->isSource(scene)) {
					keep = true;
					break;
				}
			}
			if (!keep) {
				cards.erase(cards.begin() + i);
				card->releaseMeter();
				flow->removeWidget(card);
				card->deleteLater();
			}
		}

		/* Põe as cenas novas. */
		bool moved = false;
		for (obs_source_t *scene : found) {
			if (cardFor(scene)) {
				continue;
			}
			auto *card = new Card(holder, scene);
			card->setSize(sizeBox->currentIndex());
			flow->addWidget(card);
			cards.push_back(card);
			moved = true;
		}

		/* Põe as miniaturas na ordem da lista de cenas do OBS. */
		std::vector<Card *> ordered;
		ordered.reserve(cards.size());
		for (obs_source_t *scene : found) {
			if (Card *card = cardFor(scene)) {
				ordered.push_back(card);
			}
		}
		if (ordered.size() == cards.size() && ordered != cards) {
			for (Card *card : cards) {
				flow->removeWidget(card);
			}
			for (Card *card : ordered) {
				flow->addWidget(card);
			}
			cards = ordered;
			moved = true;
		}

		/* O "+ Entrada" fica sempre por último. */
		if (moved) {
			flow->removeWidget(addCard);
			flow->addWidget(addCard);
		}

		for (obs_source_t *scene : found) {
			obs_source_release(scene);
		}

		empty->setVisible(cards.empty());
		hint->setVisible(!cards.empty());
	}

	static int loadSize()
	{
		char *path = obs_module_config_path(SIZE_FILE);
		obs_data_t *data = path ? obs_data_create_from_json_file_safe(path, "bak") : nullptr;
		bfree(path);
		int index = 1;
		if (data) {
			obs_data_set_default_int(data, "size", 1);
			index = (int)obs_data_get_int(data, "size");
			obs_data_release(data);
		}
		return std::clamp(index, 0, 2);
	}

	static void saveSize(int index)
	{
		obs_data_t *data = obs_data_create();
		obs_data_set_int(data, "size", index);
		char *dir = obs_module_config_path("");
		if (dir) {
			os_mkdirs(dir);
			bfree(dir);
		}
		char *path = obs_module_config_path(SIZE_FILE);
		if (path) {
			obs_data_save_json_safe(data, path, "tmp", "bak");
			bfree(path);
		}
		obs_data_release(data);
	}

	QLabel *hint = nullptr;
	QLabel *empty = nullptr;
	AddCard *addCard = nullptr;
	QComboBox *sizeBox = nullptr;
	QScrollArea *area = nullptr;
	QWidget *holder = nullptr;
	FlowLayout *flow = nullptr;
	QTimer *timer = nullptr;
	std::vector<Card *> cards;
	int ticks = 0;
};

CameraDock *dock = nullptr;

/* Janela para as perguntas (nome, remover...): a que está na frente, seja a do OBS ou a do Projeto Live. */
QWidget *dialogParent()
{
	QWidget *active = QApplication::activeWindow();
	return active ? active : static_cast<QWidget *>(obs_frontend_get_main_window());
}

/* ------------------------------------------------------------------------- */
/* Botão direito: criar, editar, renomear e remover entradas                  */

/* Abre a janela "Adicionar fonte" original do OBS, já apontada para esta entrada.
 * Essa janela põe a fonte na cena atual do OBS: com o modo estúdio ligado, é a cena da prévia, então
 * a entrada vai para a prévia antes (nada muda no ar). Sem o modo estúdio, a cena atual é a do ar, e
 * só dá para abrir sem cortar se a entrada já estiver no ar. */
void openAddSource(obs_source_t *sceneSource)
{
	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	QAction *add = mainWindow ? mainWindow->findChild<QAction *>("actionAddSource") : nullptr;
	if (!add) {
		return;
	}

	if (obs_frontend_preview_program_mode_active()) {
		obs_frontend_set_current_preview_scene(sceneSource);
	} else {
		obs_source_t *program = obs_frontend_get_current_scene();
		bool onAir = program == sceneSource;
		obs_source_release(program);
		if (!onAir) {
			QMessageBox::information(dialogParent(), msg("Entries.AddSource"), msg("Entries.NeedStudio"));
			return;
		}
	}

	/* Espera o OBS terminar de trocar a prévia antes de abrir a janela. */
	QPointer<QAction> action(add);
	QTimer::singleShot(0, add, [action]() {
		if (action) {
			action->trigger();
		}
	});
}

/* "Organizar fontes": mostra o painel Fontes original do OBS com as fontes desta entrada. O painel
 * Fontes mostra a cena atual do OBS: com o modo estúdio, é a da prévia, então a entrada vai antes
 * para a prévia (nada muda no ar). Sem o modo estúdio, só dá se a entrada já estiver no ar. */
void organizeEntry(obs_source_t *sceneSource)
{
	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *sources = mainWindow ? mainWindow->findChild<QDockWidget *>("sourcesDock") : nullptr;
	if (!sources) {
		return;
	}

	if (obs_frontend_preview_program_mode_active()) {
		obs_frontend_set_current_preview_scene(sceneSource);
	} else {
		obs_source_t *program = obs_frontend_get_current_scene();
		bool onAir = program == sceneSource;
		obs_source_release(program);
		if (!onAir) {
			QMessageBox::information(dialogParent(), msg("Entries.Organize"), msg("Entries.NeedStudio"));
			return;
		}
	}

	/* Liga o painel se estiver desligado e o traz para a frente (mesmo numa aba). */
	sources->setVisible(true);
	sources->raise();
	if (sources->isFloating()) {
		sources->activateWindow();
	}
}

void renameEntry(obs_source_t *sceneSource)
{
	QString current = QString::fromUtf8(obs_source_get_name(sceneSource));
	bool ok = false;
	QString text = QInputDialog::getText(dialogParent(), msg("Entries.Rename"), msg("Entries.RenamePrompt"),
					     QLineEdit::Normal, current, &ok)
			       .trimmed();
	if (!ok || text.isEmpty() || text == current) {
		return;
	}
	if (nameTaken(text)) {
		QMessageBox::warning(dialogParent(), msg("Entries.Rename"), msg("Entries.NameTaken"));
		return;
	}
	obs_source_set_name(sceneSource, text.toUtf8().constData());
}

void deleteEntry(obs_source_t *sceneSource)
{
	/* Não tira a cena que está no ar ou na prévia: senão a saída fica sem cena. */
	obs_source_t *program = obs_frontend_get_current_scene();
	obs_source_t *preview = obs_frontend_get_current_preview_scene();
	bool busy = sceneSource == program || sceneSource == preview;
	obs_source_release(program);
	obs_source_release(preview);
	if (busy) {
		QMessageBox::information(dialogParent(), msg("Entries.DeleteTitle"), msg("Entries.Busy"));
		return;
	}

	QString text = msg("Entries.DeleteConfirm").arg(QString::fromUtf8(obs_source_get_name(sceneSource)));
	if (QMessageBox::question(dialogParent(), msg("Entries.DeleteTitle"), text) != QMessageBox::Yes) {
		return;
	}
	obs_source_remove(sceneSource);
}

/* Cria uma cena nova (entrada). Depois abre a janela "Adicionar fonte" do OBS para ela. */
void newEntry()
{
	bool ok = false;
	QString text = QInputDialog::getText(dialogParent(), msg("Entries.NewTitle"), msg("Entries.NewPrompt"),
					     QLineEdit::Normal, freeName(msg("Entries.DefaultName")), &ok)
			       .trimmed();
	if (!ok || text.isEmpty()) {
		return;
	}
	if (nameTaken(text)) {
		QMessageBox::warning(dialogParent(), msg("Entries.NewTitle"), msg("Entries.NameTaken"));
		return;
	}

	obs_scene_t *scene = obs_scene_create(text.toUtf8().constData());
	if (!scene) {
		return;
	}
	obs_source_t *sceneSource = obs_scene_get_source(scene);
	for (CameraDock *each : instances) {
		each->rebuildNow();
	}
	openAddSource(sceneSource);
	obs_scene_release(scene);
}

void showEmptyMenu(const QPoint &where)
{
	QMenu menu;
	QAction *create = menu.addAction(msg("Entries.New"));
	if (menu.exec(where) == create) {
		newEntry();
	}
}

void showCardMenu(Card *card, const QPoint &where)
{
	/* Segura a cena enquanto o menu está aberto: a miniatura pode ser refeita no meio. */
	obs_source_t *scene = card->source();
	if (!scene) {
		return;
	}

	QMenu menu;
	QAction *addAction = menu.addAction(msg("Entries.AddSource"));
	QAction *organizeAction = menu.addAction(msg("Entries.Organize"));
	menu.addSeparator();
	QAction *renameAction = menu.addAction(msg("Entries.Rename"));
	QAction *deleteAction = menu.addAction(msg("Entries.Delete"));

	QAction *chosen = menu.exec(where);
	if (chosen == addAction) {
		openAddSource(scene);
	} else if (chosen == organizeAction) {
		organizeEntry(scene);
	} else if (chosen == renameAction) {
		renameEntry(scene);
	} else if (chosen == deleteAction) {
		deleteEntry(scene);
	}

	obs_source_release(scene);
}

} // namespace

void pl_camera_dock_load(void)
{
	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (!mainWindow) {
		return;
	}

	dock = new CameraDock(mainWindow);
	if (!obs_frontend_add_dock_by_id("projeto-live-cameras", obs_module_text("Cameras.Title"), dock)) {
		delete dock;
		dock = nullptr;
		return;
	}
	QObject::connect(dock, &QObject::destroyed, []() { dock = nullptr; });
}

QWidget *pl_camera_dock_create(QWidget *parent)
{
	return new CameraDock(parent);
}

void pl_entries_add_source(obs_source_t *scene)
{
	if (scene) {
		openAddSource(scene);
	}
}
