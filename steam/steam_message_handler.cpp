#include "steam_message_handler.h"
#include "steam_networking_manager.h"
#include "steam_vpn_bridge.h"
#include "net/vpn_protocol.h"
#include <iostream>
#include <cstring>
#include <chrono>
#include <steam_api.h>
#include <isteamnetworkingmessages.h>

SteamMessageHandler::SteamMessageHandler(ISteamNetworkingMessages* interface, 
                                         SteamNetworkingManager* manager)
    : m_pMessagesInterface_(interface)
    , manager_(manager)
    , internalIoContext_(std::make_unique<asio::io_context>())
    , ioContext_(internalIoContext_.get())
    , running_(false)
    , currentPollInterval_(MIN_POLL_INTERVAL) {}

SteamMessageHandler::~SteamMessageHandler() {
    stop();
}

void SteamMessageHandler::setIoContext(asio::io_context* externalContext) {
    if (!running_ && externalContext) {
        ioContext_ = externalContext;
    }
}

void SteamMessageHandler::start() {
    if (running_) return;
    running_ = true;
    
    std::cout << "[SteamMessageHandler] Starting message handler..." << std::endl;
    
    // 创建定时器
    pollTimer_ = std::make_unique<asio::steady_timer>(*ioContext_);
    
    // 开始轮询
    schedulePoll();
    
    // 如果使用内部 io_context，需要在单独线程中运行
    if (ioContext_ == internalIoContext_.get()) {
        std::cout << "[SteamMessageHandler] Using internal io_context, starting thread..." << std::endl;
        ioThread_ = std::make_unique<std::thread>(&SteamMessageHandler::runInternalLoop, this);
    } else {
        std::cout << "[SteamMessageHandler] Using external io_context" << std::endl;
    }
}

void SteamMessageHandler::stop() {
    if (!running_) return;
    running_ = false;
    
    // 取消定时器
    if (pollTimer_) {
        asio::error_code ec;
        pollTimer_->cancel(ec);
    }
    
    // 停止内部 io_context
    if (ioContext_ == internalIoContext_.get() && internalIoContext_) {
        internalIoContext_->stop();
    }
    
    // 等待线程结束
    if (ioThread_ && ioThread_->joinable()) {
        ioThread_->join();
    }
    
    pollTimer_.reset();
    ioThread_.reset();
}

void SteamMessageHandler::runInternalLoop() {
    // 使用 work_guard 保持 io_context 运行
    auto workGuard = asio::make_work_guard(*internalIoContext_);
    
    while (running_) {
        try {
            internalIoContext_->run();
            break;  // run() 正常返回说明被 stop() 了
        } catch (const std::exception& e) {
            std::cerr << "Exception in message handler loop: " << e.what() << std::endl;
            if (running_) {
                internalIoContext_->restart();
            }
        }
    }
}

void SteamMessageHandler::schedulePoll() {
    if (!running_ || !pollTimer_) return;
    
    pollTimer_->expires_after(currentPollInterval_);
    pollTimer_->async_wait([this](const asio::error_code& ec) {
        if (!ec && running_) {
            pollMessages();
            schedulePoll();
        }
    });
}

void SteamMessageHandler::pollMessages() {
    if (!m_pMessagesInterface_) return;
    
    // 从 ISteamNetworkingMessages 接收消息
    ISteamNetworkingMessage* pIncomingMsgs[64];
    int numMsgs = m_pMessagesInterface_->ReceiveMessagesOnChannel(VPN_CHANNEL, pIncomingMsgs, 64);
    
    for (int i = 0; i < numMsgs; ++i) {
        ISteamNetworkingMessage* pIncomingMsg = pIncomingMsgs[i];
        const uint8_t* data = (const uint8_t*)pIncomingMsg->m_pData;
        size_t size = pIncomingMsg->m_cbSize;
        CSteamID senderSteamID = pIncomingMsg->m_identityPeer.GetSteamID();

        // Check if this is a VPN message
        if (size >= sizeof(VpnMessageHeader)) {
            VpnMessageType msgType = static_cast<VpnMessageType>(data[0]);
            
            // SESSION_HELLO 消息只是用于建立会话，不需要特殊处理
            if (msgType == VpnMessageType::SESSION_HELLO) {
                std::cout << "[SteamMessageHandler] Received SESSION_HELLO from " 
                          << senderSteamID.ConvertToUint64() << std::endl;
                pIncomingMsg->Release();
                continue;
            }
            
            // Forward other VPN messages to VPN bridge
            if (manager_) {
                SteamVpnBridge* bridge = manager_->getVpnBridge();
                if (bridge) {
                    bridge->handleVpnMessage(data, size, senderSteamID);
                }
            }
        }
        
        pIncomingMsg->Release();
    }
    
    // Adaptive polling: 有消息时缩短间隔，无消息时逐渐增加间隔
    if (numMsgs > 0) {
        currentPollInterval_ = MIN_POLL_INTERVAL;
    } else {
        currentPollInterval_ = std::min(currentPollInterval_ + POLL_INCREMENT, MAX_POLL_INTERVAL);
    }
}