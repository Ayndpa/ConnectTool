#include "connect_tool_core.h"
#include <iostream>
#include <algorithm>

ConnectToolCore::ConnectToolCore() {
}

ConnectToolCore::~ConnectToolCore() {
    shutdown();
}

bool ConnectToolCore::initSteam() {
    if (steamInitialized) return true;

    if (!SteamAPI_Init()) {
        std::cerr << "SteamAPI_Init() failed." << std::endl;
        return false;
    }

    steamManager = std::make_unique<SteamNetworkingManager>();
    if (!steamManager->initialize()) {
        std::cerr << "Failed to initialize Steam Networking Manager" << std::endl;
        return false;
    }

    roomManager = std::make_unique<SteamRoomManager>(steamManager.get());
    vpnBridge = std::make_unique<SteamVpnBridge>(steamManager.get());
    steamManager->setVpnBridge(vpnBridge.get());

    steamManager->startMessageHandler();
    steamInitialized = true;
    return true;
}

void ConnectToolCore::shutdown() {
    if (vpnBridge) {
        vpnBridge->stop();
    }
    if (steamManager) {
        steamManager->stopMessageHandler();
        steamManager->shutdown();
        steamManager.reset();
    }
    if (steamInitialized) {
        SteamAPI_Shutdown();
        steamInitialized = false;
    }
}

void ConnectToolCore::update() {
    if (steamInitialized) {
        SteamAPI_RunCallbacks();
    }
}

bool ConnectToolCore::createLobby(std::string& outLobbyId) {
    if (!roomManager) return false;
    roomManager->createLobby();
    // Note: createLobby is async, so we might not have the ID immediately.
    // However, for the purpose of this core wrapper, we might need to wait or just return success initiating.
    // The current implementation of createLobby in SteamRoomManager seems to trigger a callback.
    // For now, we return true if the request was sent.
    // Ideally we should wait for the callback or have a way to check status.
    // But let's assume the user will poll GetLobbyInfo.
    return true; 
}

bool ConnectToolCore::joinLobby(const std::string& lobbyIdStr) {
    if (!roomManager) return false;
    try {
        unsigned long long roomId = std::stoull(lobbyIdStr);
        CSteamID lobbyID(roomId);
        if (lobbyID.IsValid() && lobbyID.IsLobby()) {
            roomManager->joinLobby(lobbyID);
            return true;
        }
    } catch (...) {
        return false;
    }
    return false;
}

void ConnectToolCore::leaveLobby() {
    if (roomManager) {
        roomManager->leaveLobby();
    }
    if (steamManager) {
        steamManager->disconnect();
    }
}

bool ConnectToolCore::isInLobby() const {
    return roomManager && roomManager->getCurrentLobby().IsValid();
}

CSteamID ConnectToolCore::getCurrentLobbyId() const {
    if (roomManager) return roomManager->getCurrentLobby();
    return CSteamID();
}

std::vector<CSteamID> ConnectToolCore::getLobbyMembers() const {
    if (roomManager) return roomManager->getLobbyMembers();
    return {};
}

std::vector<FriendLobbyInfo> ConnectToolCore::getFriendLobbies() {
    return SteamUtilsHelper::getFriendLobbies();
}

bool ConnectToolCore::inviteFriend(const std::string& friendSteamIdStr) {
    if (!roomManager || !isInLobby()) return false;
    try {
        unsigned long long friendId = std::stoull(friendSteamIdStr);
        CSteamID friendSteamID(friendId);
        if (SteamMatchmaking()) {
            return SteamMatchmaking()->InviteUserToLobby(roomManager->getCurrentLobby(), friendSteamID);
        }
    } catch (...) {}
    return false;
}

bool ConnectToolCore::isVPNEnabled() const {
    return vpnBridge && vpnBridge->isRunning();
}

std::string ConnectToolCore::getLocalVPNIP() const {
    if (vpnBridge) return vpnBridge->getLocalIP();
    return "";
}

std::string ConnectToolCore::getTunDeviceName() const {
    if (vpnBridge) return vpnBridge->getTunDeviceName();
    return "";
}

SteamVpnBridge::Statistics ConnectToolCore::getVPNStatistics() const {
    if (vpnBridge) return vpnBridge->getStatistics();
    return {};
}

std::map<uint32_t, RouteEntry> ConnectToolCore::getVPNRoutingTable() const {
    if (vpnBridge) return vpnBridge->getRoutingTable();
    return {};
}
ConnectToolCore::MemberConnectionInfo ConnectToolCore::getMemberConnectionInfo(const CSteamID& memberID) {
    int ping = 0;
    std::string relayInfo = "-";

    if (steamManager) {
        HSteamNetConnection conn = steamManager->getConnectionForPeer(memberID);
        if (conn != k_HSteamNetConnection_Invalid) {
            ping = steamManager->getConnectionPing(conn);
            relayInfo = steamManager->getConnectionRelayInfo(conn);
        }
    }
    return {ping, relayInfo};
}
