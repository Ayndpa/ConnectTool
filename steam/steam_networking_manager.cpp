#include "steam_networking_manager.h"
#include "steam_vpn_bridge.h"
#include "config/config_manager.h"
#include "net/vpn_protocol.h"
#include <iostream>
#include <algorithm>

SteamNetworkingManager *SteamNetworkingManager::instance = nullptr;

// STEAM_CALLBACK 回调函数 - 当有新的会话请求时
void SteamNetworkingManager::OnSessionRequest(SteamNetworkingMessagesSessionRequest_t *pCallback)
{
    CSteamID remoteSteamID = pCallback->m_identityRemote.GetSteamID();
    std::cout << "[SteamNetworkingManager] Session request from " << remoteSteamID.ConvertToUint64() << std::endl;
    
    // 自动接受来自已知节点的会话请求
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        if (peers_.find(remoteSteamID) != peers_.end()) {
            m_pMessagesInterface->AcceptSessionWithUser(pCallback->m_identityRemote);
            std::cout << "[SteamNetworkingManager] Accepted session from known peer" << std::endl;
        }
    }
}

// STEAM_CALLBACK 回调函数 - 当会话失败时
void SteamNetworkingManager::OnSessionFailed(SteamNetworkingMessagesSessionFailed_t *pCallback)
{
    CSteamID remoteSteamID = pCallback->m_info.m_identityRemote.GetSteamID();
    std::cout << "[SteamNetworkingManager] Session failed with " << remoteSteamID.ConvertToUint64() 
              << ": " << pCallback->m_info.m_szEndDebug << std::endl;
}

SteamNetworkingManager::SteamNetworkingManager()
    : m_pMessagesInterface(nullptr)
    , messageHandler_(nullptr)
    , vpnBridge_(nullptr)
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

    // 允许 P2P (ICE) 直连
    int32 nIceEnable = k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Public;
    SteamNetworkingUtils()->SetConfigValue(
        k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable,
        k_ESteamNetworkingConfig_Global,
        0,
        k_ESteamNetworkingConfig_Int32,
        &nIceEnable);

    // 使用配置管理器中的设置
    const auto& config = ConfigManager::instance().getConfig();
    int32 sendRate = config.networking.send_rate_mb * 1024 * 1024;  // MB/s -> bytes/s
    
    SteamNetworkingUtils()->SetConfigValue(
        k_ESteamNetworkingConfig_SendRateMin,
        k_ESteamNetworkingConfig_Global,
        0,
        k_ESteamNetworkingConfig_Int32,
        &sendRate);
    
    SteamNetworkingUtils()->SetConfigValue(
        k_ESteamNetworkingConfig_SendRateMax,
        k_ESteamNetworkingConfig_Global,
        0,
        k_ESteamNetworkingConfig_Int32,
        &sendRate);

    // 增大发送缓冲区大小
    int32 sendBufferSize = config.networking.send_buffer_size_mb * 1024 * 1024;
    SteamNetworkingUtils()->SetConfigValue(
        k_ESteamNetworkingConfig_SendBufferSize,
        k_ESteamNetworkingConfig_Global,
        0,
        k_ESteamNetworkingConfig_Int32,
        &sendBufferSize);

    // 禁用 Nagle 算法以减少延迟
    int32 nagleTime = config.networking.nagle_time;
    SteamNetworkingUtils()->SetConfigValue(
        k_ESteamNetworkingConfig_NagleTime,
        k_ESteamNetworkingConfig_Global,
        0,
        k_ESteamNetworkingConfig_Int32,
        &nagleTime);

    std::cout << "[SteamNetworkingManager] Bandwidth optimization: SendRate=" 
              << (sendRate / 1024 / 1024) << " MB/s, SendBufferSize=" 
              << (sendBufferSize / 1024 / 1024) << " MB" << std::endl;

    // 初始化 relay 网络访问
    SteamNetworkingUtils()->InitRelayNetworkAccess();

    // 获取 ISteamNetworkingMessages 接口
    m_pMessagesInterface = SteamNetworkingMessages();
    if (!m_pMessagesInterface)
    {
        std::cerr << "Failed to get ISteamNetworkingMessages interface" << std::endl;
        return false;
    }

    // Initialize message handler
    messageHandler_ = new SteamMessageHandler(m_pMessagesInterface, this);

    std::cout << "[SteamNetworkingManager] Steam Networking Manager initialized with ISteamNetworkingMessages" << std::endl;

    return true;
}

void SteamNetworkingManager::shutdown()
{
    // 关闭所有会话
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        for (const auto& peerID : peers_) {
            SteamNetworkingIdentity identity;
            identity.SetSteamID(peerID);
            if (m_pMessagesInterface) {
                m_pMessagesInterface->CloseSessionWithUser(identity);
            }
        }
        peers_.clear();
    }
    
    SteamAPI_Shutdown();
}

bool SteamNetworkingManager::sendMessageToUser(CSteamID peerID, const void* data, uint32_t size, int flags)
{
    if (!m_pMessagesInterface) return false;
    
    SteamNetworkingIdentity identity;
    identity.SetSteamID(peerID);
    
    EResult result = m_pMessagesInterface->SendMessageToUser(identity, data, size, flags, VPN_CHANNEL);
    return result == k_EResultOK;
}

void SteamNetworkingManager::broadcastMessage(const void* data, uint32_t size, int flags)
{
    if (!m_pMessagesInterface) return;
    
    std::lock_guard<std::mutex> lock(peersMutex_);
    for (const auto& peerID : peers_) {
        SteamNetworkingIdentity identity;
        identity.SetSteamID(peerID);
        m_pMessagesInterface->SendMessageToUser(identity, data, size, flags, VPN_CHANNEL);
    }
}

void SteamNetworkingManager::addPeer(CSteamID peerID)
{
    // 不添加自己
    if (peerID == SteamUser()->GetSteamID()) return;
    
    bool isNewPeer = false;
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        isNewPeer = peers_.insert(peerID).second;
    }
    
    if (isNewPeer) {
        std::cout << "[SteamNetworkingManager] Added peer: " << peerID.ConvertToUint64() << std::endl;
        
        // 主动发送 SESSION_HELLO 消息来初始化 P2P 会话
        // 这会触发 ISteamNetworkingMessages 建立底层连接
        VpnMessageHeader helloMsg;
        helloMsg.type = VpnMessageType::SESSION_HELLO;
        helloMsg.length = 0;
        
        SteamNetworkingIdentity identity;
        identity.SetSteamID(peerID);
        
        // 使用可靠发送 + 自动重启断开的会话
        int flags = k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_AutoRestartBrokenSession;
        EResult result = m_pMessagesInterface->SendMessageToUser(identity, &helloMsg, sizeof(helloMsg), flags, VPN_CHANNEL);
        
        if (result == k_EResultOK) {
            std::cout << "[SteamNetworkingManager] Sent SESSION_HELLO to " << peerID.ConvertToUint64() << std::endl;
        } else {
            std::cout << "[SteamNetworkingManager] Failed to send SESSION_HELLO to " << peerID.ConvertToUint64() 
                      << ", result: " << result << std::endl;
        }
        
        // 通知 VPN bridge
        if (vpnBridge_) {
            vpnBridge_->onUserJoined(peerID);
        }
    }
}

void SteamNetworkingManager::removePeer(CSteamID peerID)
{
    std::lock_guard<std::mutex> lock(peersMutex_);
    if (peers_.erase(peerID) > 0) {
        std::cout << "[SteamNetworkingManager] Removed peer: " << peerID.ConvertToUint64() << std::endl;
        
        // 关闭会话
        SteamNetworkingIdentity identity;
        identity.SetSteamID(peerID);
        if (m_pMessagesInterface) {
            m_pMessagesInterface->CloseSessionWithUser(identity);
        }
        
        // 通知 VPN bridge
        if (vpnBridge_) {
            vpnBridge_->onUserLeft(peerID);
        }
    }
}

void SteamNetworkingManager::clearPeers()
{
    std::lock_guard<std::mutex> lock(peersMutex_);
    for (const auto& peerID : peers_) {
        SteamNetworkingIdentity identity;
        identity.SetSteamID(peerID);
        if (m_pMessagesInterface) {
            m_pMessagesInterface->CloseSessionWithUser(identity);
        }
        
        if (vpnBridge_) {
            vpnBridge_->onUserLeft(peerID);
        }
    }
    peers_.clear();
    std::cout << "[SteamNetworkingManager] Cleared all peers" << std::endl;
}

std::set<CSteamID> SteamNetworkingManager::getPeers() const
{
    std::lock_guard<std::mutex> lock(peersMutex_);
    return peers_;
}

int SteamNetworkingManager::getPeerPing(CSteamID peerID) const
{
    if (!m_pMessagesInterface) return -1;
    
    SteamNetworkingIdentity identity;
    identity.SetSteamID(peerID);
    
    SteamNetConnectionRealTimeStatus_t status;
    ESteamNetworkingConnectionState state = m_pMessagesInterface->GetSessionConnectionInfo(identity, nullptr, &status);
    
    if (state == k_ESteamNetworkingConnectionState_Connected) {
        return status.m_nPing;
    }
    return -1;
}

bool SteamNetworkingManager::isPeerConnected(CSteamID peerID) const
{
    if (!m_pMessagesInterface) return false;
    
    SteamNetworkingIdentity identity;
    identity.SetSteamID(peerID);
    
    ESteamNetworkingConnectionState state = m_pMessagesInterface->GetSessionConnectionInfo(identity, nullptr, nullptr);
    return state == k_ESteamNetworkingConnectionState_Connected;
}

std::string SteamNetworkingManager::getPeerConnectionType(CSteamID peerID) const
{
    if (!m_pMessagesInterface) return "N/A";
    
    SteamNetworkingIdentity identity;
    identity.SetSteamID(peerID);
    
    SteamNetConnectionInfo_t info;
    ESteamNetworkingConnectionState state = m_pMessagesInterface->GetSessionConnectionInfo(identity, &info, nullptr);
    
    if (state == k_ESteamNetworkingConnectionState_Connected) {
        if (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed) {
            return "中继";
        } else {
            return "直连";
        }
    }
    return "N/A";
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