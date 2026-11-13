/*
 * Projeto Live: a conta da igreja dentro do OBS.
 *
 * O operador entra uma vez com o e-mail e a senha da conta da igreja (Ferramentas › Conta do
 * Projeto Live, ou o selo no alto da janela do Projeto Live). A senha não é guardada: só o e-mail,
 * o id da conta e o "refresh token" (o login que dura), em conta.json, na pasta do plugin.
 *
 * Com a conta, o OBS escolhe sozinho o culto de hoje (o publicado no app do Diretor com o horário
 * mais perto de agora; se houver mais de um no dia, o operador troca no selo) e, a cada 3 segundos,
 * busca os recados do diretor "para Todos" e "para Transmissão". Eles aparecem no canto de cima, à
 * direita, da janela do Projeto Live e da janela do OBS, com respostas prontas (OK, Pronto, Agora
 * não) e "Responder…". A resposta volta para o celular do diretor.
 *
 * Tudo é a mesma conta do site do Diretor e da Projeção (Firebase, pelas chamadas web: login e
 * Firestore). Se a internet cair, a live segue normal; os recados só voltam quando ela voltar.
 *
 * As chamadas de rede rodam numa linha de trabalho só delas (curl), nunca na tela.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "pl-account.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <curl/curl.h>

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QEvent>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

/* Os mesmos dados públicos do app do Diretor e da Projeção (não são senha: quem protege os dados
 * são o login e as regras do Firestore). */
const char *const API_KEY = "AIzaSyBubqovhUfM_HFrlGfFhBygVg2FjMi5DD0";
const char *const PROJECT_ID = "projeto-live-e7ccb";
const char *const ACCOUNT_FILE = "conta.json";
const char *const SIGN_IN_URL = "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=";
const int POLL_MS = 3000;
const int CULTOS_MS = 5 * 60 * 1000;
const qint64 SLACK_MS = 2 * 60 * 1000; /* os relógios do celular e do computador nunca batem */

QString msg(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString isoNow(qint64 offsetMs = 0)
{
	return QDateTime::currentDateTimeUtc().addMSecs(offsetMs).toString(Qt::ISODateWithMs);
}

/* ------------------------------------------------------------------------- */
/* Rede (só na linha de trabalho)                                             */

std::atomic<bool> stopping{false};

size_t writeBody(char *data, size_t size, size_t count, void *user)
{
	static_cast<std::string *>(user)->append(data, size * count);
	return size * count;
}

int keepGoing(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	return stopping ? 1 : 0;
}

struct Http {
	long status = 0;
	QByteArray body;
	bool network = false; /* não chegou a falar com o servidor */
};

Http request(const char *method, const std::string &url, const QByteArray &body, const char *type,
	     const std::string &bearer)
{
	Http result;
	CURL *curl = curl_easy_init();
	if (!curl) {
		result.network = true;
		return result;
	}
	std::string answer;
	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, (std::string("Content-Type: ") + type).c_str());
	if (!bearer.empty()) {
		headers = curl_slist_append(headers, ("Authorization: Bearer " + bearer).c_str());
	}
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.constData());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &answer);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, keepGoing);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
#if defined(_WIN32) && defined(CURLSSLOPT_NATIVE_CA)
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
	CURLcode code = curl_easy_perform(curl);
	if (code == CURLE_OK) {
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
		result.body = QByteArray(answer.data(), (qsizetype)answer.size());
	} else {
		result.network = true;
	}
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return result;
}

QByteArray toJson(const QJsonObject &object)
{
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QJsonObject text(const QString &value)
{
	QJsonObject object;
	object["stringValue"] = value;
	return object;
}

QString field(const QJsonObject &fields, const char *key)
{
	return fields.value(key).toObject().value("stringValue").toString();
}

QJsonObject path(const char *name)
{
	QJsonObject object;
	object["fieldPath"] = name;
	return object;
}

QJsonObject filter(const char *name, const char *op, const QString &value)
{
	QJsonObject inner;
	inner["field"] = path(name);
	inner["op"] = op;
	inner["value"] = text(value);
	QJsonObject object;
	object["fieldFilter"] = inner;
	return object;
}

QJsonObject collection(const char *name)
{
	QJsonObject object;
	object["collectionId"] = name;
	return object;
}

std::string documents()
{
	return std::string("https://firestore.googleapis.com/v1/projects/") + PROJECT_ID +
	       "/databases/(default)/documents";
}

enum class Fail { None, Network, Expired, WrongPassword, Other };

/* O token de acesso vale uma hora; a linha de trabalho guarda por 50 minutos. */
struct Token {
	std::string refresh;
	std::string access;
	qint64 until = 0;
};
Token token; /* só a linha de trabalho mexe */

Fail accessToken(const std::string &refresh, std::string &access, std::string &newRefresh)
{
	qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (token.refresh == refresh && token.until > now) {
		access = token.access;
		newRefresh = refresh;
		return Fail::None;
	}
	char *escaped = curl_easy_escape(nullptr, refresh.c_str(), (int)refresh.size());
	QByteArray body = QByteArray("grant_type=refresh_token&refresh_token=") + escaped;
	curl_free(escaped);
	Http http = request("POST", std::string("https://securetoken.googleapis.com/v1/token?key=") + API_KEY, body,
			    "application/x-www-form-urlencoded", "");
	if (http.network) {
		return Fail::Network;
	}
	QJsonObject answer = QJsonDocument::fromJson(http.body).object();
	if (http.status != 200) {
		return http.status == 400 || http.status == 401 || http.status == 403 ? Fail::Expired : Fail::Other;
	}
	access = answer.value("id_token").toString().toStdString();
	newRefresh = answer.value("refresh_token").toString(QString::fromStdString(refresh)).toStdString();
	token = {newRefresh, access, now + 50 * 60 * 1000};
	return Fail::None;
}

/* ------------------------------------------------------------------------- */
/* A linha de trabalho                                                        */

class Worker {
public:
	void start()
	{
		stopping = false;
		thread = std::thread([this]() { run(); });
	}
	void stop()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
			jobs.clear();
		}
		wake.notify_all();
		if (thread.joinable()) {
			thread.join();
		}
	}
	void push(std::function<void()> job)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (stopping) {
				return;
			}
			jobs.push_back(std::move(job));
		}
		wake.notify_one();
	}

private:
	void run()
	{
		for (;;) {
			std::function<void()> job;
			{
				std::unique_lock<std::mutex> lock(mutex);
				wake.wait(lock, [this]() { return stopping || !jobs.empty(); });
				if (stopping) {
					return;
				}
				job = std::move(jobs.front());
				jobs.pop_front();
			}
			job();
		}
	}

	std::thread thread;
	std::mutex mutex;
	std::condition_variable wake;
	std::deque<std::function<void()>> jobs;
};

Worker worker;

/* Leva o resultado de volta para a tela. */
void onUi(std::function<void()> work)
{
	QMetaObject::invokeMethod(qApp, std::move(work), Qt::QueuedConnection);
}

/* ------------------------------------------------------------------------- */
/* O estado da conta (na tela)                                                */

struct Culto {
	QString id;
	QString nome;
	QString data; /* AAAA-MM-DD */
	QString hora; /* HH:MM */
};

struct Recado {
	QString id;
	QString texto;
	QString para;
};

class Hub : public QObject {
public:
	QString email, uid, refresh;
	std::vector<Culto> cultos;
	QString cultoId;     /* o culto que está sendo ouvido */
	QString manualCulto; /* escolhido no selo; vazio = automático */
	std::vector<Recado> recados;
	QString problem; /* o que mostrar quando algo deu errado */
	std::vector<std::function<void()>> listeners;

	Hub()
	{
		load();
		connect(&pollTimer, &QTimer::timeout, this, [this]() { poll(); });
		connect(&cultosTimer, &QTimer::timeout, this, [this]() { fetchCultos(); });
		pollTimer.start(POLL_MS);
		cultosTimer.start(CULTOS_MS);
		if (loggedIn()) {
			fetchCultos();
		}
	}

	bool loggedIn() const { return !uid.isEmpty() && !refresh.isEmpty(); }

	const Culto *culto() const
	{
		for (const Culto &c : cultos) {
			if (c.id == cultoId) {
				return &c;
			}
		}
		return nullptr;
	}

	void changed()
	{
		for (auto &listener : listeners) {
			listener();
		}
	}

	/* ---- guardar ---- */

	void load()
	{
		char *path = obs_module_config_path(ACCOUNT_FILE);
		obs_data_t *data = path ? obs_data_create_from_json_file_safe(path, "bak") : nullptr;
		bfree(path);
		if (!data) {
			return;
		}
		email = QString::fromUtf8(obs_data_get_string(data, "email"));
		uid = QString::fromUtf8(obs_data_get_string(data, "uid"));
		refresh = QString::fromUtf8(obs_data_get_string(data, "refreshToken"));
		manualCulto = QString::fromUtf8(obs_data_get_string(data, "culto"));
		obs_data_release(data);
	}

	void save()
	{
		obs_data_t *data = obs_data_create();
		obs_data_set_string(data, "email", email.toUtf8().constData());
		obs_data_set_string(data, "uid", uid.toUtf8().constData());
		obs_data_set_string(data, "refreshToken", refresh.toUtf8().constData());
		obs_data_set_string(data, "culto", manualCulto.toUtf8().constData());
		char *dir = obs_module_config_path("");
		if (dir) {
			os_mkdirs(dir);
			bfree(dir);
		}
		char *path = obs_module_config_path(ACCOUNT_FILE);
		if (path) {
			obs_data_save_json_safe(data, path, "tmp", "bak");
			bfree(path);
		}
		obs_data_release(data);
	}

	/* ---- entrar e sair ---- */

	void signIn(const QString &mail, const QString &password, std::function<void(QString)> done)
	{
		QPointer<Hub> self(this);
		QJsonObject login;
		login["email"] = mail;
		login["password"] = password;
		login["returnSecureToken"] = true;
		QByteArray body = toJson(login);
		worker.push([self, body, done]() {
			std::string url = std::string(SIGN_IN_URL) + API_KEY;
			Http http = request("POST", url, body, "application/json", "");
			QJsonObject answer = QJsonDocument::fromJson(http.body).object();
			QString error;
			if (http.network) {
				error = msg("Account.ErrNetwork");
			} else if (http.status != 200) {
				QString code = answer.value("error").toObject().value("message").toString();
				bool wrong = code.contains("PASSWORD") || code.contains("CREDENTIALS");
				wrong = wrong || code.contains("EMAIL");
				error = wrong ? msg("Account.ErrPassword")
					      : msg("Account.ErrOther") + " (" + code + ")";
			}
			onUi([self, answer, error, done]() {
				if (!self) {
					return;
				}
				if (error.isEmpty()) {
					self->signedIn(answer);
				}
				done(error);
			});
		});
	}

	void signedIn(const QJsonObject &answer)
	{
		email = answer.value("email").toString();
		uid = answer.value("localId").toString();
		refresh = answer.value("refreshToken").toString();
		manualCulto.clear();
		problem.clear();
		save();
		fetchCultos();
		changed();
	}

	void signOut()
	{
		email.clear();
		uid.clear();
		refresh.clear();
		manualCulto.clear();
		cultos.clear();
		selectCulto(QString());
		save();
		changed();
	}

	/* Algo voltou da linha de trabalho com erro de conta ou de rede. */
	void failed(Fail fail)
	{
		if (fail == Fail::Expired) {
			signOut();
			problem = msg("Account.ErrExpired");
			changed();
		} else if (fail == Fail::Network) {
			if (problem.isEmpty()) {
				problem = msg("Account.ErrNetwork");
				changed();
			}
		}
	}

	void ok(const QString &newRefresh)
	{
		bool update = false;
		if (!newRefresh.isEmpty() && newRefresh != refresh) {
			refresh = newRefresh;
			save();
		}
		if (!problem.isEmpty()) {
			problem.clear();
			update = true;
		}
		if (update) {
			changed();
		}
	}

	/* ---- cultos ---- */

	void fetchCultos()
	{
		if (!loggedIn()) {
			return;
		}
		QPointer<Hub> self(this);
		std::string refreshToken = refresh.toStdString();
		QJsonArray fields;
		fields.append(path("nome"));
		fields.append(path("data"));
		fields.append(path("hora"));
		fields.append(path("ultima.n"));
		QJsonObject select;
		select["fields"] = fields;
		QJsonObject query;
		query["select"] = select;
		query["from"] = QJsonArray{collection("cultos")};
		query["where"] = filter("owner", "EQUAL", uid);
		QJsonObject wrapper;
		wrapper["structuredQuery"] = query;
		QByteArray body = toJson(wrapper);
		worker.push([self, refreshToken, body]() {
			std::string access, next;
			Fail fail = accessToken(refreshToken, access, next);
			std::vector<Culto> list;
			if (fail == Fail::None) {
				Http http =
					request("POST", documents() + ":runQuery", body, "application/json", access);
				if (http.network) {
					fail = Fail::Network;
				} else if (http.status == 401 || http.status == 403) {
					fail = Fail::Expired;
				} else if (http.status != 200) {
					fail = Fail::Other;
				}
				for (const QJsonValue &row : QJsonDocument::fromJson(http.body).array()) {
					QJsonObject doc = row.toObject().value("document").toObject();
					QJsonObject fields = doc.value("fields").toObject();
					if (doc.isEmpty() || !fields.contains("ultima")) {
						continue; /* nunca publicado */
					}
					Culto culto;
					culto.id = doc.value("name").toString().section('/', -1);
					culto.nome = field(fields, "nome");
					culto.data = field(fields, "data");
					culto.hora = field(fields, "hora");
					list.push_back(culto);
				}
			}
			QString nextRefresh = QString::fromStdString(next);
			onUi([self, fail, list, nextRefresh]() {
				if (!self || !self->loggedIn()) {
					return;
				}
				if (fail != Fail::None) {
					self->failed(fail);
					return;
				}
				self->ok(nextRefresh);
				self->cultos = list;
				self->chooseCulto();
			});
		});
	}

	/* O culto de hoje com o horário mais perto de agora (ou o que o operador escolheu). */
	QString autoCulto() const
	{
		QString today = QDate::currentDate().toString("yyyy-MM-dd");
		int now = QTime::currentTime().msecsSinceStartOfDay() / 60000;
		QString best;
		int bestDistance = 24 * 60 + 1;
		for (const Culto &c : cultos) {
			if (c.data != today) {
				continue;
			}
			QTime time = QTime::fromString(c.hora, "HH:mm");
			int minutes = time.isValid() ? time.msecsSinceStartOfDay() / 60000 : now;
			int distance = qAbs(minutes - now);
			if (distance < bestDistance) {
				bestDistance = distance;
				best = c.id;
			}
		}
		return best;
	}

	void chooseCulto()
	{
		QString wanted = manualCulto;
		bool exists = false;
		for (const Culto &c : cultos) {
			exists = exists || c.id == wanted;
		}
		if (wanted.isEmpty() || !exists) {
			wanted = autoCulto();
		}
		selectCulto(wanted);
		changed();
	}

	void pickCulto(const QString &id)
	{
		manualCulto = id;
		save();
		chooseCulto();
	}

	void selectCulto(const QString &id)
	{
		if (id == cultoId) {
			return;
		}
		cultoId = id;
		recados.clear();
		seen.clear();
		since = isoNow(-SLACK_MS);
		startedAt = since;
		if (!id.isEmpty()) {
			claim();
		}
	}

	/* Garante que aoVivo/<culto> existe e é desta conta (as regras pedem isso para os recados). */
	void claim()
	{
		std::string refreshToken = refresh.toStdString();
		std::string url = documents() + "/aoVivo/" + cultoId.toStdString() + "?updateMask.fieldPaths=owner";
		QJsonObject fields;
		fields["owner"] = text(uid);
		QJsonObject wrapper;
		wrapper["fields"] = fields;
		QByteArray body = toJson(wrapper);
		worker.push([refreshToken, url, body]() {
			std::string access, next;
			if (accessToken(refreshToken, access, next) == Fail::None) {
				request("PATCH", url, body, "application/json", access);
			}
		});
	}

	/* ---- recados ---- */

	void poll()
	{
		if (!loggedIn() || cultoId.isEmpty() || polling) {
			return;
		}
		polling = true;
		QPointer<Hub> self(this);
		QString id = cultoId;
		std::string refreshToken = refresh.toStdString();
		QDateTime last = QDateTime::fromString(since, Qt::ISODateWithMs);
		QString from = last.addMSecs(-SLACK_MS).toString(Qt::ISODateWithMs);
		QJsonObject order;
		order["field"] = path("em");
		order["direction"] = "ASCENDING";
		QJsonObject query;
		query["from"] = QJsonArray{collection("eventos")};
		query["where"] = filter("em", "GREATER_THAN", from);
		query["orderBy"] = QJsonArray{order};
		query["limit"] = 100;
		QJsonObject wrapper;
		wrapper["structuredQuery"] = query;
		QByteArray body = toJson(wrapper);
		std::string url = documents() + "/aoVivo/" + id.toStdString() + ":runQuery";
		worker.push([self, id, refreshToken, url, body]() {
			std::string access, next;
			Fail fail = accessToken(refreshToken, access, next);
			QJsonArray rows;
			if (fail == Fail::None) {
				Http http = request("POST", url, body, "application/json", access);
				if (http.network) {
					fail = Fail::Network;
				} else if (http.status == 200) {
					rows = QJsonDocument::fromJson(http.body).array();
				}
				/* 403 aqui costuma ser o aoVivo ainda sem dono: o claim resolve na próxima. */
			}
			QString nextRefresh = QString::fromStdString(next);
			onUi([self, id, fail, rows, nextRefresh]() {
				if (!self) {
					return;
				}
				self->polling = false;
				if (fail != Fail::None) {
					self->failed(fail);
					return;
				}
				self->ok(nextRefresh);
				if (id == self->cultoId) {
					self->received(rows);
				}
			});
		});
	}

	void received(const QJsonArray &rows)
	{
		bool update = false;
		for (const QJsonValue &row : rows) {
			QJsonObject doc = row.toObject().value("document").toObject();
			if (doc.isEmpty()) {
				continue;
			}
			QJsonObject fields = doc.value("fields").toObject();
			QString id = doc.value("name").toString().section('/', -1);
			QString em = field(fields, "em");
			if (em > since) {
				since = em;
			}
			if (seen.contains(id)) {
				continue;
			}
			seen.insert(id);
			QString para = field(fields, "para");
			if (field(fields, "tipo") != "recado" || field(fields, "de") != "diretor" || em < startedAt ||
			    (para != "todos" && para != "transmissao")) {
				continue;
			}
			recados.push_back({id, field(fields, "texto"), para});
			if (recados.size() > 3) {
				recados.erase(recados.begin());
			}
			update = true;
		}
		if (update) {
			QApplication::beep();
			changed();
		}
	}

	void dismiss(const QString &id)
	{
		for (auto it = recados.begin(); it != recados.end(); ++it) {
			if (it->id == id) {
				recados.erase(it);
				changed();
				return;
			}
		}
	}

	void reply(const QString &recadoId, const QString &answer)
	{
		QString value = answer.trimmed();
		dismiss(recadoId);
		if (value.isEmpty() || !loggedIn() || cultoId.isEmpty()) {
			return;
		}
		std::string refreshToken = refresh.toStdString();
		std::string url = documents() + "/aoVivo/" + cultoId.toStdString() + "/eventos";
		QJsonObject fields;
		fields["owner"] = text(uid);
		fields["tipo"] = text("recado");
		fields["de"] = text("transmissao");
		fields["para"] = text("diretor");
		fields["texto"] = text(value);
		fields["em"] = text(isoNow());
		QJsonObject wrapper;
		wrapper["fields"] = fields;
		QByteArray body = toJson(wrapper);
		worker.push([refreshToken, url, body]() {
			std::string access, next;
			if (accessToken(refreshToken, access, next) == Fail::None) {
				request("POST", url, body, "application/json", access);
			}
		});
	}

private:
	QTimer pollTimer, cultosTimer;
	QString since, startedAt;
	QSet<QString> seen;
	bool polling = false;
};

Hub *hub = nullptr;

QString cultoLabel(const Culto &c)
{
	QDate date = QDate::fromString(c.data, "yyyy-MM-dd");
	QString when = date.isValid() ? date.toString("dd/MM") : c.data;
	if (!c.hora.isEmpty()) {
		when += " " + c.hora;
	}
	return when + " · " + (c.nome.isEmpty() ? msg("Account.NoName") : c.nome);
}

/* ------------------------------------------------------------------------- */
/* A janela da conta                                                          */

class AccountDialog : public QDialog {
public:
	explicit AccountDialog(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle(msg("Account.Title"));
		setMinimumWidth(380);
		rows = new QVBoxLayout(this);
		build();
	}

private:
	void clear()
	{
		while (QLayoutItem *item = rows->takeAt(0)) {
			if (QWidget *widget = item->widget()) {
				widget->deleteLater();
			}
			if (QLayout *inner = item->layout()) {
				while (QLayoutItem *child = inner->takeAt(0)) {
					if (child->widget()) {
						child->widget()->deleteLater();
					}
					delete child;
				}
			}
			delete item;
		}
	}

	void build()
	{
		clear();
		if (hub->loggedIn()) {
			buildIn();
		} else {
			buildOut();
		}
	}

	void buildOut()
	{
		auto *intro = new QLabel(msg("Account.Intro"), this);
		intro->setWordWrap(true);
		rows->addWidget(intro);
		auto *form = new QFormLayout();
		mail = new QLineEdit(this);
		mail->setPlaceholderText("email@igreja.com");
		password = new QLineEdit(this);
		password->setEchoMode(QLineEdit::Password);
		form->addRow(msg("Account.Email"), mail);
		form->addRow(msg("Account.Password"), password);
		rows->addLayout(form);
		status = new QLabel(hub->problem, this);
		status->setWordWrap(true);
		status->setStyleSheet("color: #ff8a80;");
		rows->addWidget(status);
		auto *enter = new QPushButton(msg("Account.SignIn"), this);
		enter->setDefault(true);
		rows->addWidget(enter);
		connect(enter, &QPushButton::clicked, this, [this, enter]() { signIn(enter); });
		connect(password, &QLineEdit::returnPressed, enter, &QPushButton::click);
	}

	void signIn(QPushButton *enter)
	{
		if (mail->text().trimmed().isEmpty() || password->text().isEmpty()) {
			status->setText(msg("Account.ErrEmpty"));
			return;
		}
		enter->setEnabled(false);
		status->setStyleSheet("");
		status->setText(msg("Account.Entering"));
		QPointer<AccountDialog> self(this);
		hub->signIn(mail->text().trimmed(), password->text(), [self, enter](QString error) {
			if (!self) {
				return;
			}
			if (error.isEmpty()) {
				self->build();
				return;
			}
			enter->setEnabled(true);
			self->status->setStyleSheet("color: #ff8a80;");
			self->status->setText(error);
		});
	}

	void buildIn()
	{
		auto *who = new QLabel(msg("Account.Connected").arg(hub->email), this);
		who->setWordWrap(true);
		rows->addWidget(who);
		rows->addWidget(new QLabel(msg("Account.Culto"), this));
		auto *combo = new QComboBox(this);
		combo->addItem(msg("Account.Auto"), QString());
		for (const Culto &c : hub->cultos) {
			combo->addItem(cultoLabel(c), c.id);
		}
		combo->setCurrentIndex(qMax(0, combo->findData(hub->manualCulto)));
		cultoCombo = combo;
		connect(combo, &QComboBox::currentIndexChanged, this, [this]() { cultoPicked(); });
		rows->addWidget(combo);
		auto *note = new QLabel(msg("Account.CultoNote"), this);
		note->setWordWrap(true);
		note->setStyleSheet("color: palette(mid);");
		rows->addWidget(note);
		auto *row = new QHBoxLayout();
		auto *out = new QPushButton(msg("Account.SignOut"), this);
		auto *close = new QPushButton(msg("Account.Close"), this);
		close->setDefault(true);
		row->addWidget(out);
		row->addStretch(1);
		row->addWidget(close);
		rows->addLayout(row);
		connect(out, &QPushButton::clicked, this, [this]() { signOut(); });
		connect(close, &QPushButton::clicked, this, &QDialog::accept);
	}

	void signOut()
	{
		hub->signOut();
		build();
	}

	void cultoPicked() { hub->pickCulto(cultoCombo->currentData().toString()); }

	QVBoxLayout *rows = nullptr;
	QLineEdit *mail = nullptr;
	QLineEdit *password = nullptr;
	QLabel *status = nullptr;
	QComboBox *cultoCombo = nullptr;
};

QPointer<AccountDialog> dialog;

void openDialog(QWidget *parent)
{
	if (!dialog) {
		QWidget *owner = parent ? parent->window() : static_cast<QWidget *>(obs_frontend_get_main_window());
		dialog = new AccountDialog(owner);
		dialog->setAttribute(Qt::WA_DeleteOnClose);
	}
	dialog->show();
	dialog->raise();
	dialog->activateWindow();
}

void toolsMenuClicked(void *)
{
	openDialog(nullptr);
}

/* ------------------------------------------------------------------------- */
/* O selo do topo                                                             */

class Badge : public QToolButton {
public:
	explicit Badge(QWidget *parent) : QToolButton(parent)
	{
		setAutoRaise(true);
		setCursor(Qt::PointingHandCursor);
		setToolButtonStyle(Qt::ToolButtonTextOnly);
		connect(this, &QToolButton::clicked, this, [this]() { openDialog(this); });
		QPointer<Badge> self(this);
		hub->listeners.push_back([self]() {
			if (self) {
				self->refresh();
			}
		});
		refresh();
	}

	void refresh()
	{
		QString label;
		QString color = "#9aa0a6";
		if (!hub->loggedIn()) {
			label = msg("Account.BadgeOut");
		} else if (!hub->problem.isEmpty()) {
			label = hub->problem;
			color = "#f2b84b";
		} else if (const Culto *c = hub->culto()) {
			label = msg("Account.BadgeCulto").arg(c->nome.isEmpty() ? msg("Account.NoName") : c->nome);
			color = "#4caf50";
		} else {
			label = msg("Account.BadgeNone");
		}
		setText(QString::fromUtf8("● ") + label);
		setStyleSheet(QString("QToolButton { color: %1; font-weight: 600; padding: 2px 8px; }").arg(color));
		setToolTip(hub->loggedIn() ? msg("Account.Connected").arg(hub->email) : msg("Account.Intro"));
	}
};

/* ------------------------------------------------------------------------- */
/* Os recados no canto                                                        */

class Toasts : public QWidget {
public:
	/* Janelinha própria, sem borda, por cima do canto da janela: a Prévia e o Programa são janelas
	 * de vídeo do sistema e cobririam qualquer coisa desenhada dentro da mesma janela. */
	explicit Toasts(QWidget *host) : QWidget(host, Qt::Tool | Qt::FramelessWindowHint)
	{
		setAttribute(Qt::WA_ShowWithoutActivating);
		setAttribute(Qt::WA_TranslucentBackground);
		box = new QVBoxLayout(this);
		box->setContentsMargins(0, 0, 0, 0);
		box->setSpacing(8);
		host->installEventFilter(this);
		QPointer<Toasts> self(this);
		hub->listeners.push_back([self]() {
			if (self) {
				self->rebuild();
			}
		});
		rebuild();
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (watched == parent()) {
			switch (event->type()) {
			case QEvent::Resize:
			case QEvent::Move:
			case QEvent::Show:
			case QEvent::Hide:
			case QEvent::WindowStateChange:
				place();
				break;
			default:
				break;
			}
		}
		return QWidget::eventFilter(watched, event);
	}

private:
	/* Segue o canto de cima, à direita, da janela; some se ela estiver escondida ou minimizada. */
	void place()
	{
		auto *host = parentWidget();
		bool wanted = !hub->recados.empty() && host->isVisible() && !host->isMinimized();
		if (!wanted) {
			hide();
			return;
		}
		adjustSize();
		QPoint corner = host->mapToGlobal(QPoint(host->width(), 0));
		move(corner.x() - width() - 16, corner.y() + 70);
		QWidget::show();
		raise();
	}

	QWidget *card(const Recado &recado)
	{
		auto *frame = new QFrame(this);
		frame->setObjectName("plRecado");
		frame->setStyleSheet("#plRecado { background: #1d1f24; border: 1px solid #3a3d44; border-radius: 8px; }"
				     "#plRecado QLabel { background: transparent; color: #f1f3f4; }"
				     "#plRecado QPushButton { padding: 3px 10px; }");
		frame->setMinimumWidth(340);
		frame->setMaximumWidth(460);
		auto *v = new QVBoxLayout(frame);
		v->setContentsMargins(12, 10, 12, 10);
		v->setSpacing(6);
		bool live = recado.para == "transmissao";
		auto *head = new QLabel(msg(live ? "Account.FromDirectorLive" : "Account.FromDirectorAll"), frame);
		head->setStyleSheet("color: #e5574d; font-size: 11px; font-weight: 700; letter-spacing: 1px;");
		v->addWidget(head);
		auto *body = new QLabel(recado.texto, frame);
		body->setWordWrap(true);
		QFont font = body->font();
		font.setPointSizeF(font.pointSizeF() * 1.25);
		body->setFont(font);
		v->addWidget(body);
		auto *row = new QHBoxLayout();
		row->setSpacing(6);
		const char *ready[] = {"Account.ReplyOk", "Account.ReplyDone", "Account.ReplyNotNow"};
		QString id = recado.id;
		for (const char *key : ready) {
			auto *button = new QPushButton(msg(key), frame);
			QString answer = msg(key);
			connect(button, &QPushButton::clicked, this, [id, answer]() { hub->reply(id, answer); });
			row->addWidget(button);
		}
		auto *write = new QPushButton(msg("Account.ReplyWrite"), frame);
		connect(write, &QPushButton::clicked, this, [this, id]() { writeReply(id); });
		row->addWidget(write);
		row->addStretch(1);
		auto *close = new QPushButton(QString::fromUtf8("×"), frame);
		close->setFlat(true);
		close->setToolTip(msg("Account.Close"));
		connect(close, &QPushButton::clicked, this, [id]() { hub->dismiss(id); });
		row->addWidget(close);
		v->addLayout(row);
		return frame;
	}

	void writeReply(const QString &id)
	{
		bool ok = false;
		QString answer = QInputDialog::getText(window(), msg("Account.ReplyTitle"), msg("Account.ReplyLabel"),
						       QLineEdit::Normal, QString(), &ok);
		if (ok) {
			hub->reply(id, answer);
		}
	}

	void rebuild()
	{
		while (QLayoutItem *item = box->takeAt(0)) {
			if (item->widget()) {
				item->widget()->hide();
				item->widget()->deleteLater();
			}
			delete item;
		}
		for (const Recado &recado : hub->recados) {
			QWidget *item = card(recado);
			box->addWidget(item);
			/* Com a janelinha já aberta, o Qt só mostraria o cartão novo depois; mostra já, para
			 * medir o tamanho certo (senão ela vai parar na borda da tela). */
			item->show();
		}
		box->activate();
		place();
		QPointer<Toasts> self(this);
		QTimer::singleShot(0, this, [self]() {
			if (self) {
				self->place();
			}
		});
	}

	QVBoxLayout *box = nullptr;
};

void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		if (auto *main = static_cast<QWidget *>(obs_frontend_get_main_window())) {
			pl_account_attach_toasts(main);
		}
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		worker.stop();
	}
}

} // namespace

QWidget *pl_account_badge_create(QWidget *parent)
{
	return hub ? new Badge(parent) : nullptr;
}

void pl_account_attach_toasts(QWidget *host)
{
	if (hub && host) {
		new Toasts(host);
	}
}

void pl_account_load(void)
{
	curl_global_init(CURL_GLOBAL_ALL);
	worker.start();
	hub = new Hub();
	obs_frontend_add_event_callback(frontendEvent, nullptr);
	obs_frontend_add_tools_menu_item(obs_module_text("Account.ToolsMenu"), toolsMenuClicked, nullptr);
}

void pl_account_unload(void)
{
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
	worker.stop();
	delete hub;
	hub = nullptr;
	curl_global_cleanup();
}
