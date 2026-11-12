/*
 * Projeto Live: painel "Câmeras".
 *
 * Uma fileira de miniaturas ao vivo, uma por entrada nossa (fonte "Celular" e "Telão (projeção)"),
 * para o operador ver tudo numa tela só — sem precisar do multiview, que tapa o OBS inteiro, nem
 * de um segundo monitor.
 *
 * Cada miniatura mostra:
 * - a imagem ao vivo (uma tela do OBS por miniatura, igual à janela de vídeo do vertical);
 * - a moldura: vermelha no ar, verde na prévia, amarela quando a imagem travou, cinza no resto;
 * - o nome, o atraso medido daquela câmera e uma barrinha com o som que vem dela;
 * - a bolinha de sinal: verde recebendo, amarela travada, vermelha sem imagem.
 *
 * A moldura amarela é o aviso de câmera caída (a "câmera de reserva" com corte automático foi
 * descartada em 11/11: cortar sozinho no meio do culto é pior que a imagem travar).
 *
 * Um clique manda a câmera para a prévia e dois cliques cortam para o ar. Como no OBS quem entra no
 * ar é uma cena (não uma fonte), o painel usa a primeira cena que contém aquela câmera; sem nenhuma
 * cena, o clique não faz nada e a miniatura explica o motivo.
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

#include <QColor>
#include <QComboBox>
#include <QEvent>
#include <QList>
#include <QMargins>
#include <QPainter>
#include <QPoint>
#include <QRect>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPlatformSurfaceEvent>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
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

const char *const MIRROR_SOURCE_ID = "projeto_live_mirror";
const char *const SIZE_FILE = "cameras.json";

const float SILENCE_DB = -60.0f;

/* Larguras da miniatura: pequeno, médio, grande. */
const int CARD_WIDTHS[3] = {170, 230, 310};

enum class Tally { Off, Preview, Program };

enum class Signal { None, Stalled, Live };

/* Barrinha com o som que vem daquela câmera. */
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

class Card;

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

/* Uma miniatura inteira: imagem, moldura, nome, atraso e barrinha de som. */
class Card : public QFrame {
public:
	Card(QWidget *parent, obs_source_t *source) : QFrame(parent)
	{
		weak = obs_source_get_weak_source(source);
		phone = pl_phone_is_source(source);

		setObjectName("plCard");
		setFrameShape(QFrame::NoFrame);
		setCursor(Qt::PointingHandCursor);

		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(3, 3, 3, 3);
		layout->setSpacing(4);

		thumb = new Thumb(this, source);
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

	/* Um clique: manda para a prévia (ou para o ar, se o modo estúdio estiver desligado e a
	 * pessoa der dois cliques). */
	void sendToPreview()
	{
		obs_source_t *scene = firstScene();
		if (!scene) {
			return;
		}
		if (obs_frontend_preview_program_mode_active()) {
			obs_frontend_set_current_preview_scene(scene);
		}
		obs_source_release(scene);
	}

	void sendToProgram()
	{
		obs_source_t *scene = firstScene();
		if (!scene) {
			return;
		}
		obs_frontend_set_current_scene(scene);
		obs_source_release(scene);
	}

	/* Chamado pelo relógio do painel; `deep` = também procurar a cena desta câmera. */
	void refresh(bool deep)
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

		Signal signal = Signal::Live;
		QString info;
		if (phone) {
			struct pl_phone_status status;
			if (pl_phone_get_status(src, &status)) {
				if (status.stalled_ms < 0) {
					signal = Signal::None;
				} else if (status.stalled_ms >= PL_PHONE_STALL_MS) {
					signal = Signal::Stalled;
				}
				if (signal == Signal::Live && status.latency_ms >= 0) {
					info = QString::number(status.latency_ms / 1000.0, 'f', 2) + " s";
				} else if (signal == Signal::Stalled) {
					info = QString::fromUtf8(obs_module_text("Cameras.Stalled"));
				} else {
					info = QString::fromUtf8(obs_module_text("Cameras.NoSignal"));
				}
				if (signal == Signal::Live && status.usb) {
					info = QString::fromUtf8(obs_module_text("Cameras.Cable"));
				}
			}
		} else {
			/* Telão da projeção: tem imagem se a fonte tem tamanho. */
			if (!obs_source_get_width(src)) {
				signal = Signal::None;
				info = QString::fromUtf8(obs_module_text("Cameras.NoSignal"));
			}
		}

		detail->setText(info);
		applyColors(tally, signal);
		updateLevel(src);

		if (deep) {
			obs_source_t *scene = firstScene();
			bool found = scene != nullptr;
			obs_source_release(scene);
			if (found != sceneFound || toolTip().isEmpty()) {
				sceneFound = found;
				setToolTip(
					QString::fromUtf8(obs_module_text(found ? "Cameras.Hint" : "Cameras.NoScene")));
			}
		}

		obs_source_release(src);
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

	/* A primeira cena (na ordem da lista do OBS) que usa esta câmera. */
	obs_source_t *firstScene()
	{
		obs_source_t *src = source();
		if (!src) {
			return nullptr;
		}
		struct obs_frontend_source_list scenes = {};
		obs_frontend_get_scenes(&scenes);
		obs_source_t *found = nullptr;
		for (size_t i = 0; i < scenes.sources.num && !found; i++) {
			obs_source_t *sceneSource = scenes.sources.array[i];
			obs_scene_t *scene = obs_scene_from_source(sceneSource);
			if (!scene) {
				continue;
			}
			struct Search {
				obs_source_t *wanted;
				bool found;
			} search = {src, false};
			obs_scene_enum_items(
				scene,
				[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
					auto *s = static_cast<Search *>(param);
					if (obs_sceneitem_get_source(item) == s->wanted) {
						s->found = true;
						return false;
					}
					return true;
				},
				&search);
			if (search.found) {
				found = obs_source_get_ref(sceneSource);
			}
		}
		obs_frontend_source_list_free(&scenes);
		obs_source_release(src);
		return found;
	}

	obs_weak_source_t *weak = nullptr;
	bool phone = false;
	bool sceneFound = false;
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

	/* Encaixa a imagem inteira na miniatura, sem deformar. */
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

		hint = new QLabel(QString::fromUtf8(obs_module_text("Cameras.Hint")), this);
		hint->setEnabled(false);
		top->addWidget(hint, 1);

		sizeBox = new QComboBox(this);
		sizeBox->addItem(QString::fromUtf8(obs_module_text("Cameras.Size.Small")));
		sizeBox->addItem(QString::fromUtf8(obs_module_text("Cameras.Size.Medium")));
		sizeBox->addItem(QString::fromUtf8(obs_module_text("Cameras.Size.Large")));
		sizeBox->setCurrentIndex(loadSize());
		connect(sizeBox, &QComboBox::currentIndexChanged, this, [this](int index) {
			for (Card *card : cards) {
				card->setSize(index);
			}
			saveSize(index);
		});
		top->addWidget(sizeBox);

		outer->addLayout(top);

		empty = new QLabel(QString::fromUtf8(obs_module_text("Cameras.Empty")), this);
		empty->setWordWrap(true);
		empty->setEnabled(false);
		outer->addWidget(empty);

		area = new QScrollArea(this);
		area->setWidgetResizable(true);
		area->setFrameShape(QFrame::NoFrame);
		holder = new QWidget(area);
		flow = new FlowLayout(holder);
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
				card->refresh(deep);
			}
		});
		timer->start();

		rebuild();
	}

private:
	/* Câmeras na frente, depois o telão, depois as outras entradas. */
	static int sourceRank(obs_source_t *source)
	{
		if (pl_phone_is_source(source)) {
			return 0;
		}
		const char *id = obs_source_get_id(source);
		return id && strcmp(id, MIRROR_SOURCE_ID) == 0 ? 1 : 2;
	}

	/* Procura as entradas e cria ou tira miniaturas. */
	void rebuild()
	{
		struct Found {
			std::vector<obs_source_t *> sources;
		} found;

		/* Tudo o que entra imagem no OBS é uma entrada: as câmeras, o telão, um vídeo, uma
		 * imagem de aviso, uma captura de tela. Sem isso, a pessoa não consegue ver nem mandar
		 * para a prévia o que está nas outras cenas. */
		obs_enum_sources(
			[](void *param, obs_source_t *source) {
				if (obs_source_get_type(source) != OBS_SOURCE_TYPE_INPUT) {
					return true;
				}
				if ((obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) == 0) {
					return true;
				}
				/* Segura a fonte: os ponteiros do obs_enum_sources só valem dentro
				 * desta função. */
				static_cast<Found *>(param)->sources.push_back(obs_source_get_ref(source));
				return true;
			},
			&found);

		/* Ordem: primeiro as câmeras, depois o telão, depois o resto pelo nome. */
		std::sort(found.sources.begin(), found.sources.end(), [](obs_source_t *a, obs_source_t *b) {
			int rankA = sourceRank(a);
			int rankB = sourceRank(b);
			if (rankA != rankB) {
				return rankA < rankB;
			}
			const char *nameA = obs_source_get_name(a);
			const char *nameB = obs_source_get_name(b);
			return strcmp(nameA ? nameA : "", nameB ? nameB : "") < 0;
		});

		/* Tira as que não existem mais. */
		for (int i = (int)cards.size() - 1; i >= 0; i--) {
			bool keep = false;
			for (obs_source_t *source : found.sources) {
				if (cards[(size_t)i]->isSource(source)) {
					keep = true;
					break;
				}
			}
			if (!keep) {
				Card *card = cards[(size_t)i];
				cards.erase(cards.begin() + i);
				card->releaseMeter();
				flow->removeWidget(card);
				card->deleteLater();
			}
		}

		/* Põe as novas, na ordem em que o OBS as devolveu. */
		for (obs_source_t *source : found.sources) {
			bool known = false;
			for (Card *card : cards) {
				if (card->isSource(source)) {
					known = true;
					break;
				}
			}
			if (known) {
				continue;
			}
			auto *card = new Card(holder, source);
			card->setSize(sizeBox->currentIndex());
			flow->addWidget(card);
			cards.push_back(card);
		}

		/* Põe as miniaturas na ordem acima. */
		std::vector<Card *> ordered;
		ordered.reserve(cards.size());
		for (obs_source_t *source : found.sources) {
			for (Card *card : cards) {
				if (card->isSource(source)) {
					ordered.push_back(card);
					break;
				}
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
		}

		for (obs_source_t *source : found.sources) {
			obs_source_release(source);
		}

		empty->setVisible(cards.empty());
		area->setVisible(!cards.empty());
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
	QComboBox *sizeBox = nullptr;
	QScrollArea *area = nullptr;
	QWidget *holder = nullptr;
	FlowLayout *flow = nullptr;
	QTimer *timer = nullptr;
	std::vector<Card *> cards;
	int ticks = 0;
};

CameraDock *dock = nullptr;

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
