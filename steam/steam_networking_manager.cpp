#include "steam_networking_manager.h"
#include "steam_vpn_bridge.h"
#include <iostream>
#include <algorithm>

SteamNetworkingManager *SteamNetworkingManager::instance = nullptr;

// STEAM_CALLBACK 回调函数 - 当连接状态改变时由 SteamAPI_RunCallbacks() 调用
void SteamNetworkingManager::OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t *pCallback)
{
    handleConnectionStatusChanged(pCallback);
}

SteamNetworkingManager::SteamNetworkingManager()
    : m_pInterface(nullptr), hListenSock(k_HSteamListenSocket_Invalid), g_isConnected(false),
      g_hConnection(k_HSteamNetConnection_Invalid),
      messageHandler_(nullptr), vpnBridge_(nullptr), hostPing_(0)
{
}

SteamNetworkingManager::~SteamNetworkingManager()
{
    stopMessageHandler();
    delete messageHandler_;
    shutdown();
}

bool SteamNetworkingManager::initialize()
{
    instance = this;
    
    // Steam API should already be initialized before calling this
    if (!SteamAPI_IsSteamRunning())
    {
        std::cerr << "Steam is not running" << std::endl;
        return false;
    }

    // 仅输出错误级别日志
    SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Error,
                                                   [](ESteamNetworkingSocketsDebugOutputType nType, const char *pszMsg)
                                                   {
                                                       std::cerr << "[SteamNet Error] " << pszMsg << std::endl;
                                                   });

    int32 logLevel = k_ESteamNetworkingSocketsDebugOutputType_Verbose;
    SteamNetworkingUtils()->SetConfigValue(
        k_ESteamNetworkingConfig_LogLevel_P2PRendezvous,
        k_ESteamNetworkingConfig_Global,
        0,
        k_ESteamNetworkingConfig_Int32,
        &logLevel);

    // 允许 P2P (ICE) 直连
    // 默认情况下 Steam 可能会保守地只允许 LAN，这里设置为 "All" 允许公网 P2P
    int32 nIceEnable = k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Public;
    SteamNetworkingUtils()->SetConfigValue(
        k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable,
        k_ESteamNetworkingConfig_Global, // <--- 关键：作用域选 Global
        0,                               // Global 时此参数填 0
        k_ESteamNetworkingConfig_Int32,
        &nIceEnable);

    // Allow connections from IPs without authentication
    int32 allowWithoutAuth = 2;
    SteamNetworkingUtils()->SetConfigValue(
        k_ESteamNetworkingConfig_IP_AllowWithoutAuth,
        k_ESteamNetworkingConfig_Global,
        0,
        k_ESteamNetworkingConfig_Int32,
        &allowWithoutAuth);

    // Create callbacks after Steam API init
    SteamNetworkingUtils()->InitRelayNetworkAccess();
    // 注意：不再使用 SetGlobalCallback，而是使用 STEAM_CALLBACK 宏
    // STEAM_CALLBACK 会自动在 SteamAPI_RunCallbacks() 时调度回调
    std::cout << "[SteamNetworkingManager] Using STEAM_CALLBACK for connection status changes" << std::endl;

    m_pInterface = SteamNetworkingSockets();

    // Initialize message handler
    messageHandler_ = new SteamMessageHandler(m_pInterface, connections, connectionsMutex, this);

    // Check if callbacks are registered
    std::cout << "Steam Networking Manager initialized successfully" << std::endl;

    return true;
}

void SteamNetworkingManager::shutdown()
{
    if (g_hConnection != k_HSteamNetConnection_Invalid)
    {
        m_pInterface->CloseConnection(g_hConnection, 0, nullptr, false);
    }
    if (hListenSock != k_HSteamListenSocket_Invalid)
    {
        m_pInterface->CloseListenSocket(hListenSock);
    }
    SteamAPI_Shutdown();
}

bool SteamNetworkingManager::connectToPeer(CSteamID peerID)
{
    // Check if already connected to this peer
    {
        std::lock_guard<std::mutex> lock(connectionsMutex);
        if (peerConnections_.find(peerID) != peerConnections_.end())
        {
            std::cout << "Already connected to peer " << peerID.ConvertToUint64() << std::endl;
            return true;
        }
    }
    
    // Don't connect to ourselves
    if (peerID == SteamUser()->GetSteamID())
    {
        return false;
    }
    
    SteamNetworkingIdentity identity;
    identity.SetSteamID(peerID);

    HSteamNetConnection conn = m_pInterface->ConnectP2P(identity, 0, 0, nullptr);

    if (conn != k_HSteamNetConnection_Invalid)
    {
        std::cout << "Attempting to connect to peer " << peerID.ConvertToUint64() << std::endl;
        
        // Store connection temporarily (will be properly added in callback)
        std::lock_guard<std::mutex> lock(connectionsMutex);
        peerConnections_[peerID] = conn;
        
        // Set as main connection if this is the first connection
        if (g_hConnection == k_HSteamNetConnection_Invalid)
        {
            g_hConnection = conn;
        }
        
        return true;
    }
    else
    {
        std::cerr << "Failed to initiate connection to peer " << peerID.ConvertToUint64() << std::endl;
        return false;
    }
}

void SteamNetworkingManager::disconnect()
{
    std::lock_guard<std::mutex> lock(connectionsMutex);
    
    // Close client connection
    if (g_hConnection != k_HSteamNetConnection_Invalid)
    {
        m_pInterface->CloseConnection(g_hConnection, 0, nullptr, false);
        g_hConnection = k_HSteamNetConnection_Invalid;
    }
    
    // Close all peer connections
    for (auto& pair : peerConnections_)
    {
        if (pair.second != k_HSteamNetConnection_Invalid)
        {
            m_pInterface->CloseConnection(pair.second, 0, nullptr, false);
        }
    }
    peerConnections_.clear();
    
    // Close all host connections
    for (auto conn : connections)
    {
        m_pInterface->CloseConnection(conn, 0, nullptr, false);
    }
    connections.clear();
    
    // Close listen socket
    if (hListenSock != k_HSteamListenSocket_Invalid)
    {
        m_pInterface->CloseListenSocket(hListenSock);
        hListenSock = k_HSteamListenSocket_Invalid;
    }
    
    // Reset state
    g_isConnected = false;
    hostPing_ = 0;
    
    std::cout << "Disconnected from network" << std::endl;
}

void SteamNetworkingManager::startMessageHandler()
{
    if (messageHandler_)
    {
        messageHandler_->start();
    }
}

void SteamNetworkingManager::stopMessageHandler()
{
    if (messageHandler_)
    {
        messageHandler_->stop();
    }
}

int SteamNetworkingManager::getConnectionPing(HSteamNetConnection conn) const
{
    SteamNetConnectionRealTimeStatus_t status;
    if (m_pInterface->GetConnectionRealTimeStatus(conn, &status, 0, nullptr))
    {
        return status.m_nPing;
    }
    return 0;
}

std::string SteamNetworkingManager::getConnectionRelayInfo(HSteamNetConnection conn) const
{
    SteamNetConnectionInfo_t info;
    if (m_pInterface->GetConnectionInfo(conn, &info))
    {
        // Check if connection is using relay
        if (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed)
        {
            return "中继";
        }
        else
        {
            return "直连";
        }
    }
    return "N/A";
}

HSteamNetConnection SteamNetworkingManager::getConnectionForPeer(CSteamID peerID) const
{
    std::lock_guard<std::mutex> lock(connectionsMutex);
    auto it = peerConnections_.find(peerID);
    if (it != peerConnections_.end())
    {
        return it->second;
    }
    return k_HSteamNetConnection_Invalid;
}

std::map<CSteamID, HSteamNetConnection> SteamNetworkingManager::getAllPeerConnections() const
{
    std::lock_guard<std::mutex> lock(connectionsMutex);
    return peerConnections_;
}

void SteamNetworkingManager::handleConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t *pInfo)
{
    std::lock_guard<std::mutex> lock(connectionsMutex);
    if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)
    {
        std::cerr << "Connection failed: " << pInfo->m_info.m_szEndDebug << std::endl;
    }
    if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_None && pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting)
    {
        m_pInterface->AcceptConnection(pInfo->m_hConn);
        CSteamID remoteSteamID = pInfo->m_info.m_identityRemote.GetSteamID();
        
        connections.push_back(pInfo->m_hConn);
        peerConnections_[remoteSteamID] = pInfo->m_hConn;
        
        // Set as main connection if this is the first connection
        if (g_hConnection == k_HSteamNetConnection_Invalid)
        {
            g_hConnection = pInfo->m_hConn;
        }
        
        g_isConnected = true;
        std::cout << "Accepted incoming connection from " << remoteSteamID.ConvertToUint64() << std::endl;
        // Log connection info
        SteamNetConnectionInfo_t info;
        SteamNetConnectionRealTimeStatus_t status;
        if (m_pInterface->GetConnectionInfo(pInfo->m_hConn, &info) && m_pInterface->GetConnectionRealTimeStatus(pInfo->m_hConn, &status, 0, nullptr))
        {
            std::cout << "Incoming connection details: ping=" << status.m_nPing << "ms, relay=" << (info.m_idPOPRelay != 0 ? "yes" : "no") << std::endl;
        }
        
        // Notify VPN bridge of new user
        if (vpnBridge_) {
            vpnBridge_->onUserJoined(remoteSteamID, pInfo->m_hConn);
        }
    }
    else if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting && pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_Connected)
    {
        g_isConnected = true;
        CSteamID remoteSteamID = pInfo->m_info.m_identityRemote.GetSteamID();
        std::cout << "Connected to peer " << remoteSteamID.ConvertToUint64() << std::endl;
        
        // Add to connections if not already there
        auto it = std::find(connections.begin(), connections.end(), pInfo->m_hConn);
        if (it == connections.end())
        {
            connections.push_back(pInfo->m_hConn);
        }
        
        // Update peer connections map
        // Update peer connections map
        peerConnections_[remoteSteamID] = pInfo->m_hConn;
        
        // Log connection info
        SteamNetConnectionInfo_t info;
        SteamNetConnectionRealTimeStatus_t status;
        if (m_pInterface->GetConnectionInfo(pInfo->m_hConn, &info) && m_pInterface->GetConnectionRealTimeStatus(pInfo->m_hConn, &status, 0, nullptr))
        {
            // Update host ping if this is the main connection
            if (pInfo->m_hConn == g_hConnection)
            {
                hostPing_ = status.m_nPing;
            }
            std::cout << "Outgoing connection details: ping=" << status.m_nPing << "ms, relay=" << (info.m_idPOPRelay != 0 ? "yes" : "no") << std::endl;
        }

        // Notify VPN bridge of new user (Outgoing connection)
        if (vpnBridge_) {
            vpnBridge_->onUserJoined(remoteSteamID, pInfo->m_hConn);
        }
    }
    else if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer || pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)
    {
        CSteamID remoteSteamID = pInfo->m_info.m_identityRemote.GetSteamID();
        
        // Notify VPN bridge of user leaving
        if (vpnBridge_) {
            vpnBridge_->onUserLeft(remoteSteamID);
        }
        
        // Remove from connections
        auto it = std::find(connections.begin(), connections.end(), pInfo->m_hConn);
        if (it != connections.end())
        {
            connections.erase(it);
        }
        
        // Remove from peer connections map
        auto peerIt = peerConnections_.find(remoteSteamID);
        if (peerIt != peerConnections_.end())
        {
            peerConnections_.erase(peerIt);
        }
        
        // Check if we still have connections
        if (connections.empty())
        {
            g_isConnected = false;
            g_hConnection = k_HSteamNetConnection_Invalid;
            hostPing_ = 0;
        }
        else if (pInfo->m_hConn == g_hConnection)
        {
            // Main connection closed, switch to another if available
            g_hConnection = connections.empty() ? k_HSteamNetConnection_Invalid : connections[0];
        }
        
        std::cout << "Connection closed with peer " << remoteSteamID.ConvertToUint64() << std::endl;
    }
}