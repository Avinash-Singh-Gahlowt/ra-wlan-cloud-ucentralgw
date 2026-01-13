/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
//
// Created by stephane bourque on 2022-07-26.
//

#include "AP_Connection.h"
#include "fmt/format.h"
#include "framework/ow_constants.h"

namespace OpenWifi {
	void AP_Connection::Process_ping(Poco::JSON::Object::Ptr ParamsObj) {
		if (ParamsObj->has(uCentralProtocol::UUID)) {
			[[maybe_unused]] uint64_t UUID = ParamsObj->get(uCentralProtocol::UUID);
			poco_trace(Logger_, fmt::format("PING({}): Current config is {}", CId_, UUID));
		} else {
			poco_warning(Logger_, fmt::format("PING({}): Missing parameter.", CId_));
		}
	}
} // namespace OpenWifi
