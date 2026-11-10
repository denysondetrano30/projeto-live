/*
 * Projeto Live: assistente de áudio (etapa 7c).
 *
 * Passo a passo para ligar o som principal da live:
 *   1. de onde vem o som (mesa no PC, mesa digital pelo USB ou microfone);
 *   2. qual entrada do PC (cada uma com uma barrinha de nível ao vivo: a que mexe é a certa);
 *   3. teste de 5 segundos: percebe som só de um lado (liga o mono) e acerta o volume (filtro de
 *      ganho, para os picos da fala ficarem perto de -6 dB);
 *   4. se a live usa som tocado no próprio PC (se não usar, muta o "Áudio da área de trabalho");
 *   5. cria a entrada como áudio global (Mic/Aux), que vale para todas as cenas.
 *
 * Abre sozinho na primeira vez que o OBS abre sem nenhuma entrada Mic/Aux; depois fica em
 * Ferramentas > "Assistente de áudio (Projeto Live)". O atraso das câmeras é posto nessa entrada
 * sozinho (pl-sync.h).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "audio-wizard.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QButtonGroup>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

#ifdef _WIN32
const char *const INPUT_ID = "wasapi_input_capture";
#elif defined(__APPLE__)
const char *const INPUT_ID = "coreaudio_input_capture";
#else
const char *const INPUT_ID = "pulse_input_capture";
#endif

const char *const CONFIG_FILE = "audio-assistente.json";
/* Canais do OBS para "Mic/Aux" 1 a 4 (1 e 2 são o "Áudio da área de trabalho"). */
const int FIRST_MIC_CHANNEL = 3;
const int LAST_MIC_CHANNEL = 6;
const float SILENCE_DB = -60.0f;
const float TARGET_PEAK_DB = -6.0f;
const int TEST_MS = 5000;

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* Uma entrada de som do PC com o medidor de nível (para achar qual é a certa). */
struct Device {
	std::string id;
	QString name;
	obs_source_t *source = nullptr;
	obs_volmeter_t *meter = nullptr;
	std::atomic<float> peak[2];
	std::atomic<int> channels{0};
	QRadioButton *button = nullptr;
	QProgressBar *bar = nullptr;
	float shown = SILENCE_DB;

	Device()
	{
		peak[0] = SILENCE_DB;
		peak[1] = SILENCE_DB;
	}
	~Device()
	{
		if (meter) {
			obs_volmeter_remove_callback(meter, levelCallback, this);
			obs_volmeter_detach_source(meter);
			obs_volmeter_destroy(meter);
		}
		obs_source_release(source);
	}

	static void levelCallback(void *param, const float magnitude[MAX_AUDIO_CHANNELS],
				  const float peakValues[MAX_AUDIO_CHANNELS], const float inputPeak[MAX_AUDIO_CHANNELS])
	{
		(void)magnitude;
		(void)peakValues;
		auto *device = static_cast<Device *>(param);
		int count = device->meter ? obs_volmeter_get_nr_channels(device->meter) : 0;
		device->channels = count;
		for (int i = 0; i < 2 && i < count; i++) {
			float value = std::isfinite(inputPeak[i]) ? inputPeak[i] : SILENCE_DB;
			/* Guarda o maior desde a última leitura da tela. */
			float old = device->peak[i].load();
			while (value > old && !device->peak[i].compare_exchange_weak(old, value)) {
			}
		}
	}

	/* Lê e zera o pico de cada lado. */
	void take(float &left, float &right)
	{
		left = peak[0].exchange(SILENCE_DB);
		right = peak[1].exchange(SILENCE_DB);
	}
};

bool sameDevice(obs_source_t *source, const std::string &deviceId)
{
	if (!source || strcmp(obs_source_get_unversioned_id(source), INPUT_ID) != 0) {
		return false;
	}
	obs_data_t *settings = obs_source_get_settings(source);
	bool same = deviceId == obs_data_get_string(settings, "device_id");
	obs_data_release(settings);
	return same;
}

bool hasMicChannel()
{
	for (int channel = FIRST_MIC_CHANNEL; channel <= LAST_MIC_CHANNEL; channel++) {
		obs_source_t *source = obs_get_output_source(channel);
		bool used = source != nullptr;
		obs_source_release(source);
		if (used) {
			return true;
		}
	}
	return false;
}

bool configDone()
{
	char *path = obs_module_config_path(CONFIG_FILE);
	obs_data_t *data = path ? obs_data_create_from_json_file_safe(path, "bak") : nullptr;
	bfree(path);
	bool done = data && obs_data_get_bool(data, "done");
	obs_data_release(data);
	return done;
}

void saveDone()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, "done", true);
	char *path = obs_module_config_path(CONFIG_FILE);
	if (path) {
		obs_data_save_json_safe(data, path, "tmp", "bak");
		bfree(path);
	}
	obs_data_release(data);
}

/* Nome livre ("Mesa de som", "Mesa de som 2", ...). */
QString freeName(const QString &base)
{
	QString name = base;
	for (int i = 2;; i++) {
		QByteArray bytes = name.toUtf8();
		obs_source_t *existing = obs_get_source_by_name(bytes.constData());
		if (!existing) {
			return name;
		}
		obs_source_release(existing);
		name = QStringLiteral("%1 %2").arg(base).arg(i);
	}
}

class AudioWizard : public QDialog {
public:
	explicit AudioWizard(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle(moduleText("AudioWizard.Title"));
		setMinimumWidth(560);
		setAttribute(Qt::WA_DeleteOnClose);

		auto *layout = new QVBoxLayout(this);
		pages = new QStackedWidget(this);
		layout->addWidget(pages);
		buildKindPage();
		buildDevicePage();
		buildTestPage();
		buildDesktopPage();
		buildDonePage();

		auto *buttons = new QHBoxLayout();
		backButton = new QPushButton(moduleText("AudioWizard.Back"), this);
		nextButton = new QPushButton(moduleText("AudioWizard.Next"), this);
		auto *cancelButton = new QPushButton(moduleText("AudioWizard.Cancel"), this);
		buttons->addWidget(cancelButton);
		buttons->addStretch(1);
		buttons->addWidget(backButton);
		buttons->addWidget(nextButton);
		layout->addLayout(buttons);
		connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
		connect(backButton, &QPushButton::clicked, this, [this]() { go(pages->currentIndex() - 1); });
		connect(nextButton, &QPushButton::clicked, this, [this]() { next(); });

		timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, [this]() { tick(); });
		timer->start(50);
		go(0);
	}

	~AudioWizard() override { devices.clear(); }

private:
	enum Page { KindPage, DevicePage, TestPage, DesktopPage, DonePage };

	QStackedWidget *pages = nullptr;
	QPushButton *backButton = nullptr;
	QPushButton *nextButton = nullptr;
	QTimer *timer = nullptr;

	QButtonGroup *kindGroup = nullptr;
	QVBoxLayout *deviceList = nullptr;
	QButtonGroup *deviceGroup = nullptr;
	std::vector<std::unique_ptr<Device>> devices;

	QLabel *testText = nullptr;
	QPushButton *testButton = nullptr;
	QProgressBar *testBar = nullptr;
	qint64 testStart = 0;
	bool testing = false;
	bool tested = false;
	float testMax[2] = {SILENCE_DB, SILENCE_DB};
	bool forceMono = false;
	float gainDb = 0.0f;

	QButtonGroup *desktopGroup = nullptr;
	QLabel *doneText = nullptr;

	QWidget *page(const char *titleKey, const char *textKey, QVBoxLayout **body)
	{
		auto *widget = new QWidget(pages);
		auto *layout = new QVBoxLayout(widget);
		auto *title = new QLabel(QStringLiteral("<h3>%1</h3>").arg(moduleText(titleKey)), widget);
		auto *text = new QLabel(moduleText(textKey), widget);
		text->setWordWrap(true);
		layout->addWidget(title);
		layout->addWidget(text);
		*body = layout;
		pages->addWidget(widget);
		return widget;
	}

	void buildKindPage()
	{
		QVBoxLayout *body;
		QWidget *widget = page("AudioWizard.Kind.Title", "AudioWizard.Kind.Text", &body);
		kindGroup = new QButtonGroup(widget);
		const char *keys[] = {"AudioWizard.Kind.Mixer", "AudioWizard.Kind.DigitalMixer",
				      "AudioWizard.Kind.Mic"};
		for (int i = 0; i < 3; i++) {
			auto *button = new QRadioButton(moduleText(keys[i]), widget);
			kindGroup->addButton(button, i);
			body->addWidget(button);
		}
		kindGroup->button(0)->setChecked(true);
		body->addStretch(1);
	}

	void buildDevicePage()
	{
		QVBoxLayout *body;
		QWidget *widget = page("AudioWizard.Device.Title", "AudioWizard.Device.Text", &body);
		deviceGroup = new QButtonGroup(widget);
		deviceList = new QVBoxLayout();
		body->addLayout(deviceList);
		body->addStretch(1);
	}

	void buildTestPage()
	{
		QVBoxLayout *body;
		QWidget *widget = page("AudioWizard.Test.Title", "AudioWizard.Test.Text", &body);
		testButton = new QPushButton(moduleText("AudioWizard.Test.Start"), widget);
		testBar = new QProgressBar(widget);
		testBar->setRange(0, TEST_MS);
		testBar->setTextVisible(false);
		testText = new QLabel(widget);
		testText->setWordWrap(true);
		body->addWidget(testButton);
		body->addWidget(testBar);
		body->addWidget(testText);
		body->addStretch(1);
		connect(testButton, &QPushButton::clicked, this, [this]() { startTest(); });
	}

	void buildDesktopPage()
	{
		QVBoxLayout *body;
		QWidget *widget = page("AudioWizard.Desktop.Title", "AudioWizard.Desktop.Text", &body);
		desktopGroup = new QButtonGroup(widget);
		auto *keep = new QRadioButton(moduleText("AudioWizard.Desktop.Keep"), widget);
		auto *mute = new QRadioButton(moduleText("AudioWizard.Desktop.Mute"), widget);
		desktopGroup->addButton(keep, 0);
		desktopGroup->addButton(mute, 1);
		mute->setChecked(true);
		body->addWidget(keep);
		body->addWidget(mute);
		body->addStretch(1);
	}

	void buildDonePage()
	{
		QVBoxLayout *body;
		page("AudioWizard.Done.Title", "AudioWizard.Done.Text", &body);
		doneText = new QLabel(pages->widget(DonePage));
		doneText->setWordWrap(true);
		body->addWidget(doneText);
		body->addStretch(1);
	}

	/* Lista as entradas de som do PC, cada uma com medidor. */
	void loadDevices()
	{
		if (!devices.empty()) {
			return;
		}
		obs_properties_t *props = obs_get_source_properties(INPUT_ID);
		obs_property_t *list = props ? obs_properties_get(props, "device_id") : nullptr;
		size_t count = list ? obs_property_list_item_count(list) : 0;
		for (size_t i = 0; i < count; i++) {
			const char *value = obs_property_list_item_string(list, i);
			const char *name = obs_property_list_item_name(list, i);
			if (!value || !*value || strcmp(value, "default") == 0) {
				continue;
			}
			auto device = std::make_unique<Device>();
			device->id = value;
			device->name = QString::fromUtf8(name ? name : value);

			obs_data_t *settings = obs_data_create();
			obs_data_set_string(settings, "device_id", value);
			device->source =
				obs_source_create_private(INPUT_ID, "Projeto Live: teste de entrada", settings);
			obs_data_release(settings);
			if (device->source) {
				device->meter = obs_volmeter_create(OBS_FADER_LOG);
				obs_volmeter_attach_source(device->meter, device->source);
				obs_volmeter_add_callback(device->meter, Device::levelCallback, device.get());
			}

			QWidget *row = new QWidget(pages->widget(DevicePage));
			auto *rowLayout = new QHBoxLayout(row);
			rowLayout->setContentsMargins(0, 0, 0, 0);
			device->button = new QRadioButton(device->name, row);
			device->bar = new QProgressBar(row);
			device->bar->setRange(0, 600);
			device->bar->setTextVisible(false);
			device->bar->setFixedWidth(180);
			rowLayout->addWidget(device->button, 1);
			rowLayout->addWidget(device->bar);
			deviceGroup->addButton(device->button, (int)devices.size());
			deviceList->addWidget(row);
			devices.push_back(std::move(device));
		}
		if (props) {
			obs_properties_destroy(props);
		}
		if (devices.empty()) {
			deviceList->addWidget(
				new QLabel(moduleText("AudioWizard.Device.None"), pages->widget(DevicePage)));
		} else {
			devices[0]->button->setChecked(true);
		}
	}

	Device *chosen()
	{
		int index = deviceGroup ? deviceGroup->checkedId() : -1;
		return index >= 0 && index < (int)devices.size() ? devices[(size_t)index].get() : nullptr;
	}

	void go(int index)
	{
		if (index < 0 || index > DonePage) {
			return;
		}
		if (index == DevicePage) {
			loadDevices();
		}
		if (index == DonePage) {
			doneText->setText(summary());
		}
		pages->setCurrentIndex(index);
		backButton->setEnabled(index > 0 && !testing);
		nextButton->setText(moduleText(index == DonePage ? "AudioWizard.Finish" : "AudioWizard.Next"));
		nextButton->setEnabled(index != DevicePage || chosen());
	}

	void next()
	{
		int index = pages->currentIndex();
		if (index == DevicePage && !chosen()) {
			return;
		}
		if (index == DonePage) {
			finish();
			accept();
			return;
		}
		go(index + 1);
	}

	/* Barrinhas de nível e o teste de 5 segundos. */
	void tick()
	{
		for (auto &device : devices) {
			float left, right;
			if (testing && device.get() == chosen()) {
				device->take(left, right);
				testMax[0] = std::max(testMax[0], left);
				testMax[1] = std::max(testMax[1], right);
			} else {
				device->take(left, right);
			}
			float level = std::max(left, right);
			/* Cai devagar, para dar tempo de ver qual mexeu. */
			device->shown = std::max(level, device->shown - 1.5f);
			if (device->bar) {
				device->bar->setValue(
					(int)std::lround((std::max(device->shown, SILENCE_DB) - SILENCE_DB) * 10.0f));
			}
		}
		if (testing) {
			qint64 elapsed = (qint64)(os_gettime_ns() / 1000000) - testStart;
			testBar->setValue((int)std::min<qint64>(elapsed, TEST_MS));
			if (elapsed >= TEST_MS) {
				endTest();
			}
		}
	}

	void startTest()
	{
		if (!chosen()) {
			return;
		}
		testMax[0] = SILENCE_DB;
		testMax[1] = SILENCE_DB;
		testing = true;
		testStart = (qint64)(os_gettime_ns() / 1000000);
		testButton->setEnabled(false);
		backButton->setEnabled(false);
		nextButton->setEnabled(false);
		testText->setText(moduleText("AudioWizard.Test.Running"));
	}

	void endTest()
	{
		testing = false;
		tested = true;
		testButton->setEnabled(true);
		testButton->setText(moduleText("AudioWizard.Test.Again"));
		backButton->setEnabled(true);
		nextButton->setEnabled(true);

		Device *device = chosen();
		int channels = device ? device->channels.load() : 0;
		float loudest = channels >= 2 ? std::max(testMax[0], testMax[1]) : testMax[0];
		if (loudest <= -50.0f) {
			forceMono = false;
			gainDb = 0.0f;
			testText->setText(moduleText("AudioWizard.Test.NoSound"));
			return;
		}
		/* Um lado com som e o outro quase mudo: a mesa está mandando só num canal. */
		forceMono = channels >= 2 && std::min(testMax[0], testMax[1]) < loudest - 25.0f;
		gainDb = std::round(std::clamp(TARGET_PEAK_DB - loudest, -10.0f, 20.0f));
		if (std::fabs(gainDb) < 2.0f) {
			gainDb = 0.0f;
		}

		QString text = moduleText("AudioWizard.Test.Peak").arg(QString::number((double)loudest, 'f', 0));
		if (forceMono) {
			text += QStringLiteral("<br>") + moduleText("AudioWizard.Test.Mono");
		}
		if (gainDb != 0.0f) {
			text += QStringLiteral("<br>") +
				moduleText("AudioWizard.Test.Gain").arg(QString::number((double)gainDb, 'f', 0));
		} else {
			text += QStringLiteral("<br>") + moduleText("AudioWizard.Test.GainOk");
		}
		testText->setText(text);
	}

	QString kindName()
	{
		int kind = kindGroup->checkedId();
		return moduleText(kind == 2 ? "AudioWizard.Name.Mic" : "AudioWizard.Name.Mixer");
	}

	QString summary()
	{
		Device *device = chosen();
		QString text = moduleText("AudioWizard.Done.Source").arg(kindName(), device ? device->name : QString());
		if (tested) {
			if (forceMono) {
				text += QStringLiteral("<br>") + moduleText("AudioWizard.Done.Mono");
			}
			if (gainDb != 0.0f) {
				text += QStringLiteral("<br>") +
					moduleText("AudioWizard.Done.Gain").arg(QString::number((double)gainDb, 'f', 0));
			}
		}
		if (desktopGroup->checkedId() == 1) {
			text += QStringLiteral("<br>") + moduleText("AudioWizard.Done.DesktopMuted");
		}
		return text;
	}

	/* Cria (ou reaproveita) a entrada global e aplica mono, ganho e o som do PC. */
	void finish()
	{
		Device *device = chosen();
		if (!device) {
			return;
		}

		obs_source_t *source = nullptr;
		int freeChannel = -1;
		for (int channel = FIRST_MIC_CHANNEL; channel <= LAST_MIC_CHANNEL; channel++) {
			obs_source_t *existing = obs_get_output_source(channel);
			if (!existing && freeChannel < 0) {
				freeChannel = channel;
			}
			if (existing && !source && sameDevice(existing, device->id)) {
				source = obs_source_get_ref(existing);
			}
			obs_source_release(existing);
		}

		if (!source) {
			QByteArray name = freeName(kindName()).toUtf8();
			obs_data_t *settings = obs_data_create();
			obs_data_set_string(settings, "device_id", device->id.c_str());
			source = obs_source_create(INPUT_ID, name.constData(), settings, nullptr);
			obs_data_release(settings);
			if (!source) {
				return;
			}
			obs_set_output_source(freeChannel >= 0 ? freeChannel : FIRST_MIC_CHANNEL, source);
		}

		if (tested) {
			uint32_t flags = obs_source_get_flags(source);
			flags = forceMono ? (flags | OBS_SOURCE_FLAG_FORCE_MONO)
					  : (flags & ~OBS_SOURCE_FLAG_FORCE_MONO);
			obs_source_set_flags(source, flags);

			/* Troca o ganho do assistente (se já houver um) pelo novo. */
			QByteArray filterName = moduleText("AudioWizard.GainFilter").toUtf8();
			obs_source_t *old = obs_source_get_filter_by_name(source, filterName.constData());
			if (old) {
				obs_source_filter_remove(source, old);
				obs_source_release(old);
			}
			if (gainDb != 0.0f) {
				obs_data_t *settings = obs_data_create();
				obs_data_set_double(settings, "db", (double)gainDb);
				obs_source_t *filter =
					obs_source_create("gain_filter", filterName.constData(), settings, nullptr);
				obs_data_release(settings);
				if (filter) {
					obs_source_filter_add(source, filter);
					obs_source_release(filter);
				}
			}
		}
		obs_source_release(source);

		if (desktopGroup->checkedId() == 1) {
			for (int channel = 1; channel <= 2; channel++) {
				obs_source_t *desktop = obs_get_output_source(channel);
				if (desktop) {
					obs_source_set_muted(desktop, true);
					obs_source_release(desktop);
				}
			}
		}
		saveDone();
		obs_frontend_save();
		blog(LOG_INFO, "[projeto-live] assistente de áudio: entrada '%s' (mono %s, ganho %.0f dB)",
		     device->name.toUtf8().constData(), forceMono ? "sim" : "não", (double)gainDb);
	}
};

QWidget *mainWindow()
{
	return static_cast<QWidget *>(obs_frontend_get_main_window());
}

void openWizard()
{
	auto *wizard = new AudioWizard(mainWindow());
	wizard->show();
	wizard->raise();
	wizard->activateWindow();
}

void menuClicked(void *)
{
	openWizard();
}

void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		return;
	}
	/* Primeira vez: nenhuma entrada Mic/Aux e o assistente nunca foi concluído. */
	if (!configDone() && !hasMicChannel()) {
		QTimer::singleShot(1500, mainWindow(), []() { openWizard(); });
	}
}

} // namespace

void pl_audio_wizard_load(void)
{
	QByteArray name = moduleText("AudioWizard.Menu").toUtf8();
	obs_frontend_add_tools_menu_item(name.constData(), menuClicked, nullptr);
	obs_frontend_add_event_callback(frontendEvent, nullptr);
}
