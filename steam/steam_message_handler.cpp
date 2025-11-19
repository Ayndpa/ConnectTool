#include "steam_message_handler.h"
#include <iostream>
#include <cstring>
#include <chrono>
#include <thread>
#include <steam_api.h>
#include <isteamnetworkingsockets.h>

SteamMessageHandler::SteamMessageHandler(boost::asio::io_context& io_context, ISteamNetworkingSockets* interface, std::vector<HSteamNetConnection>& connections, std::mutex& connectionsMutex, bool& g_isHost, int& localPort)
    : io_context_(io_context), m_pInterface_(interface), connections_(connections), connectionsMutex_(connectionsMutex), g_isHost_(g_isHost), localPort_(localPort), running_(false) {}

SteamMessageHandler::~SteamMessageHandler() {
    stop();
}

void SteamMessageHandler::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread([this]() { run(); });
    // Start io_context in a separate thread
    io_thread_ = std::thread([this]() {
        auto work = boost::asio::make_work_guard(io_context_);
        io_context_.run();
    });
}

void SteamMessageHandler::stop() {
    if (!running_) return;
    running_ = false;
    io_context_.stop();
    if (thread_.joinable()) {
        thread_.join();
    }
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
}

std::shared_ptr<MultiplexManager> SteamMessageHandler::getMultiplexManager(HSteamNetConnection conn) {
    if (multiplexManagers_.find(conn) == multiplexManagers_.end()) {
        multiplexManagers_[conn] = std::make_shared<MultiplexManager>(m_pInterface_, conn, io_context_, g_isHost_, localPort_);
    }
    return multiplexManagers_[conn];
}

void SteamMessageHandler::run() {
    while (running_) {
        // Poll networking
        m_pInterface_->RunCallbacks();

        // Receive messages
        pollMessages();

        // Sleep a bit to avoid busy loop
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void SteamMessageHandler::pollMessages() {
    std::vector<HSteamNetConnection> currentConnections;
    {
        std::lock_guard<std::mutex> lockConn(connectionsMutex_);
        currentConnections = connections_;
    }
    for (auto conn : currentConnections) {
        ISteamNetworkingMessage* pIncomingMsgs[10];
        int numMsgs = m_pInterface_->ReceiveMessagesOnConnection(conn, pIncomingMsgs, 10);
        for (int i = 0; i < numMsgs; ++i) {
            ISteamNetworkingMessage* pIncomingMsg = pIncomingMsgs[i];
            const char* data = (const char*)pIncomingMsg->m_pData;
            size_t size = pIncomingMsg->m_cbSize;
            // Handle tunnel packets with multiplexing
            if (multiplexManagers_.find(conn) == multiplexManagers_.end()) {
                multiplexManagers_[conn] = std::make_shared<MultiplexManager>(m_pInterface_, conn, io_context_, g_isHost_, localPort_);
            }
            multiplexManagers_[conn]->handleTunnelPacket(data, size);
            pIncomingMsg->Release();
        }
    }
}