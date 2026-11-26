/*
 * somun live: o deck pelo celular (decidido com ela em 25/11).
 *
 * O próprio live entrega o deck (25/11, depois do teste: "não faz sentido o deck do live morar no
 * tela"). Um servidor pequeno de HTTP na porta 5520 (na janela do live, sem thread) entrega a página
 * (data/deck/index.html) e a fonte; só entra quem leu o QR (código secreto depois do #, guardado em
 * somun-deck.json). A página pergunta o estado em /estado (a resposta espera até mudar, no máximo
 * 20 s) e manda os toques em /acao. Os toques fazem o mesmo que a mesa: entrada → Prévia, "Cortar",
 * arte liga/desliga (as da grade), volume (a mesma curva do mixer) e mudo. O QR fica no ⋯ do live
 * (pl_deck_open); "Trocar código" derruba quem estava com o código antigo.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "deck.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QDateTime>
#include <QDialog>
#include <QFile>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkInterface>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "artes.h"
#include "camera-dock.h"
#include "multistream.h"
#include "qrcodegen.h"
#include "somun-style.h"
#include "studio-window.h"

namespace {

QTimer *timer = nullptr;

/* A mesma curva do fader do mixer do OBS (OBS_FADER_LOG), para o deck e a mesa baterem. */
const double LOG_OFFSET_DB = 6.0;
const double LOG_RANGE_DB = 96.0;
const double LOG_OFFSET_VAL = -0.77815125038364363;
const double LOG_RANGE_VAL = -2.00860017176191756;

double dbToDeflection(double db)
{
	if (db >= 0.0) {
		return 1.0;
	}
	if (db <= -LOG_RANGE_DB) {
		return 0.0;
	}
	return (-std::log10(-db + LOG_OFFSET_DB) - LOG_RANGE_VAL) / (LOG_OFFSET_VAL - LOG_RANGE_VAL);
}

double deflectionToDb(double def)
{
	if (def >= 1.0) {
		return 0.0;
	}
	if (def <= 0.0) {
		return -INFINITY;
	}
	return -(LOG_RANGE_DB + LOG_OFFSET_DB) * std::pow((LOG_RANGE_DB + LOG_OFFSET_DB) / LOG_OFFSET_DB, -def) +
	       LOG_OFFSET_DB;
}

/* Os canais do Som: os globais com som e os sons da cena no ar (como o mixer da mesa). */
std::vector<obs_source_t *> soundSources()
{
	std::vector<obs_source_t *> list;
	for (int channel = 1; channel <= 6; channel++) {
		obs_source_t *source = obs_get_output_source(channel);
		if (!source) {
			continue;
		}
		if (obs_source_audio_active(source)) {
			list.push_back(source);
		} else {
			obs_source_release(source);
		}
	}
	obs_enum_sources(
		[](void *data, obs_source_t *source) {
			auto *found = static_cast<std::vector<obs_source_t *> *>(data);
			uint32_t flags = obs_source_get_output_flags(source);
			if ((flags & OBS_SOURCE_AUDIO) == 0 || !obs_source_audio_active(source) ||
			    !obs_source_active(source)) {
				return true;
			}
			for (obs_source_t *already : *found) {
				if (already == source) {
					return true;
				}
			}
			if (obs_source_t *held = obs_source_get_ref(source)) {
				found->push_back(held);
			}
			return true;
		},
		&list);
	return list;
}

void act(const QJsonObject &action)
{
	QString kind = action.value("a").toString();
	QByteArray id = action.value("id").toString().toUtf8();
	if (kind == "previa") {
		obs_source_t *scene = obs_get_source_by_name(id.constData());
		if (scene && obs_scene_from_source(scene)) {
			if (obs_frontend_preview_program_mode_active()) {
				obs_frontend_set_current_preview_scene(scene);
			} else {
				obs_frontend_set_current_scene(scene);
			}
		}
		obs_source_release(scene);
	} else if (kind == "cortar") {
		pl_studio_cut();
	} else if (kind == "arte") {
		pl_artes_deck_toggle(id.toStdString());
	} else if (kind == "vol" || kind == "mudo") {
		obs_source_t *source = obs_get_source_by_name(id.constData());
		if (source) {
			if (kind == "vol") {
				double def = std::clamp(action.value("v").toDouble(), 0.0, 1.0);
				double db = deflectionToDb(def);
				obs_source_set_volume(source, std::isinf(db) ? 0.0f : (float)std::pow(10.0, db / 20.0));
			} else {
				obs_source_set_muted(source, action.value("on").toBool());
			}
		}
		obs_source_release(source);
	}
}

QString stateJson()
{
	QJsonObject state;

	obs_source_t *program = obs_frontend_get_current_scene();
	obs_source_t *preview = obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene()
									   : nullptr;
	std::string programName = program ? obs_source_get_name(program) : "";
	std::string previewName = preview ? obs_source_get_name(preview) : "";
	obs_source_release(program);
	obs_source_release(preview);
	QJsonArray entries;
	for (const std::string &name : pl_entries_visible()) {
		QJsonObject entry;
		entry["id"] = QString::fromStdString(name);
		entry["ar"] = name == programName;
		entry["previa"] = !previewName.empty() && name == previewName;
		entries.append(entry);
	}
	state["entradas"] = entries;

	QJsonArray arts;
	for (const PLDeckArt &art : pl_artes_deck_list()) {
		QJsonObject item;
		item["id"] = QString::fromStdString(art.id);
		item["nome"] = QString::fromStdString(art.name);
		item["on"] = art.on;
		arts.append(item);
	}
	state["artes"] = arts;

	QJsonArray sounds;
	for (obs_source_t *source : soundSources()) {
		float mul = obs_source_get_volume(source);
		double db = mul > 0.0f ? 20.0 * std::log10((double)mul) : -INFINITY;
		QJsonObject item;
		item["id"] = QString::fromUtf8(obs_source_get_name(source));
		item["v"] = std::round(dbToDeflection(db) * 1000.0) / 1000.0;
		item["db"] = std::isinf(db) ? -100.0 : std::round(db * 10.0) / 10.0;
		item["mudo"] = obs_source_muted(source);
		sounds.append(item);
		obs_source_release(source);
	}
	state["som"] = sounds;

	state["noAr"] = pl_multistream_active();
	state["segundos"] = (double)(pl_multistream_active() ? pl_multistream_seconds() : 0);
	return QString::fromUtf8(QJsonDocument(state).toJson(QJsonDocument::Compact));
}

/* ---- o servidor do deck (HTTP simples, na janela do live) ---- */

const quint16 PORT = 5520;
const qint64 WAIT_MS = 20000;  /* a página espera no máximo isso por um estado novo */
const qint64 ASKED_MS = 60000; /* sem ninguém perguntar há 1 min, não monta o estado */
const int REQUEST_MAX = 16384;

QTcpServer *server = nullptr;
QString code;
QByteArray state;
quint64 version = 0;
qint64 lastAsk = 0;

struct Waiting {
	QPointer<QTcpSocket> socket;
	qint64 deadline;
};
std::vector<Waiting> waiting;

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString codePath()
{
	char *path = obs_module_config_path("somun-deck.json");
	QString result = QString::fromUtf8(path ? path : "");
	bfree(path);
	return result;
}

void newCode()
{
	const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	code.clear();
	for (int i = 0; i < 12; i++) {
		code += QChar(chars[QRandomGenerator::system()->bounded(64)]);
	}
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	QJsonObject saved;
	saved["codigo"] = code;
	QFile file(codePath());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(saved).toJson(QJsonDocument::Compact));
	}
}

void loadCode()
{
	QFile file(codePath());
	if (file.open(QIODevice::ReadOnly)) {
		code = QJsonDocument::fromJson(file.readAll()).object().value("codigo").toString();
	}
	if (code.isEmpty()) {
		newCode();
	}
}

/* O endereço deste computador na rede da igreja (IPv4 particular, de preferência). */
QString lanAddress()
{
	QString any;
	for (const QNetworkInterface &net : QNetworkInterface::allInterfaces()) {
		auto flags = net.flags();
		if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
		    (flags & QNetworkInterface::IsLoopBack)) {
			continue;
		}
		for (const QNetworkAddressEntry &entry : net.addressEntries()) {
			QHostAddress ip = entry.ip();
			if (ip.protocol() != QAbstractSocket::IPv4Protocol || ip.isLoopback()) {
				continue;
			}
			if (ip.isPrivateUse()) {
				return ip.toString();
			}
			if (any.isEmpty()) {
				any = ip.toString();
			}
		}
	}
	return any.isEmpty() ? QString("127.0.0.1") : any;
}

QString deckUrl()
{
	return QString("http://%1:%2/#%3").arg(lanAddress()).arg(PORT).arg(code);
}

void reply(QTcpSocket *socket, const char *status, const char *type, const QByteArray &body,
	   const char *cache = "no-store")
{
	if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
		return;
	}
	QByteArray head = QByteArray("HTTP/1.1 ") + status + "\r\nContent-Type: " + type +
			  "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nCache-Control: " + cache +
			  "\r\nConnection: close\r\n\r\n";
	socket->write(head);
	socket->write(body);
	socket->disconnectFromHost();
}

QByteArray stateReply()
{
	return QByteArray("{\"v\":") + QByteArray::number(version) +
	       ",\"estado\":" + (state.isEmpty() ? QByteArray("null") : state) + "}";
}

void refreshState()
{
	QByteArray now = stateJson().toUtf8();
	if (now == state) {
		return;
	}
	state = now;
	version++;
	for (Waiting &item : waiting) {
		reply(item.socket, "200 OK", "application/json", stateReply());
	}
	waiting.clear();
}

void tick()
{
	qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (lastAsk && now - lastAsk < ASKED_MS) {
		refreshState();
	}
	for (auto it = waiting.begin(); it != waiting.end();) {
		if (!it->socket) {
			it = waiting.erase(it);
		} else if (now >= it->deadline) {
			reply(it->socket, "200 OK", "application/json", stateReply());
			it = waiting.erase(it);
		} else {
			++it;
		}
	}
}

void serveFile(QTcpSocket *socket, const QString &name, const char *type, const char *cache)
{
	char *path = obs_module_file(name.toUtf8().constData());
	QFile file(QString::fromUtf8(path ? path : ""));
	bfree(path);
	if (!file.open(QIODevice::ReadOnly)) {
		reply(socket, "404 Not Found", "text/plain", "nao achei");
		return;
	}
	reply(socket, "200 OK", type, file.readAll(), cache);
}

void handle(QTcpSocket *socket, const QByteArray &method, const QString &target, const QByteArray &body)
{
	QUrl url(target);
	QString path = url.path();
	QUrlQuery query(url);
	if (method == "GET" && (path == "/" || path == "/index.html")) {
		serveFile(socket, "deck/index.html", "text/html; charset=utf-8", "no-store");
		return;
	}
	if (method == "GET" && path.startsWith("/fonts/")) {
		static const QStringList fonts = {"Outfit-Regular.ttf", "Outfit-Medium.ttf", "Outfit-SemiBold.ttf",
						  "Outfit-ExtraBold.ttf", "Outfit-ExtraLight.ttf"};
		QString name = path.mid(7);
		if (fonts.contains(name)) {
			serveFile(socket, "fonts/" + name, "font/ttf", "max-age=86400");
		} else {
			reply(socket, "404 Not Found", "text/plain", "nao achei");
		}
		return;
	}
	if (path != "/estado" && path != "/acao") {
		reply(socket, "404 Not Found", "text/plain", "nao achei");
		return;
	}
	if (code.isEmpty() || query.queryItemValue("c") != code) {
		reply(socket, "403 Forbidden", "application/json", "{\"erro\":\"codigo\"}");
		return;
	}
	lastAsk = QDateTime::currentMSecsSinceEpoch();
	if (path == "/estado" && method == "GET") {
		if (state.isEmpty()) {
			refreshState();
		}
		if (query.queryItemValue("v").toULongLong() != version) {
			reply(socket, "200 OK", "application/json", stateReply());
		} else {
			waiting.push_back({QPointer<QTcpSocket>(socket), lastAsk + WAIT_MS});
		}
		return;
	}
	if (path == "/acao" && method == "POST") {
		QJsonObject action = QJsonDocument::fromJson(body).object();
		static const QStringList allowed = {"previa", "cortar", "arte", "vol", "mudo"};
		if (allowed.contains(action.value("a").toString())) {
			act(action);
		}
		reply(socket, "204 No Content", "text/plain", QByteArray());
		/* deixa o live terminar a troca e já avisa os celulares */
		QTimer::singleShot(60, []() { refreshState(); });
		return;
	}
	reply(socket, "405 Method Not Allowed", "text/plain", "metodo");
}

void readRequest(QTcpSocket *socket)
{
	QByteArray buffer = socket->property("pl_buf").toByteArray() + socket->readAll();
	if (buffer.size() > REQUEST_MAX) {
		socket->abort();
		return;
	}
	int end = buffer.indexOf("\r\n\r\n");
	if (end < 0) {
		socket->setProperty("pl_buf", buffer);
		return;
	}
	QList<QByteArray> lines = buffer.left(end).split('\n');
	QList<QByteArray> first = lines.value(0).trimmed().split(' ');
	if (first.size() < 2) {
		socket->abort();
		return;
	}
	int length = 0;
	for (int i = 1; i < lines.size(); i++) {
		QByteArray line = lines[i].trimmed();
		if (line.toLower().startsWith("content-length:")) {
			length = line.mid(15).trimmed().toInt();
		}
	}
	if (length < 0 || length > REQUEST_MAX) {
		socket->abort();
		return;
	}
	QByteArray body = buffer.mid(end + 4);
	if (body.size() < length) {
		socket->setProperty("pl_buf", buffer);
		return;
	}
	socket->setProperty("pl_buf", QByteArray());
	socket->setProperty("pl_done", true);
	handle(socket, first[0], QString::fromUtf8(first[1]), body.left(length));
}

bool startServer()
{
	if (server && server->isListening()) {
		return true;
	}
	if (code.isEmpty()) {
		loadCode();
	}
	if (!server) {
		server = new QTcpServer();
		QObject::connect(server, &QTcpServer::newConnection, []() {
			while (QTcpSocket *socket = server->nextPendingConnection()) {
				QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
				QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
					if (!socket->property("pl_done").toBool()) {
						readRequest(socket);
					}
				});
				/* quem abriu e não pediu nada em 10 s sai */
				QTimer::singleShot(10000, socket, [socket]() {
					if (!socket->property("pl_done").toBool()) {
						socket->abort();
					}
				});
			}
		});
	}
	if (!server->listen(QHostAddress::AnyIPv4, PORT)) {
		blog(LOG_WARNING, "[somun deck] a porta %d não abriu: %s", (int)PORT,
		     server->errorString().toUtf8().constData());
		return false;
	}
	return true;
}

void stopServer()
{
	for (Waiting &item : waiting) {
		if (item.socket) {
			item.socket->abort();
		}
	}
	waiting.clear();
	if (server) {
		server->close();
		delete server;
		server = nullptr;
	}
}

/* ---- a janela do QR ---- */

QPixmap qrPixmap(const QString &url, int side)
{
	QPixmap pixmap(side, side);
	pixmap.fill(Qt::white);
	uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
	uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
	if (!qrcodegen_encodeText(url.toUtf8().constData(), temp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, 10,
				  qrcodegen_Mask_AUTO, true)) {
		return pixmap;
	}
	int modules = qrcodegen_getSize(qr);
	int border = 3;
	double cell = (double)side / (modules + 2 * border);
	QPainter painter(&pixmap);
	painter.setPen(Qt::NoPen);
	painter.setBrush(QColor(SOMUN_BLACK));
	for (int y = 0; y < modules; y++) {
		for (int x = 0; x < modules; x++) {
			if (qrcodegen_getModule(qr, x, y)) {
				painter.drawRect(
					QRectF((x + border) * cell, (y + border) * cell, cell + 0.5, cell + 0.5));
			}
		}
	}
	return pixmap;
}

class DeckDialog : public QDialog {
public:
	explicit DeckDialog(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle(text("Deck.Title"));
		setStyleSheet(somun_window_style());
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(24, 22, 24, 22);
		layout->setSpacing(14);

		auto *hint = new QLabel(text("Deck.Hint"), this);
		hint->setWordWrap(true);
		hint->setAlignment(Qt::AlignCenter);
		hint->setStyleSheet(QString("color: %1;").arg(SOMUN_GRAY2));
		hint->setFixedWidth(320);
		layout->addWidget(hint, 0, Qt::AlignHCenter);

		qr = new QLabel(this);
		qr->setStyleSheet("background: #FFFFFF; border-radius: 10px; padding: 8px;");
		layout->addWidget(qr, 0, Qt::AlignHCenter);

		problem = new QLabel(text("Deck.PortBusy"), this);
		problem->setWordWrap(true);
		problem->setAlignment(Qt::AlignCenter);
		problem->setFixedWidth(320);
		problem->setStyleSheet(QString("color: %1;").arg(SOMUN_RED));
		layout->addWidget(problem, 0, Qt::AlignHCenter);

		auto *row = new QHBoxLayout();
		row->addStretch(1);
		auto *change = new QPushButton(text("Deck.NewCode"), this);
		change->setToolTip(text("Deck.NewCodeHint"));
		connect(change, &QPushButton::clicked, this, [this]() {
			newCode();
			/* quem estava esperando com o código antigo sai */
			for (Waiting &item : waiting) {
				reply(item.socket, "403 Forbidden", "application/json", "{\"erro\":\"codigo\"}");
			}
			waiting.clear();
			update_();
		});
		row->addWidget(change);
		row->addStretch(1);
		layout->addLayout(row);
		update_();
	}

private:
	QLabel *qr;
	QLabel *problem;

	void update_()
	{
		bool ok = startServer();
		qr->setPixmap(qrPixmap(deckUrl(), 260));
		qr->setVisible(ok);
		problem->setVisible(!ok);
	}
};

QPointer<DeckDialog> dialog;

void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING && !timer) {
		startServer();
		timer = new QTimer();
		timer->setInterval(250);
		QObject::connect(timer, &QTimer::timeout, []() { tick(); });
		timer->start();
	} else if (event == OBS_FRONTEND_EVENT_EXIT && timer) {
		timer->stop();
		delete timer;
		timer = nullptr;
		stopServer();
	}
}

} // namespace

void pl_deck_open(QWidget *parent)
{
	if (dialog) {
		dialog->raise();
		dialog->activateWindow();
		return;
	}
	dialog = new DeckDialog(parent);
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->show();
}

void pl_deck_load(void)
{
	obs_frontend_add_event_callback(frontendEvent, nullptr);
}

void pl_deck_unload(void)
{
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
}
