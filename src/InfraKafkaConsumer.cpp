/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

#include "InfraKafkaConsumer.h"

#include <fmt/format.h>
#include <sstream>
#include <algorithm>

#include "Poco/Ascii.h"
#include "Poco/AutoPtr.h"
#include "Poco/Environment.h"
#include "Poco/JSON/Array.h"
#include "Poco/Notification.h"

#include "AP_WS_Server.h"
#include "framework/KafkaManager.h"
#include "framework/MicroServiceFuncs.h"
#include "framework/ow_constants.h"
#include "framework/utils.h"

namespace OpenWifi {

    namespace {
        constexpr auto kConfigTopic = "openwifi.kafka.infra.topic";
        constexpr auto kConfigThreads = "openwifi.kafka.infra.threads";

        class InfraKafkaMessage : public Poco::Notification {
          public:
            InfraKafkaMessage(std::string key, std::string payload)
                : Key_(std::move(key)), Payload_(std::move(payload)) {}

            const std::string &Key() const { return Key_; }
            const std::string &Payload() const { return Payload_; }

          private:
            std::string Key_;
            std::string Payload_;
        };
    }

    int InfraKafkaConsumer::Start() {
        if (!KafkaManager()->Enabled()) {
            return 0;
        }

        Topic_ = MicroServiceConfigGetString(kConfigTopic, "CnC_Res");
        if (Topic_.empty()) {
            Topic_ = "CnC_Res";
        }

        Accepting_.store(true, std::memory_order_release);

        Callback_ = [this](const std::string &Key, const std::string &Payload) {
            EnqueueMessage(Key, Payload);
        };

        WatcherId_ = KafkaManager()->RegisterTopicWatcher(Topic_, Callback_);
        poco_information(Logger(), fmt::format("InfraKafkaConsumer listening on topic '{}'", Topic_));

        StartWorkers();
        return 0;
    }

    void InfraKafkaConsumer::Stop() {
        if (WatcherId_ != 0) {
            KafkaManager()->UnregisterTopicWatcher(Topic_, WatcherId_);
            WatcherId_ = 0;
        }
        StopWorkers();
    }

  void InfraKafkaConsumer::HandleMessage(const std::string& Payload) {
        Poco::JSON::Parser parser;
        Poco::Dynamic::Var var;
        try {
            var = parser.parse(Payload);
        } catch (const Poco::Exception& e) {
            poco_warning(Logger(), fmt::format("InfraKafkaConsumer: invalid JSON payload: {}", e.displayText()));
            return;
        }


        Poco::JSON::Object::Ptr Message;
        try {
            Message = var.extract<Poco::JSON::Object::Ptr>();
            ProcessMessage(Message);
        } catch (const Poco::BadCastException&) {
            poco_warning(Logger(), "InfraKafkaConsumer: payload root is not a JSON object");
            return;
        }
    }

    void InfraKafkaConsumer::EnqueueMessage(const std::string &Key, const std::string &Payload) {
        if (!Accepting_.load(std::memory_order_acquire)) {
            return;
        }
        Poco::AutoPtr<InfraKafkaMessage> Notification = new InfraKafkaMessage(Key, Payload);
        WorkQueue_.enqueueNotification(Notification);
    }

    void InfraKafkaConsumer::StartWorkers() {
        std::lock_guard<std::mutex> guard(WorkerMutex_);
        if (WorkersRunning_) {
            return;
        }

        auto configured = static_cast<std::size_t>(MicroServiceConfigGetInt(kConfigThreads, 0));
        if (configured == 0) {
            configured = static_cast<std::size_t>(Poco::Environment::processorCount()) * 4;
        }
        configured = std::clamp<std::size_t>(configured, static_cast<std::size_t>(1), static_cast<std::size_t>(256));

        WorkerCount_ = configured;
        WorkerThreads_.reserve(WorkerCount_);
        WorkerRunnables_.reserve(WorkerCount_);

        Accepting_.store(true, std::memory_order_release);
        WorkersRunning_.store(true, std::memory_order_release);

        for (std::size_t i = 0; i < WorkerCount_; ++i) {
            auto runnable = std::make_unique<Worker>(*this, i);
            auto thread = std::make_unique<Poco::Thread>();
            thread->start(*runnable);
            auto threadName = fmt::format("infra:kafka:{}", i);
            Utils::SetThreadName(*thread, threadName.c_str());
            WorkerRunnables_.emplace_back(std::move(runnable));
            WorkerThreads_.emplace_back(std::move(thread));
        }
    }

    void InfraKafkaConsumer::StopWorkers() {
        std::vector<std::unique_ptr<Poco::Thread>> threadsToJoin;
        {
            std::lock_guard<std::mutex> guard(WorkerMutex_);
            if (!WorkersRunning_) {
                return;
            }

            Accepting_.store(false, std::memory_order_release);
            WorkersRunning_.store(false, std::memory_order_release);
            WorkQueue_.wakeUpAll();

            threadsToJoin.swap(WorkerThreads_);
            WorkerRunnables_.clear();
            WorkerCount_ = 0;
        }

        for (auto &thread : threadsToJoin) {
            if (thread) {
                thread->join();
            }
        }
        WorkQueue_.clear();
    }

    void InfraKafkaConsumer::WorkerLoop(std::size_t workerIndex) {
        auto threadName = fmt::format("infra:kafka:{}", workerIndex);
        Utils::SetThreadName(threadName.c_str());

        while (WorkersRunning_.load(std::memory_order_acquire)) {
            Poco::AutoPtr<Poco::Notification> note(WorkQueue_.waitDequeueNotification());
            if (!note) {
                continue;
            }

            auto *message = dynamic_cast<InfraKafkaMessage *>(note.get());
            if (message == nullptr) {
                continue;
            }

            try {
                HandleMessage(message->Payload());
            } catch (const Poco::Exception &E) {
                Logger().log(E);
            } catch (const std::exception &E) {
                poco_warning(Logger(), fmt::format("InfraKafkaConsumer: worker exception {}", E.what()));
            } catch (...) {
                poco_warning(Logger(), "InfraKafkaConsumer: worker unknown exception");
            }
        }
    }

    void InfraKafkaConsumer::ProcessMessage(const Poco::JSON::Object::Ptr &Message) {
        if (Message.isNull()) {
            return;
        }
        if (!Message->has("type")){
            poco_debug(Logger(), "InfraKafkaConsumer: Handling unsolicited msg");
            return Message->has("method") ? (HandleGeneric(Message), void()) : void();
        }
        std::string Type;
        try {
            Type = Poco::toLower(Message->get("type").convert<std::string>());
        } catch (const Poco::Exception &E) {
            poco_warning(Logger(), fmt::format("InfraKafkaConsumer: invalid 'type' field: {}", E.displayText()));
            return;
        }

        poco_debug(Logger(), fmt::format("InfraKafkaConsumer: received type='{}'", Type));

        if (Type == "infra_join") {
            HandleJoin(Message);
            return;
        }

        if (Type == "infra_leave") {
            HandleLeave(Message);
            return;
        }

    }

    std::string InfraKafkaConsumer::NormalizeSerial(const std::string &Serial) {
        std::string Result;
        Result.reserve(Serial.size());
        for (auto c : Serial) {
            if (c == '-' || c == ':' || c == ' ')
                continue;
            Result.push_back(Poco::Ascii::toLower(c));
        }
        return Result;
    }

    std::string InfraKafkaConsumer::ExtractSerialFromFrame(const Poco::Dynamic::Var &FrameVar) {
        try {
            auto Obj = FrameVar.extract<Poco::JSON::Object::Ptr>();
            if (Obj->has(uCentralProtocol::PARAMS)) {
                auto Params = Obj->getObject(uCentralProtocol::PARAMS);
                if (Params->has(uCentralProtocol::SERIAL)) {
                    return NormalizeSerial(Params->get(uCentralProtocol::SERIAL).toString());
                }
            }
            if (Obj->has(uCentralProtocol::SERIAL)) {
                return NormalizeSerial(Obj->get(uCentralProtocol::SERIAL).toString());     
            }
            if (Obj->has(uCentralProtocol::RESULT)) {
                auto ResultObj = Obj->get(uCentralProtocol::RESULT).extract<Poco::JSON::Object::Ptr>();
                if (!ResultObj.isNull() && ResultObj->has(uCentralProtocol::SERIAL)) {
                    return NormalizeSerial(ResultObj->get(uCentralProtocol::SERIAL).toString());
                }
            }
        } catch (const std::exception &e) {
            poco_error(Poco::Logger::get("INFRA-KAFKA"), fmt::format("Exception parsing payload for generic message to extract serial: {}", e.what()));
        }
        return {};
    }

    std::string InfraKafkaConsumer::ExtractSerialFromPayload(const std::string &Payload) {
        Poco::JSON::Parser Parser;
        Poco::Dynamic::Var Var;
        try {
            Var = Parser.parse(Payload);
        } catch (const Poco::Exception &E) {
            poco_warning(Poco::Logger::get("INFRA-KAFKA"), fmt::format("Exception in InfraKafkaConsumer: invalid payload {}", E.displayText()));
            return {};
        }
        return ExtractSerialFromFrame(Var);
    }

    void InfraKafkaConsumer::HandleJoin(const Poco::JSON::Object::Ptr &Message) {
        if (!Message->has("connect_message_payload")) {
            poco_warning(Logger(), "InfraKafkaConsumer: infra_join missing connect_message_payload");
            return;
        }
        const auto ConnectPayload = Message->get("connect_message_payload").toString();

        Poco::JSON::Parser Parser;
        Poco::Dynamic::Var Var;
        try {
            Var = Parser.parse(ConnectPayload);
        } catch (const Poco::Exception &E) {
            poco_warning(Logger(), fmt::format("InfraKafkaConsumer: invalid connect payload {}", E.displayText()));
            return;
        }

        auto Serial = ExtractSerialFromFrame(Var);
        if (Serial.empty()) {
            poco_warning(Logger(), "InfraKafkaConsumer: connect payload missing serial");
            return;
        }
        auto SerialNumber = Utils::SerialNumberToInt(Serial);
        auto Peer = Message->has("infra_public_ip") ? Message->get("infra_public_ip").toString() : std::string{};
        std::string GroupId{};
        if (Message->has("infra_group_id")) {
            try {
                GroupId = Message->get("infra_group_id").convert<std::string>();
            } catch (const Poco::Exception &E) {
                poco_warning(Logger(), fmt::format("InfraKafkaConsumer: unable to parse infra_group_id: {}", E.displayText()));
            }
        }
        bool Simulated = false;
        if (Message->has("isSim")) {
            try {
                Simulated = Message->get("isSim").convert<int>() != 0;
            } catch (...) {
                poco_warning(Logger(), "InfraKafkaConsumer: unable to parse isSim flag");
            }
        }

        poco_information(Logger(), fmt::format("InfraKafkaConsumer: infra_join serial={} peer={} simulated={}", Serial, Peer, Simulated));
        auto Connection = AP_WS_Server()->EnsureSyntheticConnection(SerialNumber, Peer, Simulated);
        if (!GroupId.empty()) {
            Connection->SetGroupId(GroupId);
        }
        Connection->InboundFromBroker(ConnectPayload);
    }

    void InfraKafkaConsumer::HandleLeave(const Poco::JSON::Object::Ptr &Message) {
        std::string SerialCandidate;
        if (Message->has("infra_group_infra"))
            SerialCandidate = Message->get("infra_group_infra").toString();

        if (SerialCandidate.empty()) {
            poco_warning(Logger(), "InfraKafkaConsumer: infra_leave missing serial");
            return;
        }
        auto Serial = NormalizeSerial(SerialCandidate);
        auto SerialNumber = Utils::SerialNumberToInt(Serial);
        auto Connection = AP_WS_Server()->FindConnection(SerialNumber);
        if (!Connection) {
            poco_information(Logger(), fmt::format("InfraKafkaConsumer: infra_leave for offline device {}", Serial));
            return;
        }
        poco_information(Logger(), fmt::format("InfraKafkaConsumer: infra_leave serial={}", Serial));
        Connection->EndConnection();
    }

    void InfraKafkaConsumer::HandleGeneric(const Poco::JSON::Object::Ptr &Message) {
        std::string FramePayload;
        if (Message->has("jsonrpc")) {
            std::ostringstream OS;
            Poco::JSON::Stringifier::stringify(Message, OS);
            FramePayload = OS.str();
            poco_trace(Logger(), fmt::format("Handle generic message that has jsonrpc {}",FramePayload));
        } else {
            return;
        }

        auto Serial = ExtractSerialFromPayload(FramePayload);
        if (Serial.empty()) {
            poco_warning(Logger(), "InfraKafkaConsumer: unable to determine serial for message");
            return;
        }
        auto SerialNumber = Utils::SerialNumberToInt(Serial);
        if (!AP_WS_Server()->RouteFrameToConnection(SerialNumber, FramePayload)) {
            poco_warning(Logger(), fmt::format("InfraKafkaConsumer: dropping message for offline device {}", Serial));
        } else {
            poco_trace(Logger(), fmt::format("InfraKafkaConsumer: routed payload to device {}", Serial));
        }
    }

} // namespace OpenWifi
