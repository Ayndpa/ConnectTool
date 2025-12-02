#include "steam_message_handler.h"
#include "steam_networking_manager.h"
#include "steam_vpn_bridge.h"
#include <iostream>
#include <cstring>
#include <chrono>
#include <steam_api.h>
#include <isteamnetworkingsockets.h>

SteamMessageHandler::SteamMessageHandler(ISteamNetworkingSockets* interface, 
                                         std::vector<HSteamNetConnection>& connections, 
                                         std::mutex& connectionsMutex, 
                                         SteamNetworkingManager* manager)
    : m_pInterface_(interface)
    , connections_(connections)
    , connectionsMutex_(connectionsMutex)
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
    // 注意：不要在这里调用 RunCallbacks()！
    // Steam 回调应该由主线程的 SteamAPI_RunCallbacks() 处理
    // 在这里调用会导致死锁，因为回调中可能会获取 connectionsMutex_
    
    // Receive messages
    int totalMessages = 0;
    std::vector<HSteamNetConnection> currentConnections;
    {
        std::lock_guard<std::mutex> lockConn(connectionsMutex_);
        currentConnections = connections_;
    }
    
    for (auto conn : currentConnections) {
        ISteamNetworkingMessage* pIncomingMsgs[10];
        int numMsgs = m_pInterface_->ReceiveMessagesOnConnection(conn, pIncomingMsgs, 10);
        totalMessages += numMsgs;
        
        for (int i = 0; i < numMsgs; ++i) {
            ISteamNetworkingMessage* pIncomingMsg = pIncomingMsgs[i];
            const uint8_t* data = (const uint8_t*)pIncomingMsg->m_pData;
            size_t size = pIncomingMsg->m_cbSize;
            
            std::cout << "[SteamMessageHandler] Received message of size " << size << " from connection " << conn << std::endl;

            // Check if this is a VPN message (first byte indicates message type)
            // VpnMessageType enum values range from 1 to 7
            if (size > 0 && data[0] >= 1 && data[0] <= 7) {
                // This might be a VPN message, forward to VPN bridge
                if (manager_) {
                    SteamVpnBridge* bridge = manager_->getVpnBridge();
                    if (bridge) {
                        bridge->handleVpnMessage(data, size, conn);
                    }
                }
            }
            
            pIncomingMsg->Release();
        }
    }
    
    // Adaptive polling: 有消息时缩短间隔，无消息时逐渐增加间隔
    if (totalMessages > 0) {
        currentPollInterval_ = MIN_POLL_INTERVAL;
    } else {
        currentPollInterval_ = std::min(currentPollInterval_ + POLL_INCREMENT, MAX_POLL_INTERVAL);
    }
}