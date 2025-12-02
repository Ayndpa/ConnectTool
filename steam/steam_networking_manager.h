#ifndef STEAM_NETWORKING_MANAGER_H
#define STEAM_NETWORKING_MANAGER_H

#include <vector>
#include <map>
#include <mutex>
#include <memory>
#include <steam_api.h>
#include <isteamnetworkingsockets.h>
#include <isteamnetworkingutils.h>
#include <steamnetworkingtypes.h>
#include "steam_message_handler.h"

// Forward declarations
class TCPServer;
class SteamNetworkingManager;
class SteamVpnBridge;

// User info structure
struct UserInfo {
    CSteamID steamID;
    std::string name;
    int ping;
    bool isRelay;
};

class SteamNetworkingManager {
public:
    static SteamNetworkingManager* instance;
    SteamNetworkingManager();
    ~SteamNetworkingManager();

    bool initialize();
    void shutdown();

    // P2P Connection
    bool connectToPeer(CSteamID peerID);
    void disconnect();

    // Getters
    bool isConnected() const { return g_isConnected; }
    const std::vector<HSteamNetConnection>& getConnections() const { return connections; }
    int getHostPing() const { return hostPing_; }
    int getConnectionPing(HSteamNetConnection conn) const;
    HSteamNetConnection getConnection() const { return g_hConnection; }
    HSteamNetConnection getConnectionForPeer(CSteamID peerID) const;
    std::map<CSteamID, HSteamNetConnection> getAllPeerConnections() const;
    ISteamNetworkingSockets* getInterface() const { return m_pInterface; }
    std::string getConnectionRelayInfo(HSteamNetConnection conn) const;

    // For SteamRoomManager access
    HSteamListenSocket& getListenSock() { return hListenSock; }
    ISteamNetworkingSockets* getInterface() { return m_pInterface; }

    // Message handler
    void startMessageHandler();
    void stopMessageHandler();
    SteamMessageHandler* getMessageHandler() { return messageHandler_; }

    // VPN Bridge
    void setVpnBridge(SteamVpnBridge* vpnBridge) { vpnBridge_ = vpnBridge; }
    SteamVpnBridge* getVpnBridge() { return vpnBridge_; }

private:
    // Steam API
    ISteamNetworkingSockets* m_pInterface;

    // P2P Listening
    HSteamListenSocket hListenSock;
    bool g_isConnected;
    HSteamNetConnection g_hConnection;

    // Connections
    std::vector<HSteamNetConnection> connections;
    std::map<CSteamID, HSteamNetConnection> peerConnections_;  // SteamID -> Connection mapping
    mutable std::mutex connectionsMutex;
    int hostPing_;  // Ping to host (for clients) or average ping (for host)

    // Connection config
    int g_retryCount;
    const int MAX_RETRIES = 3;
    int g_currentVirtualPort;

    // Message handler
    SteamMessageHandler* messageHandler_;

    // VPN Bridge
    SteamVpnBridge* vpnBridge_;

    // Callback - 使用 STEAM_CALLBACK 宏来确保回调正确注册
    STEAM_CALLBACK(SteamNetworkingManager, OnSteamNetConnectionStatusChanged, SteamNetConnectionStatusChangedCallback_t);
    void handleConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t *pInfo);

    friend class SteamRoomManager;
};

#endif // STEAM_NETWORKING_MANAGER_H