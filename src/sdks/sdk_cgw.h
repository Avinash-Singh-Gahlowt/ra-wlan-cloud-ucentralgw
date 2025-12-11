/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

//
// Helper for interacting with cgw-rest service.
//

#pragma once

#include <algorithm>
#include <chrono>
#include <cctype>
#include <string>

#include "Poco/Dynamic/Var.h"
#include "Poco/JSON/Object.h"
#include "Poco/JSON/Parser.h"
#include "Poco/JSON/Stringifier.h"
#include "Poco/Logger.h"
#include "Poco/Net/HTTPResponse.h"

#include "fmt/format.h"

#include "framework/MicroServiceNames.h"
#include "framework/OpenAPIRequests.h"
#include "framework/utils.h"

namespace OpenWifi::SDK::CGW {

	std::string SerialToHyphenMac(const std::string &SerialNumber);

	/**
	 * @brief Send a command to the CGW REST service and optionally return the device reply.
	 *
	 * Builds a `POST /api/v1/groups/{groupId}/infra/command` request, honours the provided timeout,
	 * and parses the response when the call is not marked as oneway.
	 *
	 * @param groupId Target CGW group identifier.
	 * @param serialNumber Device serial (converted to CGW MAC format for the REST body).
	 * @param method JSON-RPC method name to execute on the device.
	 * @param payload Prepared JSON-RPC payload to send through CGW.
	 * @param timeout Maximum time to wait for the REST request and optional response payload.
	 * @param oneway When true, do not expect a synchronous payload back from CGW.
	 * @param response Populated with the parsed device response when `oneway == false`.
	 * @param logger Logger used for informational and warning messages.
	 * @return true if the REST request succeeded and (for non-oneway) a payload was extracted.
	 */
	bool PostInfraCommand(
		const std::string &groupId, const std::string &serialNumber, const std::string &method,
		const Poco::JSON::Object::Ptr &payload, std::chrono::milliseconds timeout, bool oneway,
		Poco::JSON::Object::Ptr &response, Poco::Logger &logger);

} // namespace OpenWifi::SDK::CGW
