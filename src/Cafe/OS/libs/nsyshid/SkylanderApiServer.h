#pragma once

#include <atomic>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

namespace nsyshid
{
	class SkylanderApiServer
	{
	  public:
		static SkylanderApiServer& GetInstance();

		bool ApplyConfig();
		void Stop();

		[[nodiscard]] bool IsRunning() const;
		[[nodiscard]] std::string GetStatusText() const;

	  private:
		SkylanderApiServer() = default;
		~SkylanderApiServer();

		struct HttpResponse
		{
			int statusCode = 200;
			std::string contentType = "application/json";
			std::string body;
		};

		bool StartFromConfig(std::string& error);
		void SetStatusText(std::string statusText);
		static std::string HttpStatusText(int statusCode);
		static std::string ParseHeaderValue(const std::unordered_map<std::string, std::string>& headers, std::string_view key);
		HttpResponse HandleRoute(std::string_view method, std::string_view path, const std::string& body);
		HttpResponse HandleConfigPut(const std::string& body);
		HttpResponse HandleSlotLoad(uint8 slot, const std::string& body);
		HttpResponse HandleSlotCreate(uint8 slot, const std::string& body);
		std::string BuildInfoJson() const;
		HttpResponse MakeJsonError(int status, std::string_view errorText) const;
		HttpResponse MakeJsonOk(std::string body) const;

		template <typename TStream>
		HttpResponse ReadAndHandleRequest(TStream& stream);

		template <typename TStream>
		void WriteResponse(TStream& stream, const HttpResponse& response);

		void HttpAcceptLoop();
		void HttpsAcceptLoop();
		void MdnsLoop();
		void UdpDiscoveryLoop();

	  private:
		mutable std::mutex m_mutex;
		std::atomic_bool m_stopRequested = false;
		bool m_httpRunning = false;
		bool m_httpsRunning = false;
		bool m_mdnsRunning = false;
		bool m_udpDiscoveryRunning = false;
		std::string m_statusText = "Stopped";
		uint16 m_primaryPort = 0;
		bool m_primaryHttps = false;
		std::string m_localAddress;
		std::vector<std::string> m_localAddresses;
		std::string m_discoveryStatus = "Discovery stopped";

		std::unique_ptr<boost::asio::io_context> m_httpIoContext;
		std::unique_ptr<boost::asio::ip::tcp::acceptor> m_httpAcceptor;
		std::thread m_httpThread;

		std::unique_ptr<boost::asio::io_context> m_httpsIoContext;
		std::unique_ptr<boost::asio::ssl::context> m_httpsSslContext;
		std::unique_ptr<boost::asio::ip::tcp::acceptor> m_httpsAcceptor;
		std::thread m_httpsThread;

		std::unique_ptr<boost::asio::io_context> m_discoveryIoContext;
		std::unique_ptr<boost::asio::ip::udp::socket> m_mdnsSocket;
		std::thread m_mdnsThread;
		std::unique_ptr<boost::asio::ip::udp::socket> m_udpDiscoverySocket;
		std::thread m_udpDiscoveryThread;
	};
} // namespace nsyshid
