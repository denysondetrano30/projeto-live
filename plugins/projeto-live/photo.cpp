/*
 * somun live: foto (imagem PNG) do No ar e do vertical, salva na pasta das gravações.
 *
 * Pega um quadro já pronto da saída de vídeo (o mesmo que vai para a live ou para o vertical),
 * convertido para RGBA, e grava como PNG com data e hora no nome.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "photo.h"
#include "vertical.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.h>
#include <media-io/video-io.h>
#include <util/bmem.h>

#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QImage>
#include <QMetaObject>
#include <QStandardPaths>
#include <QTimer>
#include <QToolTip>

#include <atomic>
#include <cstring>
#include <memory>

namespace {

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

struct Grab {
	video_t *video = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;
	QString path;
	QImage image;
	std::atomic<bool> done{false};
	bool finished = false; /* só no fio do Qt */
	/* Segura a si mesmo enquanto está ligado na saída de vídeo (solto no fim do prazo). */
	std::shared_ptr<Grab> self;
};

void onFrame(void *param, struct video_data *frame);

/* No fio do Qt, depois que o quadro chegou: solta a saída, grava o arquivo e avisa. */
void finish(const std::shared_ptr<Grab> &grab)
{
	if (grab->finished) {
		return;
	}
	grab->finished = true;
	video_output_disconnect(grab->video, onFrame, grab.get());
	bool saved = !grab->image.isNull() && grab->image.save(grab->path, "PNG");
	QString message = saved ? text("Photo.Saved").arg(QDir::toNativeSeparators(grab->path)) : text("Photo.Failed");
	QToolTip::showText(QCursor::pos(), message);
	blog(saved ? LOG_INFO : LOG_WARNING, "[somun live] foto: %s", grab->path.toUtf8().constData());
}

/* No fio do vídeo: copia o primeiro quadro e pede para o Qt terminar. */
void onFrame(void *param, struct video_data *frame)
{
	auto *grab = static_cast<Grab *>(param);
	if (grab->done.load()) {
		return;
	}
	QImage image((int)grab->width, (int)grab->height, QImage::Format_RGBA8888);
	for (uint32_t y = 0; y < grab->height; y++) {
		memcpy(image.scanLine((int)y), frame->data[0] + (size_t)y * frame->linesize[0],
		       (size_t)grab->width * 4);
	}
	/* Sem transparência na foto. */
	grab->image = image.convertToFormat(QImage::Format_RGB888);
	std::shared_ptr<Grab> keep = grab->self;
	grab->done.store(true);
	QMetaObject::invokeMethod(QCoreApplication::instance(), [keep]() { finish(keep); }, Qt::QueuedConnection);
}

/* A pasta das gravações (a mesma de Configurações › Saída); se não tiver, a pasta Vídeos. */
QString photoFolder()
{
	QString folder;
	char *path = obs_frontend_get_current_record_output_path();
	if (path) {
		folder = QString::fromUtf8(path);
		bfree(path);
	}
	if (folder.isEmpty() || !QDir(folder).exists()) {
		folder = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	}
	return folder;
}

void takePhoto(video_t *video, const QString &label)
{
	if (!video) {
		return;
	}
	const struct video_output_info *info = video_output_get_info(video);
	if (!info || !info->width || !info->height) {
		return;
	}
	auto grab = std::make_shared<Grab>();
	grab->self = grab;
	grab->video = video;
	grab->width = info->width;
	grab->height = info->height;
	QString stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH-mm-ss");
	grab->path = QDir(photoFolder()).filePath(QString("%1 %2.png").arg(label, stamp));

	struct video_scale_info conversion = {};
	conversion.format = VIDEO_FORMAT_RGBA;
	conversion.width = info->width;
	conversion.height = info->height;
	conversion.range = VIDEO_RANGE_FULL;
	conversion.colorspace = info->colorspace;
	if (!video_output_connect(video, &conversion, onFrame, grab.get())) {
		QToolTip::showText(QCursor::pos(), text("Photo.Failed"));
		grab->self.reset();
		return;
	}
	/* Se em 3 s não chegar quadro nenhum, desiste (e solta a saída). */
	QTimer::singleShot(3000, QCoreApplication::instance(), [grab]() {
		video_output_disconnect(grab->video, onFrame, grab.get());
		grab->self.reset();
		if (!grab->done.load()) {
			QToolTip::showText(QCursor::pos(), text("Photo.Failed"));
		}
	});
}

} // namespace

void pl_photo_program(void)
{
	takePhoto(obs_get_video(), text("Photo.ProgramName"));
}

void pl_photo_vertical(void)
{
	takePhoto(pl_vertical_get_video(), text("Photo.VerticalName"));
}
