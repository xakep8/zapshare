#pragma once

#include <atomic>
#include <functional>
#include <iostream>
#include <json/json.hpp>
#include <memory>
#include <rtc/common.hpp>
#include <rtc/datachannel.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "rtc/rtc.hpp"
#include "utils.hpp"

using json = nlohmann::json;

namespace P2pConnection {
class BaseTemplate {
   public:
    using BytesHandler = std::function<void(std::string)>;
    void onBytes(BytesHandler handler) { m_onBytes = std::move(handler); }

   protected:
    void attachDataChannel(std::shared_ptr<rtc::DataChannel> dc) {
        m_dataChannel = std::move(dc);
        m_dataChannel->onMessage([this](rtc::message_variant message) {
            const auto* binary = std::get_if<rtc::binary>(&message);
            if (!binary) {
                std::cerr << "Expected binary message\n";
                return;
            }
            if (m_onBytes) {
                const auto* data =
                    reinterpret_cast<const char*>(binary->data());
                m_onBytes(std::string(data, binary->size()));
            }
        });
    };

   public:
    void sendBytes(std::string_view bytes) {
        if (!m_dataChannel || !m_dataChannel->isOpen()) {
            throw std::runtime_error(
                "[Send Bytes] DataChannel is not available\n");
        }

        const auto* begin = reinterpret_cast<const std::byte*>(bytes.data());
        rtc::binary payload(begin, begin + bytes.size());
        m_dataChannel->send(std::move(payload));
    };

   protected:
    std::shared_ptr<rtc::PeerConnection> m_peerConnection;
    rtc::Configuration m_config;
    std::shared_ptr<rtc::DataChannel> m_dataChannel;
    std::vector<rtc::Candidate> m_candidates;

   public:
    BaseTemplate() = default;
    virtual ~BaseTemplate() = default;

   private:
    BytesHandler m_onBytes;
};

class Sender : public BaseTemplate {
   public:
    enum class State {
        PreparingOffer,
        OfferSent,
        AnswerReceived,
        DCOpen,
        Failed
    };

   private:
    std::function<void()> m_onOfferSent;
    std::atomic<State> m_state{State::PreparingOffer};

   public:
    Sender() = default;
    ~Sender() = default;
    void setState(State state) {
        m_state.store(state, std::memory_order_release);
        m_state.notify_all();
    }
    void onOfferSent(std::function<void()> callback) {
        m_onOfferSent = std::move(callback);
    };
    void start(const std::string& secret) {
        setState(State::PreparingOffer);
        m_config.iceServers.emplace_back("stun.l.google.com", 19302);
        m_peerConnection = std::make_shared<rtc::PeerConnection>(m_config);
        m_peerConnection->onStateChange(
            [this](rtc::PeerConnection::State state) {
                std::cout << "State: " << state << "\n";
                if (state == rtc::PeerConnection::State::Failed) {
                    setState(State::Failed);
                }
            });
        m_peerConnection->onGatheringStateChange(
            [this, secret](rtc::PeerConnection::GatheringState state) {
                std::cout << "GatheringState: " << state << "\n";
                if (state == rtc::PeerConnection::GatheringState::Complete) {
                    auto description = m_peerConnection->localDescription();
                    if (!description) return;
                    json message = {{"type", description->typeString()},
                                    {"sdp", std::string(description.value())}};
                    std::cout << message << "\n";
                    if (Utils::post_offer(message, secret)) {
                        setState(State::OfferSent);
                        if (m_onOfferSent) {
                            m_onOfferSent();
                        }
                    } else {
                        std::cerr << "[Sender] Offer Send Failed\n";
                        setState(State::Failed);
                    }
                }
            });
        m_peerConnection->onLocalCandidate([this](rtc::Candidate candidate) {
            std::cout << "Candidate Found: " << candidate << "\n";
            m_candidates.push_back(candidate);
        });
        attachDataChannel(m_peerConnection->createDataChannel("zapshare"));
        m_dataChannel->onOpen([this]() {
            std::cout << "Sender data channel open; sending hello\n";
            try {
                setState(State::DCOpen);
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << "\n";
                setState(State::Failed);
            }
        });
        m_peerConnection->setLocalDescription(rtc::Description::Type::Offer);
    };
    void applyAnswer(const json& message) {
        try {
            if (!m_peerConnection) {
                setState(State::Failed);
                return;
            }

            std::string type = message.at("type").get<std::string>();
            std::string sdp = message.at("sdp").get<std::string>();

            if (type != "answer" || sdp.empty()) {
                setState(State::Failed);
                return;
            }

            rtc::Description answer(sdp, type);
            m_peerConnection->setRemoteDescription(answer);
            setState(State::AnswerReceived);
        } catch (const std::exception& error) {
            std::cerr << "[Sender] Failed to apply answer: " << error.what()
                      << "\n";
            setState(State::Failed);
        }
    };
    State getState() const { return m_state.load(std::memory_order_acquire); };
    bool waitUntil(State desired) const {
        State current = getState();
        while (current != desired) {
            if (current == State::Failed) return false;
            m_state.wait(current, std::memory_order_acquire);
            current = getState();
        }
        return true;
    }
};

class Receiver : public BaseTemplate {
   public:
    enum class State {
        WaitingOffer,
        OfferReceived,
        PreparingAnswer,
        AnswerSent,
        DCOpen,
        MessageReceived,
        Failed
    };

   private:
    std::atomic<State> m_state{State::WaitingOffer};

   public:
    Receiver() = default;
    ~Receiver() = default;
    void start(const std::string& secret) {
        setState(State::WaitingOffer);
        m_config.iceServers.emplace_back("stun.l.google.com", 19302);
        m_peerConnection = std::make_shared<rtc::PeerConnection>(m_config);
        m_peerConnection->onStateChange(
            [this](rtc::PeerConnection::State state) {
                std::cout << "State: " << state << "\n";
                if (state == rtc::PeerConnection::State::Failed) {
                    setState(State::Failed);
                }
            });
        m_peerConnection->onGatheringStateChange(
            [this, secret](rtc::PeerConnection::GatheringState state) {
                std::cout << "GatheringState: " << state << "\n";
                if (state == rtc::PeerConnection::GatheringState::Complete) {
                    auto description = m_peerConnection->localDescription();
                    if (!description || description->typeString() != "answer") {
                        std::cerr
                            << "[Receiver] Local answer was not generated\n";
                        setState(State::Failed);
                        return;
                    }
                    json message = {{"type", description->typeString()},
                                    {"sdp", std::string(description.value())}};
                    std::cout << message << "\n";
                    if (Utils::post_answer(message, secret)) {
                        setState(State::AnswerSent);
                    } else {
                        std::cerr << "[Receiver] Answer Send Failed\n";
                        setState(State::Failed);
                    }
                }
            });
        m_peerConnection->onLocalCandidate([this](rtc::Candidate candidate) {
            std::cout << "Candidate Found: " << candidate << "\n";
            m_candidates.push_back(candidate);
        });
        m_peerConnection->onDataChannel(
            [this](std::shared_ptr<rtc::DataChannel> channel) {
                attachDataChannel(std::move(channel));
                m_dataChannel->onOpen([this]() {
                    std::cout << "[RECEIVER] DataChannel Open!\n";
                    setState(State::DCOpen);
                });
                m_dataChannel->onError([this](const std::string& error) {
                    std::cerr << "[Receiver] DataChannel error: " << error
                              << "\n";
                    setState(State::Failed);
                });
                m_dataChannel->onClosed([this]() {
                    if (getState() != State::MessageReceived) {
                        std::cerr
                            << "[Receiver] DataChannel closed before hello\n";
                        setState(State::Failed);
                    }
                });
            });
    };
    void acceptOffer(const json& message) {
        try {
            if (!m_peerConnection) {
                setState(State::Failed);
                return;
            }
            std::string type = message.at("type").get<std::string>();
            std::string sdp = message.at("sdp").get<std::string>();

            if (type != "offer" || sdp.empty()) {
                setState(State::Failed);
                return;
            }
            setState(State::OfferReceived);
            rtc::Description offer(sdp, type);
            setState(State::PreparingAnswer);
            m_peerConnection->setRemoteDescription(offer);
        } catch (const std::exception& error) {
            std::cerr << "[Receiver] Failed to apply offer: " << error.what()
                      << "\n";
            setState(State::Failed);
        }
    };
    State getState() const { return m_state.load(std::memory_order_acquire); };
    void setState(State state) {
        m_state.store(state, std::memory_order_release);
        m_state.notify_all();
    }
    bool waitUntil(State desired) const {
        State current = getState();
        while (current != desired) {
            if (current == State::Failed) return false;
            m_state.wait(current, std::memory_order_acquire);
            current = getState();
        }
        return true;
    }
    State waitForStateChange(State observed) const {
        m_state.wait(observed, std::memory_order_acquire);
        return getState();
    }
};
}  // namespace P2pConnection
