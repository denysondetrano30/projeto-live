/*
 * somun live: "Relatar problema" (decidido com ela em 25/11), no ⋯ da mesa.
 *
 * Janela simples: o que aconteceu, contato (opcional) e o print da janela (dá para tirar). Vai junto
 * o registro técnico (o fim do registro do OBS de hoje), a versão instalada e a conta (e-mail).
 * Grava em Firestore › relatos (sem login; as regras só deixam criar, com tamanho limitado). Ela lê
 * na página "Relatos" do Diretor. O somun tela e o Diretor gravam no mesmo lugar.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "relatar.h"

#include <obs-module.h>
#include <util/platform.h>

#include <curl/curl.h>

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSysInfo>
#include <QTimer>
#include <QVBoxLayout>

#include <string>
#include <thread>

#include "somun-style.h"

namespace {

/* Os mesmos dados públicos do pl-account.cpp (não são senha: quem protege são as regras). */
const char *const API_KEY = "AIzaSyBubqovhUfM_HFrlGfFhBygVg2FjMi5DD0";
const char *const PROJECT_ID = "projeto-live-e7ccb";

QString msg(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QJsonObject text(const QString &value)
{
	QJsonObject object;
	object["stringValue"] = value;
	return object;
}

QString installedNumber()
{
	QFile file(QDir(QCoreApplication::applicationDirPath()).filePath("../../../somun-versao.txt"));
	return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
}

QString accountEmail()
{
	char *path = obs_module_config_path("conta.json");
	QString email;
	if (path) {
		QFile file(QString::fromUtf8(path));
		if (file.open(QIODevice::ReadOnly)) {
			email = QJsonDocument::fromJson(file.readAll()).object().value("email").toString();
		}
		bfree(path);
	}
	return email;
}

/* O fim do registro do OBS mais novo (o desta sessão). */
QString obsLog()
{
	char *path = os_get_config_path_ptr("obs-studio/logs");
	if (!path) {
		return QString();
	}
	QDir dir(QString::fromUtf8(path));
	bfree(path);
	QFileInfoList files = dir.entryInfoList({"*.txt"}, QDir::Files, QDir::Time);
	if (files.isEmpty()) {
		return QString();
	}
	QFile file(files.first().absoluteFilePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return QString();
	}
	qint64 size = file.size();
	if (size > 50000) {
		file.seek(size - 50000);
	}
	return files.first().fileName() + "\n" + QString::fromUtf8(file.readAll());
}

QString jpegDataUrl(const QPixmap &shot)
{
	if (shot.isNull()) {
		return QString();
	}
	QImage image = shot.toImage();
	if (image.width() > 1280) {
		image = image.scaledToWidth(1280, Qt::SmoothTransformation);
	}
	QByteArray bytes;
	for (int quality = 60; quality >= 20; quality -= 10) {
		bytes.clear();
		QBuffer buffer(&bytes);
		buffer.open(QIODevice::WriteOnly);
		image.save(&buffer, "JPG", quality);
		if (bytes.size() <= 560000) {
			break;
		}
	}
	return "data:image/jpeg;base64," + QString::fromLatin1(bytes.toBase64());
}

size_t toString(char *data, size_t size, size_t count, void *user)
{
	static_cast<std::string *>(user)->append(data, size * count);
	return size * count;
}

/* POST no Firestore (roda fora da janela). Devolve "" se deu certo, ou o motivo. */
QString post(const QByteArray &body)
{
	CURL *curl = curl_easy_init();
	if (!curl) {
		return "curl";
	}
	std::string url = std::string("https://firestore.googleapis.com/v1/projects/") + PROJECT_ID +
			  "/databases/(default)/documents/relatos?key=" + API_KEY;
	std::string answer;
	struct curl_slist *headers = curl_slist_append(nullptr, "Content-Type: application/json");
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.constData());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, toString);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &answer);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 40L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
#if defined(_WIN32) && defined(CURLSSLOPT_NATIVE_CA)
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
	CURLcode code = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	if (code != CURLE_OK) {
		return QString::fromUtf8(curl_easy_strerror(code));
	}
	if (status != 200) {
		return QString("HTTP %1").arg(status);
	}
	return QString();
}

class RelatarDialog : public QDialog {
public:
	RelatarDialog(QWidget *parent, const QString &print) : QDialog(parent), print(print)
	{
		setWindowTitle(msg("Relatar.Title"));
		setStyleSheet(somun_window_style());
		setMinimumWidth(460);
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(18, 16, 18, 16);
		layout->setSpacing(12);

		auto *hint = new QLabel(msg("Relatar.Hint"), this);
		hint->setWordWrap(true);
		hint->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY2));
		layout->addWidget(hint);

		body = new QPlainTextEdit(this);
		body->setPlaceholderText(msg("Relatar.What"));
		body->setMinimumHeight(130);
		layout->addWidget(body);

		contact = new QLineEdit(this);
		contact->setPlaceholderText(msg("Relatar.Contact"));
		layout->addWidget(contact);

		if (!print.isEmpty()) {
			auto *row = new QHBoxLayout();
			withPrint = new QCheckBox(msg("Relatar.WithPrint"), this);
			withPrint->setChecked(true);
			auto *thumb = new QLabel(this);
			QPixmap thumbImage;
			thumbImage.loadFromData(QByteArray::fromBase64(print.mid(print.indexOf(',') + 1).toLatin1()));
			thumb->setPixmap(thumbImage.scaledToWidth(120, Qt::SmoothTransformation));
			thumb->setStyleSheet(QString("border: 1px solid %1;").arg(SOMUN_LINE2));
			row->addWidget(thumb);
			row->addWidget(withPrint, 1);
			layout->addLayout(row);
		}

		error = new QLabel(this);
		error->setWordWrap(true);
		error->setStyleSheet(QString("color: %1;").arg(SOMUN_RED));
		error->setVisible(false);
		layout->addWidget(error);

		auto *buttons = new QHBoxLayout();
		buttons->addStretch(1);
		auto *cancel = new QPushButton(msg("Relatar.Cancel"), this);
		connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
		buttons->addWidget(cancel);
		send = new QPushButton(msg("Relatar.Send"), this);
		send->setObjectName("somunPrimary");
		send->setStyleSheet(
			QString("QPushButton { background: %1; color: %2; border: 0; border-radius: 7px; "
				"padding: 6px 16px; font-weight: 600; } QPushButton:disabled { background: #8C8C8C; }")
				.arg(SOMUN_WHITE, SOMUN_BLACK));
		connect(send, &QPushButton::clicked, this, [this]() { submit(); });
		buttons->addWidget(send);
		layout->addLayout(buttons);
	}

private:
	void submit()
	{
		QString what = body->toPlainText().trimmed();
		if (what.isEmpty()) {
			showError(msg("Relatar.Empty"));
			body->setFocus();
			return;
		}
		send->setEnabled(false);
		send->setText(msg("Relatar.Sending"));
		error->setVisible(false);

		QString number = installedNumber();
		QJsonObject fields;
		fields["texto"] = text(what.left(4000));
		fields["app"] = text("live");
		fields["versao"] = text((number.isEmpty() ? QString() : number + " · ") + "OBS " +
					QString::fromUtf8(obs_get_version_string()));
		fields["sistema"] =
			text((QSysInfo::prettyProductName() + " · " + QSysInfo::currentCpuArchitecture()).left(600));
		fields["registro"] = text(obsLog().right(55000));
		QJsonObject when;
		when["timestampValue"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
		fields["em"] = when;
		QJsonObject no;
		no["booleanValue"] = false;
		fields["resolvido"] = no;
		QString who = contact->text().trimmed();
		if (!who.isEmpty()) {
			fields["contato"] = text(who.left(200));
		}
		QString email = accountEmail();
		if (!email.isEmpty()) {
			fields["conta"] = text(email.left(200));
		}
		if (withPrint && withPrint->isChecked() && print.size() <= 800000) {
			fields["print"] = text(print);
		}
		QJsonObject document;
		document["fields"] = fields;
		QByteArray payload = QJsonDocument(document).toJson(QJsonDocument::Compact);

		QPointer<RelatarDialog> self(this);
		std::thread([self, payload]() {
			QString failure = post(payload);
			QMetaObject::invokeMethod(
				qApp,
				[self, failure]() {
					if (self) {
						self->onSent(failure);
					}
				},
				Qt::QueuedConnection);
		}).detach();
	}

public:
	void onSent(const QString &failure)
	{
		if (failure.isEmpty()) {
			body->setEnabled(false);
			contact->setEnabled(false);
			send->setText(msg("Relatar.Thanks"));
			QTimer::singleShot(1800, this, &QDialog::accept);
			return;
		}
		send->setEnabled(true);
		send->setText(msg("Relatar.Send"));
		showError(msg("Relatar.Failed").arg(failure));
	}

private:
	void showError(const QString &textValue)
	{
		error->setText(textValue);
		error->setVisible(true);
	}

	QString print;
	QPlainTextEdit *body = nullptr;
	QLineEdit *contact = nullptr;
	QCheckBox *withPrint = nullptr;
	QLabel *error = nullptr;
	QPushButton *send = nullptr;
};

} // namespace

void pl_relatar_open(QWidget *window)
{
	/* O print vem antes de a janela abrir (mostra o problema, não a própria janela). */
	/* Print do que está no monitor (a Prévia e o No ar são desenhados pela placa de vídeo, fora do
	 * Qt: o grab() da janela sai preto nelas). Sem tela, cai para o grab(). */
	QPixmap shot;
	if (window) {
		QScreen *screen = window->screen();
		if (screen) {
			QRect area = window->frameGeometry();
			shot = screen->grabWindow(0, area.x() - screen->geometry().x(),
						  area.y() - screen->geometry().y(), area.width(), area.height());
		}
		if (shot.isNull()) {
			shot = window->grab();
		}
	}
	QString print = jpegDataUrl(shot);
	auto *dialog = new RelatarDialog(window, print);
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->show();
}
