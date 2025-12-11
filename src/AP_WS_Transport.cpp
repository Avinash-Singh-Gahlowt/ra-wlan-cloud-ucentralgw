/*
 * SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
 * Copyright (c) 2025 Infernet Systems Pvt Ltd
 * Portions copyright (c) Telecom Infra Project (TIP), BSD-3-Clause
 */

//
// Transport layer implementation for AP WebSocket connections
// Manages WebSocket lifecycle & I/O and delegates processing via listener.

#include "AP_WS_Transport.h"

#include <Poco/Net/WebSocketImpl.h>
#include <sys/socket.h>
#include <netinet/tcp.h>

#include <framework/MicroServiceFuncs.h>
#include <framework/utils.h>
#include <AP_WS_Server.h>

#include <fmt/format.h>

namespace OpenWifi {

    AP_WS_Transport::AP_WS_Transport(Poco::Net::HTTPServerRequest &request,
                                     Poco::Net::HTTPServerResponse &response,
                                     std::shared_ptr<Poco::Net::SocketReactor> reactor,
                                     AP_WS_TransportListener &listener,
                                     Poco::Logger &logger)
        : reactor_(std::move(reactor)), listener_(listener), logger_(logger) {
        poco_trace(logger_, "AP_WS_Transport: constructing and upgrading to WebSocket");

        ws_ = std::make_unique<Poco::Net::WebSocket>(request, response);

        auto TS = Poco::Timespan(360, 0);

        ws_->setMaxPayloadSize(BufSize);
        ws_->setReceiveTimeout(TS);
        ws_->setNoDelay(false);
        ws_->setKeepAlive(true);
        ws_->setBlocking(false);
        poco_trace(logger_, "AP_WS_Transport: WebSocket configured");
    }

    AP_WS_Transport::~AP_WS_Transport() {
        Shutdown();
    }

    void AP_WS_Transport::Start() {
        poco_trace(logger_, "AP_WS_Transport::Start: registering event handlers");
        if (registered_) return;
        reactor_->addEventHandler(*ws_,
                                  Poco::NObserver<AP_WS_Transport, Poco::Net::ReadableNotification>(
                                      *this, &AP_WS_Transport::OnSocketReadable));
        reactor_->addEventHandler(*ws_,
                                  Poco::NObserver<AP_WS_Transport, Poco::Net::ShutdownNotification>(
                                      *this, &AP_WS_Transport::OnSocketShutdown));
        reactor_->addEventHandler(*ws_,
                                  Poco::NObserver<AP_WS_Transport, Poco::Net::ErrorNotification>(
                                      *this, &AP_WS_Transport::OnSocketError));
        registered_ = true;
    }

    void AP_WS_Transport::Shutdown() {
        poco_trace(logger_, "AP_WS_Transport::Shutdown: begin");
        if (!ws_) return;
        if (registered_) {
            poco_trace(logger_, "AP_WS_Transport::Shutdown: removing event handlers");
            reactor_->removeEventHandler(
                *ws_, Poco::NObserver<AP_WS_Transport, Poco::Net::ReadableNotification>(
                          *this, &AP_WS_Transport::OnSocketReadable));
            reactor_->removeEventHandler(
                *ws_, Poco::NObserver<AP_WS_Transport, Poco::Net::ShutdownNotification>(
                          *this, &AP_WS_Transport::OnSocketShutdown));
            reactor_->removeEventHandler(
                *ws_, Poco::NObserver<AP_WS_Transport, Poco::Net::ErrorNotification>(
                          *this, &AP_WS_Transport::OnSocketError));
            registered_ = false;
        }
        try {
            poco_trace(logger_, "AP_WS_Transport::Shutdown: closing socket");
            ws_->close();
        } catch (...) {
            poco_error(logger_, "Exception in closing AP_WS_Transport socket");
        }
    }

    void AP_WS_Transport::OnSocketShutdown(
        [[maybe_unused]] const Poco::AutoPtr<Poco::Net::ShutdownNotification> &pNf) {
        poco_trace(logger_, fmt::format("SOCKET-SHUTDOWN: Closing."));
        Shutdown();
        listener_.OnTransportClosed();
    }

    void AP_WS_Transport::OnSocketError(
        [[maybe_unused]] const Poco::AutoPtr<Poco::Net::ErrorNotification> &pNf) {
        poco_trace(logger_, fmt::format("SOCKET-ERROR: Closing."));
        Shutdown();
        listener_.OnTransportClosed();
    }

    void AP_WS_Transport::OnSocketReadable(
        [[maybe_unused]] const Poco::AutoPtr<Poco::Net::ReadableNotification> &pNf) {
        //we can remove this block and directly go to the try block and call ProcessIncomingFrame .... 
        poco_trace(logger_, "AP_WS_Transport::OnSocketReadable: fired");
        if (!listener_.IsServerRunning())
            return;

        // Validate device before processing frames
        if (!listener_.IsDeviceValidated()) {
            try {
                if (!listener_.TryValidateDevice(*ws_)) {
                    Shutdown();
                    listener_.OnTransportClosed();
                    return;
                }
            } catch (...) {
                Shutdown();
                listener_.OnTransportClosed();
                return;
            }
        }

        try {
            poco_trace(logger_, "AP_WS_Transport::OnSocketReadable: processing frame(s)");
            ProcessIncomingFrame();
        } catch (...) {
            Shutdown();
            listener_.OnTransportClosed();
        }
    }

    void AP_WS_Transport::ProcessIncomingFrame() {
        Poco::Buffer<char> IncomingFrame(0);

        int Op, flags{};
        auto IncomingSize = ws_->receiveFrame(IncomingFrame, flags);
        poco_trace(logger_, fmt::format("AP_WS_Transport::ProcessIncomingFrame: size={} flags={}", IncomingSize, flags));

        Op = flags & Poco::Net::WebSocket::FRAME_OP_BITMASK;

        if (IncomingSize == 0 && flags == 0 && Op == 0) {
            poco_information(logger_,
                             fmt::format("DISCONNECT: device has disconnected."));
            Shutdown();
            listener_.OnTransportClosed();
            return;
        }

        IncomingFrame.append(0);

        listener_.RecordFrameReceived((std::size_t)IncomingSize);
        listener_.RecordMessageCount();
        listener_.UpdateLastContact();

        switch (Op) {
            case Poco::Net::WebSocket::FRAME_OP_PING: {
                poco_trace(logger_, fmt::format("WS-PING: received. PONG sent back."));
                ws_->sendFrame("", 0,
                               (int)Poco::Net::WebSocket::FRAME_OP_PONG |
                                   (int)Poco::Net::WebSocket::FRAME_FLAG_FIN);
                listener_.OnPing();
            } break;
            case Poco::Net::WebSocket::FRAME_OP_PONG: {
                poco_trace(logger_, fmt::format("PONG: received and ignored."));
            } break;
            case Poco::Net::WebSocket::FRAME_OP_TEXT: {
                poco_trace(logger_, fmt::format("FRAME: Frame received (length={}, flags={}).", IncomingSize, flags));
                auto payload = std::string(IncomingFrame.begin());
                listener_.OnTextFrame(payload);
            } break;
            case Poco::Net::WebSocket::FRAME_OP_CLOSE: {
                poco_trace(logger_, fmt::format("CLOSE: Close received."));
                Shutdown();
                listener_.OnTransportClosed();
            } break;
            default: {
                poco_trace(logger_, fmt::format("FRAME: Op={} ignored.", Op));
            } break;
        }
    }

    bool AP_WS_Transport::Send(const std::string &payload) {
        try {
            poco_trace(logger_, fmt::format("AP_WS_Transport::Send: payload size={}", payload.size()));
            size_t BytesSent = ws_->sendFrame(payload.c_str(), (int)payload.size());

#if defined(__APPLE__)
            // wait for a number of ms... until our friend is done receiving
            tcp_connection_info info;
            int timeout = 2000;
            auto expireAt = std::chrono::system_clock::now() + std::chrono::milliseconds(timeout);
            do {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                socklen_t opt_len = sizeof(info);
                getsockopt(ws_->impl()->sockfd(), SOL_SOCKET, TCP_CONNECTION_INFO, (void *)&info,
                           &opt_len);
            } while (!info.tcpi_tfo_syn_data_acked && expireAt > std::chrono::system_clock::now());
            if (!info.tcpi_tfo_syn_data_acked)
                return false;
#else
            tcp_info info;
            int timeout = 4000;
            auto expireAt = std::chrono::system_clock::now() + std::chrono::milliseconds(timeout);
            do {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                socklen_t opt_len = sizeof(info);
                getsockopt(ws_->impl()->sockfd(), SOL_TCP, TCP_INFO, (void *)&info, &opt_len);
            } while (info.tcpi_unacked > 0 && expireAt > std::chrono::system_clock::now());

            if (info.tcpi_unacked > 0) {
                return false;
            }
#endif
            listener_.RecordFrameSent(BytesSent);
            return BytesSent == payload.size();
        } catch (const Poco::Exception &E) {
            listener_.Logger().log(E);
        }
        return false;
    }

} // namespace OpenWifi
