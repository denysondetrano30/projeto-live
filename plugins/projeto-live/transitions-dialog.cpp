/*
 * Projeto Live: janelinha das transições.
 *
 * Abre pela engrenagem pequena do meio da janela Projeto Live e some quando é fechada (não fica
 * nenhum painel fixo como no OBS). Nela: a lista das transições (clicar escolhe a que está em uso),
 * a duração, o "+" para criar uma transição nova (Stinger, Deslizar...), dois cliques para as opções
 * dela e o botão direito para renomear ou apagar. Criar, renomear e apagar passam pelo próprio OBS
 * (OBSBasic::PLAddTransition etc.), para as listas e as transições por cena continuarem certas.
 *
 * "Transição desta entrada": o mesmo que o OBS guarda por cena (configurações privadas "transition"
 * e "transition_duration"), no botão direito de cada Entrada.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "transitions-dialog.h"
#include "source-row.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <memory>
#include <string>
#include <vector>

namespace {

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QWidget *obsMain()
{
	return static_cast<QWidget *>(obs_frontend_get_main_window());
}

struct TransitionInfo {
	QString name;
	QString uuid;
	bool configurable = false;
	bool fixed = false;
};

std::vector<TransitionInfo> listTransitions()
{
	std::vector<TransitionInfo> list;
	obs_frontend_source_list transitions = {};
	obs_frontend_get_transitions(&transitions);
	for (size_t i = 0; i < transitions.sources.num; i++) {
		obs_source_t *each = transitions.sources.array[i];
		TransitionInfo info;
		info.name = QString::fromUtf8(obs_source_get_name(each));
		info.uuid = QString::fromUtf8(obs_source_get_uuid(each));
		info.configurable = obs_source_configurable(each);
		info.fixed = obs_transition_fixed(each);
		list.push_back(info);
	}
	obs_frontend_source_list_free(&transitions);
	return list;
}

/* Nova referência da transição com esse uuid (ou nullptr). */
obs_source_t *findTransition(const QString &uuid)
{
	obs_source_t *found = nullptr;
	obs_frontend_source_list transitions = {};
	obs_frontend_get_transitions(&transitions);
	for (size_t i = 0; i < transitions.sources.num && !found; i++) {
		obs_source_t *each = transitions.sources.array[i];
		if (uuid == QString::fromUtf8(obs_source_get_uuid(each))) {
			found = obs_source_get_ref(each);
		}
	}
	obs_frontend_source_list_free(&transitions);
	return found;
}

QString currentUuid()
{
	obs_source_t *current = obs_frontend_get_current_transition();
	QString uuid = current ? QString::fromUtf8(obs_source_get_uuid(current)) : QString();
	obs_source_release(current);
	return uuid;
}

void callObs(const char *slot, const QString &value)
{
	if (QWidget *main = obsMain()) {
		QMetaObject::invokeMethod(main, slot, Qt::QueuedConnection, Q_ARG(QString, value));
	}
}

class TransitionsDialog : public QDialog {
public:
	explicit TransitionsDialog(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle(text("Transitions.Title"));
		setAttribute(Qt::WA_DeleteOnClose);
		setMinimumWidth(320);

		auto *layout = new QVBoxLayout(this);

		auto *head = new QHBoxLayout();
		auto *title = new QLabel(text("Transitions.List"), this);
		QFont bold = title->font();
		bold.setBold(true);
		title->setFont(bold);
		head->addWidget(title, 1);
		auto *add = plIconButton(this, "icon-plus", ":/res/images/plus.svg", text("Transitions.Add"));
		connect(add, &QToolButton::clicked, this, [this, add]() { addMenu(add); });
		head->addWidget(add);
		layout->addLayout(head);

		list = new QListWidget(this);
		list->setToolTip(text("Transitions.ListTip"));
		list->setContextMenuPolicy(Qt::CustomContextMenu);
		connect(list, &QListWidget::currentRowChanged, this, [this](int row) { choose(row); });
		connect(list, &QListWidget::itemDoubleClicked, this,
			[this](QListWidgetItem *item) { openOptions(item->data(Qt::UserRole).toString()); });
		connect(list, &QListWidget::customContextMenuRequested, this,
			[this](const QPoint &pos) { itemMenu(pos); });
		layout->addWidget(list, 1);

		auto *durationRow = new QHBoxLayout();
		durationLabel = new QLabel(text("Transitions.Duration"), this);
		durationRow->addWidget(durationLabel);
		duration = new QSpinBox(this);
		duration->setRange(50, 20000);
		duration->setSingleStep(50);
		duration->setSuffix(" ms");
		connect(duration, &QSpinBox::valueChanged, this,
			[](int value) { obs_frontend_set_transition_duration(value); });
		durationRow->addWidget(duration, 1);
		layout->addLayout(durationRow);

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
		layout->addWidget(buttons);

		timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, [this]() { refresh(); });
		timer->start(400);
		refresh();
	}

private:
	void refresh()
	{
		std::vector<TransitionInfo> now = listTransitions();
		QString current = currentUuid();

		QStringList signature;
		for (const TransitionInfo &each : now) {
			signature << each.uuid + "|" + each.name;
		}
		if (signature != shown) {
			shown = signature;
			transitions = now;
			QSignalBlocker block(list);
			list->clear();
			for (const TransitionInfo &each : transitions) {
				auto *item = new QListWidgetItem(each.name, list);
				item->setData(Qt::UserRole, each.uuid);
			}
		}

		bool fixed = false;
		for (int row = 0; row < (int)transitions.size(); row++) {
			if (transitions[row].uuid == current) {
				fixed = transitions[row].fixed;
				if (list->currentRow() != row) {
					QSignalBlocker block(list);
					list->setCurrentRow(row);
				}
			}
		}
		durationLabel->setEnabled(!fixed);
		duration->setEnabled(!fixed);
		int ms = obs_frontend_get_transition_duration();
		if (duration->value() != ms && !duration->hasFocus()) {
			QSignalBlocker block(duration);
			duration->setValue(ms);
		}
	}

	void choose(int row)
	{
		if (row < 0 || row >= (int)transitions.size()) {
			return;
		}
		obs_source_t *transition = findTransition(transitions[row].uuid);
		if (transition) {
			obs_frontend_set_current_transition(transition);
			obs_source_release(transition);
		}
		refresh();
	}

	/* "+": os tipos de transição que têm opções (Deslizar, Passar, Stinger, Para a cor...). */
	void addMenu(QWidget *button)
	{
		QMenu menu(this);
		size_t index = 0;
		const char *id = nullptr;
		while (obs_enum_transition_types(index++, &id)) {
			if (!obs_is_source_configurable(id)) {
				continue;
			}
			QString type = QString::fromUtf8(id);
			QAction *action = menu.addAction(QString::fromUtf8(obs_source_get_display_name(id)));
			connect(action, &QAction::triggered, this, [type]() { callObs("PLAddTransition", type); });
		}
		menu.exec(button->mapToGlobal(QPoint(0, button->height())));
	}

	void openOptions(const QString &uuid)
	{
		obs_source_t *transition = findTransition(uuid);
		if (transition && obs_source_configurable(transition)) {
			obs_frontend_open_source_properties(transition);
		}
		obs_source_release(transition);
	}

	void itemMenu(const QPoint &pos)
	{
		QListWidgetItem *item = list->itemAt(pos);
		if (!item) {
			return;
		}
		QString uuid = item->data(Qt::UserRole).toString();
		obs_source_t *transition = findTransition(uuid);
		bool configurable = transition && obs_source_configurable(transition);
		obs_source_release(transition);
		if (!configurable) {
			return;
		}

		QMenu menu(this);
		QAction *options = menu.addAction(text("Transitions.Options"));
		QAction *rename = menu.addAction(text("Transitions.Rename"));
		menu.addSeparator();
		QAction *remove = menu.addAction(text("Transitions.Remove"));
		QAction *chosen = menu.exec(list->viewport()->mapToGlobal(pos));
		if (chosen == options) {
			openOptions(uuid);
		} else if (chosen == rename) {
			callObs("PLRenameTransition", uuid);
		} else if (chosen == remove) {
			callObs("PLRemoveTransition", uuid);
		}
	}

	QListWidget *list = nullptr;
	QLabel *durationLabel = nullptr;
	QSpinBox *duration = nullptr;
	QTimer *timer = nullptr;
	QStringList shown;
	std::vector<TransitionInfo> transitions;
};

QPointer<TransitionsDialog> openDialog;

/* Lê e grava a transição própria de uma cena (o mesmo lugar onde o OBS guarda). */
void setEntryTransition(obs_weak_source_t *weak, const QString &name)
{
	obs_source_t *scene = obs_weak_source_get_source(weak);
	if (!scene) {
		return;
	}
	obs_data_t *data = obs_source_get_private_settings(scene);
	obs_data_set_string(data, "transition", name.toUtf8().constData());
	obs_data_release(data);
	obs_source_release(scene);
}

void setEntryDuration(obs_weak_source_t *weak, int ms)
{
	obs_source_t *scene = obs_weak_source_get_source(weak);
	if (!scene) {
		return;
	}
	obs_data_t *data = obs_source_get_private_settings(scene);
	obs_data_set_int(data, "transition_duration", ms);
	obs_data_release(data);
	obs_source_release(scene);
}

} // namespace

void pl_transitions_dialog_show(QWidget *parent)
{
	if (!openDialog) {
		openDialog = new TransitionsDialog(parent);
	}
	openDialog->show();
	openDialog->raise();
	openDialog->activateWindow();
}

void pl_entry_transition_menu(QMenu *menu, obs_source_t *scene)
{
	if (!menu || !scene) {
		return;
	}
	QMenu *sub = menu->addMenu(text("Transitions.Entry"));

	obs_data_t *data = obs_source_get_private_settings(scene);
	obs_data_set_default_int(data, "transition_duration", 300);
	QString chosen = QString::fromUtf8(obs_data_get_string(data, "transition"));
	int ms = (int)obs_data_get_int(data, "transition_duration");
	obs_data_release(data);

	std::shared_ptr<obs_weak_source_t> weak(obs_source_get_weak_source(scene), obs_weak_source_release);

	auto addChoice = [&](const QString &label, const QString &name) {
		QAction *action = sub->addAction(label);
		action->setCheckable(true);
		action->setChecked(name == chosen);
		QObject::connect(action, &QAction::triggered, sub, [weak, name]() { setEntryTransition(weak.get(), name); });
	};
	addChoice(text("Transitions.EntryDefault"), QString());
	for (const TransitionInfo &each : listTransitions()) {
		addChoice(each.name, each.name);
	}

	sub->addSeparator();
	auto *spin = new QSpinBox(sub);
	spin->setRange(50, 20000);
	spin->setSingleStep(50);
	spin->setSuffix(" ms");
	spin->setPrefix(text("Transitions.EntryDuration"));
	spin->setValue(ms);
	QObject::connect(spin, &QSpinBox::valueChanged, sub, [weak](int value) { setEntryDuration(weak.get(), value); });
	auto *spinAction = new QWidgetAction(sub);
	spinAction->setDefaultWidget(spin);
	sub->addAction(spinAction);
}
