#ifndef STEAM_MESSAGE_HANDLER_H
#define STEAM_MESSAGE_HANDLER_H

#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <memory>
#include <boost/asio.hpp>
#include <steamnetworkingtypes.h>
#include "../net/tcp_server.h"
#include "../net/multiplex_manager.h"

class SteamMessageHandler {
public:
    SteamMessageHandler(boost::asio::io_context& io_context, ISteamNetworkingSockets* interface, std::vector<HSteamNetConnection>& connections, std::mutex& connectionsMutex, bool& g_isHost, int& localPort);
    ~SteamMessageHandler();

    void start();
    void stop();

    std::shared_ptr<MultiplexManager> getMultiplexManager(HSteamNetConnection conn);

private:
    void run();
    void pollMessages();

    boost::asio::io_context& io_context_;
    ISteamNetworkingSockets* m_pInterface_;
    std::vector<HSteamNetConnection>& connections_;
    std::mutex& connectionsMutex_;
    bool& g_isHost_;
    int& localPort_;

    std::map<HSteamNetConnection, std::shared_ptr<MultiplexManager>> multiplexManagers_;

    std::thread thread_;
    std::thread io_thread_;
    bool running_;
};

#endif // STEAM_MESSAGE_HANDLER_H