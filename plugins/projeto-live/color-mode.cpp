/*
 * somun live: o modo "Cor".
 *
 * Serve para corrigir e igualar a cor das câmeras antes do culto, olhando todas juntas e grandes:
 * - no alto, o Look (um LUT só, para tudo que vai ao ar; quem aplica é artes.cpp);
 * - cada câmera tem dois faders com palavras: frio/quente e escuro/claro, com o valor do lado. Dois
 *   cliques no fader voltam ao zero;
 * - uma câmera é a "referência" e as outras ganham "igualar": compara a média de cor das duas e acerta
 *   branco e brilho. Só funciona bem quando as duas veem um pedaço em comum.
 *
 * A correção é um filtro próprio na fonte da câmera ("somun cor"). Como é um filtro normal da fonte,
 * o OBS guarda junto com o projeto e ele vale em todas as entradas que usam aquela câmera.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "color-mode.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <QApplication>
#include <QCheckBox>
#include <QEvent>
#include <QFont>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "artes.h"
#include "phone-source.h"
#include "somun-style.h"

namespace {

const char *const FILTER_ID = "somun_cor_filter";
const char *const FILTER_NAME = "somun cor";

/* A temperatura neutra e quanto o fader anda (em oitavas de Kelvin, de -100 a +100). */
const double NEUTRAL_KELVIN = 5600.0;
const double KELVIN_SPAN = 0.8;

QString msg(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* ------------------------------------------------------------------------- */
/* O filtro                                                                   */

const char *const EFFECT = R"(
uniform float4x4 ViewProj;
uniform texture2d image;
uniform float3 gain;

sampler_state textureSampler {
	Filter = Linear;
	AddressU = Clamp;
	AddressV = Clamp;
};

struct VertData {
	float4 pos : POSITION;
	float2 uv : TEXCOORD0;
};

VertData VSDefault(VertData v_in)
{
	VertData v_out;
	v_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
	v_out.uv = v_in.uv;
	return v_out;
}

float4 PSCor(VertData v_in) : TARGET
{
	float4 color = image.Sample(textureSampler, v_in.uv);
	float3 light = pow(saturate(color.rgb), float3(2.2, 2.2, 2.2)) * gain;
	color.rgb = pow(saturate(light), float3(0.4545, 0.4545, 0.4545));
	return color;
}

technique Draw
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader = PSCor(v_in);
	}
}
)";

struct CorFilter {
	obs_source_t *context = nullptr;
	gs_effect_t *effect = nullptr;
	gs_eparam_t *gainParam = nullptr;
	struct vec3 gain = {};
	bool neutral = true;
};

const char *filterName(void *)
{
	return FILTER_NAME;
}

void filterUpdate(void *data, obs_data_t *settings)
{
	auto *filter = static_cast<CorFilter *>(data);
	int cor = (int)obs_data_get_int(settings, "cor");
	int luz = (int)obs_data_get_int(settings, "luz");
	double octaves = cor / 100.0 * KELVIN_SPAN;
	double base = std::pow(2.0, luz / 100.0);
	vec3_set(&filter->gain, (float)(base * std::pow(2.0, octaves * 0.5)), (float)base,
		 (float)(base * std::pow(2.0, -octaves * 0.5)));
	filter->neutral = cor == 0 && luz == 0;
}

void *filterCreate(obs_data_t *settings, obs_source_t *source)
{
	auto *filter = new CorFilter();
	filter->context = source;
	obs_enter_graphics();
	char *errors = nullptr;
	filter->effect = gs_effect_create(EFFECT, "somun-cor.effect", &errors);
	if (filter->effect) {
		filter->gainParam = gs_effect_get_param_by_name(filter->effect, "gain");
	} else {
		blog(LOG_WARNING, "[projeto-live] filtro de cor: %s", errors ? errors : "erro no efeito");
	}
	bfree(errors);
	obs_leave_graphics();
	filterUpdate(filter, settings);
	return filter;
}

void filterDestroy(void *data)
{
	auto *filter = static_cast<CorFilter *>(data);
	obs_enter_graphics();
	gs_effect_destroy(filter->effect);
	obs_leave_graphics();
	delete filter;
}

void filterRender(void *data, gs_effect_t *)
{
	auto *filter = static_cast<CorFilter *>(data);
	if (!filter->effect || filter->neutral) {
		obs_source_skip_video_filter(filter->context);
		return;
	}
	if (!obs_source_process_filter_begin(filter->context, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) {
		return;
	}
	gs_effect_set_vec3(filter->gainParam, &filter->gain);
	obs_source_process_filter_end(filter->context, filter->effect, 0, 0);
}

/* O filtro da câmera, com referência. Com create, põe um se ainda não tiver. */
obs_source_t *cameraFilter(obs_source_t *camera, bool create)
{
	obs_source_t *filter = obs_source_get_filter_by_name(camera, FILTER_NAME);
	if (!filter && create) {
		filter = obs_source_create(FILTER_ID, FILTER_NAME, nullptr, nullptr);
		if (filter) {
			obs_source_filter_add(camera, filter);
		}
	}
	return filter;
}

int readValue(obs_source_t *camera, const char *key)
{
	obs_source_t *filter = cameraFilter(camera, false);
	if (!filter) {
		return 0;
	}
	obs_data_t *settings = obs_source_get_settings(filter);
	int value = (int)obs_data_get_int(settings, key);
	obs_data_release(settings);
	obs_source_release(filter);
	return value;
}

void writeValue(obs_source_t *camera, const char *key, int value)
{
	obs_source_t *filter = cameraFilter(camera, true);
	if (!filter) {
		return;
	}
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, key, value);
	obs_source_update(filter, settings);
	obs_data_release(settings);
	obs_source_release(filter);
}

bool isReference(obs_source_t *camera)
{
	return readValue(camera, "referencia") != 0;
}

/* ------------------------------------------------------------------------- */
/* Quais fontes são câmeras                                                   */

bool isCamera(obs_source_t *source)
{
	if (!source) {
		return false;
	}
	if (pl_phone_is_source(source)) {
		return true;
	}
	const char *id = obs_source_get_unversioned_id(source);
	if (!id) {
		return false;
	}
	for (const char *known : {"dshow_input", "av_capture_input", "v4l2_input", "decklink-input"}) {
		if (strcmp(id, known) == 0) {
			return true;
		}
	}
	return false;
}

struct CameraEntry {
	obs_source_t *camera; /* com referência */
	QString title;
	long long order;
};

bool collectCamera(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto **found = static_cast<obs_source_t **>(param);
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!*found && isCamera(source)) {
		*found = obs_source_get_ref(source);
	}
	return true;
}

/* As câmeras das entradas (não escondidas), na ordem da fileira, sem repetir a mesma câmera. */
std::vector<CameraEntry> listCameras()
{
	std::vector<CameraEntry> list;
	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_source_t *sceneSource = scenes.sources.array[i];
		obs_data_t *data = obs_source_get_private_settings(sceneSource);
		bool hidden = data && obs_data_get_bool(data, "pl_escondida");
		long long order = data && obs_data_has_user_value(data, "pl_ordem") ? obs_data_get_int(data, "pl_ordem")
										    : 100000 + (long long)i;
		obs_data_release(data);
		if (hidden) {
			continue;
		}
		obs_source_t *camera = nullptr;
		obs_scene_enum_items(obs_scene_from_source(sceneSource), collectCamera, &camera);
		if (!camera) {
			continue;
		}
		bool repeated = false;
		for (const CameraEntry &entry : list) {
			repeated = repeated || entry.camera == camera;
		}
		if (repeated) {
			obs_source_release(camera);
			continue;
		}
		list.push_back({camera, QString::fromUtf8(obs_source_get_name(sceneSource)), order});
	}
	obs_frontend_source_list_free(&scenes);
	std::stable_sort(list.begin(), list.end(),
			 [](const CameraEntry &a, const CameraEntry &b) { return a.order < b.order; });
	return list;
}

/* ------------------------------------------------------------------------- */
/* Medir a cor média de uma câmera (para "igualar")                           */

bool averageOf(obs_source_t *source, double out[3])
{
	uint32_t w = obs_source_get_width(source);
	uint32_t h = obs_source_get_height(source);
	if (!w || !h) {
		return false;
	}
	const uint32_t cx = 64, cy = 36;
	gs_texrender_t *render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(cx, cy, GS_RGBA);
	bool ok = false;
	if (render && stage && gs_texrender_begin(render, cx, cy)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		obs_source_video_render(source);
		gs_blend_state_pop();
		gs_texrender_end(render);
		gs_stage_texture(stage, gs_texrender_get_texture(render));
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(stage, &data, &linesize)) {
			double sum[3] = {0.0, 0.0, 0.0};
			for (uint32_t y = 0; y < cy; y++) {
				const uint8_t *line = data + y * linesize;
				for (uint32_t x = 0; x < cx; x++) {
					/* Em luz linear, para a conta de ganho ficar certa. */
					for (int c = 0; c < 3; c++) {
						sum[c] += std::pow(line[x * 4 + c] / 255.0, 2.2);
					}
				}
			}
			for (int c = 0; c < 3; c++) {
				out[c] = sum[c] / (cx * cy);
			}
			gs_stagesurface_unmap(stage);
			ok = true;
		}
	}
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(render);
	return ok;
}

struct MatchJob {
	obs_weak_source_t *reference = nullptr;
	obs_weak_source_t *camera = nullptr;
	QPointer<QLabel> status;
	std::function<void()> done;
};

void finishMatch(MatchJob *job, bool ok, const double ref[3], const double cam[3])
{
	obs_source_t *camera = obs_weak_source_get_source(job->camera);
	bool applied = false;
	if (camera && ok && ref[0] > 0.002 && ref[2] > 0.002 && cam[0] > 0.002 && cam[2] > 0.002) {
		double lumaRef = 0.2126 * ref[0] + 0.7152 * ref[1] + 0.0722 * ref[2];
		double lumaCam = 0.2126 * cam[0] + 0.7152 * cam[1] + 0.0722 * cam[2];
		if (lumaRef > 0.002 && lumaCam > 0.002) {
			double octaves = std::log2((ref[0] / ref[2]) / (cam[0] / cam[2]));
			int cor = readValue(camera, "cor") + (int)std::lround(octaves / KELVIN_SPAN * 100.0);
			int luz = readValue(camera, "luz") + (int)std::lround(std::log2(lumaRef / lumaCam) * 100.0);
			writeValue(camera, "cor", std::clamp(cor, -100, 100));
			writeValue(camera, "luz", std::clamp(luz, -300, 300));
			applied = true;
		}
	}
	obs_source_release(camera);
	if (job->status) {
		job->status->setText(applied ? QString() : msg("Color.MatchFailed"));
	}
	if (job->done) {
		job->done();
	}
	obs_weak_source_release(job->reference);
	obs_weak_source_release(job->camera);
	delete job;
}

/* Roda na thread de vídeo; a resposta volta para a thread da interface. */
void matchTask(void *param)
{
	auto *job = static_cast<MatchJob *>(param);
	std::vector<double> ref(3, 0.0), cam(3, 0.0);
	bool ok = false;
	obs_source_t *reference = obs_weak_source_get_source(job->reference);
	obs_source_t *camera = obs_weak_source_get_source(job->camera);
	if (reference && camera) {
		obs_enter_graphics();
		ok = averageOf(reference, ref.data()) && averageOf(camera, cam.data());
		obs_leave_graphics();
	}
	obs_source_release(reference);
	obs_source_release(camera);
	QMetaObject::invokeMethod(
		qApp, [job, ok, ref, cam]() { finishMatch(job, ok, ref.data(), cam.data()); }, Qt::QueuedConnection);
}

/* ------------------------------------------------------------------------- */
/* Peças da tela                                                              */

/* Fader que volta ao zero com dois cliques. */
class ZeroSlider : public QSlider {
public:
	explicit ZeroSlider(QWidget *parent) : QSlider(Qt::Horizontal, parent) { setFocusPolicy(Qt::NoFocus); }

	std::function<void()> reset;

protected:
	void mouseDoubleClickEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton && reset) {
			reset();
			event->accept();
			return;
		}
		QSlider::mouseDoubleClickEvent(event);
	}
};

/* palavra | fader | palavra | valor */
class WordFader : public QWidget {
public:
	WordFader(QWidget *parent, const QString &left, const QString &right, int minimum, int maximum,
		  std::function<QString(int)> format)
		: QWidget(parent),
		  format(std::move(format))
	{
		auto *row = new QHBoxLayout(this);
		row->setContentsMargins(0, 0, 0, 0);
		row->setSpacing(6);
		auto *leftLabel = new QLabel(left, this);
		auto *rightLabel = new QLabel(right, this);
		for (QLabel *label : {leftLabel, rightLabel}) {
			label->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY2));
		}
		leftLabel->setMinimumWidth(44);
		rightLabel->setMinimumWidth(44);
		row->addWidget(leftLabel);
		slider = new ZeroSlider(this);
		slider->setRange(minimum, maximum);
		slider->setToolTip(msg("Color.ZeroHint"));
		row->addWidget(slider, 1);
		row->addWidget(rightLabel);
		value = new QLabel(this);
		value->setMinimumWidth(62);
		value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		row->addWidget(value);
		slider->reset = [this]() {
			slider->setValue(0);
		};
		connect(slider, &QSlider::valueChanged, this, [this](int current) {
			value->setText(this->format(current));
			if (changed) {
				changed(current);
			}
		});
		value->setText(this->format(0));
	}

	void setQuietly(int current)
	{
		if (slider->isSliderDown() || slider->value() == current) {
			return;
		}
		QSignalBlocker block(slider);
		slider->setValue(current);
		value->setText(format(current));
	}

	std::function<void(int)> changed;

private:
	std::function<QString(int)> format;
	ZeroSlider *slider = nullptr;
	QLabel *value = nullptr;
};

/* A imagem ao vivo de uma câmera, já com a correção. */
class CameraView : public QWidget {
public:
	CameraView(QWidget *parent, obs_source_t *source) : QWidget(parent)
	{
		weak = obs_source_get_weak_source(source);
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

	~CameraView() override
	{
		destroyDisplay();
		obs_weak_source_release(weak);
	}

	QPaintEngine *paintEngine() const override { return nullptr; }

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
		auto *self = static_cast<CameraView *>(data);
		obs_source_t *source = obs_weak_source_get_source(self->weak);
		if (!source) {
			return;
		}
		uint32_t sw = obs_source_get_width(source);
		uint32_t sh = obs_source_get_height(source);
		if (sw && sh && cx && cy) {
			float scale = std::min((float)cx / (float)sw, (float)cy / (float)sh);
			int w = std::max((int)std::lround(sw * scale), 1);
			int h = std::max((int)std::lround(sh * scale), 1);
			gs_projection_push();
			gs_viewport_push();
			gs_set_viewport(((int)cx - w) / 2, ((int)cy - h) / 2, w, h);
			gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
			obs_source_video_render(source);
			gs_viewport_pop();
			gs_projection_pop();
		}
		obs_source_release(source);
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

	void destroyDisplay()
	{
		if (display) {
			obs_display_remove_draw_callback(display, draw, this);
			obs_display_destroy(display);
			display = nullptr;
		}
	}

	obs_weak_source_t *weak = nullptr;
	obs_display_t *display = nullptr;
};

QString kelvinText(int value)
{
	double kelvin = NEUTRAL_KELVIN * std::pow(2.0, value / 100.0 * KELVIN_SPAN);
	return QString("%1 K").arg((int)(std::lround(kelvin / 100.0) * 100));
}

QString lightText(int value)
{
	QString text = QString::number(std::abs(value) / 100.0, 'f', 1).replace('.', ',');
	if (value > 0) {
		return "+" + text;
	}
	return value < 0 ? QString::fromUtf8("−") + text : text;
}

class ColorPanel;

/* Uma câmera: imagem, nome, referência/igualar, os dois faders e o lugar do cadeado. */
class CameraTile : public QFrame {
public:
	CameraTile(QWidget *parent, ColorPanel *panel, obs_source_t *source, const QString &title);
	~CameraTile() override { obs_weak_source_release(weak); }

	bool isSource(obs_source_t *source) const
	{
		obs_source_t *mine = obs_weak_source_get_source(weak);
		bool same = mine == source;
		obs_source_release(mine);
		return same;
	}

	void refresh(obs_source_t *reference);

private:
	void setReference();
	void match();

	ColorPanel *panel = nullptr;
	obs_weak_source_t *weak = nullptr;
	WordFader *cor = nullptr;
	WordFader *luz = nullptr;
	QPushButton *referenceButton = nullptr;
	QPushButton *matchButton = nullptr;
	QLabel *lockLabel = nullptr;
	QLabel *status = nullptr;
};

class ColorPanel : public QWidget {
public:
	ColorPanel(QWidget *parent, std::function<void()> onExit) : QWidget(parent)
	{
		setStyleSheet(QString("QSlider::groove:horizontal { height: 4px; background: %1; border-radius: 2px; }"
				      "QSlider::handle:horizontal { background: %2; width: 14px; height: 14px;"
				      " margin: -5px 0; border-radius: 7px; }")
				      .arg(QString(SOMUN_LINE2))
				      .arg(QString(SOMUN_WHITE)));
		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(0, 0, 0, 0);
		outer->setSpacing(10);

		/* No alto: o Look, o aviso do igualar e o "sair". */
		auto *top = new QHBoxLayout();
		top->setSpacing(10);
		auto *lookTitle = new QLabel(msg("Artes.Look"), this);
		lookTitle->setObjectName("somunPanelTitle");
		lookTitle->setToolTip(msg("Artes.LookHint"));
		top->addWidget(lookTitle);
		lookButton = new QPushButton(this);
		lookButton->setFocusPolicy(Qt::NoFocus);
		lookButton->setToolTip(msg("Artes.LookHint"));
		lookButton->setMinimumWidth(170);
		connect(lookButton, &QPushButton::clicked, this, [this]() { chooseLook(); });
		top->addWidget(lookButton);
		lookCheck = new QCheckBox(msg("Artes.LookOn"), this);
		lookCheck->setFocusPolicy(Qt::NoFocus);
		connect(lookCheck, &QCheckBox::toggled, this, [](bool on) { pl_look_set_on(on); });
		top->addWidget(lookCheck);
		auto *amountLabel = new QLabel(msg("Artes.LookAmount"), this);
		amountLabel->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY2));
		top->addWidget(amountLabel);
		lookAmount = new QSlider(Qt::Horizontal, this);
		lookAmount->setRange(0, 100);
		lookAmount->setFocusPolicy(Qt::NoFocus);
		lookAmount->setFixedWidth(180);
		top->addWidget(lookAmount);
		lookValue = new QLabel(this);
		lookValue->setMinimumWidth(42);
		top->addWidget(lookValue);
		connect(lookAmount, &QSlider::valueChanged, this, [this](int value) {
			lookValue->setText(QString("%1%").arg(value));
			pl_look_set_amount(value);
		});
		top->addStretch(1);
		auto *hint = new QLabel(msg("Color.MatchHint"), this);
		hint->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY));
		top->addWidget(hint);
		if (onExit) {
			auto *exitButton = new QPushButton(msg("Color.Exit"), this);
			exitButton->setFocusPolicy(Qt::NoFocus);
			connect(exitButton, &QPushButton::clicked, this, [onExit]() { onExit(); });
			top->addWidget(exitButton);
		}
		outer->addLayout(top);

		empty = new QLabel(msg("Color.Empty"), this);
		empty->setAlignment(Qt::AlignCenter);
		empty->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY));
		outer->addWidget(empty, 1);

		grid = new QGridLayout();
		grid->setSpacing(10);
		outer->addLayout(grid, 1);

		timer = new QTimer(this);
		timer->setInterval(400);
		connect(timer, &QTimer::timeout, this, [this]() { refresh(); });
	}

	void refreshNow() { refresh(); }

	/* A câmera que é a referência, com referência do OBS (quem chama solta). */
	static obs_source_t *findReference(const std::vector<CameraEntry> &cameras)
	{
		for (const CameraEntry &entry : cameras) {
			if (isReference(entry.camera)) {
				return obs_source_get_ref(entry.camera);
			}
		}
		return nullptr;
	}

	/* Só uma câmera é a referência. */
	void makeReference(obs_source_t *chosen)
	{
		bool wasReference = isReference(chosen);
		std::vector<CameraEntry> cameras = listCameras();
		for (CameraEntry &entry : cameras) {
			if (entry.camera != chosen && isReference(entry.camera)) {
				writeValue(entry.camera, "referencia", 0);
			}
			obs_source_release(entry.camera);
		}
		writeValue(chosen, "referencia", wasReference ? 0 : 1);
		refresh();
	}

	obs_source_t *currentReference()
	{
		std::vector<CameraEntry> cameras = listCameras();
		obs_source_t *reference = findReference(cameras);
		for (CameraEntry &entry : cameras) {
			obs_source_release(entry.camera);
		}
		return reference;
	}

protected:
	void showEvent(QShowEvent *event) override
	{
		QWidget::showEvent(event);
		timer->start();
		refresh();
	}

	void hideEvent(QHideEvent *event) override
	{
		QWidget::hideEvent(event);
		timer->stop();
	}

private:
	void chooseLook()
	{
		QString file = QFileDialog::getOpenFileName(this, msg("Artes.ChooseLook"), pl_look_file(),
							    msg("Artes.LookFilter"));
		if (!file.isEmpty()) {
			pl_look_set_file(file);
			pl_look_set_on(true);
			refresh();
		}
	}

	void refreshLook()
	{
		QString file = pl_look_file();
		QString text = file.isEmpty() ? msg("Artes.LookChoose") : QFileInfo(file).completeBaseName();
		if (lookButton->text() != text) {
			lookButton->setText(text);
		}
		lookCheck->setEnabled(!file.isEmpty());
		if (lookCheck->isChecked() != pl_look_on()) {
			QSignalBlocker block(lookCheck);
			lookCheck->setChecked(pl_look_on());
		}
		if (!lookAmount->isSliderDown() && lookAmount->value() != pl_look_amount()) {
			QSignalBlocker block(lookAmount);
			lookAmount->setValue(pl_look_amount());
		}
		lookValue->setText(QString("%1%").arg(lookAmount->value()));
	}

	void refresh()
	{
		refreshLook();
		std::vector<CameraEntry> cameras = listCameras();

		bool same = cameras.size() == tiles.size();
		for (size_t i = 0; same && i < cameras.size(); i++) {
			same = tiles[i]->isSource(cameras[i].camera);
		}
		if (!same) {
			for (CameraTile *tile : tiles) {
				grid->removeWidget(tile);
				tile->hide();
				tile->deleteLater();
			}
			tiles.clear();
			int count = (int)cameras.size();
			int columns = std::max((int)std::ceil(std::sqrt((double)count)), 1);
			for (int i = 0; i < count; i++) {
				auto *tile =
					new CameraTile(this, this, cameras[(size_t)i].camera, cameras[(size_t)i].title);
				grid->addWidget(tile, i / columns, i % columns);
				tiles.push_back(tile);
			}
			empty->setVisible(cameras.empty());
		}

		obs_source_t *reference = findReference(cameras);
		for (CameraTile *tile : tiles) {
			tile->refresh(reference);
		}
		obs_source_release(reference);
		for (CameraEntry &entry : cameras) {
			obs_source_release(entry.camera);
		}
	}

	QPushButton *lookButton = nullptr;
	QCheckBox *lookCheck = nullptr;
	QSlider *lookAmount = nullptr;
	QLabel *lookValue = nullptr;
	QLabel *empty = nullptr;
	QGridLayout *grid = nullptr;
	QTimer *timer = nullptr;
	std::vector<CameraTile *> tiles;
};

CameraTile::CameraTile(QWidget *parent, ColorPanel *panel, obs_source_t *source, const QString &title)
	: QFrame(parent),
	  panel(panel)
{
	weak = obs_source_get_weak_source(source);
	setObjectName("somunPanel");
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(6);

	layout->addWidget(new CameraView(this, source), 1);

	auto *head = new QHBoxLayout();
	auto *name = new QLabel(title, this);
	name->setTextFormat(Qt::PlainText);
	QFont bold = name->font();
	bold.setBold(true);
	name->setFont(bold);
	head->addWidget(name);
	/* O lugar do aviso "travada" (quando o celular for mandado pela mesa, em etapa futura). */
	lockLabel = new QLabel(msg("Color.Locked"), this);
	lockLabel->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY2));
	lockLabel->setVisible(false);
	head->addWidget(lockLabel);
	head->addStretch(1);
	status = new QLabel(this);
	status->setStyleSheet(QString("color: %1;").arg(SOMUN_RED));
	head->addWidget(status);
	matchButton = new QPushButton(msg("Color.Match"), this);
	matchButton->setFocusPolicy(Qt::NoFocus);
	connect(matchButton, &QPushButton::clicked, this, [this]() { match(); });
	head->addWidget(matchButton);
	referenceButton = new QPushButton(msg("Color.Reference"), this);
	referenceButton->setFocusPolicy(Qt::NoFocus);
	referenceButton->setCheckable(true);
	connect(referenceButton, &QPushButton::clicked, this, [this]() { setReference(); });
	head->addWidget(referenceButton);
	layout->addLayout(head);

	cor = new WordFader(this, msg("Color.Cold"), msg("Color.Warm"), -100, 100, kelvinText);
	layout->addWidget(cor);
	luz = new WordFader(this, msg("Color.Dark"), msg("Color.Bright"), -300, 300, lightText);
	layout->addWidget(luz);
	cor->changed = [this](int value) {
		if (obs_source_t *camera = obs_weak_source_get_source(weak)) {
			writeValue(camera, "cor", value);
			obs_source_release(camera);
		}
	};
	luz->changed = [this](int value) {
		if (obs_source_t *camera = obs_weak_source_get_source(weak)) {
			writeValue(camera, "luz", value);
			obs_source_release(camera);
		}
	};
}

void CameraTile::refresh(obs_source_t *reference)
{
	obs_source_t *camera = obs_weak_source_get_source(weak);
	if (!camera) {
		return;
	}
	cor->setQuietly(readValue(camera, "cor"));
	luz->setQuietly(readValue(camera, "luz"));
	bool mine = reference == camera;
	if (referenceButton->isChecked() != mine) {
		referenceButton->setChecked(mine);
		if (mine) {
			somun_make_primary(referenceButton);
		} else {
			referenceButton->setStyleSheet(QString());
		}
	}
	matchButton->setVisible(!mine);
	matchButton->setEnabled(reference != nullptr);
	matchButton->setToolTip(msg(reference ? "Color.MatchHint" : "Color.NeedReference"));
	obs_source_release(camera);
}

void CameraTile::setReference()
{
	if (obs_source_t *camera = obs_weak_source_get_source(weak)) {
		panel->makeReference(camera);
		obs_source_release(camera);
	}
}

void CameraTile::match()
{
	obs_source_t *camera = obs_weak_source_get_source(weak);
	obs_source_t *reference = panel->currentReference();
	if (camera && reference && camera != reference) {
		status->clear();
		auto *job = new MatchJob();
		job->reference = obs_source_get_weak_source(reference);
		job->camera = obs_source_get_weak_source(camera);
		job->status = status;
		QPointer<ColorPanel> owner(panel);
		job->done = [owner]() {
			if (owner) {
				owner->refreshNow();
			}
		};
		obs_queue_task(OBS_TASK_GRAPHICS, matchTask, job, false);
	}
	obs_source_release(camera);
	obs_source_release(reference);
}

} // namespace

void pl_color_mode_load(void)
{
	struct obs_source_info info = {};
	info.id = FILTER_ID;
	info.type = OBS_SOURCE_TYPE_FILTER;
	/* Não aparece na lista de "adicionar filtro": quem cuida dele é o modo Cor. */
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CAP_OBSOLETE;
	info.get_name = filterName;
	info.create = filterCreate;
	info.destroy = filterDestroy;
	info.update = filterUpdate;
	info.video_render = filterRender;
	obs_register_source(&info);
}

QWidget *pl_color_panel_create(QWidget *parent, std::function<void()> onExit)
{
	return new ColorPanel(parent, std::move(onExit));
}
