#ifndef VPN_PROTOCOL_H
#define VPN_PROTOCOL_H

#include <cstdint>
#include <array>
#include <chrono>
#include <string>
#include <steam_api.h>
#include <isteamnetworkingsockets.h>

// ============================================================================
// 分布式 IP 分配协议常量（默认值）
// 这些值可以通过 ConfigManager 在运行时覆盖
// ============================================================================

// Steam Networking Messages API 消息大小限制
// Unreliable 消息限制约为 1200 字节，Reliable 消息限制约为 512KB
// 我们使用 Unreliable 模式发送 IP 数据包以降低延迟
constexpr size_t STEAM_UNRELIABLE_MSG_SIZE_LIMIT = 1200;

// VPN 消息封装开销
// VpnMessageHeader (3 bytes) + VpnPacketWrapper (32 bytes NodeID) = 35 bytes
constexpr size_t VPN_MESSAGE_OVERHEAD = 35;

// 推荐的 MTU 值：Steam 限制 - 封装开销 - 安全余量
// 1200 - 35 - 65 = 1100 (保留 65 字节安全余量)
constexpr int RECOMMENDED_MTU = 1100;

// 应用程序私密盐（用于 Node ID 生成）- 默认值
constexpr const char* APP_SECRET_SALT = "ConnectTool_VPN_Salt_v1";

// 协议时间常量（毫秒）- 默认值
constexpr int64_t PROBE_TIMEOUT_MS = 500;           // 探测超时时间
constexpr int64_t HEARTBEAT_INTERVAL_MS = 60000;    // 心跳间隔（60秒）
constexpr int64_t LEASE_TIME_MS = 120000;           // 租约时间（120秒）
constexpr int64_t LEASE_EXPIRY_MS = 360000;         // 租约过期时间（360秒，3个租约周期）
constexpr int64_t HEARTBEAT_EXPIRY_MS = 180000;     // 心跳过期时间（3分钟）

// Node ID 大小（SHA-256 = 32字节 = 256位）
constexpr size_t NODE_ID_SIZE = 32;

// ============================================================================
// Node ID 类型定义
// ============================================================================
using NodeID = std::array<uint8_t, NODE_ID_SIZE>;

// ============================================================================
// VPN消息类型
// ============================================================================
enum class VpnMessageType : uint8_t {
    IP_PACKET = 1,              // IP数据包（包含发送者 Node ID）
    ROUTE_UPDATE = 3,           // 路由表更新
    
    // 分布式 IP 协商协议消息
    PROBE_REQUEST = 10,         // 地址探测请求
    PROBE_RESPONSE = 11,        // 冲突响应（包含 Node ID 和心跳时间戳）
    ADDRESS_ANNOUNCE = 12,      // 地址宣布
    FORCED_RELEASE = 13,        // 强制释放指令
    HEARTBEAT = 14,             // 心跳/续租包
    HEARTBEAT_ACK = 15,         // 心跳确认
    
    // 会话管理消息
    SESSION_HELLO = 20          // 会话初始化消息（用于建立 P2P 连接）
};

// ============================================================================
// 协议消息结构
// ============================================================================
#pragma pack(push, 1)

/**
 * @brief VPN消息头
 */
struct VpnMessageHeader {
    VpnMessageType type;    // 消息类型
    uint16_t length;        // 数据长度
};

/**
 * @brief IP数据包包装（包含发送者 Node ID）
 * 用于数据包级别的冲突检测
 */
struct VpnPacketWrapper {
    NodeID senderNodeId;    // 发送者的 Node ID
    // 后续跟随实际的 IP 数据包
};

/**
 * @brief 探测请求消息体
 */
struct ProbeRequestPayload {
    uint32_t ipAddress;     // 请求的 IP 地址（网络字节序）
    NodeID nodeId;          // 请求者的 Node ID
};

/**
 * @brief 冲突响应消息体
 */
struct ProbeResponsePayload {
    uint32_t ipAddress;     // 冲突的 IP 地址（网络字节序）
    NodeID nodeId;          // 当前持有者的 Node ID
    int64_t lastHeartbeatMs; // 最后心跳时间戳（毫秒，相对于 epoch）
};

/**
 * @brief 地址宣布消息体
 */
struct AddressAnnouncePayload {
    uint32_t ipAddress;     // 宣布的 IP 地址（网络字节序）
    NodeID nodeId;          // 宣布者的 Node ID
};

/**
 * @brief 强制释放消息体
 */
struct ForcedReleasePayload {
    uint32_t ipAddress;     // 需要释放的 IP 地址（网络字节序）
    NodeID winnerNodeId;    // 获胜者的 Node ID
};

/**
 * @brief 心跳消息体
 */
struct HeartbeatPayload {
    uint32_t ipAddress;     // 心跳的 IP 地址（网络字节序）
    NodeID nodeId;          // 发送者的 Node ID
    int64_t timestampMs;    // 时间戳（毫秒）
};

#pragma pack(pop)

// ============================================================================
// 节点信息
// ============================================================================

/**
 * @brief 节点信息（用于分布式协议）
 */
struct NodeInfo {
    NodeID nodeId;                                      // 256位 Node ID
    CSteamID steamId;                                   // Steam ID
    uint32_t ipAddress;                                 // 分配的 IP 地址
    std::chrono::steady_clock::time_point lastHeartbeat; // 最后心跳时间
    std::string name;                                   // 用户名
    bool isLocal;                                       // 是否是本机
    
    // 检查节点是否活跃
    bool isActive() const {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHeartbeat).count();
        return elapsed < HEARTBEAT_EXPIRY_MS;
    }
    
    // 检查租约是否过期
    bool isLeaseExpired() const {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHeartbeat).count();
        return elapsed >= LEASE_EXPIRY_MS;
    }
};

/**
 * @brief IP路由表项（ISteamNetworkingMessages 版本，无需连接句柄）
 */
struct RouteEntry {
    CSteamID steamID;           // 对应的Steam ID
    uint32_t ipAddress;         // IP地址（主机字节序）
    std::string name;           // 用户名
    bool isLocal;               // 是否是本机
    NodeID nodeId;              // 节点 ID（用于冲突检测）
};

#endif // VPN_PROTOCOL_H