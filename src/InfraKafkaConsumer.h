/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#pragma once

#include "framework/SubSystemServer.h"
#include "framework/utils.h"
#include "framework/OpenWifiTypes.h"
#include "Poco/Logger.h"
#include "Poco/JSON/Object.h"
#include "Poco/JSON/Parser.h"
#include "Poco/NotificationQueue.h"
#include "Poco/Thread.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace OpenWifi {

    /**
     * @brief Consumes Kafka events form topic CnC_Res and asynchronously routes them.
     *
     * The consumer manages a small worker pool that pulls from a shared queue, allowing the
     * gateway to keep up with bursts of infra_join, infra_leave, and RPC responses without
     * blocking the Kafka delivery thread.
     */
    class InfraKafkaConsumer : public SubSystemServer {
      public:
        /**
         * @brief Provides access to the singleton consumer instance.
         */
        static auto instance() {
            static auto instance_ = new InfraKafkaConsumer;
            return instance_;
        }

        int Start() override;
        void Stop() override;

      private:
        /**
         * @brief Runnable adaptor that forwards work items to InfraKafkaConsumer::WorkerLoop.
         */
        class Worker : public Poco::Runnable {
          public:
            Worker(InfraKafkaConsumer &parent, std::size_t index) : Parent_(parent), Index_(index) {}

            void run() override { Parent_.WorkerLoop(Index_); }

          private:
            InfraKafkaConsumer &Parent_;
            std::size_t Index_;
        };

        std::string Topic_;
        std::uint64_t WatcherId_ = 0;
        Types::TopicNotifyFunction Callback_;
        Poco::NotificationQueue WorkQueue_;
        std::vector<std::unique_ptr<Poco::Thread>> WorkerThreads_;
        std::vector<std::unique_ptr<Worker>> WorkerRunnables_;
        std::mutex WorkerMutex_;
        std::atomic_bool WorkersRunning_{false};
        std::atomic_bool Accepting_{false};
        std::size_t WorkerCount_ = 0;

        InfraKafkaConsumer() noexcept : SubSystemServer("InfraKafkaConsumer", "INFRA-KAFKA", "openwifi.kafka.infra") {}

        /**
         * @brief Parses a payload envelope and schedules follow-up processing.
         */
        void HandleMessage(const std::string &Payload);
        /**
         * @brief Routes a decoded payload to the appropriate infra handler.
         */
        void ProcessMessage(const Poco::JSON::Object::Ptr &Message);
        /**
         * @brief Reacts to infra_join events by wiring up a synthetic websocket session.
         */
        void HandleJoin(const Poco::JSON::Object::Ptr &Message);
        /**
         * @brief Handles infra_leave events by shutting down any associated synthetic session.
         */
        void HandleLeave(const Poco::JSON::Object::Ptr &Message);
        /**
         * @brief Processes miscellaneous frames destined for an existing synthetic session(state).
         */
        void HandleGeneric(const Poco::JSON::Object::Ptr &Message);

        /**
         * @brief Normalizes serial strings by removing separators and lowercasing characters.
         */
        static std::string NormalizeSerial(const std::string &Serial);
        /**
         * @brief Extracts a device serial number from a parsed JSON frame.
         */
        static std::string ExtractSerialFromFrame(const Poco::Dynamic::Var &FrameVar);
        static std::string ExtractSerialFromPayload(const std::string &Payload);
        /**
         * @brief Adds a message to the worker queue for asynchronous handling.
         */
        void EnqueueMessage(const std::string &Key, const std::string &Payload);
        void StartWorkers();
        void StopWorkers();
        void WorkerLoop(std::size_t workerIndex);
    };

    inline auto InfraKafkaConsumer() { return InfraKafkaConsumer::instance(); }

} // namespace OpenWifi
