/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
#include "AP_KAFKA_Connection.h"

#include <cctype>
#include <sstream>

#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Net/IPAddress.h>
#include <Poco/String.h>

#include <fmt/format.h>

#include "AP_ServerProvider.h"
#include "StorageService.h"
#include "framework/KafkaManager.h"
#include "framework/KafkaTopics.h"
#include "framework/MicroServiceFuncs.h"
#include "framework/ow_constants.h"
#include "framework/utils.h"

namespace OpenWifi {

	AP_KAFKA_Connection::AP_KAFKA_Connection(Poco::Logger &L,
											 std::shared_ptr<LockedDbSession> session,
											 uint64_t connection_id)
		: AP_Connection(L, session, connection_id) {
		uuid_ = MicroServiceRandom(std::numeric_limits<std::uint64_t>::max() - 1);
		GetAPServer()->IncrementConnectionCount();
	}

	AP_KAFKA_Connection::~AP_KAFKA_Connection() {
		std::lock_guard G(ConnectionMutex_);
		GetAPServer()->DecrementConnectionCount();
		EndConnection();
		poco_debug(Logger_, fmt::format("TERMINATION({}): Session={}, Connection removed.",
										SerialNumber_, State_.sessionId));
	}

	void AP_KAFKA_Connection::Start() {
		LastContact_ = Utils::Now();
		State_.started = LastContact_;
	}

	bool AP_KAFKA_Connection::ValidatedDevice() {
		if (Dead_) {
			return false;
		}
		DeviceValidated_ = true;
		State_.VerifiedCertificate = GWObjects::VALID_CERTIFICATE;
		return true;
	}

	void AP_KAFKA_Connection::ProcessIncomingFrame() {
		if (Dead_) {
			return;
		}

		std::string payload;
		{
			std::lock_guard G(ConnectionMutex_);
			if (PendingPayload_.empty()) {
				return;
			}
			State_.LastContact = LastContact_ = Utils::Now();
			State_.RX += PendingPayload_.size();
			GetAPServer()->AddRX(PendingPayload_.size());
			State_.MessageCount++;
			if (!(DeviceValidated_ || ValidatedDevice())) {
				return;
			}
			payload = PendingPayload_;
			PendingPayload_.clear();
		}

		bool KillConnection = false;

		try {
			Poco::JSON::Parser parser;
			auto parsed = parser.parse(payload);
			auto obj = parsed.extract<Poco::JSON::Object::Ptr>();

			if (obj->has(uCentralProtocol::JSONRPC)) {
				if (obj->has(uCentralProtocol::METHOD) && obj->has(uCentralProtocol::PARAMS)) {
					ProcessJSONRPCEvent(obj);
				} else if (obj->has(uCentralProtocol::RESULT) && obj->has(uCentralProtocol::ID)) {
					ProcessJSONRPCResult(obj);
				} else {
					poco_warning(Logger_,
								 fmt::format("INVALID-PAYLOAD({}): Payload is not JSON-RPC 2.0: {}",
											 CId_, payload));
				}
			} else if (obj->has(uCentralProtocol::RADIUS)) {
				ProcessIncomingRadiusData(obj);
			} else {
				std::ostringstream os;
				obj->stringify(os);
				poco_warning(
					Logger_,
					fmt::format("FRAME({}): illegal transaction header, missing 'jsonrpc': {}", CId_,
								os.str()));
			}

		} catch (const Poco::Exception &E) {
			Logger_.log(E);
			KillConnection=true;
		} catch (const std::exception &E) {
			poco_warning(Logger_,
						 fmt::format("std::exception({}): {} Payload:{} Session:{}", CId_, E.what(),
									 payload, State_.sessionId));
			KillConnection=true;
		} catch (...) {
			poco_error(Logger_,
					   fmt::format("UnknownException({}): Payload:{} Session:{}", CId_, payload,
								   State_.sessionId));
			KillConnection=true;
		}

		if (KillConnection) {
			poco_warning(Logger_, fmt::format("DISCONNECTING({}): Errors: {}", CId_, KillConnection));
			EndConnection();
		}
	}

	void AP_KAFKA_Connection::setEssentials(const std::string &IP, const std::string &InfraSerial,
											uint64_t InfraGroupId) {

		CN_ = SerialNumber_ = InfraSerial;
		CId_= Address_ = IP;
		SerialNumberInt_ = Utils::SerialNumberToInt(SerialNumber_);
		InfraGroupId_ = InfraGroupId;
		
	}

	bool AP_KAFKA_Connection::Send(const std::string &Payload) {
		if (!KafkaManager()->Enabled()) {
			return false;
		}

		if (InfraGroupId_ == 0) {
			poco_warning(Logger_, fmt::format("Kafka send skipped: infra_group_id missing for {}",
											  SerialNumber_));
			return false;
		}

		Poco::JSON::Object::Ptr msgObject;
		try {
			Poco::JSON::Parser parser;
			msgObject = parser.parse(Payload).extract<Poco::JSON::Object::Ptr>();
		} catch (...) {
			poco_warning(Logger_, fmt::format("Parsing error payload while sending on kafka {}",  SerialNumber_));
			return false;
		}

		auto commandTimeout = [](const std::string &method) -> uint64_t {
			if (method == "configure") return 120;
			if (method == "upgrade") return 30;
			if (method == "reboot") return 30;
			if (method == "factory") return 30;
			if (method == "leds") return 120;
			if (method == "trace") return 300;
			if (method == "request") return 120;
			if (method == "wifiscan") return 120;
			if (method == "eventqueue") return 30;
			if (method == "telemetry") return 30;
			if (method == "ping") return 60;
			if (method == "rrm") return 60;
			if (method == "certupdate") return 60;
			if (method == "transfer") return 60;
			if (method == "script") return 60;
			if (method == "powercycle") return 60;
			if (method == "fixedconfig") return 120;
			if (method == "cablediagnostics") return 120;
			if (method == "reenroll") return 120;
			return 120;
		};

		uint64_t timeoutSeconds = 120;
		if (msgObject && msgObject->has(uCentralProtocol::METHOD)) {
			timeoutSeconds = commandTimeout(msgObject->get(uCentralProtocol::METHOD).toString());
		}

		Poco::JSON::Object kafkaPayload;
		kafkaPayload.set("type", "infrastructure_group_infra_message_enqueue");
		kafkaPayload.set("infra_group_id", std::to_string(InfraGroupId_));
		kafkaPayload.set("infra_group_infra", SerialNumber_);
		kafkaPayload.set("msg", msgObject);
		kafkaPayload.set("uuid", MicroServiceCreateUUID());
		kafkaPayload.set("timeout", timeoutSeconds);

		KafkaManager()->PostMessage(KafkaTopics::CNC, std::to_string(InfraGroupId_), kafkaPayload,false);
		State_.TX += Payload.size();
		GetAPServer()->AddTX(Payload.size());
		return true;
	}

	void AP_KAFKA_Connection::EndConnection() {
		bool expectedValue = false;
		if (!Dead_.compare_exchange_strong(expectedValue, true, std::memory_order_release,
										   std::memory_order_relaxed)) {
			return;
		}

		if (!SerialNumber_.empty() && State_.LastContact != 0) {
			StorageService()->SetDeviceLastRecordedContact(SerialNumber_, State_.LastContact);
		}

		if (!SerialNumber_.empty()) {
			DeviceDisconnectionCleanup(SerialNumber_, uuid_);
		}
		GetAPServer()->AddCleanupSession(State_.sessionId, SerialNumberInt_);
	}

} // namespace OpenWifi
