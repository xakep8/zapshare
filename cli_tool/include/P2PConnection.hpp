#pragma once

#include <atomic>
#include <functional>
#include <iostream>
#include <json/json.hpp>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "rtc/rtc.hpp"
#include "utils.hpp"

using json = nlohmann::json;

namespace P2pConnection {
static class BaseTemplate {
   protected:
    std::shared_ptr<rtc::PeerConnection> m_peerConnection;
    rtc::Configuration m_config;
    std::shared_ptr<rtc::DataChannel> m_dataChannel;
    std::vector<rtc::Candidate> m_candidates;

   public:
    BaseTemplate() = default;
    virtual ~BaseTemplate() = default;
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
        m_peerConnection->onStateChange([this](rtc::PeerConnection::State state) {
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
        m_dataChannel = m_peerConnection->createDataChannel("hello");
        m_dataChannel->onOpen([this]() {
            std::cout << "Sender data channel open; sending hello\n";
            m_dataChannel->send(std::string("hello"));
            setState(State::DCOpen);
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
        m_peerConnection->onStateChange([this](rtc::PeerConnection::State state) {
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
                    if (!description ||
                        description->typeString() != "answer") {
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
                m_dataChannel = channel;
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
                m_dataChannel->onMessage([this](rtc::message_variant message) {
                    if (auto text = std::get_if<std::string>(&message)) {
                        std::cout << "Message: " << *text << "\n";
                        if (*text == "hello") {
                            setState(State::MessageReceived);
                        } else {
                            std::cerr << "[Receiver] Unexpected message\n";
                            setState(State::Failed);
                        }
                    } else {
                        std::cerr << "[Receiver] Expected a text message\n";
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
