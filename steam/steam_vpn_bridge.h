#ifndef STEAM_VPN_BRIDGE_H
#define STEAM_VPN_BRIDGE_H

#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <vector>
#include <string>
#include <cstdint>
#include <steam_api.h>
#include <isteamnetworkingsockets.h>

#include "../tun/tun_interface.h"
#include "../net/vpn_protocol.h"
#include "../net/ip_negotiator.h"
#include "../net/heartbeat_manager.h"

// Forward declarations
class SteamNetworkingManager;

/**
 * @brief Steam VPN桥接器
 * 
 * 负责在虚拟网卡和Steam网络之间转发IP数据包
 * 使用分布式 IP 协商协议自动分配 IP 地址
 */
class SteamVpnBridge {
public:
    SteamVpnBridge(SteamNetworkingManager* steamManager);
    ~SteamVpnBridge();

    /**
     * @brief 启动VPN桥接
     * @param tunDeviceName TUN设备名称（可选）
     * @param virtualSubnet 虚拟子网（如 "10.0.0.0"）
     * @param subnetMask 子网掩码（如 "255.255.255.0"）
     * @return true 成功，false 失败
     */
    bool start(const std::string& tunDeviceName = "", 
               const std::string& virtualSubnet = "10.0.0.0",
               const std::string& subnetMask = "255.255.255.0");

    /**
     * @brief 停止VPN桥接
     */
    void stop();

    /**
     * @brief 检查VPN是否正在运行
     */
    bool isRunning() const { return running_; }

    /**
     * @brief 获取本地分配的IP地址
     */
    std::string getLocalIP() const;

    /**
     * @brief 获取TUN设备名称
     */
    std::string getTunDeviceName() const;

    /**
     * @brief 获取路由表
     */
    std::map<uint32_t, RouteEntry> getRoutingTable() const;

    /**
     * @brief 处理来自Steam的VPN消息
     * @param data 消息数据
     * @param length 消息长度
     * @param fromConn 来源连接
     */
    void handleVpnMessage(const uint8_t* data, size_t length, HSteamNetConnection fromConn);

    /**
     * @brief 当新用户加入时
     * @param steamID 用户的Steam ID
     * @param conn 连接句柄
     */
    void onUserJoined(CSteamID steamID, HSteamNetConnection conn);

    /**
     * @brief 当用户离开时清理路由
     * @param steamID 用户的Steam ID
     */
    void onUserLeft(CSteamID steamID);

    /**
     * @brief 获取统计信息
     */
    struct Statistics {
        uint64_t packetsSent;
        uint64_t packetsReceived;
        uint64_t bytesSent;
        uint64_t bytesReceived;
        uint64_t packetsDropped;
    };
    Statistics getStatistics() const;

private:
    // TUN设备读取线程
    void tunReadThread();

    // IP地址转字符串
    static std::string ipToString(uint32_t ip);

    // 字符串转IP地址
    static uint32_t stringToIp(const std::string& ipStr);

    // 从IP包中提取目标地址
    static uint32_t extractDestIP(const uint8_t* packet, size_t length);

    // 从IP包中提取源地址
    static uint32_t extractSourceIP(const uint8_t* packet, size_t length);

    // 判断是否是广播地址
    bool isBroadcastAddress(uint32_t ip) const;
    
    // 发送 VPN 消息
    void sendVpnMessage(VpnMessageType type, const uint8_t* payload, size_t payloadLength, 
                        HSteamNetConnection conn, bool reliable = true);
    void broadcastVpnMessage(VpnMessageType type, const uint8_t* payload, size_t payloadLength, 
                             bool reliable = true);
    
    // IP 协商成功回调
    void onNegotiationSuccess(uint32_t ipAddress, const NodeID& nodeId);
    
    // 节点过期回调
    void onNodeExpired(const NodeID& nodeId, uint32_t ipAddress);
    
    // 更新路由表
    void updateRoute(const NodeID& nodeId, CSteamID steamId, uint32_t ipAddress,
                     HSteamNetConnection conn, const std::string& name);
    void removeRoute(uint32_t ipAddress);
    
    // 发送路由更新
    void broadcastRouteUpdate();
    void sendRouteUpdateTo(HSteamNetConnection conn);

    // Steam网络管理器
    SteamNetworkingManager* steamManager_;

    // TUN设备
    std::unique_ptr<tun::TunInterface> tunDevice_;

    // 运行状态
    std::atomic<bool> running_;

    // TUN读取线程
    std::unique_ptr<std::thread> tunReadThread_;

    // 路由表（IP地址 -> 路由信息）
    std::map<uint32_t, RouteEntry> routingTable_;
    mutable std::mutex routingMutex_;

    // IP地址池配置
    uint32_t baseIP_;
    uint32_t subnetMask_;

    // 本地IP地址
    uint32_t localIP_;

    // 统计信息
    Statistics stats_;
    mutable std::mutex statsMutex_;
    
    // 分布式协议组件
    IpNegotiator ipNegotiator_;
    HeartbeatManager heartbeatManager_;
};

#endif // STEAM_VPN_BRIDGE_H
