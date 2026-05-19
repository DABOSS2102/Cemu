#include "SkylanderApiServer.h"

#include <regex>
#include <sstream>
#include <unordered_map>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "SkylanderPortalManager.h"
#include "config/CemuConfig.h"

namespace nsyshid
{
	namespace
	{
		constexpr size_t kMaxRequestBodySize = 10 * 1024 * 1024;
		const std::regex kSlotRegex(R"(^/api/skylanders/slots/([0-9]{1,2})(?:/(load|create))?$)");
	}

	SkylanderApiServer& SkylanderApiServer::GetInstance()
	{
		static SkylanderApiServer s_instance;
		return s_instance;
	}

	SkylanderApiServer::~SkylanderApiServer()
	{
		Stop();
	}

	bool SkylanderApiServer::IsRunning() const
	{
		std::lock_guard lock(m_mutex);
		return m_httpRunning || m_httpsRunning;
	}

	std::string SkylanderApiServer::GetStatusText() const
	{
		std::lock_guard lock(m_mutex);
		return m_statusText;
	}

	void SkylanderApiServer::SetStatusText(std::string statusText)
	{
		std::lock_guard lock(m_mutex);
		m_statusText = std::move(statusText);
	}

	void SkylanderApiServer::Stop()
	{
		m_stopRequested = true;
		std::thread httpThread;
		std::thread httpsThread;
		{
			std::lock_guard lock(m_mutex);
			if (m_httpAcceptor)
			{
				boost::system::error_code ec;
				m_httpAcceptor->close(ec);
			}
			if (m_httpsAcceptor)
			{
				boost::system::error_code ec;
				m_httpsAcceptor->close(ec);
			}
			if (m_httpIoContext)
				m_httpIoContext->stop();
			if (m_httpsIoContext)
				m_httpsIoContext->stop();

			httpThread = std::move(m_httpThread);
			httpsThread = std::move(m_httpsThread);
		}
		if (httpThread.joinable())
			httpThread.join();
		if (httpsThread.joinable())
			httpsThread.join();

		{
			std::lock_guard lock(m_mutex);
			m_httpAcceptor.reset();
			m_httpsAcceptor.reset();
			m_httpIoContext.reset();
			m_httpsIoContext.reset();
			m_httpsSslContext.reset();
			m_httpRunning = false;
			m_httpsRunning = false;
			m_statusText = "Stopped";
		}
	}

	bool SkylanderApiServer::ApplyConfig()
	{
		Stop();
		std::string error;
		if (!StartFromConfig(error))
		{
			if (error.empty())
				error = "Failed to start Skylander API server";
			SetStatusText(error);
			return false;
		}
		return true;
	}

	bool SkylanderApiServer::StartFromConfig(std::string& error)
	{
		error.clear();
		const auto& usbCfg = GetConfig().emulated_usb_devices;
		if (!usbCfg.skylander_api_enabled.GetValue())
		{
			SetStatusText("Stopped (disabled)");
			return true;
		}

		const bool enableHttp = usbCfg.skylander_api_http_enabled.GetValue();
		const bool enableHttps = usbCfg.skylander_api_https_enabled.GetValue();
		const auto httpsCertPath = usbCfg.skylander_api_https_cert_path.GetValue();
		const auto httpsKeyPath = usbCfg.skylander_api_https_key_path.GetValue();
		if (!enableHttp && !enableHttps)
		{
			error = "API enabled but both HTTP and HTTPS are disabled";
			return false;
		}
		if (enableHttps && (httpsCertPath.empty() || httpsKeyPath.empty()))
		{
			error = "HTTPS enabled but cert/key path is empty";
			return false;
		}

		m_stopRequested = false;
		try
		{
			if (enableHttp)
			{
				const auto host = usbCfg.skylander_api_http_host.GetValue();
				const uint16 port = usbCfg.skylander_api_http_port.GetValue();
				m_httpIoContext = std::make_unique<boost::asio::io_context>();
				boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::make_address(host), port);
				m_httpAcceptor = std::make_unique<boost::asio::ip::tcp::acceptor>(*m_httpIoContext, endpoint);
				m_httpRunning = true;
				m_httpThread = std::thread([this]() { HttpAcceptLoop(); });
			}
			if (enableHttps)
			{
				const auto host = usbCfg.skylander_api_https_host.GetValue();
				const uint16 port = usbCfg.skylander_api_https_port.GetValue();
				m_httpsIoContext = std::make_unique<boost::asio::io_context>();
				m_httpsSslContext = std::make_unique<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);
				m_httpsSslContext->use_certificate_chain_file(httpsCertPath);
				m_httpsSslContext->use_private_key_file(httpsKeyPath, boost::asio::ssl::context::pem);
				boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::make_address(host), port);
				m_httpsAcceptor = std::make_unique<boost::asio::ip::tcp::acceptor>(*m_httpsIoContext, endpoint);
				m_httpsRunning = true;
				m_httpsThread = std::thread([this]() { HttpsAcceptLoop(); });
			}
		}
		catch (const std::exception& ex)
		{
			error = ex.what();
			Stop();
			return false;
		}

		SetStatusText("Running");
		return true;
	}

	std::string SkylanderApiServer::HttpStatusText(int statusCode)
	{
		switch (statusCode)
		{
		case 200: return "OK";
		case 201: return "Created";
		case 400: return "Bad Request";
		case 413: return "Payload Too Large";
		case 404: return "Not Found";
		case 405: return "Method Not Allowed";
		case 409: return "Conflict";
		case 500: return "Internal Server Error";
		default: return "Unknown";
		}
	}

	std::string SkylanderApiServer::ParseHeaderValue(const std::unordered_map<std::string, std::string>& headers, std::string_view key)
	{
		const auto it = headers.find(std::string(key));
		if (it == headers.end())
			return {};
		return it->second;
	}

	SkylanderApiServer::HttpResponse SkylanderApiServer::MakeJsonError(int status, std::string_view errorText) const
	{
		rapidjson::StringBuffer s;
		rapidjson::Writer<rapidjson::StringBuffer> w(s);
		w.StartObject();
		w.Key("ok");
		w.Bool(false);
		w.Key("error");
		w.String(errorText.data(), (rapidjson::SizeType)errorText.size());
		w.EndObject();
		return HttpResponse{status, "application/json", s.GetString()};
	}

	SkylanderApiServer::HttpResponse SkylanderApiServer::MakeJsonOk(std::string body) const
	{
		return HttpResponse{200, "application/json", std::move(body)};
	}

	template <typename TStream>
	SkylanderApiServer::HttpResponse SkylanderApiServer::ReadAndHandleRequest(TStream& stream)
	{
		boost::asio::streambuf requestBuf;
		boost::asio::read_until(stream, requestBuf, "\r\n\r\n");

		std::istream requestStream(&requestBuf);
		std::string requestLine;
		std::getline(requestStream, requestLine);
		if (!requestLine.empty() && requestLine.back() == '\r')
			requestLine.pop_back();

		std::string method;
		std::string path;
		std::string version;
		{
			std::istringstream rl(requestLine);
			rl >> method >> path >> version;
		}
		if (method.empty() || path.empty())
			return MakeJsonError(400, "Malformed request line");

		std::unordered_map<std::string, std::string> headers;
		std::string headerLine;
		while (std::getline(requestStream, headerLine))
		{
			if (headerLine == "\r" || headerLine.empty())
				break;
			if (!headerLine.empty() && headerLine.back() == '\r')
				headerLine.pop_back();
			if (const auto pos = headerLine.find(':'); pos != std::string::npos)
			{
				auto key = headerLine.substr(0, pos);
				auto value = headerLine.substr(pos + 1);
				const auto firstNotSpace = value.find_first_not_of(' ');
				if (firstNotSpace == std::string::npos)
					value.clear();
				else if (firstNotSpace > 0)
					value.erase(0, firstNotSpace);
				headers[std::move(key)] = std::move(value);
			}
		}

		std::string body;
		size_t contentLength = 0;
		if (const auto contentLen = ParseHeaderValue(headers, "Content-Length"); !contentLen.empty())
		{
			try
			{
				contentLength = (size_t)std::stoull(contentLen);
			}
			catch (...)
			{
				return MakeJsonError(400, "Invalid Content-Length");
			}
		}
		if (contentLength > kMaxRequestBodySize)
			return MakeJsonError(413, "Payload too large");

		if (contentLength > 0)
		{
			body.resize(contentLength);
			size_t alreadyBuffered = std::min(contentLength, (size_t)requestBuf.size());
			if (alreadyBuffered > 0)
				requestStream.read(body.data(), (std::streamsize)alreadyBuffered);
			if (alreadyBuffered < contentLength)
			{
				boost::asio::read(stream, boost::asio::buffer(body.data() + alreadyBuffered, contentLength - alreadyBuffered));
			}
		}

		return HandleRoute(method, path, body);
	}

	template <typename TStream>
	void SkylanderApiServer::WriteResponse(TStream& stream, const HttpResponse& response)
	{
		const std::string responseText = fmt::format(
			"HTTP/1.1 {} {}\r\n"
			"Content-Type: {}\r\n"
			"Content-Length: {}\r\n"
			"Connection: close\r\n\r\n{}",
			response.statusCode,
			HttpStatusText(response.statusCode),
			response.contentType,
			response.body.size(),
			response.body);
		boost::asio::write(stream, boost::asio::buffer(responseText));
	}

	void SkylanderApiServer::HttpAcceptLoop()
	{
		try
		{
			while (!m_stopRequested.load())
			{
				boost::asio::ip::tcp::socket socket(*m_httpIoContext);
				boost::system::error_code ec;
				m_httpAcceptor->accept(socket, ec);
				if (ec)
				{
					if (m_stopRequested.load())
						break;
					continue;
				}
				auto response = ReadAndHandleRequest(socket);
				WriteResponse(socket, response);
				socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
				socket.close(ec);
			}
		}
		catch (const std::exception& ex)
		{
			SetStatusText(fmt::format("HTTP listener failed: {}", ex.what()));
		}
		std::lock_guard lock(m_mutex);
		m_httpRunning = false;
	}

	void SkylanderApiServer::HttpsAcceptLoop()
	{
		try
		{
			while (!m_stopRequested.load())
			{
				boost::asio::ip::tcp::socket socket(*m_httpsIoContext);
				boost::system::error_code ec;
				m_httpsAcceptor->accept(socket, ec);
				if (ec)
				{
					if (m_stopRequested.load())
						break;
					continue;
				}
				boost::asio::ssl::stream<boost::asio::ip::tcp::socket> sslStream(std::move(socket), *m_httpsSslContext);
				sslStream.handshake(boost::asio::ssl::stream_base::server, ec);
				if (ec)
				{
					cemuLog_log(LogType::Force, "Skylander API HTTPS handshake failed: {}", ec.message());
					continue;
				}
				auto response = ReadAndHandleRequest(sslStream);
				WriteResponse(sslStream, response);
				sslStream.shutdown(ec);
			}
		}
		catch (const std::exception& ex)
		{
			SetStatusText(fmt::format("HTTPS listener failed: {}", ex.what()));
		}
		std::lock_guard lock(m_mutex);
		m_httpsRunning = false;
	}

	SkylanderApiServer::HttpResponse SkylanderApiServer::HandleConfigPut(const std::string& body)
	{
		rapidjson::Document d;
		d.Parse(body.c_str());
		if (d.HasParseError() || !d.IsObject())
			return MakeJsonError(400, "Invalid JSON payload");

		auto& cfg = GetConfig().emulated_usb_devices;
		if (d.HasMember("apiEnabled") && d["apiEnabled"].IsBool())
			cfg.skylander_api_enabled = d["apiEnabled"].GetBool();
		if (d.HasMember("httpEnabled") && d["httpEnabled"].IsBool())
			cfg.skylander_api_http_enabled = d["httpEnabled"].GetBool();
		if (d.HasMember("httpsEnabled") && d["httpsEnabled"].IsBool())
			cfg.skylander_api_https_enabled = d["httpsEnabled"].GetBool();
		if (d.HasMember("httpHost") && d["httpHost"].IsString())
			cfg.skylander_api_http_host = d["httpHost"].GetString();
		if (d.HasMember("httpsHost") && d["httpsHost"].IsString())
			cfg.skylander_api_https_host = d["httpsHost"].GetString();
		if (d.HasMember("httpPort") && d["httpPort"].IsUint())
			cfg.skylander_api_http_port = (uint16)d["httpPort"].GetUint();
		if (d.HasMember("httpsPort") && d["httpsPort"].IsUint())
			cfg.skylander_api_https_port = (uint16)d["httpsPort"].GetUint();
		if (d.HasMember("storagePath") && d["storagePath"].IsString())
		{
			std::string storageError;
			if (!SkylanderPortalManager::GetInstance().SetStorageFolderPath(_utf8ToPath(d["storagePath"].GetString()), storageError))
				return MakeJsonError(400, storageError);
		}
		if (d.HasMember("httpsCertPath") && d["httpsCertPath"].IsString())
			cfg.skylander_api_https_cert_path = d["httpsCertPath"].GetString();
		if (d.HasMember("httpsKeyPath") && d["httpsKeyPath"].IsString())
			cfg.skylander_api_https_key_path = d["httpsKeyPath"].GetString();

		GetConfigHandle().Save();
		ApplyConfig();

		rapidjson::StringBuffer s;
		rapidjson::Writer<rapidjson::StringBuffer> w(s);
		w.StartObject();
		w.Key("ok");
		w.Bool(true);
		w.Key("status");
		const auto status = GetStatusText();
		w.String(status.c_str(), (rapidjson::SizeType)status.size());
		w.EndObject();
		return MakeJsonOk(s.GetString());
	}

	SkylanderApiServer::HttpResponse SkylanderApiServer::HandleSlotLoad(uint8 slot, const std::string& body)
	{
		rapidjson::Document d;
		d.Parse(body.c_str());
		if (d.HasParseError() || !d.IsObject() || !d.HasMember("file") || !d["file"].IsString())
			return MakeJsonError(400, "Expected JSON object with string 'file'");

		std::string error;
		if (!SkylanderPortalManager::GetInstance().LoadSkylanderFromStorage(slot, d["file"].GetString(), error))
			return MakeJsonError(409, error);

		rapidjson::StringBuffer s;
		rapidjson::Writer<rapidjson::StringBuffer> w(s);
		w.StartObject();
		w.Key("ok");
		w.Bool(true);
		w.EndObject();
		return MakeJsonOk(s.GetString());
	}

	SkylanderApiServer::HttpResponse SkylanderApiServer::HandleSlotCreate(uint8 slot, const std::string& body)
	{
		rapidjson::Document d;
		d.Parse(body.c_str());
		if (d.HasParseError() || !d.IsObject() || !d.HasMember("file") || !d["file"].IsString() ||
			!d.HasMember("skyId") || !d["skyId"].IsUint() || !d.HasMember("skyVar") || !d["skyVar"].IsUint())
			return MakeJsonError(400, "Expected 'file', 'skyId', and 'skyVar'");

		std::string error;
		if (!SkylanderPortalManager::GetInstance().CreateSkylanderInStorage(slot, d["file"].GetString(),
																			 (uint16)d["skyId"].GetUint(), (uint16)d["skyVar"].GetUint(), error))
			return MakeJsonError(409, error);

		rapidjson::StringBuffer s;
		rapidjson::Writer<rapidjson::StringBuffer> w(s);
		w.StartObject();
		w.Key("ok");
		w.Bool(true);
		w.EndObject();
		return MakeJsonOk(s.GetString());
	}

	SkylanderApiServer::HttpResponse SkylanderApiServer::HandleRoute(std::string_view method, std::string_view path, const std::string& body)
	{
		if (method == "GET" && path == "/api/skylanders/health")
		{
			rapidjson::StringBuffer s;
			rapidjson::Writer<rapidjson::StringBuffer> w(s);
			w.StartObject();
			w.Key("ok");
			w.Bool(true);
			w.Key("status");
			const auto status = GetStatusText();
			w.String(status.c_str(), (rapidjson::SizeType)status.size());
			w.EndObject();
			return MakeJsonOk(s.GetString());
		}
		if (method == "GET" && path == "/api/skylanders/config")
		{
			const auto& cfg = GetConfig().emulated_usb_devices;
			rapidjson::StringBuffer s;
			rapidjson::Writer<rapidjson::StringBuffer> w(s);
			w.StartObject();
			w.Key("apiEnabled");
			w.Bool(cfg.skylander_api_enabled.GetValue());
			w.Key("httpEnabled");
			w.Bool(cfg.skylander_api_http_enabled.GetValue());
			w.Key("httpHost");
			w.String(cfg.skylander_api_http_host.GetValue().c_str());
			w.Key("httpPort");
			w.Uint(cfg.skylander_api_http_port.GetValue());
			w.Key("httpsEnabled");
			w.Bool(cfg.skylander_api_https_enabled.GetValue());
			w.Key("httpsHost");
			w.String(cfg.skylander_api_https_host.GetValue().c_str());
			w.Key("httpsPort");
			w.Uint(cfg.skylander_api_https_port.GetValue());
			w.Key("storagePath");
			w.String(cfg.skylander_api_storage_path.GetValue().c_str());
			w.Key("httpsCertPath");
			w.String(cfg.skylander_api_https_cert_path.GetValue().c_str());
			w.Key("httpsKeyPath");
			w.String(cfg.skylander_api_https_key_path.GetValue().c_str());
			w.EndObject();
			return MakeJsonOk(s.GetString());
		}
		if (method == "PUT" && path == "/api/skylanders/config")
			return HandleConfigPut(body);
		if (method == "GET" && path == "/api/skylanders/slots")
		{
			const auto slots = SkylanderPortalManager::GetInstance().GetSlots();
			rapidjson::StringBuffer s;
			rapidjson::Writer<rapidjson::StringBuffer> w(s);
			w.StartObject();
			w.Key("slots");
			w.StartArray();
			for (const auto& slot : slots)
			{
				w.StartObject();
				w.Key("uiSlot");
				w.Uint(slot.uiSlot);
				w.Key("loaded");
				w.Bool(slot.loaded);
				if (slot.loaded)
				{
					w.Key("portalSlot");
					w.Uint(slot.portalSlot);
					w.Key("skyId");
					w.Uint(slot.skyId);
					w.Key("skyVar");
					w.Uint(slot.skyVar);
					w.Key("name");
					const auto name = g_skyportal.FindSkylander(slot.skyId, slot.skyVar);
					w.String(name.c_str(), (rapidjson::SizeType)name.size());
					w.Key("filePath");
					const auto filePathUtf8 = _pathToUtf8(slot.filePath);
					w.String(filePathUtf8.c_str(), (rapidjson::SizeType)filePathUtf8.size());
				}
				w.EndObject();
			}
			w.EndArray();
			w.EndObject();
			return MakeJsonOk(s.GetString());
		}
		if (method == "GET" && path == "/api/skylanders/catalog")
		{
			rapidjson::StringBuffer s;
			rapidjson::Writer<rapidjson::StringBuffer> w(s);
			w.StartObject();
			w.Key("catalog");
			w.StartArray();
			for (const auto& it : g_skyportal.GetListSkylanders())
			{
				w.StartObject();
				w.Key("skyId");
				w.Uint(it.first.first);
				w.Key("skyVar");
				w.Uint(it.first.second);
				w.Key("name");
				w.String(it.second);
				w.EndObject();
			}
			w.EndArray();
			w.EndObject();
			return MakeJsonOk(s.GetString());
		}

		std::match_results<std::string_view::const_iterator> match;
		if (std::regex_match(path.begin(), path.end(), match, kSlotRegex))
		{
			const auto slotNum = std::stoi(std::string(match[1].first, match[1].second));
			if (slotNum >= MAX_SKYLANDERS)
				return MakeJsonError(400, "Invalid slot index");
			const uint8 slot = (uint8)slotNum;
			const std::string action(match[2].first, match[2].second);
			if (method == "DELETE" && action.empty())
			{
				SkylanderPortalManager::GetInstance().ClearSkylander(slot);
				return MakeJsonOk(R"({"ok":true})");
			}
			if (method == "POST" && action == "load")
				return HandleSlotLoad(slot, body);
			if (method == "POST" && action == "create")
				return HandleSlotCreate(slot, body);
			return MakeJsonError(405, "Unsupported method for slot route");
		}

		return MakeJsonError(404, "Route not found");
	}
} // namespace nsyshid
