/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

//
// Helper for interacting with cgw-rest service.
//

#include "sdk_cgw.h"

namespace OpenWifi::SDK::CGW {

	std::string SerialToHyphenMac(const std::string &SerialNumber) {
		auto mac = Utils::SerialToMAC(SerialNumber);
		std::transform(mac.begin(), mac.end(), mac.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		std::replace(mac.begin(), mac.end(), ':', '-');
		return mac;
	}


	bool PostInfraCommand(
		const std::string &groupId, const std::string &serialNumber, const std::string &method,
		const Poco::JSON::Object::Ptr &payload, std::chrono::milliseconds timeout, bool oneway,
		Poco::JSON::Object::Ptr &response, Poco::Logger &logger) {

		response = nullptr;

		if (timeout.count() <= 0)
			timeout = std::chrono::milliseconds(30000);

		int timeoutSeconds = std::max<int>(
			1, static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(timeout).count()));

		Poco::JSON::Object body;
		body.set("mac_addr", SerialToHyphenMac(serialNumber));
		body.set("method", method);
		body.set("timeout", timeoutSeconds);
		body.set("params", payload);

		auto endpointPath = fmt::format("/api/v1/groups/{}/infra/command", groupId);
		std::ostringstream serializedBodyStream;
		Poco::JSON::Stringifier::stringify(body, serializedBodyStream);
		poco_information(logger,
				fmt::format("CGW REST POST {} payload={}", endpointPath, serializedBodyStream.str()));

		Poco::JSON::Object::Ptr Res;
		OpenAPIRequestPost request(
			uSERVICE_CGWREST, endpointPath, {}, body,
			static_cast<uint64_t>(timeout.count()),
			fmt::format("CGW command {} {}", serialNumber, method));
		auto status = request.Do(Res);
		poco_information(logger,
				fmt::format("CGW REST POST {} status={}", endpointPath,
						static_cast<int>(status)));
		if (status < Poco::Net::HTTPResponse::HTTP_OK ||
			status >= Poco::Net::HTTPResponse::HTTP_MULTIPLE_CHOICES) {
			poco_warning(logger, fmt::format(
				"POST command {} for {} failed. HTTP status {}", method, serialNumber,
				static_cast<int>(status)));
			return false;
		}

		if (oneway)
			return true;

		if (Res.isNull()) {
			poco_warning(logger, fmt::format(
				"POST command {} for {} returned empty payload.", method, serialNumber));
			return false;
		}

		Poco::Dynamic::Var payloadVar;
		if (Res->has("received_payload")) {
			payloadVar = Res->get("received_payload");
		}
		else {
			poco_warning(logger, fmt::format(
				"POST command {} for {} missing received_payload field.", method, serialNumber));
			return false;
		}

		try {
			if (payloadVar.type() == typeid(Poco::JSON::Object::Ptr)) {
				response = payloadVar.extract<Poco::JSON::Object::Ptr>();
			}
		} catch (const Poco::Exception &E) {
			poco_warning(logger, fmt::format(
				"POST command {} for {} returned unparsable payload: {}", method, serialNumber,
				E.displayText()));
			return false;
		} catch (const std::exception &E) {
			poco_warning(logger, fmt::format(
				"POST command {} for {} returned unparsable payload: {}", method, serialNumber,
				E.what()));
			return false;
		}
		poco_debug(logger,fmt::format("Response for POST CGW-REST /DeviceRequest is: {}", !response.isNull()));
		return !response.isNull();
	}

} // namespace OpenWifi::SDK::CGW
