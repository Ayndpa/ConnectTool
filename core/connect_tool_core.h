#pragma once

#include <memory>
#include <string>
#include <vector>
#include <mutex>

#include "../steam/steam_networking_manager.h"
#include "../steam/steam_room_manager.h"
#include "../steam/steam_vpn_bridge.h"
#include "../steam/steam_utils.h"

class ConnectToolCore {
public:
    ConnectToolCore();
    ~ConnectToolCore();

    bool initSteam();
    void shutdown();
    void update(); // Should be called periodically

    // Lobby
    bool createLobby(std::string& outLobbyId);
    bool joinLobby(const std::string& lobbyIdStr);
    void leaveLobby();
    bool isInLobby() const;
    CSteamID getCurrentLobbyId() const;
    std::vector<CSteamID> getLobbyMembers() const;
    
    // Friends
    std::vector<FriendLobbyInfo> getFriendLobbies();
    bool inviteFriend(const std::string& friendSteamIdStr);

    // VPN Monitoring
    bool isVPNEnabled() const;
    std::string getLocalVPNIP() const;
    std::string getTunDeviceName() const;
    SteamVpnBridge::Statistics getVPNStatistics() const;
    std::map<uint32_t, RouteEntry> getVPNRoutingTable() const;

    // Helper to get connection info for a member
    struct MemberConnectionInfo {
        int ping;
        std::string relayInfo;
    };
    MemberConnectionInfo getMemberConnectionInfo(const CSteamID& memberID);

private:
    std::unique_ptr<SteamNetworkingManager> steamManager;
    std::unique_ptr<SteamRoomManager> roomManager;
    std::unique_ptr<SteamVpnBridge> vpnBridge;

    bool steamInitialized = false;
};
