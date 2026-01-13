/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
//
// Created by stephane bourque on 2022-07-26.
//

#include "AP_Connection.h"
#include "VenueBroadcaster.h"

namespace OpenWifi {
	void AP_Connection::Process_venuebroadcast(Poco::JSON::Object::Ptr ParamsObj) {
		if (ParamsObj->has("data") && ParamsObj->has("serial") && ParamsObj->has("timestamp")) {
			VenueBroadcaster()->Broadcast(ParamsObj->get("serial").toString(),
										  ParamsObj->getObject("data"),
										  ParamsObj->get("timestamp"));
		}
	}
} // namespace OpenWifi
