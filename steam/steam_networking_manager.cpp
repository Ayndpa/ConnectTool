#include "steam_networking_manager.h"
#include "steam_message_handler.h"
#include "steam_room_manager.h"
#include "config/config_manager.h"
#include <iostream>
#include <steam_api.h>
#include <isteamnetworkingutils.h>

SteamNetworkingManager* SteamNetworkingManager::instance = nullptr;

// STEAM_CALLBACK 回调函数 - 当收到会话请求时
void SteamNetworkingManager::OnSessionRequest(SteamNetworkingMessagesSessionRequest_t *pCallback)
{
    std::cout << "[SteamNetworkingManager] Session request from " << pCallback->m_identityRemote.GetSteamID().ConvertToUint64() << std::endl;
    
    // 接受所有来自房间成员的连接请求
    // TODO: 添加更严格的验证
    m_pMessagesInterface->AcceptSessionWithUser(pCallback->m_identityRemote);
    
    std::set<CSteamID> members = getRoomMembers();
    CSteamID remoteSteamID = pCallback->m_identityRemote.GetSteamID();
    if (members.find(remoteSteamID) != members.end()) {
        std::cout << "[SteamNetworkingManager] Accepted session from room member" << std::endl;
    } else {
        std::cout << "[SteamNetworkingManager] Accepted session (user not yet in local member list)" << std::endl;
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
    , roomManager_(nullptr)
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
    // 关闭与房间成员的所有会话
    std::set<CSteamID> members = getRoomMembers();
    for (const auto& memberID : members) {
        SteamNetworkingIdentity identity;
        identity.SetSteamID(memberID);
        if (m_pMessagesInterface) {
            m_pMessagesInterface->CloseSessionWithUser(identity);
        }
    }
    
    SteamAPI_Shutdown();
}

bool SteamNetworkingManager::sendMessageToUser(CSteamID peerID, const void* data, uint32_t size, int flags)
{
    if (!m_pMessagesInterface) return false;
    
    SteamNetworkingIdentity identity;
    identity.SetSteamID(peerID);
    
    // 对于可靠消息，自动添加 AutoRestartBrokenSession flag 以支持断线重连
    if (flags & k_nSteamNetworkingSend_Reliable) {
        flags |= k_nSteamNetworkingSend_AutoRestartBrokenSession;
    }
    
    EResult result = m_pMessagesInterface->SendMessageToUser(identity, data, size, flags, VPN_CHANNEL);
    return result == k_EResultOK;
}

void SteamNetworkingManager::broadcastMessage(const void* data, uint32_t size, int flags)
{
    if (!m_pMessagesInterface) return;
    
    // 对于可靠消息，自动添加 AutoRestartBrokenSession flag 以支持断线重连
    if (flags & k_nSteamNetworkingSend_Reliable) {
        flags |= k_nSteamNetworkingSend_AutoRestartBrokenSession;
    }
    
    // 实时从房间获取成员列表
    std::set<CSteamID> members = getRoomMembers();
    for (const auto& memberID : members) {
        SteamNetworkingIdentity identity;
        identity.SetSteamID(memberID);
        m_pMessagesInterface->SendMessageToUser(identity, data, size, flags, VPN_CHANNEL);
    }
}

std::set<CSteamID> SteamNetworkingManager::getRoomMembers() const
{
    if (!roomManager_) return std::set<CSteamID>();
    return roomManager_->getMembers(false);
}

bool SteamNetworkingManager::isInRoom() const
{
    if (!roomManager_) return false;
    return roomManager_->getCurrentLobby().IsValid();
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
    } else if (peerID == SteamUser()->GetSteamID()) {
        return "本机";
    }
    return "挂起";
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