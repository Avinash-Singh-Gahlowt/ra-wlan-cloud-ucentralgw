//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#pragma once

#include <array>
#include <ctime>
#include <mutex>
#include <thread>

#include "Poco/AutoPtr.h"
#include "Poco/Net/HTTPRequestHandler.h"
#include "Poco/Net/HTTPRequestHandlerFactory.h"
#include "Poco/Net/HTTPServer.h"
#include "Poco/Net/HTTPServerRequest.h"
#include "Poco/Net/ParallelSocketAcceptor.h"
#include "Poco/Net/SocketAcceptor.h"
#include "Poco/Net/SocketReactor.h"
#include "Poco/Timer.h"

#include "AP_SERVER.h"
#include "AP_WS_Connection.h"
#include "AP_WS_Reactor_Pool.h"

namespace OpenWifi {

	class AP_WS_Server : public AP_Server {
	  public:
		static auto instance() {
			static auto instance_ = new AP_WS_Server;
			return instance_;
		}

		int Start() override;
		void Stop() override;
		bool IsCertOk() { return IssuerCert_ != nullptr; }
		bool ValidateCertificate(const std::string &ConnectionId,
								 const Poco::Crypto::X509Certificate &Certificate);

		inline bool IsSimSerialNumber(const std::string &SerialNumber) const {
			return IsSim(SerialNumber) &&
				   SerialNumber == SimulatorId_;
		}

		inline static bool IsSim(const std::string &SerialNumber) {
			return SerialNumber.substr(0, 6) == "53494d";
		}

		void run() override; // Garbage collector thread.
		[[nodiscard]] inline bool IsSimEnabled() const { return SimulatorEnabled_; }
		[[nodiscard]] inline bool AllowSerialNumberMismatch() const {
			return AllowSerialNumberMismatch_;
		}
		[[nodiscard]] inline uint64_t MismatchDepth() const { return MismatchDepth_; }
		[[nodiscard]] inline bool UseProvisioning() const { return LookAtProvisioning_; }
		[[nodiscard]] inline bool UseDefaults() const { return UseDefaultConfig_; }
		[[nodiscard]] inline std::pair<std::shared_ptr<Poco::Net::SocketReactor>,
									   std::shared_ptr<LockedDbSession>>
		NextReactor() {
			return Reactor_pool_->NextReactor();
		}

		inline void AddConnection(std::shared_ptr<AP_WS_Connection> Connection) {
			AP_Server::AddConnection(std::move(Connection));
		}

		[[nodiscard]] inline bool DeviceRequiresSecureRTTY(uint64_t serialNumber) const {
			std::shared_ptr<AP_Connection> Connection;
			{
				auto hashIndex = MACHash::Hash(serialNumber);
				std::lock_guard DeviceLock(SerialNumbersMutex_[hashIndex]);
				auto DeviceHint = SerialNumbers_[hashIndex].find(serialNumber);
				if (DeviceHint == end(SerialNumbers_[hashIndex]) || DeviceHint->second == nullptr)
					return false;
				Connection = DeviceHint->second;
			}
			return Connection->RTTYMustBeSecure_;
		}

		bool Disconnect(uint64_t SerialNumber);

		void CleanupSessions();

	  private:
		std::unique_ptr<Poco::Crypto::X509Certificate> IssuerCert_;
		std::vector<Poco::Crypto::X509Certificate> ClientCasCerts_;
		std::list<std::unique_ptr<Poco::Net::HTTPServer>> WebServers_;
		Poco::ThreadPool DeviceConnectionPool_{"ws:dev-pool", 4, 256};
		Poco::Net::SocketReactor Reactor_;
		Poco::Thread ReactorThread_;
		std::string SimulatorId_;
		bool LookAtProvisioning_ = false;
		bool UseDefaultConfig_ = true;
		bool SimulatorEnabled_ = false;
		bool AllowSerialNumberMismatch_ = true;

		std::unique_ptr<AP_WS_ReactorThreadPool> Reactor_pool_;

		std::uint64_t MismatchDepth_ = 2;

		Poco::Thread CleanupThread_;
		Poco::Thread GarbageCollector_;

		AP_WS_Server() noexcept
			: AP_Server("WebSocketServer", "WS-SVR", "ucentral.websocket") {}
	};

	inline auto AP_WS_Server() { return AP_WS_Server::instance(); }

} // namespace OpenWifi
