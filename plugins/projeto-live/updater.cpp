/*
 * somun live: atualização automática (decidido com ela em 25/11). Só Windows por enquanto.
 *
 * O somun live e o somun tela vêm no MESMO instalador, então a atualização é uma só: o instalador
 * inteiro, publicado pelo workflow "Projeto Live: instalador" em Releases > "33.0.1-projetolive"
 * (baixa sem login), junto com "somun-versao.json" ({ numero, tamanho }). O instalador grava o
 * número dele em <pasta do somun>\somun-versao.txt.
 *
 * - Confere 20 s depois de abrir e a cada 2 horas. Versão nova = número de lá maior que o daqui.
 * - Baixa escondido para a pasta temporária, mas NUNCA com a transmissão ou a gravação ligadas (se
 *   ligar no meio, para e tenta de novo depois). São ~400 MB.
 * - Pronto: a mesa mostra "versão nova" (só fora do ar), com "Atualizar agora" (fecha, instala e
 *   abre de novo) ou "Depois" (instala sozinho ao fechar o somun live, se o somun tela também
 *   estiver fechado — nunca fecha o telão no meio do culto).
 * - O somun tela faz o mesmo do lado dele (src/electron/utils/updater.ts); os dois usam o mesmo
 *   arquivo baixado.
 * - Sem somun-versao.txt (instalação antiga ou montagem de teste): não faz nada.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "updater.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <curl/curl.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QMetaObject>
#include <QStandardPaths>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#endif

namespace {

const char *BASE = "https://github.com/denysondetrano30/projeto-live/releases/download/33.0.1-projetolive";

std::thread worker;
std::mutex lock;
std::condition_variable wake;
std::atomic<bool> stopping{false};
std::atomic<bool> busyNow{false};
std::atomic<bool> installing{false};
QString readyFile; /* protegido por lock */

int localNumber()
{
	/* obs64.exe fica em <pasta>\OBS\bin\64bit */
	QFile file(QDir(QCoreApplication::applicationDirPath()).filePath("../../../somun-versao.txt"));
	if (!file.open(QIODevice::ReadOnly)) {
		return 0;
	}
	return QString::fromUtf8(file.readAll()).trimmed().toInt();
}

size_t toString(char *data, size_t size, size_t count, void *user)
{
	static_cast<std::string *>(user)->append(data, size * count);
	return size * count;
}

size_t toFile(char *data, size_t size, size_t count, void *user)
{
	return fwrite(data, size, count, static_cast<FILE *>(user)) * size;
}

int keepGoing(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	/* para de baixar se fechar o programa ou se entrar no ar */
	return (stopping || busyNow) ? 1 : 0;
}

void common(CURL *curl, const std::string &url)
{
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, keepGoing);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "somun-live");
#if defined(_WIN32) && defined(CURLSSLOPT_NATIVE_CA)
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
}

bool fetchInfo(int &numero, qint64 &tamanho)
{
	CURL *curl = curl_easy_init();
	if (!curl) {
		return false;
	}
	std::string answer;
	common(curl, std::string(BASE) + "/somun-versao.json?t=" + std::to_string(time(nullptr)));
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, toString);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &answer);
	long status = 0;
	bool ok = curl_easy_perform(curl) == CURLE_OK;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);
	if (!ok || status != 200) {
		return false;
	}
	QJsonObject info = QJsonDocument::fromJson(QByteArray::fromStdString(answer)).object();
	numero = info.value("numero").toInt();
	tamanho = (qint64)info.value("tamanho").toDouble();
	return numero > 0 && tamanho > 0;
}

bool download(const QString &part)
{
	FILE *out = nullptr;
#ifdef _WIN32
	out = _wfopen(reinterpret_cast<const wchar_t *>(part.utf16()), L"wb");
#else
	out = fopen(part.toUtf8().constData(), "wb");
#endif
	if (!out) {
		return false;
	}
	CURL *curl = curl_easy_init();
	bool ok = false;
	if (curl) {
		common(curl, std::string(BASE) + "/ProjetoLive-Instalador.exe?t=" + std::to_string(time(nullptr)));
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, toFile);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
		/* sem limite de tempo total (arquivo grande), mas desiste se ficar parado */
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
		long status = 0;
		ok = curl_easy_perform(curl) == CURLE_OK;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
		ok = ok && status == 200;
		curl_easy_cleanup(curl);
	}
	fclose(out);
	return ok;
}

/* Uma conferida. Devolve false se precisa tentar de novo logo (no ar ou rede caiu no meio). */
bool check()
{
	int mine = localNumber();
	if (mine <= 0) {
		return true;
	}
	/* Apaga os instaladores já usados (até a versão instalada), ~400 MB cada. */
	QDir temp(QStandardPaths::writableLocation(QStandardPaths::TempLocation));
	for (const QString &old : temp.entryList({"somun-atualizacao-*"}, QDir::Files)) {
		int n = old.mid(18).section('.', 0, 0).toInt();
		if (n > 0 && n <= mine) {
			temp.remove(old);
		}
	}
	int numero = 0;
	qint64 tamanho = 0;
	if (!fetchInfo(numero, tamanho) || numero <= mine) {
		return true;
	}
	QString file = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
			       .filePath(QString("somun-atualizacao-%1.exe").arg(numero));
	auto complete = [&]() {
		return QFileInfo(file).exists() && QFileInfo(file).size() == tamanho;
	};
	if (!complete()) {
		if (busyNow) {
			return false;
		}
		QString part = file + ".live.part";
		bool ok = download(part);
		if (!ok || QFileInfo(part).size() != tamanho) {
			QFile::remove(part);
			return false;
		}
		if (!complete()) {
			QFile::remove(file);
			QFile::rename(part, file);
		} else {
			QFile::remove(part);
		}
	}
	if (complete()) {
		std::lock_guard<std::mutex> guard(lock);
		readyFile = file;
		blog(LOG_INFO, "[somun] versão nova %d pronta para instalar", numero);
	}
	return true;
}

void loop()
{
	auto wait = [](std::chrono::seconds seconds) {
		std::unique_lock<std::mutex> guard(lock);
		wake.wait_for(guard, seconds, []() { return stopping.load(); });
	};
	wait(std::chrono::seconds(20));
	while (!stopping) {
		bool done = check();
		wait(done ? std::chrono::seconds(2 * 60 * 60) : std::chrono::seconds(10 * 60));
	}
}

#ifdef _WIN32
bool running(const wchar_t *image)
{
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		return false;
	}
	PROCESSENTRY32W entry = {};
	entry.dwSize = sizeof(entry);
	bool found = false;
	if (Process32FirstW(snapshot, &entry)) {
		do {
			if (_wcsicmp(entry.szExeFile, image) == 0) {
				found = true;
				break;
			}
		} while (Process32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);
	return found;
}

/* Pelo cmd (escondido), porque o instalador pede administrador e o Windows mostra a pergunta. */
void launch(const QString &command)
{
	std::wstring line = L"cmd.exe /d /c " + command.toStdWString();
	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};
	if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
			   nullptr, &startup, &process)) {
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
	}
}
#endif

QString takeReady()
{
	std::lock_guard<std::mutex> guard(lock);
	QString file = readyFile;
	if (!QFileInfo(file).exists()) {
		file.clear();
	}
	return file;
}

void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event != OBS_FRONTEND_EVENT_EXIT) {
		return;
	}
#ifdef _WIN32
	/* "Depois": instala ao fechar, se o somun tela também estiver fechado. */
	QString file = takeReady();
	if (!installing && !file.isEmpty() && !running(L"FreeShow.exe")) {
		launch(QString("start \"\" \"%1\" /S").arg(QDir::toNativeSeparators(file)));
	}
#endif
}

} // namespace

bool pl_updater_ready()
{
	return !takeReady().isEmpty();
}

void pl_updater_set_busy(bool busy)
{
	busyNow = busy;
}

void pl_updater_install_now()
{
#ifdef _WIN32
	QString file = takeReady();
	if (file.isEmpty()) {
		return;
	}
	installing = true;
	/* Instala (o instalador fecha o somun tela, se estiver aberto) e abre o somun live de novo. */
	QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
	launch(QString("\"start \"\" /wait \"%1\" /S & start \"\" /d \"%2\" \"%3\"\"")
		       .arg(QDir::toNativeSeparators(file),
			    QDir::toNativeSeparators(QCoreApplication::applicationDirPath()), exe));
	if (auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window())) {
		QMetaObject::invokeMethod(main, "close", Qt::QueuedConnection);
	}
#endif
}

void pl_updater_load(void)
{
#ifdef _WIN32
	stopping = false;
	worker = std::thread(loop);
	obs_frontend_add_event_callback(frontendEvent, nullptr);
#endif
}

void pl_updater_unload(void)
{
#ifdef _WIN32
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
	stopping = true;
	wake.notify_all();
	if (worker.joinable()) {
		worker.join();
	}
#endif
}
