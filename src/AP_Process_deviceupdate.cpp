/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
//
// Created by stephane bourque on 2022-07-26.
//

#include "AP_Connection.h"
#include "StorageService.h"

#include "fmt/format.h"

namespace OpenWifi {

	void AP_Connection::Process_deviceupdate(Poco::JSON::Object::Ptr ParamsObj,
												std::string &Serial) {
		if (!State_.Connected) {
			poco_warning(Logger_,
						 fmt::format("INVALID-PROTOCOL({}): Device '{}' is not following protocol",
									 CId_, CN_));
			Errors_++;
			return;
		}
		if (ParamsObj->has("currentPassword")) {
			auto Password = ParamsObj->get("currentPassword").toString();

			StorageService()->SetDevicePassword(*DbSession_,Serial, Password);
			poco_trace(
				Logger_,
				fmt::format("DEVICE-UPDATE({}): Device is updating its login password.", Serial));
		}
	}

} // namespace OpenWifi
