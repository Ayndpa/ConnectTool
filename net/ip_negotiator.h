#ifndef IP_NEGOTIATOR_H
#define IP_NEGOTIATOR_H

#include "vpn_protocol.h"
#include "node_identity.h"
#include <functional>
#include <vector>
#include <mutex>
#include <chrono>
#include <map>
#include <set>

/**
 * @brief IP 协商状态
 */
enum class NegotiationState {
    IDLE,           // 空闲
    PROBING,        // 正在探测（发送了探测请求，等待响应）
    STABLE          // IP 已确定
};

/**
 * @brief 冲突信息
 */
struct ConflictInfo {
    NodeID nodeId;
    int64_t lastHeartbeatMs;
    CSteamID senderSteamID;
};

/**
 * @brief 消息发送回调（使用 CSteamID）
 */
using VpnSendMessageCallback = std::function<void(VpnMessageType type, const uint8_t* payload, 
                                                size_t length, CSteamID targetSteamID, bool reliable)>;
using VpnBroadcastMessageCallback = std::function<void(VpnMessageType type, const uint8_t* payload, 
                                                     size_t length, bool reliable)>;

/**
 * @brief IP 协商成功回调
 */
using NegotiationSuccessCallback = std::function<void(uint32_t ipAddress, const NodeID& nodeId)>;

/**
 * @brief 分布式 IP 协商器（ISteamNetworkingMessages 版本）
 * 
 * 实现基于 Node ID 的 P2P IP 地址分配协议
 */
class IpNegotiator {
public:
    IpNegotiator();
    
    /**
     * @brief 初始化协商器
     * @param localSteamID 本地 Steam ID
     * @param baseIP 子网基地址
     * @param subnetMask 子网掩码
     */
    void initialize(CSteamID localSteamID, uint32_t baseIP, uint32_t subnetMask);
    
    /**
     * @brief 设置消息发送回调
     */
    void setSendCallback(VpnSendMessageCallback sendCb, VpnBroadcastMessageCallback broadcastCb);
    
    /**
     * @brief 设置协商成功回调
     */
    void setSuccessCallback(NegotiationSuccessCallback callback);
    
    /**
     * @brief 开始 IP 协商
     */
    void startNegotiation();
    
    /**
     * @brief 检查协商超时（应在主循环中定期调用）
     */
    void checkTimeout();
    
    /**
     * @brief 处理探测请求
     */
    void handleProbeRequest(const ProbeRequestPayload& request, CSteamID senderSteamID);
    
    /**
     * @brief 处理探测响应
     */
    void handleProbeResponse(const ProbeResponsePayload& response, CSteamID senderSteamID);
    
    /**
     * @brief 处理地址宣布
     */
    void handleAddressAnnounce(const AddressAnnouncePayload& announce, CSteamID peerSteamID,
                               const std::string& peerName);
    
    /**
     * @brief 处理强制释放
     */
    void handleForcedRelease(const ForcedReleasePayload& release, CSteamID senderSteamID);
    
    /**
     * @brief 获取当前状态
     */
    NegotiationState getState() const { return state_; }
    
    /**
     * @brief 获取本地 IP（仅在 STABLE 状态有效）
     */
    uint32_t getLocalIP() const { return localIP_; }
    
    /**
     * @brief 获取本地 Node ID
     */
    const NodeID& getLocalNodeID() const { return localNodeId_; }
    
    /**
     * @brief 获取候选 IP
     */
    uint32_t getCandidateIP() const { return candidateIP_; }
    
    /**
     * @brief 发送地址宣布
     */
    void sendAddressAnnounce();
    
    /**
     * @brief 向特定用户发送地址宣布
     */
    void sendAddressAnnounceTo(CSteamID targetSteamID);
    
    /**
     * @brief 标记 IP 为已使用（用于避免冲突）
     */
    void markIPUsed(uint32_t ip);
    
    /**
     * @brief 标记 IP 为未使用
     */
    void markIPUnused(uint32_t ip);

private:
    // 生成候选 IP
    uint32_t generateCandidateIP(uint32_t offset);
    
    // 查找下一个可用 IP
    uint32_t findNextAvailableIP(uint32_t startIP);
    
    // 发送探测请求
    void sendProbeRequest();
    
    // 发送强制释放
    void sendForcedRelease(uint32_t ipAddress, CSteamID targetSteamID);
    
    // 本地信息
    NodeID localNodeId_;
    CSteamID localSteamID_;
    uint32_t localIP_;
    
    // 网络配置
    uint32_t baseIP_;
    uint32_t subnetMask_;
    
    // 协商状态
    NegotiationState state_;
    uint32_t candidateIP_;
    uint32_t probeOffset_;
    std::chrono::steady_clock::time_point probeStartTime_;
    
    // 收集到的冲突响应
    std::vector<ConflictInfo> collectedConflicts_;
    std::mutex conflictsMutex_;
    
    // 已使用的 IP 集合
    std::set<uint32_t> usedIPs_;
    std::mutex usedIPsMutex_;
    
    // 回调
    VpnSendMessageCallback sendCallback_;
    VpnBroadcastMessageCallback broadcastCallback_;
    NegotiationSuccessCallback successCallback_;
};

#endif // IP_NEGOTIATOR_H
