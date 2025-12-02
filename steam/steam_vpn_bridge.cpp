#include "steam_vpn_bridge.h"
#include "steam_networking_manager.h"
#include <iostream>
#include <cstring>
#include <sstream> // 新增: 用于构建命令字符串
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <algorithm>
#include <set>
#include <chrono>
#include <cstdlib>
#include <thread>

SteamVpnBridge::SteamVpnBridge(SteamNetworkingManager* steamManager)
    : steamManager_(steamManager)
    , running_(false)
    , baseIP_(0)
    , subnetMask_(0)
    , nextIP_(0)
    , negotiationState_(IpNegotiationState::IDLE)
    , candidateIP_(0)
    , localIP_(0) // 初始化
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
    // MTU 1400 是个安全值，Steam P2P 也就是 ~1200-1300 字节的有效载荷
    if (!tunDevice_->open(tunDeviceName, 1400)) { 
        std::cerr << "Failed to open TUN device: " << tunDevice_->get_last_error() << std::endl;
        return false;
    }

    std::cout << "TUN device created: " << tunDevice_->get_device_name() << std::endl;
    std::cout << "IMPORTANT: Pinging your own IP (Self) will NOT generate packets in the TUN device." << std::endl;
    std::cout << "Please ping other IPs in the subnet to test connectivity." << std::endl;

    // 初始化IP地址池
    baseIP_ = stringToIp(virtualSubnet);
    if (baseIP_ == 0) {
        std::cerr << "Invalid virtual subnet: " << virtualSubnet << std::endl;
        return false;
    }
    subnetMask_ = stringToIp(subnetMask);
    nextIP_ = baseIP_ + 1; 

    // 开始IP协商流程
    startIpNegotiation();

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
            // 提取目标IP
            uint32_t destIP = extractDestIP(buffer, bytesRead);
            
            // 封装VPN消息
            std::vector<uint8_t> vpnPacket;
            VpnMessageHeader header;
            header.type = VpnMessageType::IP_PACKET;
            header.length = htons(static_cast<uint16_t>(bytesRead));
            
            vpnPacket.resize(sizeof(VpnMessageHeader) + bytesRead);
            memcpy(vpnPacket.data(), &header, sizeof(VpnMessageHeader));
            memcpy(vpnPacket.data() + sizeof(VpnMessageHeader), buffer, bytesRead);

            // 通过Steam发送
            ISteamNetworkingSockets* steamInterface = steamManager_->getInterface();
            
            // 检查是否是广播地址
            if (isBroadcastAddress(destIP)) {
                // 广播包：转发给所有连接
                const auto& connections = steamManager_->getConnections();
                
                for (auto conn : connections) {
                    EResult result = steamInterface->SendMessageToConnection(
                        conn,
                        vpnPacket.data(),
                        static_cast<uint32_t>(vpnPacket.size()),
                        k_nSteamNetworkingSend_UnreliableNoNagle,
                        nullptr
                    );
                    
                    std::lock_guard<std::mutex> lock(statsMutex_);
                    if (result == k_EResultOK) {
                        stats_.packetsSent++;
                        stats_.bytesSent += bytesRead;
                    } else {
                        stats_.packetsDropped++;
                    }
                }
            } else {
                // 单播包：查找路由并转发
                HSteamNetConnection targetConn = k_HSteamNetConnection_Invalid;
                bool routeFound = false;
                
                {
                    std::lock_guard<std::mutex> lock(routingMutex_);
                    auto it = routingTable_.find(destIP);
                    if (it != routingTable_.end()) {
                        targetConn = it->second.conn;
                        routeFound = true;
                    }
                }

                if (!routeFound) {
                    continue;
                }

                if (targetConn == k_HSteamNetConnection_Invalid) {
                    continue;
                }

                // 单播发送
                EResult result = steamInterface->SendMessageToConnection(
                    targetConn,
                    vpnPacket.data(),
                    static_cast<uint32_t>(vpnPacket.size()),
                    k_nSteamNetworkingSend_UnreliableNoNagle,
                    nullptr
                );

                std::lock_guard<std::mutex> lock(statsMutex_);
                if (result == k_EResultOK) {
                    stats_.packetsSent++;
                    stats_.bytesSent += bytesRead;
                } else {
                    stats_.packetsDropped++;
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        checkIpNegotiationTimeout();
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

    switch (header.type) {
        case VpnMessageType::IP_PACKET:
            if (tunDevice_) {
                tunDevice_->write(payload, payloadLength);
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.packetsReceived++;
                stats_.bytesReceived += payloadLength;
            }
            break;

        case VpnMessageType::ROUTE_UPDATE: {
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
                    if ((ipAddress & subnetMask_) != (baseIP_ & subnetMask_)) {
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
                    
                    std::cout << "Route learned: " << ipToString(ipAddress) 
                              << " -> " << entry.name << std::endl;
                }
            }
            if (anyUpdate) {
                printRoutingTable();
            }
            break;
        }

        case VpnMessageType::IP_PROBE:
            handleIpProbe(payload, payloadLength, fromConn);
            break;

        case VpnMessageType::IP_CONFLICT:
            handleIpConflict(payload, payloadLength, fromConn);
            break;
            
        default:
            break;
    }
}

void SteamVpnBridge::onUserJoined(CSteamID steamID, HSteamNetConnection conn) {
    std::cout << "User joined: " << steamID.ConvertToUint64() << ". Calculating IP..." << std::endl;

    uint32_t seedIP = generateIPFromSteamID(steamID);
    uint32_t ip = findNextAvailableIP(seedIP);

    std::lock_guard<std::mutex> lock(routingMutex_);
    
    // Remove existing
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
    std::cout << "Assigned IP " << ipToString(ip) << " to user " << steamID.ConvertToUint64() << std::endl;
    printRoutingTable();

    broadcastRouteUpdate();
}

void SteamVpnBridge::onUserLeft(CSteamID steamID) {
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
}

SteamVpnBridge::Statistics SteamVpnBridge::getStatistics() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void SteamVpnBridge::broadcastRouteUpdate() {
    std::vector<uint8_t> message;
    std::vector<uint8_t> routeData;

    std::lock_guard<std::mutex> lock(routingMutex_);
    for (const auto& entry : routingTable_) {
        uint64_t steamID = entry.second.steamID.ConvertToUint64();
        uint32_t ipAddress = htonl(entry.second.ipAddress);
        
        size_t offset = routeData.size();
        routeData.resize(offset + 12);
        memcpy(routeData.data() + offset, &steamID, 8);
        memcpy(routeData.data() + offset + 8, &ipAddress, 4);
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
        steamInterface->SendMessageToConnection(
            conn,
            message.data(),
            static_cast<uint32_t>(message.size()),
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
    return 0;
}

uint32_t SteamVpnBridge::extractDestIP(const uint8_t* packet, size_t length) {
    if (length < 20) return 0;
    // 检查IP版本
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
    // 全1广播地址 255.255.255.255
    if (ip == 0xFFFFFFFF) {
        return true;
    }
    
    // 子网广播地址（如 10.0.0.255 对于 10.0.0.0/24）
    uint32_t subnetBroadcast = (baseIP_ & subnetMask_) | (~subnetMask_);
    if (ip == subnetBroadcast) {
        return true;
    }
    
    // 多播地址 224.0.0.0 - 239.255.255.255 (224.0.0.0/4)
    // 第一个字节在 224-239 范围内
    uint8_t firstOctet = (ip >> 24) & 0xFF;
    if (firstOctet >= 224 && firstOctet <= 239) {
        return true;
    }
    
    return false;
}

uint32_t SteamVpnBridge::generateIPFromSteamID(CSteamID steamID) {
    uint64_t steamID64 = steamID.ConvertToUint64();
    uint32_t hash = static_cast<uint32_t>(steamID64 ^ (steamID64 >> 32));
    
    // 避免 .0 (Network) 和 .255 (Broadcast)
    uint32_t maxOffset = (~subnetMask_) - 1; 
    uint32_t offset = (hash % maxOffset) + 1;
    
    uint32_t ip = (baseIP_ & subnetMask_) | offset;
    return ip;
}

void SteamVpnBridge::startIpNegotiation() {
    std::lock_guard<std::mutex> lock(ipAllocationMutex_);
    
    CSteamID mySteamID = SteamUser()->GetSteamID();
    uint32_t seedIP;
    
    if (negotiationState_ == IpNegotiationState::IDLE || candidateIP_ == 0) {
        seedIP = generateIPFromSteamID(mySteamID);
    } else {
        uint32_t currentOffset = candidateIP_ & ~subnetMask_;
        uint32_t nextOffset = currentOffset + 1;
        if (nextOffset > 253) nextOffset = 1;
        seedIP = (baseIP_ & subnetMask_) | nextOffset;
    }
    
    negotiationState_ = IpNegotiationState::NEGOTIATING;
    candidateIP_ = findNextAvailableIP(seedIP);
    
    std::cout << "Negotiating IP: " << ipToString(candidateIP_) << std::endl;
    
    // 构建探测包
    std::vector<uint8_t> payload(12);
    uint32_t ipNet = htonl(candidateIP_);
    uint64_t sid = mySteamID.ConvertToUint64();
    memcpy(payload.data(), &ipNet, 4);
    memcpy(payload.data() + 4, &sid, 8);
    
    std::vector<uint8_t> message;
    VpnMessageHeader header;
    header.type = VpnMessageType::IP_PROBE;
    header.length = htons(static_cast<uint16_t>(payload.size()));
    
    message.resize(sizeof(VpnMessageHeader) + payload.size());
    memcpy(message.data(), &header, sizeof(VpnMessageHeader));
    memcpy(message.data() + sizeof(VpnMessageHeader), payload.data(), payload.size());
    
    ISteamNetworkingSockets* steamInterface = steamManager_->getInterface();
    const auto& connections = steamManager_->getConnections();
    for (auto conn : connections) {
        steamInterface->SendMessageToConnection(conn, message.data(), static_cast<uint32_t>(message.size()), k_nSteamNetworkingSend_Reliable, nullptr);
    }
    
    probeStartTime_ = std::chrono::steady_clock::now();
}

void SteamVpnBridge::checkIpNegotiationTimeout() {
    if (negotiationState_ != IpNegotiationState::NEGOTIATING) return;
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - probeStartTime_).count();
    
    // 300ms 等待窗口
    if (elapsed > 300) { 
        std::cout << "IP negotiation success. Local IP: " << ipToString(candidateIP_) << std::endl;
        
        negotiationState_ = IpNegotiationState::STABLE;
        localIP_ = candidateIP_;
        
        // 配置TUN设备
        std::string localIPStr = ipToString(localIP_);
        std::string subnetMaskStr = ipToString(subnetMask_); 
        
        // 【关键修改】不要使用 255.255.255.255 作为掩码，否则Windows不知道该网卡属于哪个网段
        // 使用实际的子网掩码（例如 255.255.255.0），这样当你 ping 网段内其他不存在的IP时，
        // 流量也会被路由到 TUN 设备，方便你在 tunReadThread 中进行测试。
        if (tunDevice_->set_ip(localIPStr, subnetMaskStr) && tunDevice_->set_up(true)) {
            
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
            
            broadcastRouteUpdate();
        } else {
             std::cerr << "Failed to configure TUN device IP." << std::endl;
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
    
    bool conflict = false;
    
    if (negotiationState_ == IpNegotiationState::STABLE) {
        if (targetIP == localIP_) {
            conflict = true;
        }
    } else if (negotiationState_ == IpNegotiationState::NEGOTIATING) {
        if (targetIP == candidateIP_) {
            CSteamID mySteamID = SteamUser()->GetSteamID();
            if (mySteamID.ConvertToUint64() > peerSteamIDVal) {
                return; // 我赢了
            } else {
                startIpNegotiation(); // 我输了，重试
                return;
            }
        }
    }
    
    if (conflict) {
        std::vector<uint8_t> payload(4);
        uint32_t ipNet = htonl(targetIP);
        memcpy(payload.data(), &ipNet, 4);
        
        std::vector<uint8_t> message;
        VpnMessageHeader header;
        header.type = VpnMessageType::IP_CONFLICT;
        header.length = htons(static_cast<uint16_t>(payload.size()));
        
        message.resize(sizeof(VpnMessageHeader) + payload.size());
        memcpy(message.data(), &header, sizeof(VpnMessageHeader));
        memcpy(message.data() + sizeof(VpnMessageHeader), payload.data(), payload.size());
        
        steamManager_->getInterface()->SendMessageToConnection(fromConn, message.data(), static_cast<uint32_t>(message.size()), k_nSteamNetworkingSend_Reliable, nullptr);
    }
}

void SteamVpnBridge::handleIpConflict(const uint8_t* data, size_t length, HSteamNetConnection fromConn) {
    if (length < 4) return;
    uint32_t conflictIP;
    memcpy(&conflictIP, data, 4);
    conflictIP = ntohl(conflictIP);
    
    if ((negotiationState_ == IpNegotiationState::NEGOTIATING && conflictIP == candidateIP_) ||
        (negotiationState_ == IpNegotiationState::STABLE && conflictIP == localIP_)) {
        startIpNegotiation();
    }
}

void SteamVpnBridge::printRoutingTable() {
    std::lock_guard<std::mutex> lock(routingMutex_);
    // 为了防止刷屏，可以注释掉
    // std::cout << "Routing Table Size: " << routingTable_.size() << std::endl;
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
    
    int attempts = 0;
    while (usedIPs.count(potentialIP) && attempts < 254) {
        offset++;
        if (offset > 253) offset = 1;
        potentialIP = (baseIP_ & subnetMask_) | offset;
        attempts++;
    }
    
    return potentialIP;
}