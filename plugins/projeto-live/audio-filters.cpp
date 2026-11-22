/*
 * somun live: janela "Filtros do som" (parte H1, 22/11).
 *
 * Botão direito num canal do painel Som › "Filtros do som...". Em vez da janela de filtros do OBS
 * (completa, mas cheia de botões e nomes técnicos), três linhas com liga/desliga e um fader com o
 * valor do lado, mais mono e balanço:
 *
 * - Tirar ruído: o filtro de redução de ruído do OBS no modo RNNoise (inteligência artificial; tira
 *   chiado, ar-condicionado, ventilador). Esse modo não tem "força", então é só liga/desliga.
 * - Segurar picos: um compressor (o fader é o limiar: quanto mais baixo, mais segura) e um
 *   limitador em -1 dB logo depois, para o grito do pregador nunca estourar a live.
 * - Ganho: o filtro de ganho do OBS. Reaproveita o "Volume (assistente)" que o assistente de áudio
 *   criou, se existir, para não somar dois ganhos.
 * - Mono (o som que só chega num lado vai para os dois) e balanço.
 *
 * Por baixo são os filtros do próprio OBS, com nomes "somun: ...": aparecem também na janela do OBS
 * ("Avançado"), e desligar aqui só desativa o filtro (o ajuste fica guardado). Tudo vale na hora,
 * para ouvir enquanto mexe (sem botão Salvar, como uma mesa de som).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "audio-filters.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QCheckBox>
#include <QDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QString>
#include <QVBoxLayout>

#include <cmath>
#include <map>
#include <string>

#include "somun-style.h"

namespace {

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

const char *NOISE_ID = "noise_suppress_filter_v2";
const char *COMPRESSOR_ID = "compressor_filter";
const char *LIMITER_ID = "limiter_filter";
const char *GAIN_ID = "gain_filter";

/* Nomes fixos dos nossos filtros (não traduzidos: é por eles que a janela os acha de novo). */
const char *NOISE_NAME = "somun: tirar ruído";
const char *COMPRESSOR_NAME = "somun: segurar picos";
const char *LIMITER_NAME = "somun: limite";
const char *GAIN_NAME = "somun: ganho";

/* O filtro com este nome, ou nullptr. Devolve com referência (soltar com obs_source_release). */
obs_source_t *findFilter(obs_source_t *source, const char *name)
{
	return obs_source_get_filter_by_name(source, name);
}

/* O ganho: o nosso, ou o que o assistente de áudio criou. */
obs_source_t *findGain(obs_source_t *source)
{
	obs_source_t *gain = findFilter(source, GAIN_NAME);
	if (!gain) {
		gain = findFilter(source, obs_module_text("AudioWizard.GainFilter"));
	}
	return gain;
}

/* Põe os nossos filtros na ordem certa no fim da lista: ruído, ganho, picos, limite. */
void putInOrder(obs_source_t *source)
{
	obs_source_t *ordered[] = {findFilter(source, NOISE_NAME), findGain(source),
				   findFilter(source, COMPRESSOR_NAME), findFilter(source, LIMITER_NAME)};
	for (obs_source_t *filter : ordered) {
		if (filter) {
			obs_source_filter_set_order(source, filter, OBS_ORDER_MOVE_BOTTOM);
			obs_source_release(filter);
		}
	}
}

/* Cria o filtro se ainda não existir; devolve com referência. */
obs_source_t *ensureFilter(obs_source_t *source, const char *id, const char *name, obs_data_t *settings)
{
	obs_source_t *filter = findFilter(source, name);
	if (filter) {
		return filter;
	}
	filter = obs_source_create(id, name, settings, nullptr);
	if (filter) {
		obs_source_filter_add(source, filter);
		putInOrder(source);
		blog(LOG_INFO, "[projeto-live] filtro '%s' criado em '%s'", name, obs_source_get_name(source));
	}
	return filter;
}

bool filterOn(obs_source_t *filter)
{
	return filter && obs_source_enabled(filter);
}

class AudioFiltersDialog : public QDialog {
public:
	AudioFiltersDialog(obs_source_t *target, QWidget *parent) : QDialog(parent)
	{
		weak = obs_source_get_weak_source(target);
		setAttribute(Qt::WA_DeleteOnClose);
		setWindowTitle(text("AudioFilters.Title").arg(QString::fromUtf8(obs_source_get_name(target))));
		setMinimumWidth(460);

		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(16, 16, 16, 16);
		layout->setSpacing(12);

		auto *grid = new QGridLayout();
		grid->setHorizontalSpacing(12);
		grid->setVerticalSpacing(10);
		grid->setColumnStretch(1, 1);
		int row = 0;

		/* Tirar ruído: liga/desliga (o RNNoise não tem força). */
		noise = new QCheckBox(text("AudioFilters.Noise"), this);
		noise->setToolTip(text("AudioFilters.NoiseHint"));
		auto *noiseTag = new QLabel(text("AudioFilters.NoiseTag"), this);
		noiseTag->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY));
		grid->addWidget(noise, row, 0);
		grid->addWidget(noiseTag, row, 1, 1, 2);
		row++;

		/* Segurar picos: o fader é o limiar do compressor (-40 a 0 dB). */
		peaks = new QCheckBox(text("AudioFilters.Peaks"), this);
		peaks->setToolTip(text("AudioFilters.PeaksHint"));
		peaksFader = fader(-40, 0);
		peaksValue = valueLabel();
		grid->addWidget(peaks, row, 0);
		grid->addWidget(peaksFader, row, 1);
		grid->addWidget(peaksValue, row, 2);
		row++;

		/* Ganho: -20 a +20 dB. */
		gain = new QCheckBox(text("AudioFilters.Gain"), this);
		gain->setToolTip(text("AudioFilters.GainHint"));
		gainFader = fader(-20, 20);
		gainValue = valueLabel();
		grid->addWidget(gain, row, 0);
		grid->addWidget(gainFader, row, 1);
		grid->addWidget(gainValue, row, 2);
		row++;

		auto *line = new QFrame(this);
		line->setFrameShape(QFrame::HLine);
		grid->addWidget(line, row++, 0, 1, 3);

		/* Mono e balanço (0 = só esquerda, 50 = centro, 100 = só direita). */
		mono = new QCheckBox(text("AudioFilters.Mono"), this);
		mono->setToolTip(text("AudioFilters.MonoHint"));
		grid->addWidget(mono, row++, 0, 1, 3);
		auto *balanceLabel = new QLabel(text("AudioFilters.Balance"), this);
		balanceFader = fader(0, 100);
		balanceValue = valueLabel();
		grid->addWidget(balanceLabel, row, 0);
		grid->addWidget(balanceFader, row, 1);
		grid->addWidget(balanceValue, row, 2);
		row++;
		layout->addLayout(grid);

		auto *buttons = new QHBoxLayout();
		auto *advanced = new QPushButton(text("AudioFilters.Advanced"), this);
		advanced->setToolTip(text("AudioFilters.AdvancedHint"));
		auto *close = new QPushButton(text("AudioFilters.Close"), this);
		somun_make_primary(close);
		buttons->addWidget(advanced);
		buttons->addStretch(1);
		buttons->addWidget(close);
		layout->addLayout(buttons);

		load();

		connect(noise, &QCheckBox::toggled, this, [this](bool on) { setNoise(on); });
		connect(peaks, &QCheckBox::toggled, this, [this](bool on) { setPeaks(on); });
		connect(peaksFader, &QSlider::valueChanged, this, [this](int) { setPeaks(peaks->isChecked()); });
		connect(gain, &QCheckBox::toggled, this, [this](bool on) { setGain(on); });
		connect(gainFader, &QSlider::valueChanged, this, [this](int) { setGain(gain->isChecked()); });
		connect(mono, &QCheckBox::toggled, this, [this](bool on) { setMono(on); });
		connect(balanceFader, &QSlider::valueChanged, this, [this](int value) { setBalance(value); });
		connect(advanced, &QPushButton::clicked, this, [this]() {
			obs_source_t *source = obs_weak_source_get_source(weak);
			if (source) {
				obs_frontend_open_source_filters(source);
				obs_source_release(source);
			}
		});
		connect(close, &QPushButton::clicked, this, &QDialog::close);
	}

	~AudioFiltersDialog() override { obs_weak_source_release(weak); }

private:
	QSlider *fader(int min, int max)
	{
		auto *slider = new QSlider(Qt::Horizontal, this);
		slider->setRange(min, max);
		slider->setPageStep(2);
		return slider;
	}

	QLabel *valueLabel()
	{
		auto *label = new QLabel(this);
		label->setMinimumWidth(64);
		label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		return label;
	}

	/* Lê o que já existe no canal (filtros, mono, balanço) e põe na tela, sem disparar mudanças. */
	void load()
	{
		obs_source_t *source = obs_weak_source_get_source(weak);
		if (!source) {
			return;
		}
		QSignalBlocker b1(noise), b2(peaks), b3(peaksFader), b4(gain), b5(gainFader), b6(mono),
			b7(balanceFader);

		obs_source_t *noiseFilter = findFilter(source, NOISE_NAME);
		noise->setChecked(filterOn(noiseFilter));
		obs_source_release(noiseFilter);

		obs_source_t *compressor = findFilter(source, COMPRESSOR_NAME);
		peaks->setChecked(filterOn(compressor));
		int threshold = -18;
		if (compressor) {
			obs_data_t *settings = obs_source_get_settings(compressor);
			threshold = (int)obs_data_get_double(settings, "threshold");
			obs_data_release(settings);
		}
		peaksFader->setValue(threshold);
		obs_source_release(compressor);

		obs_source_t *gainFilter = findGain(source);
		gain->setChecked(filterOn(gainFilter));
		int db = 0;
		if (gainFilter) {
			obs_data_t *settings = obs_source_get_settings(gainFilter);
			db = (int)std::lround(obs_data_get_double(settings, "db"));
			obs_data_release(settings);
		}
		gainFader->setValue(db);
		obs_source_release(gainFilter);

		mono->setChecked((obs_source_get_flags(source) & OBS_SOURCE_FLAG_FORCE_MONO) != 0);
		balanceFader->setValue((int)std::lround(obs_source_get_balance_value(source) * 100.0f));
		obs_source_release(source);
		updateValues();
	}

	void updateValues()
	{
		peaksValue->setText(QString("%1 dB").arg(peaksFader->value()));
		int db = gainFader->value();
		gainValue->setText(QString("%1%2 dB").arg(db > 0 ? "+" : "").arg(db));
		int balance = balanceFader->value();
		if (balance == 50) {
			balanceValue->setText(text("AudioFilters.Center"));
		} else if (balance < 50) {
			balanceValue->setText(text("AudioFilters.Left").arg(50 - balance));
		} else {
			balanceValue->setText(text("AudioFilters.Right").arg(balance - 50));
		}
		peaksFader->setEnabled(peaks->isChecked());
		gainFader->setEnabled(gain->isChecked());
	}

	void setNoise(bool on)
	{
		obs_source_t *source = obs_weak_source_get_source(weak);
		if (!source) {
			return;
		}
		obs_source_t *filter = findFilter(source, NOISE_NAME);
		if (!filter && on) {
			obs_data_t *settings = obs_data_create();
			obs_data_set_string(settings, "method", "rnnoise");
			filter = ensureFilter(source, NOISE_ID, NOISE_NAME, settings);
			obs_data_release(settings);
		}
		if (filter) {
			obs_source_set_enabled(filter, on);
			obs_source_release(filter);
		}
		obs_source_release(source);
		updateValues();
	}

	void setPeaks(bool on)
	{
		obs_source_t *source = obs_weak_source_get_source(weak);
		if (!source) {
			return;
		}
		obs_source_t *compressor = findFilter(source, COMPRESSOR_NAME);
		obs_source_t *limiter = findFilter(source, LIMITER_NAME);
		if (on) {
			obs_data_t *settings = obs_data_create();
			obs_data_set_double(settings, "ratio", 4.0);
			obs_data_set_double(settings, "threshold", (double)peaksFader->value());
			obs_data_set_int(settings, "attack_time", 6);
			obs_data_set_int(settings, "release_time", 60);
			obs_data_set_double(settings, "output_gain", 0.0);
			if (!compressor) {
				compressor = ensureFilter(source, COMPRESSOR_ID, COMPRESSOR_NAME, settings);
			} else {
				obs_source_update(compressor, settings);
			}
			obs_data_release(settings);
			if (!limiter) {
				obs_data_t *limit = obs_data_create();
				obs_data_set_double(limit, "threshold", -1.0);
				obs_data_set_int(limit, "release_time", 60);
				limiter = ensureFilter(source, LIMITER_ID, LIMITER_NAME, limit);
				obs_data_release(limit);
			}
		}
		if (compressor) {
			obs_source_set_enabled(compressor, on);
			obs_source_release(compressor);
		}
		if (limiter) {
			obs_source_set_enabled(limiter, on);
			obs_source_release(limiter);
		}
		obs_source_release(source);
		updateValues();
	}

	void setGain(bool on)
	{
		obs_source_t *source = obs_weak_source_get_source(weak);
		if (!source) {
			return;
		}
		obs_source_t *filter = findGain(source);
		if (on) {
			obs_data_t *settings = obs_data_create();
			obs_data_set_double(settings, "db", (double)gainFader->value());
			if (!filter) {
				filter = ensureFilter(source, GAIN_ID, GAIN_NAME, settings);
			} else {
				obs_source_update(filter, settings);
			}
			obs_data_release(settings);
		}
		if (filter) {
			obs_source_set_enabled(filter, on);
			obs_source_release(filter);
		}
		obs_source_release(source);
		updateValues();
	}

	void setMono(bool on)
	{
		obs_source_t *source = obs_weak_source_get_source(weak);
		if (!source) {
			return;
		}
		uint32_t flags = obs_source_get_flags(source);
		obs_source_set_flags(source,
				     on ? (flags | OBS_SOURCE_FLAG_FORCE_MONO) : (flags & ~OBS_SOURCE_FLAG_FORCE_MONO));
		obs_source_release(source);
	}

	void setBalance(int value)
	{
		obs_source_t *source = obs_weak_source_get_source(weak);
		if (source) {
			obs_source_set_balance_value(source, (float)value / 100.0f);
			obs_source_release(source);
		}
		updateValues();
	}

	obs_weak_source_t *weak = nullptr;
	QCheckBox *noise = nullptr;
	QCheckBox *peaks = nullptr;
	QSlider *peaksFader = nullptr;
	QLabel *peaksValue = nullptr;
	QCheckBox *gain = nullptr;
	QSlider *gainFader = nullptr;
	QLabel *gainValue = nullptr;
	QCheckBox *mono = nullptr;
	QSlider *balanceFader = nullptr;
	QLabel *balanceValue = nullptr;
};

std::map<std::string, QPointer<AudioFiltersDialog>> openDialogs;

} // namespace

void pl_audio_filters_open(obs_source_t *source, QWidget *parent)
{
	if (!source) {
		return;
	}
	std::string key = obs_source_get_uuid(source);
	QPointer<AudioFiltersDialog> &dialog = openDialogs[key];
	if (dialog) {
		dialog->raise();
		dialog->activateWindow();
		return;
	}
	dialog = new AudioFiltersDialog(source, parent);
	dialog->show();
}
