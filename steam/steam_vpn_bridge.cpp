#include "steam_vpn_bridge.h"
#include "steam_networking_manager.h"
#include <iostream>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

SteamVpnBridge::SteamVpnBridge(SteamNetworkingManager* steamManager)
    : steamManager_(steamManager)
    , running_(false)
    , baseIP_(0)
    , subnetMask_(0)
    , localIP_(0)
{
    memset(&stats_, 0, sizeof(stats_));
}

SteamVpnBridge::~SteamVpnBridge() {
    stop();
}

bool SteamVpnBridge::start(const std::string& tunDeviceName,
                            const std::string& virtualSubnet,
                            const std::string& subnetMask) {
    if (running_) {
        std::cerr << "VPN bridge is already running" << std::endl;
        return false;
    }

    // 创建TUN设备
    tunDevice_ = tun::create_tun();
    if (!tunDevice_) {
        std::cerr << "Failed to create TUN device" << std::endl;
        return false;
    }

    // 打开TUN设备
    if (!tunDevice_->open(tunDeviceName, 1400)) { 
        std::cerr << "Failed to open TUN device: " << tunDevice_->get_last_error() << std::endl;
        return false;
    }

    std::cout << "TUN device created: " << tunDevice_->get_device_name() << std::endl;

    // 初始化IP地址池
    baseIP_ = stringToIp(virtualSubnet);
    if (baseIP_ == 0) {
        std::cerr << "Invalid virtual subnet: " << virtualSubnet << std::endl;
        return false;
    }
    subnetMask_ = stringToIp(subnetMask);

    // 初始化 IP 协商器
    CSteamID mySteamID = SteamUser()->GetSteamID();
    ipNegotiator_.initialize(mySteamID, baseIP_, subnetMask_);
    
    // 设置回调
    ipNegotiator_.setSendCallback(
        [this](VpnMessageType type, const uint8_t* payload, size_t len, HSteamNetConnection conn, bool reliable) {
            sendVpnMessage(type, payload, len, conn, reliable);
        },
        [this](VpnMessageType type, const uint8_t* payload, size_t len, bool reliable) {
            broadcastVpnMessage(type, payload, len, reliable);
        }
    );
    
    ipNegotiator_.setSuccessCallback(
        [this](uint32_t ip, const NodeID& nodeId) {
            onNegotiationSuccess(ip, nodeId);
        }
    );
    
    // 初始化心跳管理器
    heartbeatManager_.setSendCallback(
        [this](VpnMessageType type, const uint8_t* payload, size_t len, bool reliable) {
            broadcastVpnMessage(type, payload, len, reliable);
        }
    );
    
    heartbeatManager_.setNodeExpiredCallback(
        [this](const NodeID& nodeId, uint32_t ip) {
            onNodeExpired(nodeId, ip);
        }
    );

    // 开始IP协商
    ipNegotiator_.startNegotiation();

    // 设置非阻塞模式
    tunDevice_->set_non_blocking(true);

    // 启动处理线程
    running_ = true;
    tunReadThread_ = std::make_unique<std::thread>(&SteamVpnBridge::tunReadThread, this);

    std::cout << "Steam VPN bridge started successfully" << std::endl;
    return true;
}

void SteamVpnBridge::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    // 停止心跳管理器
    heartbeatManager_.stop();

    // 等待线程结束
    if (tunReadThread_ && tunReadThread_->joinable()) {
        tunReadThread_->join();
    }

    // 关闭TUN设备
    if (tunDevice_) {
        tunDevice_->close();
    }

    // 清理路由表
    {
        std::lock_guard<std::mutex> lock(routingMutex_);
        routingTable_.clear();
    }

    localIP_ = 0;

    std::cout << "Steam VPN bridge stopped" << std::endl;
}

std::string SteamVpnBridge::getLocalIP() const {
    if (localIP_ == 0) {
        return "Not assigned";
    }
    return ipToString(localIP_);
}

std::string SteamVpnBridge::getTunDeviceName() const {
    if (tunDevice_ && tunDevice_->is_open()) {
        return tunDevice_->get_device_name();
    }
    return "N/A";
}

std::map<uint32_t, RouteEntry> SteamVpnBridge::getRoutingTable() const {
    std::lock_guard<std::mutex> lock(routingMutex_);
    return routingTable_;
}

void SteamVpnBridge::tunReadThread() {
    std::cout << "TUN read thread started" << std::endl;
    
    uint8_t buffer[2048];
    
    while (running_) {
        // 从TUN设备读取数据包
        int bytesRead = tunDevice_->read(buffer, sizeof(buffer));
        
        if (bytesRead > 0) {
            // 提取目标IP
            uint32_t destIP = extractDestIP(buffer, bytesRead);
            
            // 封装VPN消息（包含 Node ID）
            std::vector<uint8_t> vpnPacket;
            VpnMessageHeader header;
            header.type = VpnMessageType::IP_PACKET;
            
            VpnPacketWrapper wrapper;
            wrapper.senderNodeId = ipNegotiator_.getLocalNodeID();
            
            size_t totalPayloadSize = sizeof(VpnPacketWrapper) + bytesRead;
            header.length = htons(static_cast<uint16_t>(totalPayloadSize));
            
            vpnPacket.resize(sizeof(VpnMessageHeader) + totalPayloadSize);
            memcpy(vpnPacket.data(), &header, sizeof(VpnMessageHeader));
            memcpy(vpnPacket.data() + sizeof(VpnMessageHeader), &wrapper, sizeof(VpnPacketWrapper));
            memcpy(vpnPacket.data() + sizeof(VpnMessageHeader) + sizeof(VpnPacketWrapper), buffer, bytesRead);

            ISteamNetworkingSockets* steamInterface = steamManager_->getInterface();
            
            if (isBroadcastAddress(destIP)) {
                // 广播包
                const auto& connections = steamManager_->getConnections();
                for (auto conn : connections) {
                    EResult result = steamInterface->SendMessageToConnection(
                        conn, vpnPacket.data(), static_cast<uint32_t>(vpnPacket.size()),
                        k_nSteamNetworkingSend_UnreliableNoNagle, nullptr);
                    
                    std::lock_guard<std::mutex> lock(statsMutex_);
                    if (result == k_EResultOK) {
                        stats_.packetsSent++;
                        stats_.bytesSent += bytesRead;
                    } else {
                        stats_.packetsDropped++;
                    }
                }
            } else {
                // 单播包
                HSteamNetConnection targetConn = k_HSteamNetConnection_Invalid;
                {
                    std::lock_guard<std::mutex> lock(routingMutex_);
                    auto it = routingTable_.find(destIP);
                    if (it != routingTable_.end()) {
                        targetConn = it->second.conn;
                    }
                }

                if (targetConn != k_HSteamNetConnection_Invalid) {
                    EResult result = steamInterface->SendMessageToConnection(
                        targetConn, vpnPacket.data(), static_cast<uint32_t>(vpnPacket.size()),
                        k_nSteamNetworkingSend_UnreliableNoNagle, nullptr);

                    std::lock_guard<std::mutex> lock(statsMutex_);
                    if (result == k_EResultOK) {
                        stats_.packetsSent++;
                        stats_.bytesSent += bytesRead;
                    } else {
                        stats_.packetsDropped++;
                    }
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // 检查协商超时
        ipNegotiator_.checkTimeout();
    }
    
    std::cout << "TUN read thread stopped" << std::endl;
}

void SteamVpnBridge::handleVpnMessage(const uint8_t* data, size_t length, HSteamNetConnection fromConn) {
    if (length < sizeof(VpnMessageHeader)) return;

    VpnMessageHeader header;
    memcpy(&header, data, sizeof(VpnMessageHeader));
    uint16_t payloadLength = ntohs(header.length);

    if (length < sizeof(VpnMessageHeader) + payloadLength) return;

    const uint8_t* payload = data + sizeof(VpnMessageHeader);
    
    // 获取发送者信息
    SteamNetConnectionInfo_t connInfo;
    CSteamID peerSteamID;
    std::string peerName;
    if (steamManager_->getInterface()->GetConnectionInfo(fromConn, &connInfo)) {
        peerSteamID = connInfo.m_identityRemote.GetSteamID();
        peerName = SteamFriends()->GetFriendPersonaName(peerSteamID);
    }

    switch (header.type) {
        case VpnMessageType::IP_PACKET:
            if (tunDevice_ && payloadLength > sizeof(VpnPacketWrapper)) {
                VpnPacketWrapper wrapper;
                memcpy(&wrapper, payload, sizeof(VpnPacketWrapper));
                
                const uint8_t* ipPacket = payload + sizeof(VpnPacketWrapper);
                size_t ipPacketLen = payloadLength - sizeof(VpnPacketWrapper);
                
                // 检测数据包级别冲突
                uint32_t sourceIP = extractSourceIP(ipPacket, ipPacketLen);
                if (sourceIP != 0) {
                    HSteamNetConnection conflictConn = heartbeatManager_.detectConflict(sourceIP, wrapper.senderNodeId);
                    if (conflictConn != k_HSteamNetConnection_Invalid) {
                        // 发送强制释放
                        ForcedReleasePayload forcePayload;
                        forcePayload.ipAddress = htonl(sourceIP);
                        forcePayload.winnerNodeId = ipNegotiator_.getLocalNodeID();
                        sendVpnMessage(VpnMessageType::FORCED_RELEASE,
                                       reinterpret_cast<const uint8_t*>(&forcePayload),
                                       sizeof(forcePayload), conflictConn, true);
                    }
                }
                
                // 检查目标 IP
                uint32_t destIP = extractDestIP(ipPacket, ipPacketLen);
                
                // 如果是发给本地的包，或者是广播包，写入 TUN 设备
                if (destIP == localIP_ || isBroadcastAddress(destIP)) {
                    tunDevice_->write(ipPacket, ipPacketLen);
                    std::lock_guard<std::mutex> lock(statsMutex_);
                    stats_.packetsReceived++;
                    stats_.bytesReceived += ipPacketLen;
                }
                // 如果是发给其他节点的包，转发（P2P 中继）
                else {
                    HSteamNetConnection targetConn = k_HSteamNetConnection_Invalid;
                    {
                        std::lock_guard<std::mutex> lock(routingMutex_);
                        auto it = routingTable_.find(destIP);
                        if (it != routingTable_.end() && !it->second.isLocal) {
                            targetConn = it->second.conn;
                        }
                    }
                    
                    if (targetConn != k_HSteamNetConnection_Invalid && targetConn != fromConn) {
                        // 转发原始消息
                        sendVpnMessage(VpnMessageType::IP_PACKET, payload, payloadLength, targetConn, false);
                    }
                }
            }
            break;

        case VpnMessageType::ROUTE_UPDATE: {
            bool hasNewRoutes = false;
            size_t offset = 0;
            while (offset + 12 <= payloadLength) {
                uint64_t steamID;
                uint32_t ipAddress;
                memcpy(&steamID, payload + offset, 8);
                memcpy(&ipAddress, payload + offset + 8, 4);
                ipAddress = ntohl(ipAddress);
                offset += 12;

                CSteamID csteamID(steamID);
                
                // 跳过自己的路由
                if (csteamID == SteamUser()->GetSteamID()) {
                    continue;
                }
                
                // 检查是否已经有这个路由
                bool isNewRoute = false;
                {
                    std::lock_guard<std::mutex> lock(routingMutex_);
                    auto it = routingTable_.find(ipAddress);
                    isNewRoute = (it == routingTable_.end());
                }
                
                if (!isNewRoute) {
                    continue; // 已有这个路由，跳过
                }
                
                hasNewRoutes = true;
                
                // 检查是否有直接连接到该节点
                HSteamNetConnection directConn = k_HSteamNetConnection_Invalid;
                const auto& connections = steamManager_->getConnections();
                for (auto c : connections) {
                    SteamNetConnectionInfo_t info;
                    if (steamManager_->getInterface()->GetConnectionInfo(c, &info)) {
                        if (info.m_identityRemote.GetSteamID() == csteamID) {
                            directConn = c;
                            break;
                        }
                    }
                }
                
                // 使用直接连接，如果没有则通过消息发送者中继
                HSteamNetConnection routeConn = (directConn != k_HSteamNetConnection_Invalid) ? directConn : fromConn;

                if ((ipAddress & subnetMask_) == (baseIP_ & subnetMask_)) {
                    NodeID nodeId = NodeIdentity::generate(csteamID);
                    updateRoute(nodeId, csteamID, ipAddress, routeConn, 
                                SteamFriends()->GetFriendPersonaName(csteamID));
                }
            }
            
            // 如果学到了新路由，广播给其他节点（不包括发送者）
            if (hasNewRoutes) {
                const auto& connections = steamManager_->getConnections();
                for (auto conn : connections) {
                    if (conn != fromConn) {
                        sendRouteUpdateTo(conn);
                    }
                }
            }
            break;
        }

        case VpnMessageType::PROBE_REQUEST: {
            if (payloadLength >= sizeof(ProbeRequestPayload)) {
                ProbeRequestPayload request;
                memcpy(&request, payload, sizeof(ProbeRequestPayload));
                ipNegotiator_.handleProbeRequest(request, fromConn);
            }
            break;
        }
            
        case VpnMessageType::PROBE_RESPONSE: {
            if (payloadLength >= sizeof(ProbeResponsePayload)) {
                ProbeResponsePayload response;
                memcpy(&response, payload, sizeof(ProbeResponsePayload));
                ipNegotiator_.handleProbeResponse(response, fromConn);
            }
            break;
        }
            
        case VpnMessageType::ADDRESS_ANNOUNCE: {
            if (payloadLength >= sizeof(AddressAnnouncePayload)) {
                AddressAnnouncePayload announce;
                memcpy(&announce, payload, sizeof(AddressAnnouncePayload));
                
                // 检查是否是新的路由
                uint32_t announcedIP = ntohl(announce.ipAddress);
                bool isNewRoute = false;
                {
                    std::lock_guard<std::mutex> lock(routingMutex_);
                    auto it = routingTable_.find(announcedIP);
                    isNewRoute = (it == routingTable_.end());
                }
                
                ipNegotiator_.handleAddressAnnounce(announce, fromConn, peerSteamID, peerName);
                
                // 更新路由表
                updateRoute(announce.nodeId, peerSteamID, announcedIP, fromConn, peerName);
                
                // 如果是新路由，广播整个路由表给所有人
                if (isNewRoute) {
                    broadcastRouteUpdate();
                }
            }
            break;
        }
            
        case VpnMessageType::FORCED_RELEASE: {
            if (payloadLength >= sizeof(ForcedReleasePayload)) {
                ForcedReleasePayload release;
                memcpy(&release, payload, sizeof(ForcedReleasePayload));
                ipNegotiator_.handleForcedRelease(release, fromConn);
            }
            break;
        }
            
        case VpnMessageType::HEARTBEAT: {
            if (payloadLength >= sizeof(HeartbeatPayload)) {
                HeartbeatPayload heartbeat;
                memcpy(&heartbeat, payload, sizeof(HeartbeatPayload));
                heartbeatManager_.handleHeartbeat(heartbeat, fromConn, peerSteamID, peerName);
            }
            break;
        }
            
        default:
            break;
    }
}

void SteamVpnBridge::onUserJoined(CSteamID steamID, HSteamNetConnection conn) {
    std::cout << "User joined: " << steamID.ConvertToUint64() << std::endl;
    
    // 如果本地已经有稳定的 IP，发送自己的地址宣布给新加入的用户
    if (ipNegotiator_.getState() == NegotiationState::STABLE) {
        ipNegotiator_.sendAddressAnnounceTo(conn);
        
        // 同时把完整的路由表发送给新用户
        sendRouteUpdateTo(conn);
    }
}

void SteamVpnBridge::onUserLeft(CSteamID steamID) {
    std::cout << "User left: " << steamID.ConvertToUint64() << std::endl;
    
    // 从路由表中移除
    std::lock_guard<std::mutex> lock(routingMutex_);
    for (auto it = routingTable_.begin(); it != routingTable_.end(); ) {
        if (it->second.steamID == steamID) {
            heartbeatManager_.unregisterNode(it->second.nodeId);
            ipNegotiator_.markIPUnused(it->first);
            it = routingTable_.erase(it);
        } else {
            ++it;
        }
    }
}

SteamVpnBridge::Statistics SteamVpnBridge::getStatistics() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void SteamVpnBridge::onNegotiationSuccess(uint32_t ipAddress, const NodeID& nodeId) {
    localIP_ = ipAddress;
    
    // 配置 TUN 设备
    std::string localIPStr = ipToString(localIP_);
    std::string subnetMaskStr = ipToString(subnetMask_);
    
    if (tunDevice_->set_ip(localIPStr, subnetMaskStr) && tunDevice_->set_up(true)) {
        // 添加本地路由
        CSteamID mySteamID = SteamUser()->GetSteamID();
        updateRoute(nodeId, mySteamID, localIP_, k_HSteamNetConnection_Invalid, 
                    SteamFriends()->GetPersonaName());
        
        // 初始化并启动心跳管理器
        heartbeatManager_.initialize(nodeId, localIP_);
        heartbeatManager_.registerNode(nodeId, mySteamID, localIP_, 
                                       k_HSteamNetConnection_Invalid, SteamFriends()->GetPersonaName());
        heartbeatManager_.start();
        
        broadcastRouteUpdate();
    } else {
        std::cerr << "Failed to configure TUN device IP." << std::endl;
    }
}

void SteamVpnBridge::onNodeExpired(const NodeID& nodeId, uint32_t ipAddress) {
    removeRoute(ipAddress);
    ipNegotiator_.markIPUnused(ipAddress);
}

void SteamVpnBridge::updateRoute(const NodeID& nodeId, CSteamID steamId, uint32_t ipAddress,
                                  HSteamNetConnection conn, const std::string& name) {
    RouteEntry entry;
    entry.steamID = steamId;
    entry.conn = conn;
    entry.ipAddress = ipAddress;
    entry.name = name;
    entry.isLocal = (conn == k_HSteamNetConnection_Invalid);
    entry.nodeId = nodeId;
    
    {
        std::lock_guard<std::mutex> lock(routingMutex_);
        // 移除该 SteamID 的旧条目
        for (auto it = routingTable_.begin(); it != routingTable_.end(); ) {
            if (it->second.steamID == steamId && it->first != ipAddress) {
                it = routingTable_.erase(it);
            } else {
                ++it;
            }
        }
        routingTable_[ipAddress] = entry;
    }
    
    // 标记 IP 为已使用
    ipNegotiator_.markIPUsed(ipAddress);
    
    std::cout << "Route updated: " << ipToString(ipAddress) << " -> " << name << std::endl;
}

void SteamVpnBridge::removeRoute(uint32_t ipAddress) {
    std::lock_guard<std::mutex> lock(routingMutex_);
    routingTable_.erase(ipAddress);
}

void SteamVpnBridge::broadcastRouteUpdate() {
    std::vector<uint8_t> message;
    std::vector<uint8_t> routeData;

    {
        std::lock_guard<std::mutex> lock(routingMutex_);
        for (const auto& entry : routingTable_) {
            uint64_t steamID = entry.second.steamID.ConvertToUint64();
            uint32_t ipAddress = htonl(entry.second.ipAddress);
            
            size_t offset = routeData.size();
            routeData.resize(offset + 12);
            memcpy(routeData.data() + offset, &steamID, 8);
            memcpy(routeData.data() + offset + 8, &ipAddress, 4);
        }
    }

    VpnMessageHeader header;
    header.type = VpnMessageType::ROUTE_UPDATE;
    header.length = htons(static_cast<uint16_t>(routeData.size()));

    message.resize(sizeof(VpnMessageHeader) + routeData.size());
    memcpy(message.data(), &header, sizeof(VpnMessageHeader));
    memcpy(message.data() + sizeof(VpnMessageHeader), routeData.data(), routeData.size());

    ISteamNetworkingSockets* steamInterface = steamManager_->getInterface();
    const auto& connections = steamManager_->getConnections();
    
    for (auto conn : connections) {
        steamInterface->SendMessageToConnection(conn, message.data(), 
            static_cast<uint32_t>(message.size()), k_nSteamNetworkingSend_Reliable, nullptr);
    }
}

void SteamVpnBridge::sendRouteUpdateTo(HSteamNetConnection targetConn) {
    std::vector<uint8_t> message;
    std::vector<uint8_t> routeData;

    {
        std::lock_guard<std::mutex> lock(routingMutex_);
        for (const auto& entry : routingTable_) {
            uint64_t steamID = entry.second.steamID.ConvertToUint64();
            uint32_t ipAddress = htonl(entry.second.ipAddress);
            
            size_t offset = routeData.size();
            routeData.resize(offset + 12);
            memcpy(routeData.data() + offset, &steamID, 8);
            memcpy(routeData.data() + offset + 8, &ipAddress, 4);
        }
    }

    VpnMessageHeader header;
    header.type = VpnMessageType::ROUTE_UPDATE;
    header.length = htons(static_cast<uint16_t>(routeData.size()));

    message.resize(sizeof(VpnMessageHeader) + routeData.size());
    memcpy(message.data(), &header, sizeof(VpnMessageHeader));
    memcpy(message.data() + sizeof(VpnMessageHeader), routeData.data(), routeData.size());

    steamManager_->getInterface()->SendMessageToConnection(targetConn, message.data(), 
        static_cast<uint32_t>(message.size()), k_nSteamNetworkingSend_Reliable, nullptr);
}

void SteamVpnBridge::sendVpnMessage(VpnMessageType type, const uint8_t* payload, 
                                     size_t payloadLength, HSteamNetConnection conn, bool reliable) {
    std::vector<uint8_t> message;
    VpnMessageHeader header;
    header.type = type;
    header.length = htons(static_cast<uint16_t>(payloadLength));
    
    message.resize(sizeof(VpnMessageHeader) + payloadLength);
    memcpy(message.data(), &header, sizeof(VpnMessageHeader));
    if (payloadLength > 0 && payload) {
        memcpy(message.data() + sizeof(VpnMessageHeader), payload, payloadLength);
    }
    
    int flags = reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_UnreliableNoNagle;
    steamManager_->getInterface()->SendMessageToConnection(
        conn, message.data(), static_cast<uint32_t>(message.size()), flags, nullptr);
}

void SteamVpnBridge::broadcastVpnMessage(VpnMessageType type, const uint8_t* payload, 
                                          size_t payloadLength, bool reliable) {
    const auto& connections = steamManager_->getConnections();
    for (auto conn : connections) {
        sendVpnMessage(type, payload, payloadLength, conn, reliable);
    }
}

std::string SteamVpnBridge::ipToString(uint32_t ip) {
    char buffer[INET_ADDRSTRLEN];
    struct in_addr addr;
    addr.s_addr = htonl(ip);
    inet_ntop(AF_INET, &addr, buffer, INET_ADDRSTRLEN);
    return std::string(buffer);
}

uint32_t SteamVpnBridge::stringToIp(const std::string& ipStr) {
    struct in_addr addr;
    if (inet_pton(AF_INET, ipStr.c_str(), &addr) == 1) {
        return ntohl(addr.s_addr);
    }
    return 0;
}

uint32_t SteamVpnBridge::extractDestIP(const uint8_t* packet, size_t length) {
    if (length < 20) return 0;
    uint8_t version = (packet[0] >> 4) & 0x0F;
    if (version != 4) return 0;

    uint32_t destIP;
    memcpy(&destIP, packet + 16, 4);
    return ntohl(destIP);
}

uint32_t SteamVpnBridge::extractSourceIP(const uint8_t* packet, size_t length) {
    if (length < 20) return 0;
    uint8_t version = (packet[0] >> 4) & 0x0F;
    if (version != 4) return 0;

    uint32_t srcIP;
    memcpy(&srcIP, packet + 12, 4);
    return ntohl(srcIP);
}

bool SteamVpnBridge::isBroadcastAddress(uint32_t ip) const {
    if (ip == 0xFFFFFFFF) return true;
    
    uint32_t subnetBroadcast = (baseIP_ & subnetMask_) | (~subnetMask_);
    if (ip == subnetBroadcast) return true;
    
    uint8_t firstOctet = (ip >> 24) & 0xFF;
    if (firstOctet >= 224 && firstOctet <= 239) return true;
    
    return false;
}
