/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
//
// Created by stephane bourque on 2021-12-05.
//

#pragma once

#include "AP_ServerProvider.h"
#include "RESTObjects/RESTAPI_GWobjects.h"
#include "StorageService.h"
#include <Poco/JSON/Parser.h>

namespace OpenWifi {

	inline void CompleteDeviceInfo(const GWObjects::Device &Device, Poco::JSON::Object &Answer) {
		GWObjects::ConnectionState CS;
		GetAPServer()->GetState(Device.SerialNumber, CS);
		GWObjects::HealthCheck HC;
		GetAPServer()->GetHealthcheck(Device.SerialNumber, HC);
		std::string Stats;
		GetAPServer()->GetStatistics(Device.SerialNumber, Stats);

		Poco::JSON::Object DeviceInfo;
		Device.to_json(DeviceInfo);
		Answer.set("deviceInfo", DeviceInfo);
		Poco::JSON::Object CSInfo;
		CS.to_json(Device.SerialNumber, CSInfo);
		Answer.set("connectionInfo", CSInfo);
		Poco::JSON::Object HCInfo;
		HC.to_json(HCInfo);
		Answer.set("healthCheckInfo", HCInfo);
		try {
			Poco::JSON::Parser P;
			auto StatsInfo = P.parse(Stats).extract<Poco::JSON::Object::Ptr>();
			Answer.set("statsInfo", StatsInfo);
		} catch (...) {
			Poco::JSON::Object Empty;
			Answer.set("statsInfo", Empty);
		}
	}

} // namespace OpenWifi