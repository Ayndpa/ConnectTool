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
#include <algorithm>
#include <set>
#include <chrono>

SteamVpnBridge::SteamVpnBridge(SteamNetworkingManager* steamManager)
    : steamManager_(steamManager)
    , running_(false)
    , baseIP_(0)
    , subnetMask_(0)
    , nextIP_(0)
    , negotiationState_(IpNegotiationState::IDLE)
    , candidateIP_(0)
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
    if (!tunDevice_->open(tunDeviceName, 1400)) { // MTU设置为1400以留出Steam封装开销
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
    nextIP_ = baseIP_ + 1;  // .0 通常保留给网络地址

    // 开始IP协商流程
    startIpNegotiation();

    // 设置非阻塞模式
    tunDevice_->set_non_blocking(true);

    // 启动处理线程
    running_ = true;
    tunReadThread_ = std::make_unique<std::thread>(&SteamVpnBridge::tunReadThread, this);
    tunWriteThread_ = std::make_unique<std::thread>(&SteamVpnBridge::tunWriteThread, this);

    std::cout << "Steam VPN bridge started successfully" << std::endl;
    return true;
}

void SteamVpnBridge::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    // 等待线程结束
    if (tunReadThread_ && tunReadThread_->joinable()) {
        tunReadThread_->join();
    }
    if (tunWriteThread_ && tunWriteThread_->joinable()) {
        tunWriteThread_->join();
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

    // 清理IP分配
    {
        std::lock_guard<std::mutex> lock(ipAllocationMutex_);
        allocatedIPs_.clear();
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
            std::cout << "TUN read thread: Read " << bytesRead << " bytes from TUN device." << std::endl;
            // 提取目标IP
            uint32_t destIP = extractDestIP(buffer, bytesRead);
            
            if (destIP == 0) {
                // 无效的IP包
                std::cerr << "TUN read thread: Dropping invalid IP packet." << std::endl;
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.packetsDropped++;
                continue;
            }
            std::cout << "TUN read thread: Extracted destination IP: " << ipToString(destIP) << std::endl;

            // 查找路由
            HSteamNetConnection targetConn = k_HSteamNetConnection_Invalid;
            std::vector<HSteamNetConnection> broadcastConns;

            {
                std::lock_guard<std::mutex> lock(routingMutex_);
                auto it = routingTable_.find(destIP);
                if (it != routingTable_.end()) {
                    if (!it->second.isLocal) {
                        targetConn = it->second.conn;
                        std::cout << "TUN read thread: Found route for " << ipToString(destIP) << ", target connection: " << it->second.steamID.ConvertToUint64() << std::endl;
                    } else {
                        // 发送给本地，忽略
                        std::cout << "TUN read thread: Packet for local IP " << ipToString(destIP) << ", ignoring." << std::endl;
                        continue;
                    }
                } else {
                    // 如果是广播或未知目标，发送给所有连接
                    std::cout << "TUN read thread: No route found for destination IP: " << ipToString(destIP) << ", broadcasting to all peers." << std::endl;
                    
                    std::set<HSteamNetConnection> uniqueConns;
                    for (const auto& entry : routingTable_) {
                        if (!entry.second.isLocal && entry.second.conn != k_HSteamNetConnection_Invalid) {
                            uniqueConns.insert(entry.second.conn);
                        }
                    }
                    broadcastConns.assign(uniqueConns.begin(), uniqueConns.end());
                }
            }

            if (targetConn != k_HSteamNetConnection_Invalid || !broadcastConns.empty()) {
                // 封装VPN消息
                std::vector<uint8_t> vpnPacket;
                VpnMessageHeader header;
                header.type = VpnMessageType::IP_PACKET;
                header.length = htons(bytesRead);
                
                vpnPacket.resize(sizeof(VpnMessageHeader) + bytesRead);
                memcpy(vpnPacket.data(), &header, sizeof(VpnMessageHeader));
                memcpy(vpnPacket.data() + sizeof(VpnMessageHeader), buffer, bytesRead);

                // 通过Steam发送
                ISteamNetworkingSockets* steamInterface = steamManager_->getInterface();
                
                if (targetConn != k_HSteamNetConnection_Invalid) {
                    EResult result = steamInterface->SendMessageToConnection(
                        targetConn,
                        vpnPacket.data(),
                        vpnPacket.size(),
                        k_nSteamNetworkingSend_Reliable,
                        nullptr
                    );

                    if (result == k_EResultOK) {
                        std::cout << "TUN read thread: Successfully sent " << vpnPacket.size() << " bytes to Steam connection " << targetConn << std::endl;
                        std::lock_guard<std::mutex> lock(statsMutex_);
                        stats_.packetsSent++;
                        stats_.bytesSent += bytesRead;
                    } else {
                        std::cerr << "TUN read thread: Failed to send packet to Steam connection " << targetConn << ", result: " << result << std::endl;
                        std::lock_guard<std::mutex> lock(statsMutex_);
                        stats_.packetsDropped++;
                    }
                } else {
                    // 广播发送
                    int sentCount = 0;
                    for (auto conn : broadcastConns) {
                        EResult result = steamInterface->SendMessageToConnection(
                            conn,
                            vpnPacket.data(),
                            vpnPacket.size(),
                            k_nSteamNetworkingSend_Reliable,
                            nullptr
                        );
                        if (result == k_EResultOK) {
                            sentCount++;
                        }
                    }
                    
                    if (sentCount > 0) {
                        std::cout << "TUN read thread: Broadcasted packet to " << sentCount << " peers." << std::endl;
                        std::lock_guard<std::mutex> lock(statsMutex_);
                        stats_.packetsSent += sentCount;
                        stats_.bytesSent += bytesRead * sentCount;
                    }
                }
            }
        } else if (bytesRead < 0) {
            // 读取错误，等待一下再试
            std::cerr << "TUN read thread: Error reading from TUN device, bytesRead=" << bytesRead << ". Retrying soon." << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            // 没有数据，等待一下
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // 检查IP协商超时
        checkIpNegotiationTimeout();
    }
    
    std::cout << "TUN read thread stopped" << std::endl;
}

void SteamVpnBridge::tunWriteThread() {
    std::cout << "TUN write thread started" << std::endl;
    
    while (running_) {
        std::vector<OutgoingPacket> packetsToSend;
        
        std::lock_guard<std::mutex> lock(sendQueueMutex_);
        if (!sendQueue_.empty()) {
            packetsToSend = std::move(sendQueue_);
            sendQueue_.clear();
        }
        
        for (const auto& packet : packetsToSend) {
            // 写入TUN设备
            int bytesWritten = tunDevice_->write(packet.data.data(), packet.data.size());
            
            if (bytesWritten > 0) {
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.packetsReceived++;
                stats_.bytesReceived += bytesWritten;
                std::cout << "TUN write thread: Wrote " << bytesWritten << " bytes to TUN device." << std::endl;
            } else {
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.packetsDropped++;
                std::cerr << "TUN write thread: Failed to write packet to TUN device, bytesWritten=" << bytesWritten << std::endl;
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    std::cout << "TUN write thread stopped" << std::endl;
}

void SteamVpnBridge::handleVpnMessage(const uint8_t* data, size_t length, HSteamNetConnection fromConn) {
    if (length < sizeof(VpnMessageHeader)) {
        return;
    }

    VpnMessageHeader header;
    memcpy(&header, data, sizeof(VpnMessageHeader));
    uint16_t payloadLength = ntohs(header.length);

    if (length < sizeof(VpnMessageHeader) + payloadLength) {
        return;
    }

    const uint8_t* payload = data + sizeof(VpnMessageHeader);

    switch (header.type) {
        case VpnMessageType::IP_PACKET: {
            // 将IP包写入TUN设备
            OutgoingPacket packet;
            packet.data.resize(payloadLength);
            memcpy(packet.data.data(), payload, payloadLength);
            packet.targetConn = fromConn;

            std::lock_guard<std::mutex> lock(sendQueueMutex_);
            sendQueue_.push_back(std::move(packet));
            break;
        }



        case VpnMessageType::ROUTE_UPDATE: {
            // 路由表更新
            size_t offset = 0;
            bool anyUpdate = false;
            while (offset + 12 <= payloadLength) {  // 12 = 8 (SteamID) + 4 (IP)
                uint64_t steamID;
                uint32_t ipAddress;
                memcpy(&steamID, payload + offset, 8);
                memcpy(&ipAddress, payload + offset + 8, 4);
                ipAddress = ntohl(ipAddress);
                offset += 12;

                CSteamID csteamID(steamID);
                
                // 查找连接
                HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
                const auto& connections = steamManager_->getConnections();
                for (auto c : connections) {
                    SteamNetConnectionInfo_t info;
                    if (steamManager_->getInterface()->GetConnectionInfo(c, &info)) {
                        if (info.m_identityRemote.GetSteamID() == csteamID) {
                            conn = c;
                            break;
                        }
                    }
                }

                if (conn != k_HSteamNetConnection_Invalid) {
                    // Validate IP against our subnet
                    if ((ipAddress & subnetMask_) != (baseIP_ & subnetMask_)) {
                        std::cerr << "Ignoring route update for IP " << ipToString(ipAddress) 
                                  << " (SteamID " << csteamID.ConvertToUint64() << ")"
                                  << " because it does not match our subnet." << std::endl;
                        continue;
                    }

                    RouteEntry entry;
                    entry.steamID = csteamID;
                    entry.conn = conn;
                    entry.ipAddress = ipAddress;
                    entry.name = SteamFriends()->GetFriendPersonaName(csteamID);
                    entry.isLocal = false;

                    std::lock_guard<std::mutex> lock(routingMutex_);
                    routingTable_[ipAddress] = entry;
                    anyUpdate = true;
                    
                    std::cout << "Route updated: " << ipToString(ipAddress) 
                              << " -> " << entry.name << std::endl;
                }
            }
            if (anyUpdate) {
                printRoutingTable();
            }
            break;
        }

        case VpnMessageType::PING:
        case VpnMessageType::PONG:
            // 心跳包处理（暂时忽略）
            break;

        case VpnMessageType::IP_PROBE:
            handleIpProbe(payload, payloadLength, fromConn);
            break;

        case VpnMessageType::IP_CONFLICT:
            handleIpConflict(payload, payloadLength, fromConn);
            break;
    }
}

void SteamVpnBridge::onUserJoined(CSteamID steamID, HSteamNetConnection conn) {
    std::cout << "User joined: " << steamID.ConvertToUint64() << ". Negotiating IP..." << std::endl;

    // 1. Calculate deterministic IP
    uint32_t seedIP = generateIPFromSteamID(steamID);
    uint32_t ip = findNextAvailableIP(seedIP);

    // 2. Add to routing table immediately (Optimistic)
    {
        std::lock_guard<std::mutex> lock(routingMutex_);
        
        // Remove any existing entries for this SteamID to avoid duplicates
        for (auto it = routingTable_.begin(); it != routingTable_.end(); ) {
            if (it->second.steamID == steamID) {
                it = routingTable_.erase(it);
            } else {
                ++it;
            }
        }

        RouteEntry entry;
        entry.steamID = steamID;
        entry.conn = conn;
        entry.ipAddress = ip;
        entry.name = SteamFriends()->GetFriendPersonaName(steamID);
        entry.isLocal = false;
        routingTable_[ip] = entry;
    }
    std::cout << "Optimistically assigned IP " << ipToString(ip) << " to user " << steamID.ConvertToUint64() << std::endl;
    printRoutingTable();

    // 3. Broadcast route update
    broadcastRouteUpdate();
}

void SteamVpnBridge::onUserLeft(CSteamID steamID) {
    // 从路由表中移除
    uint32_t ipToRemove = 0;
    {
        std::lock_guard<std::mutex> lock(routingMutex_);
        for (auto it = routingTable_.begin(); it != routingTable_.end(); ++it) {
            if (it->second.steamID == steamID) {
                ipToRemove = it->first;
                routingTable_.erase(it);
                break;
            }
        }
    }

    if (ipToRemove != 0) {
        std::cout << "Removed route for IP " << ipToString(ipToRemove) 
                  << " from user " << steamID.ConvertToUint64() << std::endl;
        printRoutingTable();
    }
}

SteamVpnBridge::Statistics SteamVpnBridge::getStatistics() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}



void SteamVpnBridge::broadcastRouteUpdate() {
    // 构建路由更新消息
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
    header.length = htons(routeData.size());

    message.resize(sizeof(VpnMessageHeader) + routeData.size());
    memcpy(message.data(), &header, sizeof(VpnMessageHeader));
    memcpy(message.data() + sizeof(VpnMessageHeader), routeData.data(), routeData.size());

    // 发送给所有连接
    ISteamNetworkingSockets* steamInterface = steamManager_->getInterface();
    const auto& connections = steamManager_->getConnections();
    
    for (auto conn : connections) {
        steamInterface->SendMessageToConnection(
            conn,
            message.data(),
            message.size(),
            k_nSteamNetworkingSend_Reliable,
            nullptr
        );
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
    std::cerr << "Failed to convert IP string '" << ipStr << "' to integer. Error: " << errno << std::endl;
    return 0;
}

uint32_t SteamVpnBridge::extractDestIP(const uint8_t* packet, size_t length) {
    // IPv4包头最小20字节
    if (length < 20) {
        return 0;
    }

    // 检查IP版本
    uint8_t version = (packet[0] >> 4) & 0x0F;
    if (version != 4) {
        return 0;  // 只支持IPv4
    }

    // 目标IP在偏移16-19字节
    uint32_t destIP;
    memcpy(&destIP, packet + 16, 4);
    return ntohl(destIP);
}

uint32_t SteamVpnBridge::extractSourceIP(const uint8_t* packet, size_t length) {
    // IPv4包头最小20字节
    if (length < 20) {
        return 0;
    }

    // 检查IP版本
    uint8_t version = (packet[0] >> 4) & 0x0F;
    if (version != 4) {
        return 0;  // 只支持IPv4
    }

    // 源IP在偏移12-15字节
    uint32_t srcIP;
    memcpy(&srcIP, packet + 12, 4);
    return ntohl(srcIP);
}

uint32_t SteamVpnBridge::generateIPFromSteamID(CSteamID steamID) {
    // 使用SteamID的低32位来生成IP地址
    uint64_t steamID64 = steamID.ConvertToUint64();
    uint32_t hash = static_cast<uint32_t>(steamID64 ^ (steamID64 >> 32));
    
    // 确保IP在子网范围内，并且不是网络地址或广播地址
    uint32_t maxOffset = (~subnetMask_) - 1;  // 减1避免广播地址
    uint32_t offset = (hash % maxOffset) + 1;  // 加1避免网络地址
    
    uint32_t ip = (baseIP_ & subnetMask_) | offset;
    
    return ip;
}

void SteamVpnBridge::startIpNegotiation() {
    std::lock_guard<std::mutex> lock(ipAllocationMutex_);
    
    CSteamID mySteamID = SteamUser()->GetSteamID();
    uint32_t seedIP;
    
    // 1. 确定起始偏移量
    if (negotiationState_ == IpNegotiationState::IDLE || candidateIP_ == 0) {
        // 第一次协商，从Hash开始
        seedIP = generateIPFromSteamID(mySteamID);
    } else {
        // 重试，从当前+1开始
        uint32_t currentOffset = candidateIP_ & ~subnetMask_;
        uint32_t nextOffset = currentOffset + 1;
        if (nextOffset > 253) nextOffset = 1;
        seedIP = (baseIP_ & subnetMask_) | nextOffset;
    }
    
    negotiationState_ = IpNegotiationState::NEGOTIATING;
    
    // 2. 计算候选IP（本地冲突检测）
    candidateIP_ = findNextAvailableIP(seedIP);
    
    std::cout << "Starting IP negotiation. Candidate IP: " << ipToString(candidateIP_) << std::endl;
    
    // 4. 广播探测包
    // Payload: { Target_IP (4 bytes), My_SteamID (8 bytes) }
    std::vector<uint8_t> payload(12);
    uint32_t ipNet = htonl(candidateIP_);
    uint64_t sid = mySteamID.ConvertToUint64();
    memcpy(payload.data(), &ipNet, 4);
    memcpy(payload.data() + 4, &sid, 8);
    
    std::vector<uint8_t> message;
    VpnMessageHeader header;
    header.type = VpnMessageType::IP_PROBE;
    header.length = htons(payload.size());
    
    message.resize(sizeof(VpnMessageHeader) + payload.size());
    memcpy(message.data(), &header, sizeof(VpnMessageHeader));
    memcpy(message.data() + sizeof(VpnMessageHeader), payload.data(), payload.size());
    
    // 广播
    ISteamNetworkingSockets* steamInterface = steamManager_->getInterface();
    const auto& connections = steamManager_->getConnections();
    for (auto conn : connections) {
        steamInterface->SendMessageToConnection(conn, message.data(), message.size(), k_nSteamNetworkingSend_Reliable, nullptr);
    }
    
    probeStartTime_ = std::chrono::steady_clock::now();
}

void SteamVpnBridge::checkIpNegotiationTimeout() {
    if (negotiationState_ != IpNegotiationState::NEGOTIATING) return;
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - probeStartTime_).count();
    
    if (elapsed > 300) { // 300ms 等待窗口
        std::cout << "IP negotiation success. Assigned IP: " << ipToString(candidateIP_) << std::endl;
        
        negotiationState_ = IpNegotiationState::STABLE;
        localIP_ = candidateIP_;
        
        // 配置TUN设备
        std::string localIPStr = ipToString(localIP_);
        std::string maskStr = ipToString(subnetMask_);
        
        if (tunDevice_->set_ip(localIPStr, maskStr) && tunDevice_->set_up()) {
             // 添加本地路由
            RouteEntry localRoute;
            localRoute.steamID = SteamUser()->GetSteamID();
            localRoute.conn = k_HSteamNetConnection_Invalid;
            localRoute.ipAddress = localIP_;
            localRoute.name = SteamFriends()->GetPersonaName();
            localRoute.isLocal = true;

            {
                std::lock_guard<std::mutex> lock(routingMutex_);
                routingTable_[localIP_] = localRoute;
            }
            
            // 广播路由更新，让别人知道我占用了这个IP
            broadcastRouteUpdate();
            printRoutingTable();
        } else {
             std::cerr << "Failed to configure TUN device with negotiated IP." << std::endl;
        }
    }
}

void SteamVpnBridge::handleIpProbe(const uint8_t* data, size_t length, HSteamNetConnection fromConn) {
    if (length < 12) return;
    
    uint32_t targetIP;
    uint64_t peerSteamIDVal;
    memcpy(&targetIP, data, 4);
    memcpy(&peerSteamIDVal, data + 4, 8);
    targetIP = ntohl(targetIP);
    
    // 检查是否冲突
    bool conflict = false;
    
    if (negotiationState_ == IpNegotiationState::STABLE) {
        if (targetIP == localIP_) {
            conflict = true;
        }
    } else if (negotiationState_ == IpNegotiationState::NEGOTIATING) {
        if (targetIP == candidateIP_) {
            // 同时探测，进行仲裁
            CSteamID mySteamID = SteamUser()->GetSteamID();
            if (mySteamID.ConvertToUint64() > peerSteamIDVal) {
                // 我赢了，忽略对方请求
                std::cout << "IP Probe collision for " << ipToString(targetIP) << ". I win (MyID > PeerID). Ignoring peer." << std::endl;
                return; 
            } else {
                // 我输了，放弃当前IP，重新协商
                std::cout << "IP Probe collision for " << ipToString(targetIP) << ". I lose (MyID < PeerID). Retrying." << std::endl;
                startIpNegotiation(); // 这会重新计算并广播
                return;
            }
        }
    }
    
    if (conflict) {
        std::cout << "Received IP Probe for my IP " << ipToString(targetIP) << " from " << peerSteamIDVal << ". Sending Conflict." << std::endl;
        
        // 发送冲突响应
        std::vector<uint8_t> payload(4);
        uint32_t ipNet = htonl(targetIP);
        memcpy(payload.data(), &ipNet, 4);
        
        std::vector<uint8_t> message;
        VpnMessageHeader header;
        header.type = VpnMessageType::IP_CONFLICT;
        header.length = htons(payload.size());
        
        message.resize(sizeof(VpnMessageHeader) + payload.size());
        memcpy(message.data(), &header, sizeof(VpnMessageHeader));
        memcpy(message.data() + sizeof(VpnMessageHeader), payload.data(), payload.size());
        
        ISteamNetworkingSockets* steamInterface = steamManager_->getInterface();
        steamInterface->SendMessageToConnection(fromConn, message.data(), message.size(), k_nSteamNetworkingSend_Reliable, nullptr);
    }
}

void SteamVpnBridge::handleIpConflict(const uint8_t* data, size_t length, HSteamNetConnection fromConn) {
    if (length < 4) return;
    
    uint32_t conflictIP;
    memcpy(&conflictIP, data, 4);
    conflictIP = ntohl(conflictIP);
    
    if (negotiationState_ == IpNegotiationState::NEGOTIATING && conflictIP == candidateIP_) {
        std::cout << "Received IP Conflict for candidate IP " << ipToString(conflictIP) << ". Retrying..." << std::endl;
        startIpNegotiation(); 
    } else    if (negotiationState_ == IpNegotiationState::STABLE && conflictIP == localIP_) {
        std::cout << "Received IP Conflict for my stable IP " << ipToString(conflictIP) << ". Yielding and restarting negotiation." << std::endl;
        startIpNegotiation();
    }
}

void SteamVpnBridge::printRoutingTable() {
    std::lock_guard<std::mutex> lock(routingMutex_);
    std::cout << "\n=== Current VPN Routing Table ===" << std::endl;
    std::cout << "Total entries: " << routingTable_.size() << std::endl;
    for (const auto& entry : routingTable_) {
        std::cout << "IP: " << ipToString(entry.first) 
                  << " | Name: " << entry.second.name 
                  << " | SteamID: " << entry.second.steamID.ConvertToUint64() 
                  << " | Local: " << (entry.second.isLocal ? "Yes" : "No") 
                  << std::endl;
    }
    std::cout << "=================================\n" << std::endl;
}

uint32_t SteamVpnBridge::findNextAvailableIP(uint32_t startIP) {
    std::set<uint32_t> usedIPs;
    {
        std::lock_guard<std::mutex> routeLock(routingMutex_);
        for (const auto& entry : routingTable_) {
            usedIPs.insert(entry.second.ipAddress);
        }
    }

    uint32_t offset = startIP & ~subnetMask_;
    if (offset == 0 || offset > 253) offset = 1;
    
    uint32_t potentialIP = (baseIP_ & subnetMask_) | offset;
    
    // 循环查找未被占用的IP
    int attempts = 0;
    while (usedIPs.count(potentialIP) && attempts < 254) {
        offset++;
        if (offset > 253) offset = 1;
        potentialIP = (baseIP_ & subnetMask_) | offset;
        attempts++;
    }
    
    return potentialIP;
}
