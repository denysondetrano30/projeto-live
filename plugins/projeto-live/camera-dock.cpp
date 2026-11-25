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
#include "media-bar.h"

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
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPlatformSurfaceEvent>
#include <QPoint>
#include <QDockWidget>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFontMetrics>
#include <QPixmap>
#include <QStringList>
#include <QPointer>
#include <QPushButton>
#include <QRect>
#include <QRectF>
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
#include "webcam-source.h"
#include "transitions-dialog.h"
#include "vertical-pan.h"

namespace {

const char *const SIZE_FILE = "cameras.json";

const float SILENCE_DB = -60.0f;

/* Larguras da miniatura: pequeno, médio, grande. */
const int CARD_WIDTHS[3] = {170, 230, 310};

/* Largura da tirinha (entrada encolhida). */
const int COMPACT_WIDTH = 150;

/* Espaço entre as miniaturas (o mesmo do FlowLayout). */
const int CARD_GAP = 8;

/* Quem abre a janelinha "Fontes desta entrada" (a janela do somun live avisa; sem ela, nulo). */
void (*sourcesOpener)() = nullptr;

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

	/* Tipo de cada peça (propriedade "plKind" do widget): 0 = entrada grande (quebra linha à esquerda),
	 * 1 = entrada encolhida (barrinha em pé, à direita), 2 = coluna das escondidas (na ponta direita). */
	static int kindOf(QLayoutItem *item)
	{
		QWidget *widget = item->widget();
		return widget ? widget->property("plKind").toInt() : 0;
	}

	/* Largura que as barrinhas e a coluna das escondidas ocupam à direita. */
	int rightWidth() const
	{
		int width = 0;
		for (QLayoutItem *item : items) {
			if (item->isEmpty()) {
				continue;
			}
			int kind = kindOf(item);
			if (kind == 1 || kind == 2) {
				width += item->sizeHint().width() + GAP;
			}
		}
		return width;
	}

private:
	int layoutRows(const QRect &rect, bool place) const
	{
		QMargins margins = contentsMargins();
		QRect area = rect.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
		/* As grandes quebram linha só na parte esquerda; a direita fica para as barrinhas. */
		int right = area.right() - rightWidth();
		int x = area.x();
		int y = area.y();
		int rowHeight = 0;
		int firstRow = 0;
		for (QLayoutItem *item : items) {
			if (item->isEmpty() || kindOf(item) != 0) {
				continue;
			}
			QSize hint = item->sizeHint();
			int next = x + hint.width() + GAP;
			if (rowHeight > 0 && next - GAP > right + 1) {
				if (firstRow == 0) {
					firstRow = rowHeight;
				}
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
		if (firstRow == 0) {
			firstRow = rowHeight > 0 ? rowHeight : 120;
		}
		int bottom = y + rowHeight;

		/* À direita: as barrinhas em pé (altura de uma fileira) e, na ponta, as escondidas. */
		int sx = right + 1 + GAP;
		for (QLayoutItem *item : items) {
			if (item->isEmpty() || kindOf(item) == 0) {
				continue;
			}
			QSize hint = item->sizeHint();
			int height = kindOf(item) == 1 ? firstRow : std::max(hint.height(), 30);
			if (place) {
				item->setGeometry(QRect(sx, area.y(), hint.width(), height));
			}
			sx += hint.width() + GAP;
			bottom = std::max(bottom, area.y() + height);
		}
		return bottom - rect.y() + margins.bottom();
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

/* O primeiro "Celular sem app" (navegador) desta cena, com referência; nulo se não houver. */
obs_source_t *firstWebcam(const ItemList &items)
{
	for (obs_sceneitem_t *item : items) {
		obs_source_t *src = obs_sceneitem_get_source(item);
		int stalled;
		if (src && pl_webcam_get_status(src, &stalled)) {
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
void showEntryAgain(const QString &name);
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
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void dragEnterEvent(QDragEnterEvent *event) override;
	void dropEvent(QDropEvent *event) override;
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
/* Marcas de cada entrada (botão direito): encolhida, escondida e "vai limpa". Ficam guardadas na  */
/* própria cena, então vão junto com o projeto do OBS e valem nas duas telas.                     */

const char *const KEY_SHRUNK = "pl_encolhida";
const char *const KEY_HIDDEN = "pl_escondida";
const char *const KEY_CLEAN = "pl_vai_limpa";
const char *const KEY_ORDER = "pl_ordem";

/* Tipo do que se arrasta (o nome da entrada). */
const char *const ENTRY_MIME = "application/x-somun-entry";

/* Lugar da entrada na fileira (arrastando com o mouse); sem lugar escolhido, vale a ordem do OBS. */
int entryOrder(obs_source_t *scene, int fallback)
{
	obs_data_t *data = obs_source_get_private_settings(scene);
	int order = data && obs_data_has_user_value(data, KEY_ORDER) ? (int)obs_data_get_int(data, KEY_ORDER)
								     : 100000 + fallback;
	obs_data_release(data);
	return order;
}

/* Põe as entradas no lugar escolhido arrastando (as sem lugar ficam na ordem do OBS, depois). */
void sortByOrder(std::vector<obs_source_t *> &list)
{
	std::vector<std::pair<int, obs_source_t *>> keyed;
	for (size_t i = 0; i < list.size(); i++) {
		keyed.emplace_back(entryOrder(list[i], (int)i), list[i]);
	}
	std::stable_sort(keyed.begin(), keyed.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
	for (size_t i = 0; i < list.size(); i++) {
		list[i] = keyed[i].second;
	}
}

/* Arrastou "dragged" e soltou em cima de "target" (antes ou depois dele): guarda a ordem nova. */
void reorderEntries(const QString &dragged, const QString &target, bool after);

/* Atualiza agora todas as cópias do painel (monitor 1 e monitor 2). */
void refreshAllNow();

bool entryFlag(obs_source_t *scene, const char *key)
{
	obs_data_t *data = obs_source_get_private_settings(scene);
	bool on = data && obs_data_get_bool(data, key);
	obs_data_release(data);
	return on;
}

void setEntryFlag(obs_source_t *scene, const char *key, bool on)
{
	obs_data_t *data = obs_source_get_private_settings(scene);
	if (data) {
		obs_data_set_bool(data, key, on);
		obs_data_release(data);
	}
}

/* A entrada tem o telão (a Projeção) dentro? */
bool hasProjection(obs_source_t *scene)
{
	ItemList items = sceneItems(scene);
	bool found = false;
	for (obs_sceneitem_t *item : items) {
		const char *id = obs_source_get_unversioned_id(obs_sceneitem_get_source(item));
		if (id && (strcmp(id, "projeto_live_mirror") == 0 || strcmp(id, "projeto_live_mirror_vertical") == 0)) {
			found = true;
			break;
		}
	}
	releaseItems(items);
	return found;
}

/* "Vai limpa": ignora o que está "No programa". Sem escolha da pessoa, o telão já vem marcado
 * (versículo e aviso já vêm desenhados de dentro dele). */
bool entryGoesClean(obs_source_t *scene)
{
	obs_data_t *data = obs_source_get_private_settings(scene);
	bool chosen = data && obs_data_has_user_value(data, KEY_CLEAN);
	bool on = chosen ? obs_data_get_bool(data, KEY_CLEAN) : hasProjection(scene);
	obs_data_release(data);
	return on;
}

/* ------------------------------------------------------------------------- */

/* Largura da barrinha em pé de uma entrada encolhida. */
const int STRIP_WIDTH = 34;

/* O miolo da barrinha em pé: a luzinha em cima e o nome escrito de baixo para cima. */
class StripLabel : public QWidget {
public:
	explicit StripLabel(QWidget *parent) : QWidget(parent) { setMinimumHeight(40); }

	void setText(const QString &value)
	{
		if (text != value) {
			text = value;
			update();
		}
	}

	void setDot(const QColor &color)
	{
		if (dot != color) {
			dot = color;
			update();
		}
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(Qt::NoPen);
		painter.setBrush(dot);
		painter.drawEllipse(QRectF(width() / 2.0 - 4.0, 4.0, 8.0, 8.0));
		painter.setPen(QColor("#F2F2F2"));
		painter.translate(width() / 2.0, height() - 4.0);
		painter.rotate(-90.0);
		QFontMetrics metrics(font());
		QString shown = metrics.elidedText(text, Qt::ElideRight, std::max(height() - 22, 10));
		painter.drawText(QPointF(0.0, metrics.ascent() / 2.0 - 1.0), shown);
	}

private:
	QString text;
	QColor dot = QColor("#5A5A5A");
};

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
		setAcceptDrops(true);

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

		/* Marca "vai limpa" (ignora o que está No programa): uma palavrinha cinza. */
		cleanTag = new QLabel(msg("Entries.CleanTag"), this);
		cleanTag->setStyleSheet("color: #8C8C8C;");
		cleanTag->setToolTip(msg("Entries.CleanHint"));
		cleanTag->setVisible(false);
		row->addWidget(cleanTag);

		detail = new QLabel(this);
		detail->setEnabled(false);
		row->addWidget(detail);

		layout->addLayout(row);

		/* Encolhida: no lugar da imagem e da linha do nome, a barrinha em pé. */
		strip = new StripLabel(this);
		strip->setVisible(false);
		layout->addWidget(strip, 1);

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

	void setSize(int index) { setTileWidth(CARD_WIDTHS[std::clamp(index, 0, 2)]); }

	/* Largura da imagem (a altura segue o 16:9). A grade do Acompanhar escolhe a largura sozinha. */
	void setTileWidth(int cardWidth)
	{
		tileWidth = std::max(cardWidth, 80);
		thumb->setFixedSize(tileWidth, tileWidth * 9 / 16);
		setFixedWidth(compact ? STRIP_WIDTH : tileWidth + 6);
	}

	/* O painel inteiro encolhido (todas em tirinha). */
	void setCompact(bool on)
	{
		dockCompact = on;
		applyCompact();
	}

	/* Escondida pela pessoa (botão direito > Esconder): o painel tira da fileira. */
	bool isHiddenByUser() const { return hiddenByUser; }

	/* Nome da entrada (como aparece no cartão). */
	QString entryName() const { return name->text(); }

	/* Encolhida (por ela ou pelo painel inteiro)? */
	bool isCompact() const { return compact; }

	/* Na grade do Acompanhar: nome e estado em letra maior, para ler de longe. */
	void setBig(bool on)
	{
		QFont nameFont = name->font();
		nameFont.setPointSizeF(on ? 12.0 : nameFont.pointSizeF());
		nameFont.setWeight(on ? QFont::DemiBold : QFont::Normal);
		name->setFont(nameFont);
		QFont badgeFont = badge->font();
		if (on) {
			badgeFont.setPointSizeF(11.0);
		}
		badge->setFont(badgeFont);
	}

	/* Mouse: um clique (sem arrastar) manda para a prévia ao soltar; arrastar muda o lugar da entrada. */
	void pressStart(const QPoint &global)
	{
		pressGlobal = global;
		pressing = true;
	}

	void pressMove(const QPoint &global, Qt::MouseButtons buttons)
	{
		if (pressing && (buttons & Qt::LeftButton) &&
		    (global - pressGlobal).manhattanLength() >= QApplication::startDragDistance()) {
			pressing = false;
			startDrag();
		}
	}

	void pressEnd()
	{
		if (pressing) {
			pressing = false;
			sendToPreview();
		}
	}

	void dragArrives(QDragEnterEvent *event)
	{
		if (event->mimeData()->hasFormat(ENTRY_MIME)) {
			event->acceptProposedAction();
		}
	}

	/* Soltou uma entrada aqui: ela vai para antes desta (metade esquerda) ou depois (direita). */
	void dropHere(QDropEvent *event, const QPoint &local)
	{
		if (!event->mimeData()->hasFormat(ENTRY_MIME)) {
			return;
		}
		QString dragged = QString::fromUtf8(event->mimeData()->data(ENTRY_MIME));
		event->acceptProposedAction();
		bool after = compact ? local.y() > height() / 2 : local.x() > width() / 2;
		if (dragged != entryName()) {
			/* Depois de o arrastar terminar: a fileira vai ser refeita. */
			QString target = entryName();
			QTimer::singleShot(0, [dragged, target, after]() { reorderEntries(dragged, target, after); });
		}
	}

	/* Um clique: manda para a prévia; no modo normal (sem prévia) já põe no ar, com a transição. */
	void sendToPreview()
	{
		obs_source_t *scene = source();
		if (!scene) {
			return;
		}
		if (obs_frontend_preview_program_mode_active()) {
			obs_frontend_set_current_preview_scene(scene);
		} else {
			obs_frontend_set_current_scene(scene);
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
		strip->setText(name->text());

		/* As marcas do botão direito (podem ter mudado em outra cópia do painel). */
		shrunkByUser = entryFlag(src, KEY_SHRUNK);
		hiddenByUser = entryFlag(src, KEY_HIDDEN);
		applyCompact();
		bool clean = entryGoesClean(src);
		if (cleanTag->isHidden() == (clean && !compact)) {
			cleanTag->setVisible(clean && !compact);
		}

		Tally tally = Tally::Off;
		if (obs_source_active(src)) {
			tally = Tally::Program;
		} else if (obs_source_showing(src)) {
			tally = Tally::Preview;
		}

		ItemList items = sceneItems(src);
		bool blank = items.empty();
		obs_source_t *cam = firstPhone(items);
		obs_source_t *web = cam ? nullptr : firstWebcam(items);
		releaseItems(items);

		Signal signal = Signal::Live;
		bool weakWifi = false;
		bool phoneGone = false;
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
				/* Parte G5: Wi-Fi fraco (ping alto ou engasgando) em vermelho no lugar do atraso. */
				if (signal == Signal::Live && status.weak) {
					weakWifi = true;
					info = msg("Cameras.WeakSignal");
				}
			}
			obs_source_release(cam);
		} else if (web) {
			/* Celular sem app: caiu = a última imagem fica parada e o aviso vermelho aparece aqui. */
			int stalled = -1;
			pl_webcam_get_status(web, &stalled);
			if (stalled < 0) {
				signal = Signal::None;
				info = msg("WebCam.WaitingShort");
			} else if (stalled >= PL_PHONE_STALL_MS) {
				signal = Signal::Stalled;
				weakWifi = true;
				phoneGone = true;
				info = msg("WebCam.Gone");
			}
			obs_source_release(web);
		} else if (blank) {
			signal = Signal::None;
			info = msg("Entries.Blank");
		}

		detail->setText(info);
		if (weakWifi != shownWeak || phoneGone != shownGone) {
			shownWeak = weakWifi;
			shownGone = phoneGone;
			detail->setEnabled(weakWifi);
			detail->setStyleSheet(weakWifi ? QString("color: #E5483B;") : QString());
			detail->setToolTip(phoneGone  ? msg("WebCam.GoneHint")
					   : weakWifi ? msg("Cameras.WeakSignalHint")
						      : QString());
		}
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

	/* A barrinha em pé (encolhida) não tem imagem: o mouse cai no próprio cartão. */
	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton) {
			pressStart(event->globalPosition().toPoint());
		}
		QFrame::mousePressEvent(event);
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		pressMove(event->globalPosition().toPoint(), event->buttons());
		QFrame::mouseMoveEvent(event);
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton) {
			pressEnd();
		}
		QFrame::mouseReleaseEvent(event);
	}

	void mouseDoubleClickEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton) {
			sendToProgram();
		}
		QFrame::mouseDoubleClickEvent(event);
	}

	void dragEnterEvent(QDragEnterEvent *event) override { dragArrives(event); }

	void dropEvent(QDropEvent *event) override { dropHere(event, event->position().toPoint()); }

private:
	/* Começa a arrastar esta entrada (o desenho que acompanha o mouse é o nome dela). */
	void startDrag()
	{
		QString label = entryName();
		QFontMetrics metrics(font());
		QPixmap pixmap(metrics.horizontalAdvance(label) + 24, metrics.height() + 12);
		pixmap.fill(QColor("#232323"));
		QPainter painter(&pixmap);
		painter.setPen(QColor("#F2F2F2"));
		painter.drawText(pixmap.rect(), Qt::AlignCenter, label);
		painter.end();

		auto *mime = new QMimeData();
		mime->setData(ENTRY_MIME, label.toUtf8());
		auto *drag = new QDrag(this);
		drag->setMimeData(mime);
		drag->setPixmap(pixmap);
		drag->setHotSpot(QPoint(pixmap.width() / 2, pixmap.height() / 2));
		drag->exec(Qt::MoveAction);
	}

	/* Tirinha (sem imagem) quando ela encolheu esta entrada ou o painel inteiro está encolhido. */
	void applyCompact()
	{
		bool on = dockCompact || shrunkByUser;
		if (compact == on) {
			return;
		}
		compact = on;
		thumb->setVisible(!on);
		detail->setVisible(!on);
		if (on) {
			level->setVisible(false);
			cleanTag->setVisible(false);
		}
		dot->setVisible(!on);
		name->setVisible(!on);
		if (on) {
			badge->setVisible(false);
		}
		strip->setVisible(on);
		/* O layout põe a barrinha em pé à direita (veja FlowLayout). */
		setProperty("plKind", on ? 1 : 0);
		setFixedWidth(on ? STRIP_WIDTH : tileWidth + 6);
	}

	/* Cores do somun: vermelho no ar, branco na prévia, vermelho também quando travou (o aviso mais
	 * importante: descobrir antes de cortar para ela). */
	void applyColors(Tally tally, Signal signal)
	{
		const char *border = "#3A3A3A";
		const char *badgeText = nullptr;
		const char *badgeColor = nullptr;
		const char *dotColor = "#5A5A5A";
		if (tally == Tally::Program) {
			border = "#E5483B";
			badgeText = "Cameras.OnAir";
			badgeColor = "#E5483B";
			dotColor = "#E5483B";
		} else if (tally == Tally::Preview) {
			border = "#FFFFFF";
			badgeText = "Cameras.Preview";
			badgeColor = "#FFFFFF";
			dotColor = "#FFFFFF";
		}
		if (signal == Signal::Stalled) {
			border = "#E5483B";
			badgeText = "Cameras.StalledBadge";
			badgeColor = "#E5483B";
			dotColor = "#E5483B";
		} else if (signal == Signal::None && tally == Tally::Off) {
			border = "#2A2A2A";
		}

		if (badgeText) {
			if (shownBadge != badgeText) {
				shownBadge = badgeText;
				badge->setText(QString::fromUtf8(obs_module_text(badgeText)));
				badge->setStyleSheet(QString("color: %1;").arg(badgeColor));
			}
			badge->setVisible(!compact);
		} else {
			shownBadge.clear();
			badge->setVisible(false);
		}

		strip->setDot(QColor(dotColor));
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
		if (compact) {
			return;
		}
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
		if (level->isHidden()) {
			level->setVisible(true);
		}
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
	bool shownWeak = false;
	bool shownGone = false;
	std::string shownDot;
	Thumb *thumb = nullptr;
	QLabel *dot = nullptr;
	QLabel *badge = nullptr;
	QLabel *name = nullptr;
	QLabel *detail = nullptr;
	QLabel *cleanTag = nullptr;
	StripLabel *strip = nullptr;
	QPoint pressGlobal;
	bool pressing = false;
	Level *level = nullptr;
	obs_volmeter_t *meter = nullptr;
	std::atomic<float> peak{SILENCE_DB};
	int shown = 0;
	int tileWidth = CARD_WIDTHS[1];
	bool compact = false;
	bool dockCompact = false;
	bool shrunkByUser = false;
	bool hiddenByUser = false;

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
	/* Soltar uma entrada arrastada em cima desta muda a ordem (veja Card::dropHere). */
	setAcceptDrops(true);

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
		card->pressStart(event->globalPosition().toPoint());
	}
	QWidget::mousePressEvent(event);
}

void Thumb::mouseMoveEvent(QMouseEvent *event)
{
	if (card) {
		card->pressMove(event->globalPosition().toPoint(), event->buttons());
	}
	QWidget::mouseMoveEvent(event);
}

void Thumb::mouseReleaseEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton && card) {
		card->pressEnd();
	}
	QWidget::mouseReleaseEvent(event);
}

void Thumb::dragEnterEvent(QDragEnterEvent *event)
{
	if (card) {
		card->dragArrives(event);
	}
}

void Thumb::dropEvent(QDropEvent *event)
{
	if (card) {
		card->dropHere(event, mapTo(card, event->position().toPoint()));
	}
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

/* A coluna das entradas escondidas, na ponta direita: uma pílula cinza por entrada, uma embaixo da
 * outra. Um clique na pílula traz a entrada de volta. Só aparece se houver alguma escondida. */
class HiddenColumn : public QWidget {
public:
	explicit HiddenColumn(QWidget *parent) : QWidget(parent)
	{
		setProperty("plKind", 2);
		layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(4);
		setFixedWidth(120);
		setVisible(false);
	}

	/* Refaz as pílulas se a lista de escondidas mudou. */
	void setNames(const QStringList &names)
	{
		if (names == shown) {
			return;
		}
		shown = names;
		while (QLayoutItem *item = layout->takeAt(0)) {
			if (QWidget *widget = item->widget()) {
				widget->deleteLater();
			}
			delete item;
		}
		for (const QString &name : names) {
			auto *pill = new QPushButton(name, this);
			pill->setCursor(Qt::PointingHandCursor);
			pill->setFocusPolicy(Qt::NoFocus);
			pill->setToolTip(msg("Entries.OthersHint"));
			pill->setStyleSheet(
				"QPushButton { background: #232323; color: #A8A8A8; border: 1px solid #3A3A3A;"
				" border-radius: 12px; padding: 3px 10px; text-align: left; }"
				"QPushButton:hover { color: #F2F2F2; }");
			connect(pill, &QPushButton::clicked, this, [name]() { showEntryAgain(name); });
			layout->addWidget(pill);
		}
		layout->addStretch(1);
		adjustSize();
		updateGeometry();
	}

private:
	QVBoxLayout *layout = nullptr;
	QStringList shown;
};

class CameraDock;
std::vector<CameraDock *> instances;

class CameraDock : public QWidget {
public:
	/* grid: a grade do monitor 2 ("Acompanhar"), com as entradas do tamanho que couber na tela. */
	explicit CameraDock(QWidget *parent, bool grid = false) : QWidget(parent), grid(grid)
	{
		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(6, 6, 6, 6);
		outer->setSpacing(6);

		auto *top = new QHBoxLayout();
		top->setContentsMargins(0, 0, 0, 0);
		top->setSpacing(6);

		/* A explicação fica só na dica do mouse (tela limpa). */
		setToolTip(msg("Cameras.Hint"));

		/* Com o Acompanhar ligado, as entradas daqui encolhem e este aviso aparece. */
		compactNote = new QLabel(msg("Entries.CompactNote"), this);
		compactNote->setStyleSheet("color: #8C8C8C;");
		compactNote->setVisible(false);
		top->addWidget(compactNote);
		top->addStretch(1);

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

		if (!grid) {
			outer->addLayout(top);
		} else {
			/* Na grade o tamanho é automático: sem a escolha de tamanho. */
			sizeBox->setVisible(false);
			delete top;
		}

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
		area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		holder = new QWidget(area);
		holder->setContextMenuPolicy(Qt::CustomContextMenu);
		connect(holder, &QWidget::customContextMenuRequested, this,
			[this](const QPoint &pos) { showEmptyMenu(holder->mapToGlobal(pos)); });
		flow = new FlowLayout(holder);
		hiddenColumn = new HiddenColumn(holder);
		flow->addWidget(hiddenColumn);
		addCard = new AddCard(holder);
		addCard->setSize(sizeBox->currentIndex());
		flow->addWidget(addCard);
		if (grid) {
			addCard->setVisible(false);
		}
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
			syncHidden();
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

	/* Lê as marcas e arruma a fileira agora (depois de encolher, esconder ou trazer de volta). */
	void refreshNow()
	{
		for (Card *card : cards) {
			card->refresh();
		}
		syncHidden();
	}

	/* Encolhe todas as entradas em tirinhas (o Acompanhar está mostrando as imagens no monitor 2). */
	void setCompact(bool on)
	{
		if (grid || compact == on) {
			return;
		}
		compact = on;
		for (Card *card : cards) {
			card->setCompact(on);
		}
		addCard->setVisible(!on);
		sizeBox->setVisible(!on);
		compactNote->setVisible(on);
		holder->updateGeometry();
		emitHeightChange();
	}

	bool isCompact() const { return compact; }

protected:
	void resizeEvent(QResizeEvent *event) override
	{
		QWidget::resizeEvent(event);
		if (grid) {
			fitGrid();
		}
	}

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
	/* A janela de cima pode querer encolher a faixa (feito por quem nos contém, pelo tamanho mínimo). */
	void emitHeightChange()
	{
		if (compact) {
			setMaximumHeight(72);
		} else {
			setMaximumHeight(QWIDGETSIZE_MAX);
		}
	}

	/* Escondidas saem da fileira (e viram pílulas na coluna da direita); encolhidas viram barrinhas em
	 * pé à direita. Quando isso muda, o layout se refaz e a grade do monitor 2 recalcula o tamanho
	 * das que sobraram grandes. */
	void syncHidden()
	{
		QStringList hiddenNames;
		QString signature;
		for (Card *card : cards) {
			bool show = !card->isHiddenByUser();
			if (card->isHidden() == show) {
				card->setVisible(show);
			}
			if (!show) {
				hiddenNames.append(card->entryName());
			}
			signature += show ? (card->isCompact() ? 'c' : 'v') : 'h';
		}
		hiddenColumn->setNames(hiddenNames);
		bool showHidden = !hiddenNames.isEmpty() && !compact;
		if (hiddenColumn->isHidden() == showHidden) {
			hiddenColumn->setVisible(showHidden);
		}
		signature += hiddenNames.join('|');
		if (signature != shownSignature) {
			shownSignature = signature;
			flow->invalidate();
			holder->updateGeometry();
			if (grid) {
				gridWidth = 0;
				fitGrid();
			}
		}
	}

	/* Grade: escolhe quantas colunas deixam cada entrada grande o maior possível sem rolar, na parte
	 * esquerda (a direita fica para as barrinhas e as escondidas). */
	void fitGrid()
	{
		int n = 0;
		for (Card *card : cards) {
			if (!card->isHiddenByUser() && !card->isCompact()) {
				n++;
			}
		}
		if (n == 0) {
			return;
		}
		int width = area->width() - 4 - flow->rightWidth();
		int height = area->height() - 4;
		/* Embaixo de cada imagem: nome e estado, mais a moldura. */
		const int below = 40;
		int best = 0;
		for (int cols = 1; cols <= n; cols++) {
			int rows = (n + cols - 1) / cols;
			int byWidth = (width - (cols - 1) * CARD_GAP) / cols - 6;
			int byHeight = ((height - (rows - 1) * CARD_GAP) / rows - below) * 16 / 9;
			best = std::max(best, std::min(byWidth, byHeight));
		}
		best = std::max(best, 160);
		if (best == gridWidth) {
			return;
		}
		gridWidth = best;
		for (Card *card : cards) {
			card->setTileWidth(best);
		}
	}

	/* Tamanho, encolhida e letra grande: o que o cartão novo precisa saber. */
	void prepare(Card *card)
	{
		if (grid) {
			card->setBig(true);
			card->setTileWidth(gridWidth > 0 ? gridWidth : CARD_WIDTHS[2]);
		} else {
			card->setSize(sizeBox->currentIndex());
			card->setCompact(compact);
		}
	}

	/* Uma miniatura por cena do OBS, na mesma ordem da lista de cenas. */
	void rebuild()
	{
		size_t before = cards.size();
		std::vector<obs_source_t *> found;
		struct obs_frontend_source_list scenes = {};
		obs_frontend_get_scenes(&scenes);
		for (size_t i = 0; i < scenes.sources.num; i++) {
			found.push_back(obs_source_get_ref(scenes.sources.array[i]));
		}
		obs_frontend_source_list_free(&scenes);
		sortByOrder(found);

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
			prepare(card);
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

		/* O "⋯ outras entradas" e o "+ Entrada" ficam sempre por último. */
		if (moved) {
			flow->removeWidget(hiddenColumn);
			flow->addWidget(hiddenColumn);
			flow->removeWidget(addCard);
			flow->addWidget(addCard);
		}

		for (obs_source_t *scene : found) {
			obs_source_release(scene);
		}

		empty->setVisible(false);

		if (grid && cards.size() != before) {
			gridWidth = 0;
			fitGrid();
		}
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

	bool grid = false;
	bool compact = false;
	int gridWidth = 0;
	QLabel *compactNote = nullptr;
	QLabel *hint = nullptr;
	QLabel *empty = nullptr;
	AddCard *addCard = nullptr;
	HiddenColumn *hiddenColumn = nullptr;
	QString shownSignature;
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

	/* Na janela do somun live: a janelinha "Fontes desta entrada" (segue a entrada da prévia). */
	if (sourcesOpener) {
		sourcesOpener();
		return;
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
	/* Se a entrada tem vídeo: tocar/pausar e recomeçar, logo no começo. */
	pl_media_menu(&menu, scene);
	QAction *addAction = menu.addAction(msg("Entries.AddSource"));
	QAction *organizeAction = menu.addAction(msg(sourcesOpener ? "Entries.Sources" : "Entries.Organize"));
	pl_entry_transition_menu(&menu, scene);
	menu.addSeparator();
	QAction *frameAction = nullptr;
	if (pl_vertical_frame_available(scene)) {
		frameAction = menu.addAction(msg("Pan.FrameMenu"));
		menu.addSeparator();
	}
	/* Parte B: encolher (tirinha), esconder (sai da fileira) e "vai limpa" (ignora o No programa). */
	bool shrunk = entryFlag(scene, KEY_SHRUNK);
	QAction *shrinkAction = menu.addAction(msg(shrunk ? "Entries.Grow" : "Entries.Shrink"));
	QAction *hideAction = menu.addAction(msg("Entries.Hide"));
	QAction *cleanAction = menu.addAction(msg("Entries.Clean"));
	cleanAction->setCheckable(true);
	cleanAction->setChecked(entryGoesClean(scene));
	cleanAction->setToolTip(msg("Entries.CleanHint"));
	menu.setToolTipsVisible(true);
	menu.addSeparator();
	QAction *renameAction = menu.addAction(msg("Entries.Rename"));
	QAction *deleteAction = menu.addAction(msg("Entries.Delete"));

	QAction *chosen = menu.exec(where);
	if (chosen == addAction) {
		openAddSource(scene);
	} else if (chosen == organizeAction) {
		organizeEntry(scene);
	} else if (frameAction && chosen == frameAction) {
		pl_vertical_frame_open(scene, dialogParent());
	} else if (chosen == shrinkAction) {
		setEntryFlag(scene, KEY_SHRUNK, !shrunk);
	} else if (chosen == hideAction) {
		setEntryFlag(scene, KEY_HIDDEN, true);
	} else if (chosen == cleanAction) {
		setEntryFlag(scene, KEY_CLEAN, cleanAction->isChecked());
	} else if (chosen == renameAction) {
		renameEntry(scene);
	} else if (chosen == deleteAction) {
		deleteEntry(scene);
	}
	if (chosen && (chosen == shrinkAction || chosen == hideAction || chosen == cleanAction)) {
		/* Aparece na hora nos dois monitores (sem esperar o relógio do painel). */
		refreshAllNow();
	}

	obs_source_release(scene);
}

/* Guarda a ordem nova depois de arrastar: a entrada arrastada vai para antes (ou depois) da outra, e
 * todas ganham o seu lugar (pl_ordem). Vale nos dois monitores. */
void reorderEntries(const QString &dragged, const QString &target, bool after)
{
	std::vector<obs_source_t *> list;
	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		list.push_back(obs_source_get_ref(scenes.sources.array[i]));
	}
	obs_frontend_source_list_free(&scenes);
	sortByOrder(list);

	obs_source_t *moving = nullptr;
	for (auto it = list.begin(); it != list.end(); ++it) {
		if (QString::fromUtf8(obs_source_get_name(*it)) == dragged) {
			moving = *it;
			list.erase(it);
			break;
		}
	}
	if (moving) {
		auto where = list.end();
		for (auto it = list.begin(); it != list.end(); ++it) {
			if (QString::fromUtf8(obs_source_get_name(*it)) == target) {
				where = after ? it + 1 : it;
				break;
			}
		}
		list.insert(where, moving);
		for (size_t i = 0; i < list.size(); i++) {
			obs_data_t *data = obs_source_get_private_settings(list[i]);
			if (data) {
				obs_data_set_int(data, KEY_ORDER, (long long)i);
				obs_data_release(data);
			}
		}
	}
	for (obs_source_t *scene : list) {
		obs_source_release(scene);
	}
	for (CameraDock *each : instances) {
		each->rebuildNow();
	}
}

void refreshAllNow()
{
	for (CameraDock *each : instances) {
		each->refreshNow();
	}
}

/* Pílula da coluna das escondidas: traz a entrada de volta para a fileira. */
void showEntryAgain(const QString &name)
{
	obs_source_t *scene = obs_get_source_by_name(name.toUtf8().constData());
	if (scene) {
		setEntryFlag(scene, KEY_HIDDEN, false);
		refreshAllNow();
		obs_source_release(scene);
	}
}

} // namespace

/* Deck pelo celular (25/11): os nomes das entradas na ordem da fileira, sem as escondidas. */
std::vector<std::string> pl_entries_visible()
{
	std::vector<obs_source_t *> list;
	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		list.push_back(obs_source_get_ref(scenes.sources.array[i]));
	}
	obs_frontend_source_list_free(&scenes);
	sortByOrder(list);
	std::vector<std::string> names;
	for (obs_source_t *scene : list) {
		if (!entryFlag(scene, KEY_HIDDEN)) {
			names.emplace_back(obs_source_get_name(scene));
		}
		obs_source_release(scene);
	}
	return names;
}

bool pl_entry_goes_clean(obs_source_t *scene)
{
	return scene && entryGoesClean(scene);
}

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

QWidget *pl_camera_grid_create(QWidget *parent)
{
	return new CameraDock(parent, true);
}

void pl_camera_dock_set_compact(QWidget *created, bool compact)
{
	for (CameraDock *each : instances) {
		if (each == created) {
			each->setCompact(compact);
		}
	}
}

void pl_entries_set_sources_opener(void (*opener)())
{
	sourcesOpener = opener;
}

void pl_entries_show_sources(obs_source_t *scene)
{
	if (scene) {
		organizeEntry(scene);
	}
}

void pl_entries_add_source(obs_source_t *scene)
{
	if (scene) {
		openAddSource(scene);
	}
}
