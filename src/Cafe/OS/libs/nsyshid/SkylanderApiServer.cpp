#include "SkylanderApiServer.h"

#include <algorithm>
#include <array>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <cstring>
#include <vector>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "Common/version.h"
#include "SkylanderPortalManager.h"
#include "config/CemuConfig.h"

namespace nsyshid
{
	namespace
	{
		constexpr size_t kMaxRequestBodySize = 1024 * 1024;
		constexpr size_t kMaxRequestHeaderSize = 64 * 1024;
		const std::regex kSlotRegex(R"(^/api/skylanders/slots/([0-9]{1,2})(?:/(load|create))?$)");
		constexpr uint16 kMdnsPort = 5353;
		constexpr uint16 kUdpDiscoveryPort = 28779;
		constexpr std::string_view kMdnsMulticastAddress = "224.0.0.251";
		constexpr uint32 kMdnsTtlSeconds = 120;
		constexpr std::string_view kServiceType = "_cemu-skylander._tcp.local";
		constexpr std::string_view kServiceInstance = "Cemu Skylander API._cemu-skylander._tcp.local";
		constexpr std::string_view kServiceHost = "cemu-skylander.local";
		constexpr std::string_view kUdpDiscoveryMagic = "CEMU_SKYLANDER_DISCOVERY_V1";

		bool ParseDnsName(const std::vector<uint8>& packet, size_t& offset, std::string& outName, int depth = 0)
		{
			if (depth > 8)
				return false;
			std::string name;
			size_t current = offset;
			while (current < packet.size())
			{
				const uint8 len = packet[current++];
				if ((len & 0xC0) == 0xC0)
				{
					if (current >= packet.size())
						return false;
					const uint16 pointer = ((uint16)(len & 0x3F) << 8) | packet[current++];
					if (pointer >= packet.size())
						return false;
					size_t ptrOffset = pointer;
					std::string pointedName;
					if (!ParseDnsName(packet, ptrOffset, pointedName, depth + 1))
						return false;
					if (!name.empty() && !pointedName.empty())
						name += ".";
					name += pointedName;
					break;
				}
				if (len == 0)
					break;
				if (current + len > packet.size())
					return false;
				if (!name.empty())
					name += ".";
				name.append((const char*)packet.data() + current, len);
				current += len;
			}
			offset = current;
			outName = std::move(name);
			return true;
		}

		void WriteU16(std::string& out, uint16 value)
		{
			out.push_back((char)((value >> 8) & 0xFF));
			out.push_back((char)(value & 0xFF));
		}

		void WriteU32(std::string& out, uint32 value)
		{
			out.push_back((char)((value >> 24) & 0xFF));
			out.push_back((char)((value >> 16) & 0xFF));
			out.push_back((char)((value >> 8) & 0xFF));
			out.push_back((char)(value & 0xFF));
		}

		void WriteDnsName(std::string& out, std::string_view name)
		{
			size_t start = 0;
			while (start < name.size())
			{
				size_t end = name.find('.', start);
				if (end == std::string_view::npos)
					end = name.size();
				const size_t len = end - start;
				out.push_back((char)len);
				out.append(name.substr(start, len));
				start = end + 1;
			}
			out.push_back('\0');
		}

		std::vector<std::string> GetLocalIpv4Addresses()
		{
			std::set<std::string> uniqueAddresses;
			try
			{
				boost::asio::io_context io;
				boost::system::error_code ec;
				boost::asio::ip::tcp::resolver resolver(io);
				const auto hostname = boost::asio::ip::host_name(ec);
				if (!ec)
				{
					const auto results = resolver.resolve(hostname, "", ec);
					if (!ec)
					{
						for (const auto& result : results)
						{
							const auto addr = result.endpoint().address();
							if (!addr.is_v4() || addr.is_loopback() || addr.is_unspecified())
								continue;
							uniqueAddresses.emplace(addr.to_string());
						}
					}
				}
			}
			catch (...)
			{
			}

			return std::vector<std::string>(uniqueAddresses.begin(), uniqueAddresses.end());
		}

		void AppendARecord(std::string& out, std::string_view hostName, std::string_view addressText)
		{
			boost::system::error_code ec;
			const auto parsedAddress = boost::asio::ip::make_address_v4(std::string(addressText), ec);
			if (ec)
				return;
			WriteDnsName(out, hostName);
			WriteU16(out, 1);
			WriteU16(out, 1);
			WriteU32(out, kMdnsTtlSeconds);
			const auto bytes = parsedAddress.to_bytes();
			WriteU16(out, 4);
			out.append((const char*)bytes.data(), bytes.size());
		}
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
		std::thread mdnsThread;
		std::thread udpDiscoveryThread;
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
			if (m_mdnsSocket)
			{
				boost::system::error_code ec;
				m_mdnsSocket->close(ec);
			}
			if (m_udpDiscoverySocket)
			{
				boost::system::error_code ec;
				m_udpDiscoverySocket->close(ec);
			}
			if (m_httpIoContext)
				m_httpIoContext->stop();
			if (m_httpsIoContext)
				m_httpsIoContext->stop();
			if (m_discoveryIoContext)
				m_discoveryIoContext->stop();

			httpThread = std::move(m_httpThread);
			httpsThread = std::move(m_httpsThread);
			mdnsThread = std::move(m_mdnsThread);
			udpDiscoveryThread = std::move(m_udpDiscoveryThread);
		}
		if (httpThread.joinable())
			httpThread.join();
		if (httpsThread.joinable())
			httpsThread.join();
		if (mdnsThread.joinable())
			mdnsThread.join();
		if (udpDiscoveryThread.joinable())
			udpDiscoveryThread.join();

		{
			std::lock_guard lock(m_mutex);
			m_httpAcceptor.reset();
			m_httpsAcceptor.reset();
			m_mdnsSocket.reset();
			m_udpDiscoverySocket.reset();
			m_httpIoContext.reset();
			m_httpsIoContext.reset();
			m_discoveryIoContext.reset();
			m_httpsSslContext.reset();
			m_httpRunning = false;
			m_httpsRunning = false;
			m_mdnsRunning = false;
			m_udpDiscoveryRunning = false;
			m_primaryPort = 0;
			m_primaryHttps = false;
			m_localAddress.clear();
			m_localAddresses.clear();
			m_discoveryStatus = "Discovery stopped";
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
			const bool certMissing = httpsCertPath.empty();
			const bool keyMissing = httpsKeyPath.empty();
			if (certMissing && keyMissing)
				error = "HTTPS enabled but cert and key paths are empty";
			else if (certMissing)
				error = "HTTPS enabled but cert path is empty";
			else
				error = "HTTPS enabled but key path is empty";
			return false;
		}

		m_stopRequested = false;
		try
		{
			m_discoveryIoContext = std::make_unique<boost::asio::io_context>();
			m_mdnsSocket = std::make_unique<boost::asio::ip::udp::socket>(*m_discoveryIoContext);
			m_udpDiscoverySocket = std::make_unique<boost::asio::ip::udp::socket>(*m_discoveryIoContext);

			if (enableHttp)
			{
				const auto host = usbCfg.skylander_api_http_host.GetValue();
				const uint16 port = usbCfg.skylander_api_http_port.GetValue();
				m_httpIoContext = std::make_unique<boost::asio::io_context>();
				boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::make_address(host), port);
				m_httpAcceptor = std::make_unique<boost::asio::ip::tcp::acceptor>(*m_httpIoContext);
				m_httpAcceptor->open(endpoint.protocol());
				m_httpAcceptor->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
				m_httpAcceptor->bind(endpoint);
				m_httpAcceptor->listen(boost::asio::socket_base::max_listen_connections);
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
				m_httpsAcceptor = std::make_unique<boost::asio::ip::tcp::acceptor>(*m_httpsIoContext);
				m_httpsAcceptor->open(endpoint.protocol());
				m_httpsAcceptor->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
				m_httpsAcceptor->bind(endpoint);
				m_httpsAcceptor->listen(boost::asio::socket_base::max_listen_connections);
				m_httpsRunning = true;
				m_httpsThread = std::thread([this]() { HttpsAcceptLoop(); });
			}

			const uint16 primaryPort = enableHttp ? usbCfg.skylander_api_http_port.GetValue() : usbCfg.skylander_api_https_port.GetValue();
			const bool primaryHttps = !enableHttp && enableHttps;
			m_primaryPort = primaryPort;
			m_primaryHttps = primaryHttps;

			auto localAddresses = GetLocalIpv4Addresses();
			if (localAddresses.empty())
			{
				const auto host = enableHttp ? usbCfg.skylander_api_http_host.GetValue() : usbCfg.skylander_api_https_host.GetValue();
				boost::system::error_code hostEc;
				const auto parsedAddress = boost::asio::ip::make_address(host, hostEc);
				if (!hostEc && parsedAddress.is_v4() && !parsedAddress.is_unspecified() && !parsedAddress.is_loopback())
					localAddresses.emplace_back(parsedAddress.to_string());
			}
			m_localAddresses = localAddresses;
			m_localAddress = m_localAddresses.empty() ? "" : m_localAddresses.front();

			std::string discoveryResult;
			try
			{
				boost::asio::ip::udp::endpoint mdnsListenEndpoint(boost::asio::ip::udp::v4(), kMdnsPort);
				m_mdnsSocket->open(mdnsListenEndpoint.protocol());
				m_mdnsSocket->set_option(boost::asio::ip::udp::socket::reuse_address(true));
				m_mdnsSocket->bind(mdnsListenEndpoint);
				m_mdnsSocket->set_option(boost::asio::ip::multicast::join_group(boost::asio::ip::make_address_v4(std::string(kMdnsMulticastAddress))));
				m_mdnsRunning = true;
				m_mdnsThread = std::thread([this]() { MdnsLoop(); });
				discoveryResult += "mDNS on";
			}
			catch (const std::exception& ex)
			{
				discoveryResult += fmt::format("mDNS unavailable ({})", ex.what());
				m_mdnsRunning = false;
			}

			try
			{
				boost::asio::ip::udp::endpoint udpDiscoveryEndpoint(boost::asio::ip::udp::v4(), kUdpDiscoveryPort);
				m_udpDiscoverySocket->open(udpDiscoveryEndpoint.protocol());
				m_udpDiscoverySocket->set_option(boost::asio::ip::udp::socket::reuse_address(true));
				m_udpDiscoverySocket->set_option(boost::asio::socket_base::broadcast(true));
				m_udpDiscoverySocket->bind(udpDiscoveryEndpoint);
				m_udpDiscoveryRunning = true;
				m_udpDiscoveryThread = std::thread([this]() { UdpDiscoveryLoop(); });
				if (!discoveryResult.empty())
					discoveryResult += ", ";
				discoveryResult += "UDP fallback on";
			}
			catch (const std::exception& ex)
			{
				if (!discoveryResult.empty())
					discoveryResult += ", ";
				discoveryResult += fmt::format("UDP fallback unavailable ({})", ex.what());
				m_udpDiscoveryRunning = false;
			}

			std::vector<std::string> statusParts;
			if (enableHttp)
				statusParts.emplace_back(fmt::format("HTTP {}:{}", usbCfg.skylander_api_http_host.GetValue(), usbCfg.skylander_api_http_port.GetValue()));
			if (enableHttps)
				statusParts.emplace_back(fmt::format("HTTPS {}:{}", usbCfg.skylander_api_https_host.GetValue(), usbCfg.skylander_api_https_port.GetValue()));
			if (!m_localAddress.empty())
				statusParts.emplace_back(fmt::format("Connect {}://{}:{}", primaryHttps ? "https" : "http", m_localAddress, primaryPort));
			else
				statusParts.emplace_back(fmt::format("Connect {}://<lan-ip>:{}", primaryHttps ? "https" : "http", primaryPort));
			if (m_mdnsRunning)
				statusParts.emplace_back(fmt::format("mDNS {} on {}", kServiceType, primaryPort));
			else
				statusParts.emplace_back("mDNS unavailable");
			if (m_udpDiscoveryRunning)
				statusParts.emplace_back(fmt::format("UDP discovery port {}", kUdpDiscoveryPort));
			else
				statusParts.emplace_back("UDP discovery unavailable");
			m_discoveryStatus = fmt::format("{} (primary {}:{})",
											discoveryResult.empty() ? "Discovery unavailable" : discoveryResult,
											primaryHttps ? "https" : "http", primaryPort);
			std::string status = "Running (";
			for (size_t i = 0; i < statusParts.size(); ++i)
			{
				if (i != 0)
					status += " | ";
				status += statusParts[i];
			}
			status += ")";
			SetStatusText(std::move(status));
		}
		catch (const std::exception& ex)
		{
			error = ex.what();
			Stop();
			return false;
		}

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
		std::string headerAndMaybeBody;
		try
		{
			boost::asio::read_until(stream, boost::asio::dynamic_buffer(headerAndMaybeBody, kMaxRequestHeaderSize), "\r\n\r\n");
		}
		catch (const std::length_error&)
		{
			return MakeJsonError(413, "Request headers too large");
		}

		const size_t headerEndPos = headerAndMaybeBody.find("\r\n\r\n");
		if (headerEndPos == std::string::npos)
			return MakeJsonError(400, "Malformed HTTP headers");

		std::istringstream requestStream(headerAndMaybeBody.substr(0, headerEndPos + 4));
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
				const auto parsedLength = std::stoull(contentLen);
				if (parsedLength > kMaxRequestBodySize)
					return MakeJsonError(413, "Payload too large");
				contentLength = (size_t)parsedLength;
			}
			catch (...)
			{
				return MakeJsonError(400, "Invalid Content-Length");
			}
		}

		if (contentLength > 0)
		{
			body.resize(contentLength);
			size_t alreadyBuffered = 0;
			if (headerAndMaybeBody.size() > (headerEndPos + 4))
				alreadyBuffered = std::min(contentLength, headerAndMaybeBody.size() - (headerEndPos + 4));
			if (alreadyBuffered > 0)
				memcpy(body.data(), headerAndMaybeBody.data() + headerEndPos + 4, alreadyBuffered);
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
					boost::system::error_code endpointEc;
					const auto remoteEndpoint = sslStream.lowest_layer().remote_endpoint(endpointEc);
					if (!endpointEc)
						cemuLog_log(LogType::Force, "Skylander API HTTPS handshake failed from {}:{}: {}", remoteEndpoint.address().to_string(), remoteEndpoint.port(), ec.message());
					else
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

	void SkylanderApiServer::MdnsLoop()
	{
		try
		{
			std::array<uint8, 1500> buffer{};
			while (!m_stopRequested.load())
			{
				boost::asio::ip::udp::endpoint remoteEndpoint;
				boost::system::error_code ec;
				const size_t received = m_mdnsSocket->receive_from(boost::asio::buffer(buffer), remoteEndpoint, 0, ec);
				if (ec)
				{
					if (m_stopRequested.load())
						break;
					continue;
				}
				if (received < 17)
					continue;

				std::vector<uint8> packet(buffer.begin(), buffer.begin() + received);
				const uint16 flags = ((uint16)packet[2] << 8) | packet[3];
				const uint16 qdCount = ((uint16)packet[4] << 8) | packet[5];
				if ((flags & 0x8000) != 0 || qdCount == 0)
					continue;

				size_t offset = 12;
				bool hasServiceQuery = false;
				for (uint16 q = 0; q < qdCount; ++q)
				{
					std::string queryName;
					if (!ParseDnsName(packet, offset, queryName))
					{
						hasServiceQuery = false;
						break;
					}
					if (offset + 4 > packet.size())
					{
						hasServiceQuery = false;
						break;
					}
					const uint16 queryType = ((uint16)packet[offset] << 8) | packet[offset + 1];
					offset += 4;
					const bool typeMatches = queryType == 12 || queryType == 255;
					if (typeMatches && queryName == kServiceType)
					{
						hasServiceQuery = true;
						break;
					}
				}

				if (!hasServiceQuery)
					continue;

				uint16 primaryPort = 0;
				bool primaryHttps = false;
				std::string localAddress;
				std::vector<std::string> localAddresses;
				{
					std::lock_guard lock(m_mutex);
					primaryPort = m_primaryPort;
					primaryHttps = m_primaryHttps;
					localAddress = m_localAddress;
					localAddresses = m_localAddresses;
				}
				if (primaryPort == 0)
					continue;
				if (localAddresses.empty() && !localAddress.empty())
					localAddresses.push_back(localAddress);

				std::string response;
				WriteU16(response, ((uint16)packet[0] << 8) | packet[1]);
				WriteU16(response, 0x8400);
				WriteU16(response, 0);
				WriteU16(response, (uint16)(3 + std::max<size_t>(1, localAddresses.size())));
				WriteU16(response, 0);
				WriteU16(response, 0);

				WriteDnsName(response, kServiceType);
				WriteU16(response, 12);
				WriteU16(response, 1);
				WriteU32(response, kMdnsTtlSeconds);
				std::string ptrData;
				WriteDnsName(ptrData, kServiceInstance);
				WriteU16(response, (uint16)ptrData.size());
				response += ptrData;

				WriteDnsName(response, kServiceInstance);
				WriteU16(response, 33);
				WriteU16(response, 1);
				WriteU32(response, kMdnsTtlSeconds);
				std::string srvData;
				WriteU16(srvData, 0);
				WriteU16(srvData, 0);
				WriteU16(srvData, primaryPort);
				WriteDnsName(srvData, kServiceHost);
				WriteU16(response, (uint16)srvData.size());
				response += srvData;

				WriteDnsName(response, kServiceInstance);
				WriteU16(response, 16);
				WriteU16(response, 1);
				WriteU32(response, kMdnsTtlSeconds);
				const std::string txtValue = fmt::format("scheme={}", primaryHttps ? "https" : "http");
				std::string txtData;
				txtData.push_back((char)txtValue.size());
				txtData += txtValue;
				WriteU16(response, (uint16)txtData.size());
				response += txtData;

				if (localAddresses.empty())
					AppendARecord(response, kServiceHost, "127.0.0.1");
				else
				{
					for (const auto& addr : localAddresses)
						AppendARecord(response, kServiceHost, addr);
				}

				m_mdnsSocket->send_to(boost::asio::buffer(response), remoteEndpoint, 0, ec);
				if (ec)
					continue;
			}
		}
		catch (const std::exception& ex)
		{
			SetStatusText(fmt::format("mDNS discovery failed: {}", ex.what()));
		}

		std::lock_guard lock(m_mutex);
		m_mdnsRunning = false;
	}

	void SkylanderApiServer::UdpDiscoveryLoop()
	{
		try
		{
			std::array<char, 1024> buffer{};
			while (!m_stopRequested.load())
			{
				boost::asio::ip::udp::endpoint remoteEndpoint;
				boost::system::error_code ec;
				const size_t received = m_udpDiscoverySocket->receive_from(boost::asio::buffer(buffer), remoteEndpoint, 0, ec);
				if (ec)
				{
					if (m_stopRequested.load())
						break;
					continue;
				}

				std::string_view request(buffer.data(), received);
				if (request != kUdpDiscoveryMagic)
					continue;

				const std::string response = BuildInfoJson();
				m_udpDiscoverySocket->send_to(boost::asio::buffer(response), remoteEndpoint, 0, ec);
			}
		}
		catch (const std::exception& ex)
		{
			SetStatusText(fmt::format("UDP discovery failed: {}", ex.what()));
		}

		std::lock_guard lock(m_mutex);
		m_udpDiscoveryRunning = false;
	}

	std::string SkylanderApiServer::BuildInfoJson() const
	{
		const auto& cfg = GetConfig().emulated_usb_devices;
		const auto status = GetStatusText();
		bool mdnsRunning = false;
		bool udpRunning = false;
		bool primaryHttps = false;
		uint16 primaryPort = 0;
		std::string localAddress;
		std::vector<std::string> localAddresses;
		std::string discoveryStatus;
		{
			std::lock_guard lock(m_mutex);
			mdnsRunning = m_mdnsRunning;
			udpRunning = m_udpDiscoveryRunning;
			primaryHttps = m_primaryHttps;
			primaryPort = m_primaryPort;
			localAddress = m_localAddress;
			localAddresses = m_localAddresses;
			discoveryStatus = m_discoveryStatus;
		}
		rapidjson::StringBuffer s;
		rapidjson::Writer<rapidjson::StringBuffer> w(s);
		w.StartObject();
		w.Key("ok");
		w.Bool(true);
		w.Key("status");
		w.String(status.c_str(), (rapidjson::SizeType)status.size());
		w.Key("version");
		w.String(BUILD_VERSION_STRING);
		w.Key("serviceType");
		w.String(kServiceType.data(), (rapidjson::SizeType)kServiceType.size());
		w.Key("serviceInstance");
		w.String(kServiceInstance.data(), (rapidjson::SizeType)kServiceInstance.size());
		w.Key("http");
		w.StartObject();
		w.Key("enabled");
		w.Bool(cfg.skylander_api_http_enabled.GetValue());
		w.Key("host");
		w.String(cfg.skylander_api_http_host.GetValue().c_str());
		w.Key("port");
		w.Uint(cfg.skylander_api_http_port.GetValue());
		w.EndObject();
		w.Key("https");
		w.StartObject();
		w.Key("enabled");
		w.Bool(cfg.skylander_api_https_enabled.GetValue());
		w.Key("host");
		w.String(cfg.skylander_api_https_host.GetValue().c_str());
		w.Key("port");
		w.Uint(cfg.skylander_api_https_port.GetValue());
		w.EndObject();
		w.Key("discovery");
		w.StartObject();
		w.Key("mdnsAdvertised");
		w.Bool(mdnsRunning);
		w.Key("udpFallback");
		w.Bool(udpRunning);
		w.Key("udpPort");
		w.Uint(kUdpDiscoveryPort);
		w.Key("udpProbe");
		w.String(kUdpDiscoveryMagic.data(), (rapidjson::SizeType)kUdpDiscoveryMagic.size());
		w.Key("primaryScheme");
		w.String(primaryHttps ? "https" : "http");
		w.Key("primaryPort");
		w.Uint(primaryPort);
		w.Key("localAddress");
		w.String(localAddress.c_str());
		w.Key("localAddresses");
		w.StartArray();
		for (const auto& addr : localAddresses)
			w.String(addr.c_str());
		w.EndArray();
		w.Key("connectUrl");
		if (!localAddress.empty() && primaryPort != 0)
		{
			const auto connectUrl = fmt::format("{}://{}:{}", primaryHttps ? "https" : "http", localAddress, primaryPort);
			w.String(connectUrl.c_str());
		}
		else
		{
			w.String("");
		}
		w.Key("summary");
		w.String(discoveryStatus.c_str());
		w.EndObject();
		w.Key("capabilities");
		w.StartArray();
		w.String("health");
		w.String("info");
		w.String("config");
		w.String("slots");
		w.String("catalog");
		w.String("mdns-discovery");
		w.String("udp-discovery-fallback");
		w.String("https-optional");
		w.EndArray();
		w.EndObject();
		return s.GetString();
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
		if (method == "GET" && (path == "/api/skylanders/health" || path == "/api/skylanders/info"))
		{
			return MakeJsonOk(BuildInfoJson());
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
			int slotNum = 0;
			try
			{
				slotNum = std::stoi(std::string(match[1].first, match[1].second));
			}
			catch (...)
			{
				return MakeJsonError(400, "Invalid slot number");
			}
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
