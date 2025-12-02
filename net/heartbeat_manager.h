#ifndef HEARTBEAT_MANAGER_H
#define HEARTBEAT_MANAGER_H

#include "vpn_protocol.h"
#include "node_identity.h"
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

/**
 * @brief 消息发送回调
 */
using HeartbeatSendCallback = std::function<void(VpnMessageType type, const uint8_t* payload, 
                                                  size_t length, bool reliable)>;

/**
 * @brief 节点过期回调
 */
using NodeExpiredCallback = std::function<void(const NodeID& nodeId, uint32_t ipAddress)>;

/**
 * @brief 心跳/租约管理器
 * 
 * 管理节点心跳和租约，检测并清理过期节点
 */
class HeartbeatManager {
public:
    HeartbeatManager();
    ~HeartbeatManager();
    
    /**
     * @brief 初始化管理器
     * @param localNodeId 本地 Node ID
     * @param localIP 本地 IP 地址
     */
    void initialize(const NodeID& localNodeId, uint32_t localIP);
    
    /**
     * @brief 设置发送回调
     */
    void setSendCallback(HeartbeatSendCallback callback);
    
    /**
     * @brief 设置节点过期回调
     */
    void setNodeExpiredCallback(NodeExpiredCallback callback);
    
    /**
     * @brief 启动心跳线程
     */
    void start();
    
    /**
     * @brief 停止心跳线程
     */
    void stop();
    
    /**
     * @brief 更新本地 IP（协商完成后调用）
     */
    void updateLocalIP(uint32_t ip);
    
    /**
     * @brief 处理收到的心跳
     */
    void handleHeartbeat(const HeartbeatPayload& heartbeat, HSteamNetConnection fromConn,
                         CSteamID peerSteamID, const std::string& peerName);
    
    /**
     * @brief 注册远程节点
     */
    void registerNode(const NodeID& nodeId, CSteamID steamId, uint32_t ipAddress,
                      HSteamNetConnection conn, const std::string& name);
    
    /**
     * @brief 注销远程节点
     */
    void unregisterNode(const NodeID& nodeId);
    
    /**
     * @brief 根据 IP 查找 Node ID
     */
    bool findNodeByIP(uint32_t ip, NodeID& outNodeId) const;
    
    /**
     * @brief 获取所有节点信息
     */
    std::map<NodeID, NodeInfo> getAllNodes() const;
    
    /**
     * @brief 检测数据包级别冲突
     * @return 如果检测到冲突，返回需要强制释放的连接
     */
    HSteamNetConnection detectConflict(uint32_t sourceIP, const NodeID& senderNodeId);

private:
    void heartbeatLoop();
    void sendHeartbeat();
    void checkExpiredLeases();
    
    // 本地信息
    NodeID localNodeId_;
    uint32_t localIP_;
    std::chrono::steady_clock::time_point lastHeartbeatSent_;
    
    // 节点表
    std::map<NodeID, NodeInfo> nodeTable_;
    std::map<uint32_t, NodeID> ipToNodeId_;
    mutable std::mutex nodeTableMutex_;
    
    // 线程控制
    std::unique_ptr<std::thread> heartbeatThread_;
    std::atomic<bool> running_;
    
    // 回调
    HeartbeatSendCallback sendCallback_;
    NodeExpiredCallback expiredCallback_;
};

#endif // HEARTBEAT_MANAGER_H
