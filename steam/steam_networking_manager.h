#ifndef STEAM_NETWORKING_MANAGER_H
#define STEAM_NETWORKING_MANAGER_H

#include <vector>
#include <set>
#include <mutex>
#include <memory>
#include <steam_api.h>
#include <isteamnetworkingmessages.h>
#include <isteamnetworkingutils.h>
#include <steamnetworkingtypes.h>
#include "steam_message_handler.h"

// Forward declarations
class SteamNetworkingManager;
class SteamVpnBridge;

// User info structure
struct UserInfo {
    CSteamID steamID;
    std::string name;
    int ping;
    bool isRelay;
};

/**
 * @brief Steam 网络管理器（ISteamNetworkingMessages 版本）
 * 
 * 使用 ISteamNetworkingMessages 接口实现无连接的消息传递，
 * 无需手动维护连接状态，连接会自动建立和管理。
 */
class SteamNetworkingManager {
public:
    static SteamNetworkingManager* instance;
    SteamNetworkingManager();
    ~SteamNetworkingManager();

    bool initialize();
    void shutdown();

    // 发送消息给指定用户（使用 ISteamNetworkingMessages）
    bool sendMessageToUser(CSteamID peerID, const void* data, uint32_t size, int flags);
    
    // 广播消息给所有已知的节点
    void broadcastMessage(const void* data, uint32_t size, int flags);

    // 添加/移除已知节点（加入/离开房间时调用）
    void addPeer(CSteamID peerID);
    void removePeer(CSteamID peerID);
    void clearPeers();
    
    // 获取所有已知节点
    std::set<CSteamID> getPeers() const;

    // 获取与指定用户的会话信息
    int getPeerPing(CSteamID peerID) const;
    bool isPeerConnected(CSteamID peerID) const;
    std::string getPeerConnectionType(CSteamID peerID) const;

    // Getters
    bool isConnected() const { return !peers_.empty(); }
    ISteamNetworkingMessages* getMessagesInterface() const { return m_pMessagesInterface; }

    // Message handler
    void startMessageHandler();
    void stopMessageHandler();
    SteamMessageHandler* getMessageHandler() { return messageHandler_; }

    // VPN Bridge
    void setVpnBridge(SteamVpnBridge* vpnBridge) { vpnBridge_ = vpnBridge; }
    SteamVpnBridge* getVpnBridge() { return vpnBridge_; }

    // VPN 消息通道
    static constexpr int VPN_CHANNEL = 0;

private:
    // Steam Networking Messages API
    ISteamNetworkingMessages* m_pMessagesInterface;

    // 已知的节点列表（加入同一房间的用户）
    std::set<CSteamID> peers_;
    mutable std::mutex peersMutex_;

    // Message handler
    SteamMessageHandler* messageHandler_;

    // VPN Bridge
    SteamVpnBridge* vpnBridge_;

    // 回调处理
    STEAM_CALLBACK(SteamNetworkingManager, OnSessionRequest, SteamNetworkingMessagesSessionRequest_t);
    STEAM_CALLBACK(SteamNetworkingManager, OnSessionFailed, SteamNetworkingMessagesSessionFailed_t);

    friend class SteamRoomManager;
};

#endif // STEAM_NETWORKING_MANAGER_H