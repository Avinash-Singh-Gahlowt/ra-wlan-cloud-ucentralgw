/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#include "AP_ServerProvider.h"

#include <stdexcept>

namespace OpenWifi {
	namespace {
		std::atomic<AP_Server *> CurrentServer{nullptr};
	}

	void AP_ServerProvider::Register(AP_Server *server) { CurrentServer.store(server); }

	AP_Server *AP_ServerProvider::Get() {
		auto *server = CurrentServer.load();
		if (server == nullptr) {
			throw std::runtime_error("AP server provider not registered");
		}
		return server;
	}
} // namespace OpenWifi

