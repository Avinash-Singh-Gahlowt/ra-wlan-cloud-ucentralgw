/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

//
// Created by stephane bourque on 2022-02-03.
//

#pragma once

#include <mutex>
#include <string>

#include "Poco/JSON/Object.h"
#include <Poco/JSON/Parser.h>
#include "Poco/Logger.h"
#include "Poco/Net/SocketNotification.h"
#include "Poco/Net/SocketReactor.h"
#include "Poco/Net/StreamSocket.h"
#include "Poco/Net/WebSocket.h"
#include <Poco/Data/Session.h>

#include "RESTObjects/RESTAPI_GWobjects.h"
#include <AP_WS_Reactor_Pool.h>
#include <AP_WS_Transport.h>

namespace OpenWifi {

    class AP_WS_Connection : public AP_WS_TransportListener {
		static constexpr int BufSize = 256000;

	  public:
		explicit AP_WS_Connection(Poco::Net::HTTPServerRequest &request,
								  Poco::Net::HTTPServerResponse &response, uint64_t connection_id,
								  Poco::Logger &L, std::pair<std::shared_ptr<Poco::Net::SocketReactor>, std::shared_ptr<LockedDbSession>> R);
		
		/**
		 * @brief Construct a synthetic AP connection bound to an existing DB session.
		 *
		 * Skips transport creation, marks the connection as synthetic/registered,
		 * so the session behaves like a live device for routing.
		 */
		explicit AP_WS_Connection(uint64_t session_id, Poco::Logger &L,
								  std::shared_ptr<LockedDbSession> DbSession);
		~AP_WS_Connection();

		void EndConnection();
		void ProcessJSONRPCEvent(Poco::JSON::Object::Ptr &Doc);
		void ProcessJSONRPCResult(Poco::JSON::Object::Ptr Doc);
		void ProcessIncomingFrame();
		void ProcessIncomingRadiusData(const Poco::JSON::Object::Ptr &Doc);

		[[nodiscard]] bool Send(const std::string &Payload);
		[[nodiscard]] inline bool MustBeSecureRTTY() const { return RTTYMustBeSecure_; }

		bool SendRadiusAuthenticationData(const unsigned char *buffer, std::size_t size);
		bool SendRadiusAccountingData(const unsigned char *buffer, std::size_t size);
		bool SendRadiusCoAData(const unsigned char *buffer, std::size_t size);

		// Legacy socket handlers kept as stubs to preserve linkage; transport owns I/O
		void OnSocketReadable(const Poco::AutoPtr<Poco::Net::ReadableNotification> &pNf);
		void OnSocketShutdown(const Poco::AutoPtr<Poco::Net::ShutdownNotification> &pNf);
		void OnSocketError(const Poco::AutoPtr<Poco::Net::ErrorNotification> &pNf);

		/**
		 * @brief Wrapper to account for REST-bridged TX bytes.
		 */
		void AccountExternalFrameSent(std::size_t bytes);

		/**
		 * @brief Wrapper to account for REST-bridged RX bytes and bookkeeping.
		 */
		void AccountExternalResponse(std::size_t bytes);

		/**
		 * @brief Inject a Kafka-delivered frame into the connection processing pipeline.
		 *
		 * Used by `InfraKafkaConsumer::HandleJoin` and other broker paths to hand synthetic
		 * traffic to this connection.
		 */
		void InboundFromBroker(const std::string &Payload);

		/**
		 * @brief Record the remote endpoint string (IP[:port]) for a synthetic connection.
		 *
		 * Called whenever `AP_WS_Server::EnsureSyntheticConnection` reuses or activates a
		 * synthetic session so state and logging show the correct broker peer.
		 */
		void SetSyntheticPeer(const std::string &PeerEndPoint);

		/**
		 * @brief Populate serial-number metadata for a synthetic session.
		 *
		 * The broker path provides both the canonical string and numeric serial; this helper
		 * caches them so lookups in `SerialNumbers_`, logging, and provisioning logic treat
		 * the synthetic connection exactly like a WebSocket client.
		 */
		void InitializeSyntheticIdentity(const std::string &SerialString, uint64_t SerialNumber);

		/**
		 * @brief Flag the connection as synthetic/simulated and update certificate state.
		 *
		 * `EnsureSyntheticConnection` invokes this after creating  so the
		 * connection reports the same verification status.
		 */
		void SetSyntheticAttributes(bool Simulated);

		/**
		 * @brief Cache the controller group identifier associated with this live session.
		 */
		void SetGroupId(const std::string &groupId);

		/**
		 * @brief Retrieve the currently cached group identifier.
		 */
		[[nodiscard]] std::string GetGroupId() const;

		/**
		 * @brief Report whether this connection was created without a WebSocket transport.
		 *
		 * The synthetic path (`AP_WS_Server::EnsureSyntheticConnection`) sets this so higher
		 * layers can distinguish broker-driven sessions from true device sockets.
		 */
		[[nodiscard]] inline bool IsSynthetic() const { return Synthetic_; }

		bool LookForUpgrade(Poco::Data::Session &Session, uint64_t UUID, uint64_t &UpgradedUUID);
		void LogException(const Poco::Exception &E);
		inline Poco::Logger &Logger() { return Logger_; }
		bool SetWebSocketTelemetryReporting(uint64_t RPCID, uint64_t interval,
											uint64_t TelemetryWebSocketTimer,
											const std::vector<std::string> &TelemetryTypes);
		bool SetKafkaTelemetryReporting(uint64_t RPCID, uint64_t interval,
										uint64_t TelemetryKafkaTimer,
										const std::vector<std::string> &TelemetryTypes);
		bool StopWebSocketTelemetry(uint64_t RPCID);
		bool StopKafkaTelemetry(uint64_t RPCID);

		inline void GetLastStats(std::string &LastStats) {
			if(!Dead_) {
				std::lock_guard G(ConnectionMutex_);
				LastStats = RawLastStats_;
			}
		}

		inline void GetLastHealthCheck(GWObjects::HealthCheck &H) {
			if(!Dead_) {
				std::lock_guard G(ConnectionMutex_);
				H = RawLastHealthcheck_;
			}
		}

		inline void GetState(GWObjects::ConnectionState &State) {
			if(!Dead_) {
				std::lock_guard G(ConnectionMutex_);
				State = State_;
			}
		}

		inline GWObjects::DeviceRestrictions GetRestrictions() {
			std::lock_guard G(ConnectionMutex_);
			return Restrictions_;
		}

		[[nodiscard]] inline bool HasGPS() const { return hasGPS_; }
		[[nodiscard]] bool ValidatedDevice(Poco::Net::WebSocket &ws);

		inline bool GetTelemetryParameters(bool &Reporting, uint64_t &Interval,
										   uint64_t &WebSocketTimer, uint64_t &KafkaTimer,
										   uint64_t &WebSocketCount, uint64_t &KafkaCount,
										   uint64_t &WebSocketPackets,
										   uint64_t &KafkaPackets) const {
			Reporting = TelemetryReporting_;
			WebSocketTimer = TelemetryWebSocketTimer_;
			KafkaTimer = TelemetryKafkaTimer_;
			WebSocketCount = TelemetryWebSocketRefCount_;
			KafkaCount = TelemetryKafkaRefCount_;
			Interval = TelemetryInterval_;
			WebSocketPackets = TelemetryWebSocketPackets_;
			KafkaPackets = TelemetryKafkaPackets_;
			return true;
		}

		friend class AP_WS_Server;

		void Start();

	  private:
		mutable std::recursive_mutex ConnectionMutex_;
		std::mutex TelemetryMutex_;
		Poco::Logger &Logger_;
		std::shared_ptr<Poco::Net::SocketReactor> 	Reactor_;
		std::shared_ptr<LockedDbSession> 	DbSession_;
		std::unique_ptr<AP_WS_Transport> Transport_;
		// Legacy member kept to allow existing method definitions to compile (unused at runtime)
		std::unique_ptr<Poco::Net::WebSocket> WS_;
		std::string PeerEndPoint_;
		std::string SerialNumber_;
		uint64_t SerialNumberInt_ = 0;
		std::string Compatible_;
		std::atomic_bool Registered_ = false;
		std::string CId_;
		std::string CN_;
		uint64_t Errors_ = 0;
		Poco::Net::IPAddress PeerAddress_;
		volatile bool TelemetryReporting_ = false;
		std::atomic_uint64_t TelemetryWebSocketRefCount_ = 0;
		std::atomic_uint64_t TelemetryKafkaRefCount_ = 0;
		std::atomic_uint64_t TelemetryWebSocketTimer_ = 0;
		std::atomic_uint64_t TelemetryKafkaTimer_ = 0;
		std::atomic_uint64_t TelemetryInterval_ = 0;
		std::atomic_uint64_t TelemetryWebSocketPackets_ = 0;
		std::atomic_uint64_t TelemetryKafkaPackets_ = 0;
		GWObjects::ConnectionState State_;
		Utils::CompressedString RawLastStats_;
		GWObjects::HealthCheck RawLastHealthcheck_;
		std::chrono::time_point<std::chrono::high_resolution_clock> ConnectionStart_ =
			std::chrono::high_resolution_clock::now();
		std::chrono::duration<double, std::milli> ConnectionCompletionTime_{0.0};
		std::atomic<bool> 	Dead_ = false;
		std::atomic_bool DeviceValidated_ = false;
		OpenWifi::GWObjects::DeviceRestrictions Restrictions_;
		bool 			RTTYMustBeSecure_ = false;
		bool hasGPS_=false;
		std::double_t 	memory_used_=0.0, cpu_load_ = 0.0, temperature_ = 0.0;
		std::uint64_t 	uuid_=0;
		bool	Simulated_=false;
		std::atomic_uint64_t 	LastContact_=0;
		bool	Synthetic_=false;
		std::string groupId_{};

		static inline std::atomic_uint64_t ConcurrentStartingDevices_ = 0;
		bool PostTelemetry(uint64_t RPCID, const std::string &serialNumber,const Poco::JSON::Object &params);
		bool StartTelemetry(uint64_t RPCID, const std::vector<std::string> &TelemetryTypes);
		bool StopTelemetry(uint64_t RPCID);
		void UpdateCounts();
		static void DeviceDisconnectionCleanup(const std::string &SerialNumber, std::uint64_t uuid);
		void SetLastStats(const std::string &LastStats);
		void Process_connect(Poco::JSON::Object::Ptr ParamsObj, const std::string &Serial);
		void Process_state(Poco::JSON::Object::Ptr ParamsObj);
		void Process_healthcheck(Poco::JSON::Object::Ptr ParamsObj);
		void Process_log(Poco::JSON::Object::Ptr ParamsObj);
		void Process_crashlog(Poco::JSON::Object::Ptr ParamsObj);
		void Process_ping(Poco::JSON::Object::Ptr ParamsObj);
		void Process_cfgpending(Poco::JSON::Object::Ptr ParamsObj);
		void Process_recovery(Poco::JSON::Object::Ptr ParamsObj);
		void Process_deviceupdate(Poco::JSON::Object::Ptr ParamsObj, std::string &Serial);
		void Process_telemetry(Poco::JSON::Object::Ptr ParamsObj);
		void Process_venuebroadcast(Poco::JSON::Object::Ptr ParamsObj);
		void Process_event(Poco::JSON::Object::Ptr ParamsObj);
		void Process_wifiscan(Poco::JSON::Object::Ptr ParamsObj);
		void Process_alarm(Poco::JSON::Object::Ptr ParamsObj);
		void Process_rebootLog(Poco::JSON::Object::Ptr ParamsObj);

		// Transport listener callbacks (decoupled I/O -> processing)
		/**
		 * @brief Report whether the owning server is still running.
		 */
		bool IsServerRunning() const;

		/**
		 * @brief Indicate whether the device has already passed handshake validation or for synthetic its true.
		 */
		bool IsDeviceValidated() const { return DeviceValidated_; }

		/**
		 * @brief Run TLS/identity validation using the live WebSocket.
		 *
		 * Extracts peer info, validates certificates, and seeds serial metadata; returns
		 * false when the connection must be closed immediately and true if synthetic(cgw-integrated device).
		 */
		bool TryValidateDevice(Poco::Net::WebSocket &ws) { return ValidatedDevice(ws); }

		/**
		 * @brief Account for bytes received from the transport.
		 */
		void RecordFrameReceived(std::size_t bytes);

		/**
		 * @brief Account for bytes sent through the transport.
		 */
		void RecordFrameSent(std::size_t bytes);

		/**
		 * @brief Increment per-connection message counts.
		 */
		void RecordMessageCount();

		/**
		 * @brief Refresh the last-contact timestamp
		 */
		void UpdateLastContact();

		/**
		 * @brief Handle a ping frame acknowledged by the transport.
		 *
		 * Emits the appropriate Kafka notification.
		 */
		void OnPing();

		/**
		 * @brief Process an inbound TEXT frame payload.
		 *
		 * Parses the JSON message, dispatches to command handlers, and updates state/metrics.
		 */
		void OnTextFrame(const std::string &payload);
		
		/**
		 * @brief React to the transport closing the underlying socket.
		 *
		 * Triggers orderly teardown by calling `EndConnection()` once.
		 */		
		void OnTransportClosed();

		inline void SetLastHealthCheck(const GWObjects::HealthCheck &H) {
			RawLastHealthcheck_ = H;
		}

	};

} // namespace OpenWifi