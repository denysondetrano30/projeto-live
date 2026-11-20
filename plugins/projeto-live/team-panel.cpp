/*
 * somun live: a "Equipe" pequena da tela Operar.
 *
 * Uma linha por celular (fonte "Celular"): o nome e, à direita, uma bolinha — vermelha quando a pessoa
 * está falando no intercom (ou quando o diretor está falando só com ela), cinza no resto. Embaixo, a
 * tecla de falar com todos ("espaço = falar").
 *
 * - Clicar no nome abre uma caixinha: escrever um recado e os atalhos (mensagens prontas que cada
 *   operador cria no "+ atalho"; botão direito no atalho apaga). Os atalhos ficam em atalhos.json e
 *   valem para todos os nomes.
 * - Segurar o mouse no nome fala só com aquela pessoa enquanto estiver segurando.
 *
 * O fone, o microfone e o volume do diretor ficam em Configurações › Áudio › Intercom.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "team-panel.h"

#include <obs-module.h>
#include <util/platform.h>

#include <QAction>
#include <QCursor>
#include <QDateTime>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>
#include <string>
#include <vector>

#include "intercom.h"
#include "phone-source.h"
#include "somun-style.h"
#include "talk-key.h"

namespace {

const char *const SHORTCUTS_FILE = "atalhos.json";

/* Segurar o nome por mais que isto = falar; menos = clique (abre a caixinha). */
const int HOLD_MS = 250;

QString msg(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QStringList loadShortcuts()
{
	QStringList list;
	char *path = obs_module_config_path(SHORTCUTS_FILE);
	obs_data_t *data = path ? obs_data_create_from_json_file_safe(path, "bak") : nullptr;
	bfree(path);
	if (data) {
		obs_data_array_t *array = obs_data_get_array(data, "atalhos");
		size_t count = array ? obs_data_array_count(array) : 0;
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(array, i);
			QString text = QString::fromUtf8(obs_data_get_string(item, "texto")).trimmed();
			if (!text.isEmpty()) {
				list.append(text);
			}
			obs_data_release(item);
		}
		obs_data_array_release(array);
		obs_data_release(data);
	}
	return list;
}

void saveShortcuts(const QStringList &list)
{
	obs_data_t *data = obs_data_create();
	obs_data_array_t *array = obs_data_array_create();
	for (const QString &text : list) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "texto", text.toUtf8().constData());
		obs_data_array_push_back(array, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "atalhos", array);
	obs_data_array_release(array);
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path(SHORTCUTS_FILE);
	if (path) {
		obs_data_save_json_safe(data, path, "tmp", "bak");
		bfree(path);
	}
	obs_data_release(data);
}

/* Manda um recado ao celular desta fonte. Devolve se foi. */
bool sendTo(obs_weak_source_t *weak, const QString &text)
{
	obs_source_t *source = obs_weak_source_get_source(weak);
	if (!source) {
		return false;
	}
	uint32_t id = pl_phone_send_command(source, text.left(PL_COMMAND_MAX).toUtf8().constData());
	obs_source_release(source);
	return id != 0;
}

/* A caixinha que abre ao clicar no nome: recado escrito e atalhos. */
class TeamMessageBox : public QFrame {
public:
	TeamMessageBox(QWidget *anchor, obs_weak_source_t *source, const QString &name)
		: QFrame(anchor, Qt::Popup), weak(source)
	{
		/* A caixinha guarda a própria referência (a linha pode sumir antes dela). */
		obs_weak_source_addref(weak);
		this->name = name;
		setAttribute(Qt::WA_DeleteOnClose);
		setObjectName("somunBox");
		setStyleSheet(QString("QFrame#somunBox { background: %1; border: 1px solid %2; border-radius: 8px; }"
				      "QFrame#somunBox QLabel { color: %3; }"
				      "QFrame#somunBox QPushButton { background: %4; color: %3; border: 1px solid %2;"
				      " border-radius: 6px; padding: 4px 8px; }"
				      "QFrame#somunBox QPushButton:hover { background: #2C2C2C; }"
				      "QFrame#somunBox QLineEdit { background: %5; color: %3; border: 1px solid %2;"
				      " border-radius: 6px; padding: 4px 6px; }")
				      .arg(QString(SOMUN_PANEL))
				      .arg(QString(SOMUN_LINE2))
				      .arg(QString(SOMUN_TEXT))
				      .arg(QString(SOMUN_PANEL2))
				      .arg(QString(SOMUN_BLACK)));
		setMinimumWidth(260);

		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(10, 8, 10, 10);
		layout->setSpacing(6);

		auto *title = new QLabel(name, this);
		title->setTextFormat(Qt::PlainText);
		QFont bold = title->font();
		bold.setBold(true);
		title->setFont(bold);
		layout->addWidget(title);

		auto *line = new QHBoxLayout();
		text = new QLineEdit(this);
		text->setPlaceholderText(msg("Team.MessagePlaceholder"));
		text->setMaxLength(PL_COMMAND_MAX);
		auto *send = new QPushButton(msg("Team.Send"), this);
		line->addWidget(text, 1);
		line->addWidget(send);
		layout->addLayout(line);
		connect(send, &QPushButton::clicked, this, [this]() { sendTyped(); });
		connect(text, &QLineEdit::returnPressed, this, [this]() { sendTyped(); });

		/* Atalhos: mensagens prontas que o operador cria. */
		shortcutsBox = new QWidget(this);
		shortcutsLayout = new QVBoxLayout(shortcutsBox);
		shortcutsLayout->setContentsMargins(0, 0, 0, 0);
		shortcutsLayout->setSpacing(4);
		layout->addWidget(shortcutsBox);
		buildShortcuts();

		status = new QLabel(this);
		status->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY));
		status->setWordWrap(true);
		layout->addWidget(status);
		updateStatus();

		auto *timer = new QTimer(this);
		timer->setInterval(500);
		connect(timer, &QTimer::timeout, this, [this]() { updateStatus(); });
		timer->start();
	}

	~TeamMessageBox() override { obs_weak_source_release(weak); }

	void showNear(QWidget *anchor)
	{
		adjustSize();
		QPoint where = anchor->mapToGlobal(QPoint(0, anchor->height()));
		/* Não sai da tela: se faltar espaço embaixo, abre em cima; à direita, encosta. */
		if (QScreen *screen = anchor->screen()) {
			QRect area = screen->availableGeometry();
			if (where.y() + height() > area.bottom()) {
				where.setY(anchor->mapToGlobal(QPoint(0, 0)).y() - height());
			}
			if (where.x() + width() > area.right()) {
				where.setX(area.right() - width());
			}
		}
		move(where);
		show();
		text->setFocus();
	}

private:
	void sendTyped()
	{
		QString typed = text->text().trimmed();
		if (typed.isEmpty()) {
			return;
		}
		if (sendTo(weak, typed)) {
			text->clear();
			failed = QDateTime();
		} else {
			failed = QDateTime::currentDateTime().addSecs(4);
		}
		updateStatus();
	}

	void buildShortcuts()
	{
		while (QLayoutItem *item = shortcutsLayout->takeAt(0)) {
			if (QWidget *widget = item->widget()) {
				widget->deleteLater();
			}
			delete item;
		}
		QStringList list = loadShortcuts();
		for (const QString &shortcut : list) {
			auto *button = new QPushButton(shortcut, shortcutsBox);
			button->setStyleSheet("text-align: left;");
			button->setContextMenuPolicy(Qt::CustomContextMenu);
			connect(button, &QPushButton::clicked, this, [this, shortcut]() {
				failed = sendTo(weak, shortcut) ? QDateTime() : QDateTime::currentDateTime().addSecs(4);
				updateStatus();
			});
			connect(button, &QWidget::customContextMenuRequested, this,
				[this, shortcut]() { shortcutMenu(shortcut); });
			shortcutsLayout->addWidget(button);
		}
		auto *add = new QPushButton(msg("Team.AddShortcut"), shortcutsBox);
		add->setStyleSheet(QString("QPushButton { text-align: left; color: %1; border-style: dashed; }")
					   .arg(SOMUN_GRAY2));
		connect(add, &QPushButton::clicked, this, [this]() { addShortcut(); });
		shortcutsLayout->addWidget(add);
		adjustSize();
	}

	/* A caixinha é um popup: fecha antes da pergunta e abre de novo (nova) depois. Daqui em diante
	 * só se usam cópias locais, porque esta caixinha é apagada ao fechar. */
	void addShortcut()
	{
		QPointer<QWidget> anchor(parentWidget());
		obs_weak_source_t *source = weak;
		obs_weak_source_addref(source);
		QString title = name;
		close();

		bool ok = false;
		QString typed = QInputDialog::getText(anchor ? anchor->window() : nullptr, msg("Team.AddShortcutTitle"),
						      msg("Team.AddShortcutPrompt"), QLineEdit::Normal, QString(), &ok)
					.trimmed();
		if (ok && !typed.isEmpty()) {
			QStringList list = loadShortcuts();
			if (!list.contains(typed)) {
				list.append(typed.left(PL_COMMAND_MAX));
				saveShortcuts(list);
			}
		}
		if (anchor) {
			auto *again = new TeamMessageBox(anchor, source, title);
			again->showNear(anchor);
		}
		obs_weak_source_release(source);
	}

	void shortcutMenu(const QString &shortcut)
	{
		QMenu menu(this);
		QAction *remove = menu.addAction(msg("Team.RemoveShortcut"));
		if (menu.exec(QCursor::pos()) == remove) {
			QStringList list = loadShortcuts();
			list.removeAll(shortcut);
			saveShortcuts(list);
			buildShortcuts();
		}
	}

	void updateStatus()
	{
		obs_source_t *source = obs_weak_source_get_source(weak);
		struct pl_phone_status phone = {};
		bool ok = source && pl_phone_get_status(source, &phone);
		obs_source_release(source);

		QString line;
		if (failed.isValid() && QDateTime::currentDateTime() < failed) {
			line = QString("<span style='color:%1'>%2</span>").arg(SOMUN_RED).arg(msg("Team.NotSent"));
		} else if (!ok || !phone.connected) {
			line = msg("Team.NotConnected");
		} else if (phone.last_sent) {
			line = msg("Team.Last").arg(QString::fromUtf8(phone.last_text).toHtmlEscaped());
			line += phone.last_acked >= phone.last_sent ? " · " + msg("Team.Understood")
								    : " · " + msg("Team.Waiting");
		}
		if (status->text() != line) {
			status->setText(line);
		}
		text->setEnabled(ok && phone.connected);
	}

	obs_weak_source_t *weak = nullptr;
	QString name;
	QLineEdit *text = nullptr;
	QLabel *status = nullptr;
	QWidget *shortcutsBox = nullptr;
	QVBoxLayout *shortcutsLayout = nullptr;
	QDateTime failed;
};

/* Uma linha da equipe: o nome e a bolinha. Clique abre a caixinha; segurar fala só com a pessoa. */
class MemberRow : public QWidget {
public:
	MemberRow(QWidget *parent, obs_source_t *source) : QWidget(parent)
	{
		weak = obs_source_get_weak_source(source);
		name = QString::fromUtf8(obs_source_get_name(source));
		setCursor(Qt::PointingHandCursor);
		setToolTip(msg("Team.RowHint"));

		auto *layout = new QHBoxLayout(this);
		layout->setContentsMargins(4, 3, 4, 3);
		layout->setSpacing(6);
		label = new QLabel(this);
		label->setTextFormat(Qt::PlainText);
		layout->addWidget(label, 1);
		dot = new QLabel(this);
		dot->setFixedSize(8, 8);
		layout->addWidget(dot);

		hold.setSingleShot(true);
		hold.setInterval(HOLD_MS);
		QObject::connect(&hold, &QTimer::timeout, this, [this]() { setPrivate(true); });
	}

	~MemberRow() override
	{
		setPrivate(false);
		obs_weak_source_release(weak);
	}

	bool isSource(obs_source_t *source) const
	{
		obs_source_t *mine = obs_weak_source_get_source(weak);
		bool same = mine == source;
		obs_source_release(mine);
		return same && name == QString::fromUtf8(obs_source_get_name(source));
	}

	void refresh(bool directorTalkingToAll)
	{
		obs_source_t *source = obs_weak_source_get_source(weak);
		struct pl_phone_status phone = {};
		bool ok = source && pl_phone_get_status(source, &phone);
		obs_source_release(source);

		bool connected = ok && phone.connected;
		QString text = name;
		if (label->text() != text) {
			label->setText(text);
		}
		const char *textColor = connected ? SOMUN_TEXT : SOMUN_GRAY;
		const char *dotColor = "#3A3A3A";
		if (ok && (phone.talking || talking || (directorTalkingToAll && phone.intercom))) {
			dotColor = SOMUN_RED;
		} else if (connected) {
			dotColor = "#5A5A5A";
		}
		QString style = QString("color: %1;").arg(textColor);
		if (label->styleSheet() != style) {
			label->setStyleSheet(style);
		}
		QString dotStyle = QString("background: %1; border-radius: 4px;").arg(dotColor);
		if (dot->styleSheet() != dotStyle) {
			dot->setStyleSheet(dotStyle);
		}
	}

protected:
	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton) {
			hold.start();
		}
		QWidget::mousePressEvent(event);
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton) {
			if (talking) {
				setPrivate(false);
			} else if (hold.isActive()) {
				hold.stop();
				if (rect().contains(event->pos())) {
					auto *box = new TeamMessageBox(this, weak, name);
					box->showNear(this);
				}
			}
		}
		QWidget::mouseReleaseEvent(event);
	}

private:
	void setPrivate(bool on)
	{
		hold.stop();
		if (talking == on) {
			return;
		}
		talking = on;
		obs_source_t *source = obs_weak_source_get_source(weak);
		if (source) {
			pl_phone_set_private_talk(source, on);
			obs_source_release(source);
		}
		setStyleSheet(on ? QString("background: %1; border-radius: 6px;").arg(SOMUN_PANEL2) : QString());
	}

	obs_weak_source_t *weak = nullptr;
	QString name;
	QLabel *label = nullptr;
	QLabel *dot = nullptr;
	QTimer hold;
	bool talking = false;
};

class TeamPanel : public QWidget {
public:
	explicit TeamPanel(QWidget *parent) : QWidget(parent)
	{
		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(0, 0, 0, 0);
		outer->setSpacing(2);

		auto *title = new QLabel(msg("Studio.Team"), this);
		title->setObjectName("somunPanelTitle");
		outer->addWidget(title);
		outer->addSpacing(4);

		rowsLayout = new QVBoxLayout();
		rowsLayout->setContentsMargins(0, 0, 0, 0);
		rowsLayout->setSpacing(0);
		outer->addLayout(rowsLayout);

		empty = new QLabel(msg("Team.Empty"), this);
		empty->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY));
		outer->addWidget(empty);

		auto *line = new QFrame(this);
		line->setFixedHeight(1);
		line->setStyleSheet(QString("background: %1;").arg(SOMUN_LINE));
		outer->addSpacing(6);
		outer->addWidget(line);
		outer->addSpacing(4);

		footer = new QLabel(this);
		footer->setTextFormat(Qt::RichText);
		outer->addWidget(footer);

		timer.setInterval(250);
		QObject::connect(&timer, &QTimer::timeout, this, [this]() { refresh(); });
		timer.start();
		refresh();
	}

private:
	static bool collect(void *param, obs_source_t *source)
	{
		if (pl_phone_is_source(source)) {
			static_cast<std::vector<obs_source_t *> *>(param)->push_back(obs_source_get_ref(source));
		}
		return true;
	}

	void refresh()
	{
		std::vector<obs_source_t *> found;
		obs_enum_sources(collect, &found);

		bool same = found.size() == rows.size();
		for (size_t i = 0; same && i < found.size(); i++) {
			same = rows[i]->isSource(found[i]);
		}
		if (!same) {
			for (MemberRow *row : rows) {
				rowsLayout->removeWidget(row);
				row->hide();
				row->deleteLater();
			}
			rows.clear();
			for (obs_source_t *source : found) {
				auto *row = new MemberRow(this, source);
				rowsLayout->addWidget(row);
				rows.push_back(row);
			}
		}
		for (obs_source_t *source : found) {
			obs_source_release(source);
		}

		struct pl_intercom_state state;
		pl_intercom_get_state(&state);
		for (MemberRow *row : rows) {
			row->refresh(state.talk_all);
		}
		empty->setVisible(rows.empty());

		QString text;
		if (state.error[0]) {
			text = QString("<span style='color:%1'>%2</span>")
				       .arg(SOMUN_RED)
				       .arg(QString::fromUtf8(state.error).toHtmlEscaped());
		} else if (state.talk_all) {
			text = QString("<span style='color:%1'>%2</span>").arg(SOMUN_RED).arg(msg("Team.TalkingAll"));
		} else {
			QString key = pl_talk_key_name();
			text = QString("<span style='color:%1'>%2</span>")
				       .arg(SOMUN_GRAY)
				       .arg(key.isEmpty() ? msg("Team.NoKey")
							  : msg("Team.KeyHint").arg(key.toLower().toHtmlEscaped()));
		}
		if (footer->text() != text) {
			footer->setText(text);
		}
	}

	QVBoxLayout *rowsLayout = nullptr;
	QLabel *empty = nullptr;
	QLabel *footer = nullptr;
	std::vector<MemberRow *> rows;
	QTimer timer;
};

} // namespace

QWidget *pl_team_panel_create(QWidget *parent)
{
	return new TeamPanel(parent);
}
