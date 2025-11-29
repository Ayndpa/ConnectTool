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

// Forward declarations
class SteamNetworkingManager;

/**
 * @brief IP路由表项
 */
struct RouteEntry {
    CSteamID steamID;           // 对应的Steam ID
    HSteamNetConnection conn;   // 对应的Steam连接
    uint32_t ipAddress;         // IP地址（主机字节序）
    std::string name;           // 用户名
    bool isLocal;               // 是否是本机
};

/**
 * @brief VPN消息类型
 */
enum class VpnMessageType : uint8_t {
    IP_PACKET = 1,          // IP数据包
    ROUTE_UPDATE = 3,       // 路由表更新
    PING = 4,               // 心跳包
    PONG = 5,               // 心跳响应
    IP_PROBE = 6,           // IP探测
    IP_CONFLICT = 7         // IP冲突
};

/**
 * @brief VPN消息头
 */
#pragma pack(push, 1)
struct VpnMessageHeader {
    VpnMessageType type;    // 消息类型
    uint16_t length;        // 数据长度
};
#pragma pack(pop)

/**
 * @brief Steam VPN桥接器
 * 
 * 负责在虚拟网卡和Steam网络之间转发IP数据包
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
     * @brief 当新用户加入时分配IP地址
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

    // TUN设备写入线程（处理发送队列）
    void tunWriteThread();



    // 广播路由更新
    void broadcastRouteUpdate();

    // IP地址转字符串
    static std::string ipToString(uint32_t ip);

    // 字符串转IP地址
    static uint32_t stringToIp(const std::string& ipStr);

    // 从IP包中提取目标地址
    static uint32_t extractDestIP(const uint8_t* packet, size_t length);

    // 从IP包中提取源地址
    static uint32_t extractSourceIP(const uint8_t* packet, size_t length);

    // 从SteamID生成确定的IP地址
    uint32_t generateIPFromSteamID(CSteamID steamID);

    // Steam网络管理器
    SteamNetworkingManager* steamManager_;

    // TUN设备
    std::unique_ptr<tun::TunInterface> tunDevice_;

    // 运行状态
    std::atomic<bool> running_;

    // TUN读取线程
    std::unique_ptr<std::thread> tunReadThread_;

    // TUN写入线程
    std::unique_ptr<std::thread> tunWriteThread_;

    // 路由表（IP地址 -> 路由信息）
    std::map<uint32_t, RouteEntry> routingTable_;
    mutable std::mutex routingMutex_;

    // IP地址池
    uint32_t baseIP_;           // 基础IP地址
    uint32_t subnetMask_;       // 子网掩码
    uint32_t nextIP_;           // 下一个可分配的IP
    std::vector<uint32_t> allocatedIPs_;  // 已分配的IP列表
    std::mutex ipAllocationMutex_;

    // 本地IP地址
    uint32_t localIP_;

    // 统计信息
    Statistics stats_;
    mutable std::mutex statsMutex_;

    // 发送队列
    struct OutgoingPacket {
        std::vector<uint8_t> data;
        HSteamNetConnection targetConn;
    };
    std::vector<OutgoingPacket> sendQueue_;
    std::mutex sendQueueMutex_;

    // IP协商相关
    enum class IpNegotiationState {
        IDLE,
        NEGOTIATING,
        STABLE
    };
    IpNegotiationState negotiationState_;
    uint32_t candidateIP_;
    std::chrono::steady_clock::time_point probeStartTime_;
    
    // 开始IP协商
    void startIpNegotiation();
    
    // 检查IP协商超时
    void checkIpNegotiationTimeout();
    
    // 处理IP探测
    void handleIpProbe(const uint8_t* data, size_t length, HSteamNetConnection fromConn);
    
    // 处理IP冲突
    void handleIpConflict(const uint8_t* data, size_t length, HSteamNetConnection fromConn);

    // 查找下一个可用IP
    uint32_t findNextAvailableIP(uint32_t startIP);

    // 打印当前路由表
    void printRoutingTable();
};

#endif // STEAM_VPN_BRIDGE_H
