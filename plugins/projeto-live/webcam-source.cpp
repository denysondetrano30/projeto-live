/*
 * somun live: entrada "Celular sem app (navegador)" — a câmera do celular pela página somun cam.
 *
 * Como funciona (veja pedido-somun-cam-web.md na pasta central):
 * - Cada entrada tem um código longo e aleatório. Enquanto nenhum celular manda imagem, a entrada
 *   mostra um QR code com o link https://…/cam/#código (a página fica no site do Diretor).
 * - O acerto inicial passa pelo Firestore, no documento camParear/<código> (a conta da igreja é a
 *   dona; o celular, sem login, só pode escrever o pedido e a resposta). O celular escreve um
 *   "pedido"; o computador responde com a "oferta" (WebRTC, só vídeo H.264 + um canal de texto);
 *   o celular devolve a "resposta". Ninguém troca candidatos aos poucos: cada lado espera juntar
 *   todos os endereços antes de escrever.
 * - Depois disso a imagem vem direto pelo Wi-Fi (libdatachannel), sem passar pela internet. Os
 *   pacotes RTP viram quadros H.264 aqui mesmo e o FFmpeg decodifica.
 * - No ar (tally): activate/deactivate mandam "tally:1"/"tally:0" pelo canal de texto; o nome da
 *   entrada vai como "nome:...". O celular pinta a borda de vermelho.
 *
 * Leituras do Firestore (plano grátis): a cada 3 s sem celular ligado, a cada 20 s com celular.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qrcodegen.h"
#include "webcam-source.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <media-io/video-io.h>

#include <curl/curl.h>
#include <rtc/rtc.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QString>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#define WEBCAM_SOURCE_ID "somun_webcam_source"
#define S_CODE "pair_code"
#define S_LINK "pair_link"
#define S_NEW_CODE "new_code"

namespace {

const char *const PAGE_URL = "https://denysondetrano30.github.io/projeto-live-diretor/cam/#";
const char *const API_KEY = "AIzaSyBubqovhUfM_HFrlGfFhBygVg2FjMi5DD0";
const char *const DOCUMENTS =
	"https://firestore.googleapis.com/v1/projects/projeto-live-e7ccb/databases/(default)/documents";
const char *const COLLECTION = "camParear";
const int CODE_LEN = 20;
const int VIDEO_PT = 96;
const int SCREEN_W = 1280;
const int SCREEN_H = 720;

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

std::string newCode()
{
	static const char letters[] = "abcdefghijkmnpqrstuvwxyz23456789";
	std::random_device device;
	std::uniform_int_distribution<int> pick(0, (int)sizeof(letters) - 2);
	std::string code;
	for (int i = 0; i < CODE_LEN; i++) {
		code += letters[pick(device)];
	}
	return code;
}

/* ------------------------------------------------------------------------- */
/* Firestore pela internet (só na linha de trabalho de cada entrada)          */

struct Http {
	long status = 0;
	std::string body;
	bool network = false;
};

size_t writeBody(char *data, size_t size, size_t count, void *user)
{
	static_cast<std::string *>(user)->append(data, size * count);
	return size * count;
}

Http request(const char *method, const std::string &url, const std::string &body, const char *type,
	     const std::string &bearer)
{
	Http result;
	CURL *curl = curl_easy_init();
	if (!curl) {
		result.network = true;
		return result;
	}
	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, (std::string("Content-Type: ") + type).c_str());
	if (!bearer.empty()) {
		headers = curl_slist_append(headers, ("Authorization: Bearer " + bearer).c_str());
	}
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	if (strcmp(method, "GET") != 0) {
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
	}
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
#if defined(_WIN32) && defined(CURLSSLOPT_NATIVE_CA)
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
	if (curl_easy_perform(curl) == CURLE_OK) {
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
	} else {
		result.network = true;
	}
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return result;
}

enum class Login { Ok, NoAccount, Network };

/* O login da igreja (o mesmo de pl-account.cpp, em conta.json): o id da conta e um token de acesso,
 * guardado por 50 minutos e dividido por todas as entradas. */
std::mutex tokenMutex;
std::string tokenRefresh, tokenAccess;
int64_t tokenUntil = 0;

Login login(std::string &uid, std::string &access)
{
	char *path = obs_module_config_path("conta.json");
	obs_data_t *data = path ? obs_data_create_from_json_file_safe(path, "bak") : nullptr;
	bfree(path);
	std::string refresh;
	if (data) {
		uid = obs_data_get_string(data, "uid");
		refresh = obs_data_get_string(data, "refreshToken");
		obs_data_release(data);
	}
	if (uid.empty() || refresh.empty()) {
		return Login::NoAccount;
	}
	std::lock_guard<std::mutex> lock(tokenMutex);
	int64_t now = QDateTime::currentMSecsSinceEpoch();
	if (tokenRefresh == refresh && tokenUntil > now) {
		access = tokenAccess;
		return Login::Ok;
	}
	char *escaped = curl_easy_escape(nullptr, refresh.c_str(), (int)refresh.size());
	std::string body = std::string("grant_type=refresh_token&refresh_token=") + escaped;
	curl_free(escaped);
	Http http = request("POST", std::string("https://securetoken.googleapis.com/v1/token?key=") + API_KEY, body,
			    "application/x-www-form-urlencoded", "");
	if (http.network) {
		return Login::Network;
	}
	if (http.status != 200) {
		return Login::NoAccount;
	}
	QJsonObject answer = QJsonDocument::fromJson(QByteArray::fromStdString(http.body)).object();
	access = answer.value("id_token").toString().toStdString();
	tokenRefresh = refresh;
	tokenAccess = access;
	tokenUntil = now + 50 * 60 * 1000;
	return Login::Ok;
}

QJsonObject stringValue(const std::string &value)
{
	QJsonObject object;
	object["stringValue"] = QString::fromStdString(value);
	return object;
}

std::string field(const QJsonObject &fields, const char *key)
{
	return fields.value(key).toObject().value("stringValue").toString().toStdString();
}

std::string toJson(const QJsonObject &object)
{
	return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

/* ------------------------------------------------------------------------- */
/* Pacotes RTP -> quadros H.264 -> imagem                                     */

class Decoder {
public:
	~Decoder()
	{
		if (frame) {
			av_frame_free(&frame);
		}
		if (context) {
			avcodec_free_context(&context);
		}
	}

	bool open()
	{
		const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
		if (!codec) {
			return false;
		}
		context = avcodec_alloc_context3(codec);
		frame = av_frame_alloc();
		if (!context || !frame) {
			return false;
		}
		/* Sem esperar quadros a mais: o atraso fica o menor possível. */
		context->flags |= AV_CODEC_FLAG_LOW_DELAY;
		context->thread_type = FF_THREAD_SLICE;
		context->thread_count = 0;
		return avcodec_open2(context, codec, nullptr) == 0;
	}

	/* Devolve false se o quadro não decodificou (aí é preciso pedir um quadro inteiro). */
	bool decode(const std::vector<uint8_t> &annexB, obs_source_t *source)
	{
		AVPacket *packet = av_packet_alloc();
		if (!packet || av_new_packet(packet, (int)annexB.size()) < 0) {
			av_packet_free(&packet);
			return false;
		}
		memcpy(packet->data, annexB.data(), annexB.size());
		int sent = avcodec_send_packet(context, packet);
		av_packet_free(&packet);
		if (sent < 0) {
			return false;
		}
		while (avcodec_receive_frame(context, frame) == 0) {
			output(source);
			av_frame_unref(frame);
		}
		return true;
	}

private:
	void output(obs_source_t *source)
	{
		if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P) {
			return;
		}
		struct obs_source_frame out = {};
		for (int i = 0; i < 3; i++) {
			out.data[i] = frame->data[i];
			out.linesize[i] = (uint32_t)frame->linesize[i];
		}
		out.width = (uint32_t)frame->width;
		out.height = (uint32_t)frame->height;
		out.format = VIDEO_FORMAT_I420;
		out.timestamp = os_gettime_ns();
		bool full = frame->format == AV_PIX_FMT_YUVJ420P || frame->color_range == AVCOL_RANGE_JPEG;
		out.full_range = full;
		enum video_colorspace space = frame->colorspace == AVCOL_SPC_BT709 ? VIDEO_CS_709 : VIDEO_CS_601;
		video_format_get_parameters_for_format(space, full ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL,
						       VIDEO_FORMAT_I420, out.color_matrix, out.color_range_min,
						       out.color_range_max);
		obs_source_output_video(source, &out);
	}

	AVCodecContext *context = nullptr;
	AVFrame *frame = nullptr;
};

/* Junta os pacotes RTP (RFC 6184: NAL sozinho, STAP-A, FU-A) em quadros "Annex B". Se faltar um
 * pacote, joga o quadro fora e espera um quadro inteiro (IDR). */
class Depacketizer {
public:
	/* Chama done(quadro, éChave) para cada quadro inteiro; done(vazio, false) quando faltou pacote. */
	void push(const uint8_t *data, size_t size, const std::function<void(std::vector<uint8_t> &, bool)> &done)
	{
		if (size < 12 || (data[0] >> 6) != 2) {
			return;
		}
		int pt = data[1] & 0x7f;
		if (pt != VIDEO_PT) {
			return;
		}
		bool marker = (data[1] & 0x80) != 0;
		uint16_t seq = (uint16_t)((data[2] << 8) | data[3]);
		uint32_t ts = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) | ((uint32_t)data[6] << 8) |
			      data[7];
		size_t offset = 12 + 4 * (size_t)(data[0] & 0x0f);
		if (data[0] & 0x10) {
			if (size < offset + 4) {
				return;
			}
			size_t words = (size_t)((data[offset + 2] << 8) | data[offset + 3]);
			offset += 4 + 4 * words;
		}
		size_t end = size;
		if (data[0] & 0x20) {
			size_t padding = data[size - 1];
			if (padding > end) {
				return;
			}
			end -= padding;
		}
		if (offset >= end) {
			return;
		}

		if (haveTs && ts != frameTs && !buffer.empty()) {
			finish(done);
		}
		if (started && seq != (uint16_t)(lastSeq + 1)) {
			broken = true;
		}
		started = true;
		lastSeq = seq;
		haveTs = true;
		frameTs = ts;

		const uint8_t *payload = data + offset;
		size_t length = end - offset;
		int type = payload[0] & 0x1f;
		if (type >= 1 && type <= 23) {
			appendNal(payload, length);
		} else if (type == 24) { /* STAP-A */
			size_t pos = 1;
			while (pos + 2 <= length) {
				size_t nal = (size_t)((payload[pos] << 8) | payload[pos + 1]);
				pos += 2;
				if (pos + nal > length) {
					break;
				}
				appendNal(payload + pos, nal);
				pos += nal;
			}
		} else if (type == 28 && length > 2) { /* FU-A */
			bool start = (payload[1] & 0x80) != 0;
			if (start) {
				uint8_t header = (uint8_t)((payload[0] & 0xe0) | (payload[1] & 0x1f));
				noteType(header & 0x1f);
				static const uint8_t code[4] = {0, 0, 0, 1};
				buffer.insert(buffer.end(), code, code + 4);
				buffer.push_back(header);
				inFragment = true;
			}
			if (inFragment) {
				buffer.insert(buffer.end(), payload + 2, payload + length);
			} else {
				broken = true;
			}
			if (payload[1] & 0x40) {
				inFragment = false;
			}
		}
		if (marker) {
			finish(done);
		}
	}

private:
	void noteType(int type)
	{
		if (type == 5) {
			hasIdr = true;
		}
	}

	void appendNal(const uint8_t *nal, size_t size)
	{
		if (!size) {
			return;
		}
		noteType(nal[0] & 0x1f);
		static const uint8_t code[4] = {0, 0, 0, 1};
		buffer.insert(buffer.end(), code, code + 4);
		buffer.insert(buffer.end(), nal, nal + size);
	}

	void finish(const std::function<void(std::vector<uint8_t> &, bool)> &done)
	{
		if (!buffer.empty()) {
			if (!broken && !inFragment) {
				done(buffer, hasIdr);
			} else {
				done(empty, false);
			}
		}
		buffer.clear();
		broken = false;
		inFragment = false;
		hasIdr = false;
		haveTs = false;
	}

	std::vector<uint8_t> buffer;
	std::vector<uint8_t> empty;
	bool started = false;
	bool haveTs = false;
	bool broken = false;
	bool inFragment = false;
	bool hasIdr = false;
	uint16_t lastSeq = 0;
	uint32_t frameTs = 0;
};

/* ------------------------------------------------------------------------- */
/* Uma conexão com um celular                                                 */

struct WebCam;

/* Guarda o "enviar" da trilha para mandar NACK (pedido de reenvio de pacote) ao celular. A trilha só
 * recebe vídeo, então o NACK precisa sair como mensagem de controle, como o pedido de quadro inteiro. */
class RtcpSender : public rtc::MediaHandler {
public:
	void incoming(rtc::message_vector &, const rtc::message_callback &send) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		callback = send;
	}

	void sendRtcp(const std::vector<uint8_t> &packet)
	{
		rtc::message_callback send;
		{
			std::lock_guard<std::mutex> lock(mutex);
			send = callback;
		}
		if (send) {
			const std::byte *begin = reinterpret_cast<const std::byte *>(packet.data());
			send(rtc::make_message(begin, begin + packet.size(), rtc::Message::Control));
		}
	}

private:
	std::mutex mutex;
	rtc::message_callback callback;
};

struct Session {
	std::mutex mutex;
	bool dead = false;
	std::shared_ptr<rtc::PeerConnection> pc;
	std::shared_ptr<rtc::Track> track;
	std::shared_ptr<rtc::DataChannel> channel;
	std::condition_variable gathered;
	bool gatheringDone = false;
	Decoder decoder;
	Depacketizer depacketizer;
	bool needKeyframe = true;
	uint64_t lastKeyframeAsk = 0;
	bool gotVideo = false;
	/* Avisa o celular (REMB) de quanto o Wi-Fi aguenta; sem isso ele fica na velocidade de partida
	 * e derruba os quadros por segundo. */
	uint64_t lastBitrateAsk = 0;
	/* Conta quadros e bytes para anotar no log a cada 10 s (para ver os quadros por segundo). */
	uint64_t statsStart = 0;
	uint32_t statsFrames = 0;
	uint64_t statsBytes = 0;
	uint32_t statsLost = 0;      /* pacotes que não chegaram nem com reenvio */
	uint32_t statsRecovered = 0; /* pacotes que chegaram depois do NACK */
	uint32_t statsDropped = 0;   /* quadros jogados fora esperando um quadro inteiro */

	/* Fila de reordenação: segura os pacotes por número; se faltar um, pede reenvio (NACK) e espera
	 * um pouco (até 150 ms) antes de desistir dele. */
	std::shared_ptr<RtcpSender> rtcp;
	std::map<int64_t, rtc::binary> pending;
	std::map<int64_t, std::pair<uint64_t, int>> nacked; /* número -> (último pedido, vezes) */
	int64_t expected = -1;
	int64_t highest = -1;
	uint64_t gapSince = 0;
	uint32_t mediaSsrc = 0;
	bool counted = false; /* entra na conta de celulares mandando imagem (para dividir o Wi-Fi) */
};

/* Quantos celulares estão mandando imagem agora (em todas as entradas): o Wi-Fi é dividido entre eles. */
std::atomic<int> sendingPhones{0};

/* Quanto cada celular pode mandar: um sozinho usa até 20 Mb/s; com mais, cada um usa menos. */
unsigned int phoneBitrate()
{
	int phones = sendingPhones.load();
	if (phones <= 1) {
		return 20000000;
	}
	if (phones == 2) {
		return 14000000;
	}
	if (phones == 3) {
		return 10000000;
	}
	return 8000000;
}

enum class Status { Waiting, NoAccount, Network, Connecting, Connected, Left, Failed };

struct WebCam {
	obs_source_t *source = nullptr;
	std::mutex mutex; /* code, session */
	std::string code;
	std::shared_ptr<Session> session;
	std::thread worker;
	std::atomic<bool> stopping{false};
	std::condition_variable wake;
	std::atomic<bool> onAir{false};
	std::atomic<int> rtcState{-1};
	Status shown = Status::Waiting;
	std::string shownCode;
	bool screenValid = false;
	/* Quando chegou o último quadro do celular (0 = nenhum desde que a entrada abriu ou trocou de
	 * código). Depois do primeiro quadro, se o celular cair, a última imagem fica parada (sem texto
	 * na transmissão) e a mesa avisa "celular sumiu". */
	std::atomic<uint64_t> lastFrame{0};
};

void sendToPhone(WebCam *cam, const std::string &message)
{
	std::shared_ptr<Session> session;
	{
		std::lock_guard<std::mutex> lock(cam->mutex);
		session = cam->session;
	}
	if (!session) {
		return;
	}
	std::shared_ptr<rtc::DataChannel> channel;
	{
		std::lock_guard<std::mutex> lock(session->mutex);
		channel = session->channel;
	}
	try {
		if (channel && channel->isOpen()) {
			channel->send(message);
		}
	} catch (...) {
	}
}

void sendHello(WebCam *cam)
{
	sendToPhone(cam, std::string("nome:") + obs_source_get_name(cam->source));
	sendToPhone(cam, cam->onAir ? "tally:1" : "tally:0");
}

/* A tela de espera: o QR code com o link e o que está acontecendo. Vai para a entrada como um quadro
 * comum (a mesma saída da imagem do celular). */
void showScreen(WebCam *cam, Status status, const std::string &code)
{
	if (cam->screenValid && cam->shown == status && cam->shownCode == code) {
		return;
	}
	cam->shown = status;
	cam->shownCode = code;
	cam->screenValid = true;
	if (status == Status::Connected) {
		return; /* a imagem do celular toma o lugar */
	}

	QImage image(SCREEN_W, SCREEN_H, QImage::Format_ARGB32);
	image.fill(QColor("#141414"));
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setRenderHint(QPainter::TextAntialiasing);

	std::string url = PAGE_URL + code;
	uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
	uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
	int left = 90;
	int qrSide = 420;
	int top = (SCREEN_H - qrSide) / 2;
	if (qrcodegen_encodeText(url.c_str(), temp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, 10,
				 qrcodegen_Mask_AUTO, true)) {
		int modules = qrcodegen_getSize(qr);
		int border = 3;
		double cell = (double)qrSide / (modules + 2 * border);
		painter.setPen(Qt::NoPen);
		painter.setBrush(Qt::white);
		painter.drawRoundedRect(QRectF(left, top, qrSide, qrSide), 18, 18);
		painter.setBrush(QColor("#141414"));
		painter.setRenderHint(QPainter::Antialiasing, false);
		for (int y = 0; y < modules; y++) {
			for (int x = 0; x < modules; x++) {
				if (qrcodegen_getModule(qr, x, y)) {
					painter.drawRect(QRectF(left + (x + border) * cell, top + (y + border) * cell,
								cell + 0.5, cell + 0.5));
				}
			}
		}
		painter.setRenderHint(QPainter::Antialiasing, true);
	}

	int textLeft = left + qrSide + 70;
	int width = SCREEN_W - textLeft - 60;
	QFont big("Outfit");
	big.setPixelSize(38);
	big.setWeight(QFont::DemiBold);
	QFont note("Outfit");
	note.setPixelSize(24);
	QFont word("Outfit");
	word.setPixelSize(44);
	word.setWeight(QFont::ExtraBold);

	/* "cam." como no ícone do somun cam. */
	painter.setFont(word);
	painter.setPen(QColor("#F2F2F2"));
	QRect wordRect(textLeft, top, width, 60);
	painter.drawText(wordRect, Qt::AlignLeft | Qt::AlignTop, "cam");
	int wordWidth = painter.fontMetrics().horizontalAdvance("cam");
	painter.setPen(Qt::NoPen);
	painter.setBrush(QColor("#E5483B"));
	painter.drawEllipse(QRectF(textLeft + wordWidth + 4, top + 38, 11, 11));

	painter.setPen(QColor("#F2F2F2"));
	painter.setFont(big);
	painter.drawText(QRect(textLeft, top + 90, width, 110), Qt::AlignLeft | Qt::TextWordWrap, text("WebCam.Point"));

	const char *key = "WebCam.Waiting";
	bool alert = false;
	switch (status) {
	case Status::NoAccount:
		key = "WebCam.NoAccount";
		alert = true;
		break;
	case Status::Network:
		key = "WebCam.Network";
		alert = true;
		break;
	case Status::Connecting:
		key = "WebCam.Connecting";
		break;
	case Status::Left:
		key = "WebCam.Left";
		break;
	case Status::Failed:
		key = "WebCam.Failed";
		alert = true;
		break;
	default:
		break;
	}
	painter.setFont(note);
	painter.setPen(QColor(alert ? "#E5483B" : "#A8A8A8"));
	painter.drawText(QRect(textLeft, top + 215, width, 120), Qt::AlignLeft | Qt::TextWordWrap, text(key));
	painter.setPen(QColor("#8C8C8C"));
	painter.drawText(QRect(textLeft, top + 340, width, 80), Qt::AlignLeft | Qt::TextWordWrap,
			 text("WebCam.SameWifi"));
	painter.end();

	struct obs_source_frame frame = {};
	frame.data[0] = image.bits();
	frame.linesize[0] = (uint32_t)image.bytesPerLine();
	frame.width = SCREEN_W;
	frame.height = SCREEN_H;
	frame.format = VIDEO_FORMAT_BGRA;
	frame.full_range = true;
	frame.timestamp = os_gettime_ns();
	obs_source_output_video(cam->source, &frame);
}

void closeSession(const std::shared_ptr<Session> &session)
{
	if (!session) {
		return;
	}
	std::shared_ptr<rtc::PeerConnection> pc;
	{
		std::lock_guard<std::mutex> lock(session->mutex);
		session->dead = true;
		if (session->counted) {
			session->counted = false;
			sendingPhones--;
		}
		pc = session->pc;
		session->channel.reset();
		session->track.reset();
	}
	if (pc) {
		try {
			pc->close();
		} catch (...) {
		}
	}
}

/* Um pacote na ordem certa: vira quadro e imagem. */
void feedPacket(WebCam *cam, Session *session, const rtc::binary &message)
{
	auto askKeyframe = [session]() {
		uint64_t now = os_gettime_ns();
		if (session->track && now - session->lastKeyframeAsk > 300000000ULL) {
			session->lastKeyframeAsk = now;
			try {
				session->track->requestKeyframe();
			} catch (...) {
			}
		}
	};
	session->depacketizer.push(reinterpret_cast<const uint8_t *>(message.data()), message.size(),
				   [&](std::vector<uint8_t> &frame, bool keyframe) {
					   if (frame.empty()) {
						   session->needKeyframe = true;
						   session->statsDropped++;
						   askKeyframe();
						   return;
					   }
					   if (session->needKeyframe && !keyframe) {
						   session->statsDropped++;
						   askKeyframe();
						   return;
					   }
					   session->needKeyframe = false;
					   if (!session->decoder.decode(frame, cam->source)) {
						   session->needKeyframe = true;
						   askKeyframe();
						   return;
					   }
					   session->gotVideo = true;
					   session->statsFrames++;
					   cam->lastFrame = os_gettime_ns();
					   if (!session->counted) {
						   session->counted = true;
						   sendingPhones++;
						   session->lastBitrateAsk = 0; /* avisa a banda nova já */
					   }
				   });
}

/* Pede ao celular para reenviar estes pacotes (RTCP NACK genérico, RFC 4585). */
void sendNack(Session *session, std::vector<int64_t> missing)
{
	if (missing.empty() || !session->rtcp) {
		return;
	}
	std::sort(missing.begin(), missing.end());
	std::vector<uint8_t> packet = {0x81, 205, 0, 0, 0, 0, 0, 1};
	for (int shift = 24; shift >= 0; shift -= 8) {
		packet.push_back((uint8_t)(session->mediaSsrc >> shift));
	}
	size_t i = 0;
	while (i < missing.size()) {
		int64_t pid = missing[i];
		uint16_t blp = 0;
		size_t j = i + 1;
		while (j < missing.size() && missing[j] - pid <= 16) {
			if (missing[j] > pid) {
				blp |= (uint16_t)(1u << (missing[j] - pid - 1));
			}
			j++;
		}
		uint16_t seq = (uint16_t)pid;
		packet.push_back((uint8_t)(seq >> 8));
		packet.push_back((uint8_t)seq);
		packet.push_back((uint8_t)(blp >> 8));
		packet.push_back((uint8_t)blp);
		i = j;
	}
	size_t words = packet.size() / 4 - 1;
	packet[2] = (uint8_t)(words >> 8);
	packet[3] = (uint8_t)words;
	try {
		session->rtcp->sendRtcp(packet);
	} catch (...) {
	}
}

/* Recebe um pacote do celular (na linha da libdatachannel). */
void onRtp(WebCam *cam, Session *session, const rtc::binary &message)
{
	std::lock_guard<std::mutex> lock(session->mutex);
	if (session->dead) {
		return;
	}
	uint64_t now = os_gettime_ns();
	session->statsBytes += message.size();
	if (session->track && now - session->lastBitrateAsk > 1000000000ULL) {
		session->lastBitrateAsk = now;
		try {
			session->track->requestBitrate(phoneBitrate());
		} catch (...) {
		}
	}
	if (!session->statsStart) {
		session->statsStart = now;
	} else if (now - session->statsStart >= 10000000000ULL) {
		double seconds = (double)(now - session->statsStart) / 1e9;
		blog(LOG_INFO,
		     "[somun live] celular sem app: %.1f quadros/s, %.0f kb/s, reenviados %u, perdidos %u, "
		     "quadros jogados fora %u",
		     session->statsFrames / seconds, session->statsBytes * 8.0 / 1000.0 / seconds,
		     session->statsRecovered, session->statsLost, session->statsDropped);
		session->statsStart = now;
		session->statsFrames = 0;
		session->statsBytes = 0;
		session->statsRecovered = 0;
		session->statsLost = 0;
		session->statsDropped = 0;
	}

	const uint8_t *data = reinterpret_cast<const uint8_t *>(message.data());
	if (message.size() < 12 || (data[0] >> 6) != 2 || (data[1] & 0x7f) != VIDEO_PT) {
		return;
	}
	uint16_t seq16 = (uint16_t)((data[2] << 8) | data[3]);
	session->mediaSsrc = ((uint32_t)data[8] << 24) | ((uint32_t)data[9] << 16) | ((uint32_t)data[10] << 8) |
			     data[11];

	/* Número "esticado" (o de 16 bits dá a volta a cada 65536 pacotes). */
	int64_t seq = seq16;
	if (session->highest >= 0) {
		seq = (session->highest & ~0xffffLL) | seq16;
		if (seq < session->highest - 32768) {
			seq += 65536;
		} else if (seq > session->highest + 32768) {
			seq -= 65536;
		}
	}
	if (session->expected < 0) {
		session->expected = seq;
		session->highest = seq - 1;
	}
	if (seq < session->expected) {
		return; /* chegou tarde demais (já desistimos dele) ou repetido */
	}
	auto asked = session->nacked.find(seq);
	if (asked != session->nacked.end()) {
		session->statsRecovered++;
		session->nacked.erase(asked);
	}

	std::vector<int64_t> missing;
	if (seq > session->highest) {
		int64_t from = std::max(session->highest + 1, seq - 300);
		for (int64_t n = from; n < seq; n++) {
			if (!session->pending.count(n)) {
				missing.push_back(n);
				session->nacked[n] = {now, 1};
			}
		}
		session->highest = seq;
	}
	session->pending.emplace(seq, message);

	/* Pede de novo o que continua faltando (até 3 vezes, a cada 40 ms). */
	for (auto &entry : session->nacked) {
		if (entry.first >= session->expected && entry.second.second < 3 &&
		    now - entry.second.first > 40000000ULL &&
		    std::find(missing.begin(), missing.end(), entry.first) == missing.end()) {
			entry.second = {now, entry.second.second + 1};
			missing.push_back(entry.first);
		}
	}
	sendNack(session, missing);

	/* Entrega o que já está na ordem; se faltar um há mais de 150 ms, desiste dele. */
	for (;;) {
		while (!session->pending.empty() && session->pending.begin()->first == session->expected) {
			feedPacket(cam, session, session->pending.begin()->second);
			session->pending.erase(session->pending.begin());
			session->expected++;
			session->gapSince = 0;
		}
		if (session->pending.empty()) {
			break;
		}
		if (!session->gapSince) {
			session->gapSince = now;
		}
		if (now - session->gapSince > 150000000ULL || session->pending.size() > 2000) {
			session->statsLost += (uint32_t)(session->pending.begin()->first - session->expected);
			session->expected = session->pending.begin()->first;
			session->gapSince = 0;
			continue;
		}
		break;
	}
	while (!session->nacked.empty() && session->nacked.begin()->first < session->expected) {
		session->nacked.erase(session->nacked.begin());
	}
}

/* Monta a oferta para um pedido novo do celular. Devolve o texto da oferta (vazio se falhou). */
std::string startSession(WebCam *cam)
{
	auto session = std::make_shared<Session>();
	if (!session->decoder.open()) {
		blog(LOG_WARNING, "[somun live] celular sem app: sem decodificador H.264");
		return std::string();
	}
	rtc::Configuration config;
	config.iceServers.emplace_back("stun:stun.l.google.com:19302");
	config.disableAutoNegotiation = true;
	std::weak_ptr<Session> weak = session;

	try {
		session->pc = std::make_shared<rtc::PeerConnection>(config);
		session->pc->onGatheringStateChange([weak](rtc::PeerConnection::GatheringState state) {
			if (state != rtc::PeerConnection::GatheringState::Complete) {
				return;
			}
			if (auto s = weak.lock()) {
				std::lock_guard<std::mutex> lock(s->mutex);
				s->gatheringDone = true;
				s->gathered.notify_all();
			}
		});
		session->pc->onStateChange([cam, weak](rtc::PeerConnection::State state) {
			auto s = weak.lock();
			if (!s) {
				return;
			}
			std::lock_guard<std::mutex> lock(s->mutex);
			if (s->dead) {
				return;
			}
			cam->rtcState = (int)state;
			cam->wake.notify_all();
		});

		rtc::Description::Video media("video", rtc::Description::Direction::RecvOnly);
		media.addH264Codec(VIDEO_PT);
		session->track = session->pc->addTrack(media);
		auto receiving = std::make_shared<rtc::RtcpReceivingSession>();
		session->rtcp = std::make_shared<RtcpSender>();
		receiving->addToChain(session->rtcp);
		session->track->setMediaHandler(receiving);
		session->track->onMessage(
			[cam, weak](rtc::binary message) {
				if (auto s = weak.lock()) {
					onRtp(cam, s.get(), message);
				}
			},
			[](rtc::string) {});

		session->channel = session->pc->createDataChannel("somun");
		session->channel->onOpen([cam, weak]() {
			auto s = weak.lock();
			if (!s) {
				return;
			}
			{
				std::lock_guard<std::mutex> lock(s->mutex);
				if (s->dead) {
					return;
				}
			}
			sendHello(cam);
		});
		/* O celular conta, a cada 10 s, o que a câmera e o codificador dele estão fazendo
		 * ("info:{...}"): vai para o log, para achar onde os quadros se perdem. */
		session->channel->onMessage([](rtc::binary) {},
					    [](rtc::string text) {
						    if (text.rfind("info:", 0) == 0 && text.size() < 600) {
							    blog(LOG_INFO, "[somun live] celular sem app (celular): %s",
								 text.c_str() + 5);
						    }
					    });

		session->pc->setLocalDescription(rtc::Description::Type::Offer);
	} catch (const std::exception &error) {
		blog(LOG_WARNING, "[somun live] celular sem app: %s", error.what());
		closeSession(session);
		return std::string();
	}

	{
		std::unique_lock<std::mutex> lock(session->mutex);
		session->gathered.wait_for(lock, std::chrono::seconds(5), [&]() { return session->gatheringDone; });
	}
	std::string offer;
	try {
		auto description = session->pc->localDescription();
		if (description) {
			offer = std::string(*description);
		}
	} catch (...) {
	}
	if (offer.empty()) {
		closeSession(session);
		return offer;
	}

	std::shared_ptr<Session> old;
	{
		std::lock_guard<std::mutex> lock(cam->mutex);
		old = cam->session;
		cam->session = session;
	}
	closeSession(old);
	cam->rtcState = (int)rtc::PeerConnection::State::Connecting;
	return offer;
}

void applyAnswer(WebCam *cam, const std::string &answer)
{
	std::shared_ptr<Session> session;
	{
		std::lock_guard<std::mutex> lock(cam->mutex);
		session = cam->session;
	}
	if (!session) {
		return;
	}
	try {
		session->pc->setRemoteDescription(rtc::Description(answer, "answer"));
	} catch (const std::exception &error) {
		blog(LOG_WARNING, "[somun live] celular sem app: resposta inválida: %s", error.what());
	}
}

/* A linha de trabalho de cada entrada: cuida do documento de pareamento e das conexões. */
void work(WebCam *cam)
{
	std::string docCode;  /* código com o documento já criado */
	std::string answered; /* último pedido atendido */
	bool waitingAnswer = false;
	bool everConnected = false;
	Status status = Status::Waiting;

	while (!cam->stopping) {
		std::string code;
		{
			std::lock_guard<std::mutex> lock(cam->mutex);
			code = cam->code;
		}
		if (code != docCode) {
			answered.clear();
			waitingAnswer = false;
		}

		int state = cam->rtcState;
		bool connected = state == (int)rtc::PeerConnection::State::Connected;
		bool failed = state == (int)rtc::PeerConnection::State::Failed;
		bool gone = state == (int)rtc::PeerConnection::State::Disconnected ||
			    state == (int)rtc::PeerConnection::State::Closed;

		std::string uid, access;
		Login result = login(uid, access);
		if (result != Login::Ok) {
			status = result == Login::NoAccount ? Status::NoAccount : Status::Network;
		} else {
			std::string url = std::string(DOCUMENTS) + "/" + COLLECTION + "/" + code;
			if (docCode != code) {
				QJsonObject fields;
				fields["owner"] = stringValue(uid);
				fields["criado"] =
					QJsonObject{{"timestampValue",
						     QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
				QJsonObject body;
				body["fields"] = fields;
				Http created = request(
					"POST", std::string(DOCUMENTS) + "/" + COLLECTION + "?documentId=" + code,
					toJson(body), "application/json", access);
				if (created.status == 200 || created.status == 409) {
					docCode = code;
				}
			}
			Http got = request("GET", url, "", "application/json", access);
			if (got.network) {
				status = Status::Network;
			} else if (got.status == 404) {
				docCode.clear();
			} else if (got.status == 200) {
				QJsonObject fields = QJsonDocument::fromJson(QByteArray::fromStdString(got.body))
							     .object()
							     .value("fields")
							     .toObject();
				std::string pedido = field(fields, "pedido");
				if (!pedido.empty() && pedido != answered) {
					answered = pedido;
					std::string offer = startSession(cam);
					if (!offer.empty()) {
						QJsonObject update;
						update["oferta"] = stringValue(offer);
						update["ofertaPara"] = stringValue(pedido);
						QJsonObject body;
						body["fields"] = update;
						request("PATCH",
							url + "?updateMask.fieldPaths=oferta&updateMask.fieldPaths=ofertaPara",
							toJson(body), "application/json", access);
						waitingAnswer = true;
						connected = failed = gone = false;
						blog(LOG_INFO,
						     "[somun live] celular sem app '%s': celular pediu para ligar",
						     obs_source_get_name(cam->source));
					}
				} else if (waitingAnswer && field(fields, "respostaDe") == answered) {
					std::string answer = field(fields, "resposta");
					if (!answer.empty()) {
						applyAnswer(cam, answer);
						waitingAnswer = false;
					}
				}
				if (connected) {
					status = Status::Connected;
					everConnected = true;
				} else if (waitingAnswer || state == (int)rtc::PeerConnection::State::Connecting) {
					status = Status::Connecting;
				} else if (failed) {
					status = everConnected ? Status::Left : Status::Failed;
				} else if (gone && everConnected) {
					status = Status::Left;
				} else {
					status = Status::Waiting;
				}
			}
		}

		/* Conectado mas sem imagem ainda: continua mostrando "conectando". */
		if (status == Status::Connected) {
			std::shared_ptr<Session> session;
			{
				std::lock_guard<std::mutex> lock(cam->mutex);
				session = cam->session;
			}
			bool video = false;
			if (session) {
				std::lock_guard<std::mutex> lock(session->mutex);
				video = session->gotVideo;
			}
			if (!video) {
				status = Status::Connecting;
			}
		}
		if (cam->lastFrame) {
			cam->screenValid = false; /* a última imagem do celular fica (mesmo se ele caiu) */
		} else {
			showScreen(cam, status, code);
		}

		int seconds = status == Status::Connected ? 20 : (waitingAnswer ? 1 : 3);
		std::mutex sleepMutex;
		std::unique_lock<std::mutex> lock(sleepMutex);
		int before = cam->rtcState;
		cam->wake.wait_for(lock, std::chrono::seconds(seconds),
				   [&]() { return cam->stopping.load() || cam->rtcState != before; });
	}
}

/* ------------------------------------------------------------------------- */
/* A entrada no somun live                                                   */

const char *webcamName(void *)
{
	return obs_module_text("WebCamSource");
}

void webcamRenamed(void *data, calldata_t *)
{
	WebCam *cam = static_cast<WebCam *>(data);
	sendToPhone(cam, std::string("nome:") + obs_source_get_name(cam->source));
}

void webcamUpdate(void *data, obs_data_t *settings)
{
	WebCam *cam = static_cast<WebCam *>(data);
	std::string code = obs_data_get_string(settings, S_CODE);
	{
		std::lock_guard<std::mutex> lock(cam->mutex);
		if (code == cam->code) {
			return;
		}
		cam->code = code;
	}
	cam->lastFrame = 0; /* código novo: volta a tela de espera com o QR novo */
	cam->wake.notify_all();
}

void *webcamCreate(obs_data_t *settings, obs_source_t *source)
{
	if (!*obs_data_get_string(settings, S_CODE)) {
		obs_data_set_string(settings, S_CODE, newCode().c_str());
	}
	WebCam *cam = new WebCam();
	cam->source = source;
	cam->code = obs_data_get_string(settings, S_CODE);
	obs_source_set_async_unbuffered(source, true);
	signal_handler_connect(obs_source_get_signal_handler(source), "rename", webcamRenamed, cam);
	cam->worker = std::thread(work, cam);
	return cam;
}

void webcamDestroy(void *data)
{
	WebCam *cam = static_cast<WebCam *>(data);
	signal_handler_disconnect(obs_source_get_signal_handler(cam->source), "rename", webcamRenamed, cam);
	cam->stopping = true;
	cam->wake.notify_all();
	if (cam->worker.joinable()) {
		cam->worker.join();
	}
	std::shared_ptr<Session> session;
	{
		std::lock_guard<std::mutex> lock(cam->mutex);
		session = cam->session;
		cam->session.reset();
	}
	closeSession(session);
	delete cam;
}

bool newCodeClicked(obs_properties_t *props, obs_property_t *, void *data)
{
	WebCam *cam = static_cast<WebCam *>(data);
	obs_data_t *settings = obs_source_get_settings(cam->source);
	std::string code = newCode();
	obs_data_set_string(settings, S_CODE, code.c_str());
	obs_source_update(cam->source, settings);
	obs_data_release(settings);
	obs_property_t *link = obs_properties_get(props, S_LINK);
	if (link) {
		obs_property_set_description(link, (std::string(PAGE_URL) + code).c_str());
	}
	return true;
}

obs_properties_t *webcamProperties(void *data)
{
	WebCam *cam = static_cast<WebCam *>(data);
	obs_properties_t *props = obs_properties_create();
	std::string code;
	if (cam) {
		std::lock_guard<std::mutex> lock(cam->mutex);
		code = cam->code;
	}
	obs_properties_add_text(props, S_LINK, (std::string(PAGE_URL) + code).c_str(), OBS_TEXT_INFO);
	obs_properties_add_button(props, S_NEW_CODE, obs_module_text("WebCam.NewCode"), newCodeClicked);
	return props;
}

void webcamActivate(void *data)
{
	WebCam *cam = static_cast<WebCam *>(data);
	cam->onAir = true;
	sendToPhone(cam, "tally:1");
}

void webcamDeactivate(void *data)
{
	WebCam *cam = static_cast<WebCam *>(data);
	cam->onAir = false;
	sendToPhone(cam, "tally:0");
}

} // namespace

extern "C" bool pl_webcam_get_status(obs_source_t *source, int *stalled_ms)
{
	*stalled_ms = -1;
	if (!source || strcmp(obs_source_get_id(source), WEBCAM_SOURCE_ID) != 0) {
		return false;
	}
	WebCam *cam = static_cast<WebCam *>(obs_obj_get_data(source));
	if (!cam) {
		return false;
	}
	uint64_t last = cam->lastFrame;
	if (last) {
		uint64_t now = os_gettime_ns();
		*stalled_ms = now > last ? (int)std::min<uint64_t>((now - last) / 1000000ULL, 1000000000ULL) : 0;
	}
	return true;
}

extern "C" struct obs_source_info pl_webcam_source;

struct obs_source_info pl_webcam_source = [] {
	struct obs_source_info info = {};
	info.id = WEBCAM_SOURCE_ID;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = webcamName;
	info.create = webcamCreate;
	info.destroy = webcamDestroy;
	info.update = webcamUpdate;
	info.get_properties = webcamProperties;
	info.activate = webcamActivate;
	info.deactivate = webcamDeactivate;
	info.icon_type = OBS_ICON_TYPE_CAMERA;
	return info;
}();
