/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */
#pragma once

#include <atomic>

#include "AP_SERVER.h"

namespace OpenWifi {

	class AP_ServerProvider {
	  public:
		static void Register(AP_Server *server);
		[[nodiscard]] static AP_Server *Get();

	  private:
		AP_ServerProvider() = default;
	};

	inline AP_Server *GetAPServer() { return AP_ServerProvider::Get(); }

} // namespace OpenWifi

