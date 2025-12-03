#include "heartbeat_manager.h"
#include <iostream>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

HeartbeatManager::HeartbeatManager()
    : localIP_(0)
    , running_(false)
{
    localNodeId_.fill(0);
}

HeartbeatManager::~HeartbeatManager() {
    stop();
}

void HeartbeatManager::initialize(const NodeID& localNodeId, uint32_t localIP) {
    localNodeId_ = localNodeId;
    localIP_ = localIP;
    lastHeartbeatSent_ = std::chrono::steady_clock::now();
}

void HeartbeatManager::setSendCallback(HeartbeatSendCallback callback) {
    sendCallback_ = std::move(callback);
}

void HeartbeatManager::setNodeExpiredCallback(NodeExpiredCallback callback) {
    expiredCallback_ = std::move(callback);
}

void HeartbeatManager::start() {
    if (running_) return;
    
    running_ = true;
    heartbeatThread_ = std::make_unique<std::thread>(&HeartbeatManager::heartbeatLoop, this);
    std::cout << "Heartbeat manager started" << std::endl;
}

void HeartbeatManager::stop() {
    if (!running_) return;
    
    running_ = false;
    if (heartbeatThread_ && heartbeatThread_->joinable()) {
        heartbeatThread_->join();
    }
    heartbeatThread_.reset();
    
    std::cout << "Heartbeat manager stopped" << std::endl;
}

void HeartbeatManager::updateLocalIP(uint32_t ip) {
    localIP_ = ip;
}

void HeartbeatManager::heartbeatLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        
        if (!running_) break;
        
        auto now = std::chrono::steady_clock::now();
        
        // 检查是否需要发送心跳
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastHeartbeatSent_).count();
        
        if (elapsed >= HEARTBEAT_INTERVAL_MS && localIP_ != 0) {
            sendHeartbeat();
            lastHeartbeatSent_ = now;
        }
        
        // 检查并清理过期的租约
        checkExpiredLeases();
    }
}

void HeartbeatManager::sendHeartbeat() {
    if (!sendCallback_ || localIP_ == 0) return;
    
    HeartbeatPayload payload;
    payload.ipAddress = htonl(localIP_);
    payload.nodeId = localNodeId_;
    
    auto now = std::chrono::steady_clock::now();
    payload.timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    
    sendCallback_(VpnMessageType::HEARTBEAT,
                  reinterpret_cast<const uint8_t*>(&payload),
                  sizeof(payload), true);
}

void HeartbeatManager::checkExpiredLeases() {
    std::vector<std::pair<NodeID, uint32_t>> expiredNodes;
    
    {
        std::lock_guard<std::mutex> lock(nodeTableMutex_);
        for (auto it = nodeTable_.begin(); it != nodeTable_.end(); ) {
            if (!it->second.isLocal && it->second.isLeaseExpired()) {
                std::cout << "Node " << NodeIdentity::toString(it->first) 
                          << " lease expired" << std::endl;
                expiredNodes.emplace_back(it->first, it->second.ipAddress);
                ipToNodeId_.erase(it->second.ipAddress);
                it = nodeTable_.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    // 调用过期回调
    if (expiredCallback_) {
        for (const auto& [nodeId, ip] : expiredNodes) {
            expiredCallback_(nodeId, ip);
        }
    }
}

void HeartbeatManager::handleHeartbeat(const HeartbeatPayload& heartbeat, CSteamID peerSteamID,
                                        const std::string& peerName) {
    uint32_t heartbeatIP = ntohl(heartbeat.ipAddress);
    
    std::lock_guard<std::mutex> lock(nodeTableMutex_);
    auto it = nodeTable_.find(heartbeat.nodeId);
    if (it != nodeTable_.end()) {
        // 更新心跳时间
        it->second.lastHeartbeat = std::chrono::steady_clock::now();
    } else {
        // 新节点，添加到节点表
        NodeInfo nodeInfo;
        nodeInfo.nodeId = heartbeat.nodeId;
        nodeInfo.steamId = peerSteamID;
        nodeInfo.ipAddress = heartbeatIP;
        nodeInfo.lastHeartbeat = std::chrono::steady_clock::now();
        nodeInfo.name = peerName;
        nodeInfo.isLocal = false;
        nodeTable_[heartbeat.nodeId] = nodeInfo;
        ipToNodeId_[heartbeatIP] = heartbeat.nodeId;
    }
}

void HeartbeatManager::registerNode(const NodeID& nodeId, CSteamID steamId, uint32_t ipAddress,
                                     const std::string& name) {
    std::lock_guard<std::mutex> lock(nodeTableMutex_);
    
    NodeInfo nodeInfo;
    nodeInfo.nodeId = nodeId;
    nodeInfo.steamId = steamId;
    nodeInfo.ipAddress = ipAddress;
    nodeInfo.lastHeartbeat = std::chrono::steady_clock::now();
    nodeInfo.name = name;
    nodeInfo.isLocal = (nodeId == localNodeId_);
    
    nodeTable_[nodeId] = nodeInfo;
    ipToNodeId_[ipAddress] = nodeId;
}

void HeartbeatManager::unregisterNode(const NodeID& nodeId) {
    std::lock_guard<std::mutex> lock(nodeTableMutex_);
    
    auto it = nodeTable_.find(nodeId);
    if (it != nodeTable_.end()) {
        ipToNodeId_.erase(it->second.ipAddress);
        nodeTable_.erase(it);
    }
}

bool HeartbeatManager::findNodeByIP(uint32_t ip, NodeID& outNodeId) const {
    std::lock_guard<std::mutex> lock(nodeTableMutex_);
    
    auto it = ipToNodeId_.find(ip);
    if (it != ipToNodeId_.end()) {
        outNodeId = it->second;
        return true;
    }
    return false;
}

std::map<NodeID, NodeInfo> HeartbeatManager::getAllNodes() const {
    std::lock_guard<std::mutex> lock(nodeTableMutex_);
    return nodeTable_;
}

bool HeartbeatManager::detectConflict(uint32_t sourceIP, const NodeID& senderNodeId, CSteamID& outConflictingSteamID) {
    std::lock_guard<std::mutex> lock(nodeTableMutex_);
    
    auto it = ipToNodeId_.find(sourceIP);
    if (it != ipToNodeId_.end() && it->second != senderNodeId) {
        // 发现冲突：同一个 IP 来自不同的 Node ID
        std::cout << "Packet-level conflict detected for IP" << std::endl;
        
        // 比较 Node ID，返回优先级较低的节点 Steam ID
        if (NodeIdentity::hasPriority(it->second, senderNodeId)) {
            // 原来记录的 Node ID 更大，新来的需要释放
            auto nodeIt = nodeTable_.find(senderNodeId);
            if (nodeIt != nodeTable_.end()) {
                outConflictingSteamID = nodeIt->second.steamId;
                return true;
            }
        } else {
            // 新来的 Node ID 更大，更新记录并返回原来的 Steam ID
            auto nodeIt = nodeTable_.find(it->second);
            if (nodeIt != nodeTable_.end()) {
                outConflictingSteamID = nodeIt->second.steamId;
                it->second = senderNodeId;
                return true;
            }
        }
    }
    
    return false;
}