#include "steam_room_manager.h"
#include "steam_networking_manager.h"
#include "steam_vpn_bridge.h"
#include "config/config_manager.h"
#include <iostream>
#include <string>
#include <algorithm>

SteamFriendsCallbacks::SteamFriendsCallbacks(SteamNetworkingManager *manager, SteamRoomManager *roomManager) 
    : manager_(manager), roomManager_(roomManager)
{
    std::cout << "SteamFriendsCallbacks constructor called" << std::endl;
}

void SteamFriendsCallbacks::OnGameLobbyJoinRequested(GameLobbyJoinRequested_t *pCallback)
{
    std::cout << "GameLobbyJoinRequested received" << std::endl;
    if (manager_)
    {
        CSteamID lobbyID = pCallback->m_steamIDLobby;
        std::cout << "Lobby ID: " << lobbyID.ConvertToUint64() << std::endl;
        if (!manager_->isConnected())
        {
            std::cout << "Joining lobby from request: " << lobbyID.ConvertToUint64() << std::endl;
            roomManager_->joinLobby(lobbyID);
        }
        else
        {
            std::cout << "Already in a lobby, ignoring lobby join request" << std::endl;
        }
    }
    else
    {
        std::cout << "Manager is null" << std::endl;
    }
}

SteamMatchmakingCallbacks::SteamMatchmakingCallbacks(SteamNetworkingManager *manager, SteamRoomManager *roomManager) 
    : manager_(manager), roomManager_(roomManager)
{}

void SteamMatchmakingCallbacks::OnLobbyCreated(LobbyCreated_t *pCallback, bool bIOFailure)
{
    if (bIOFailure)
    {
        std::cerr << "Failed to create lobby - IO Failure" << std::endl;
        return;
    }
    if (pCallback->m_eResult == k_EResultOK)
    {
        roomManager_->setCurrentLobby(pCallback->m_ulSteamIDLobby);
        std::cout << "Lobby created: " << roomManager_->getCurrentLobby().ConvertToUint64() << std::endl;
        
        // Set Rich Presence to enable invite functionality
        SteamFriends()->SetRichPresence("steam_display", "#Status_InLobby");
        SteamFriends()->SetRichPresence("connect", std::to_string(pCallback->m_ulSteamIDLobby).c_str());
    }
    else
    {
        std::cerr << "Failed to create lobby" << std::endl;
    }
}

void SteamMatchmakingCallbacks::OnLobbyListReceived(LobbyMatchList_t *pCallback, bool bIOFailure)
{
    if (bIOFailure)
    {
        std::cerr << "Failed to receive lobby list - IO Failure" << std::endl;
        return;
    }
    roomManager_->clearLobbies();
    for (uint32 i = 0; i < pCallback->m_nLobbiesMatching; ++i)
    {
        CSteamID lobbyID = SteamMatchmaking()->GetLobbyByIndex(i);
        roomManager_->addLobby(lobbyID);
    }
    std::cout << "Received " << pCallback->m_nLobbiesMatching << " lobbies" << std::endl;
}

void SteamMatchmakingCallbacks::OnLobbyEntered(LobbyEnter_t *pCallback)
{
    if (pCallback->m_EChatRoomEnterResponse == k_EChatRoomEnterResponseSuccess)
    {
        roomManager_->setCurrentLobby(pCallback->m_ulSteamIDLobby);
        std::cout << "Entered lobby: " << pCallback->m_ulSteamIDLobby << std::endl;
        
        // Set Rich Presence to enable invite functionality
        SteamFriends()->SetRichPresence("steam_display", "#Status_InLobby");
        SteamFriends()->SetRichPresence("connect", std::to_string(pCallback->m_ulSteamIDLobby).c_str());

        // 【新增】自动启动VPN
        if (manager_->getVpnBridge()) {
            const auto& config = ConfigManager::instance().getConfig();
            std::cout << "Auto-starting VPN with settings (" 
                      << config.vpn.virtual_subnet << "/" 
                      << config.vpn.subnet_mask << ")..." << std::endl;
            manager_->getVpnBridge()->start(
                config.vpn.tun_device_name,
                config.vpn.virtual_subnet,
                config.vpn.subnet_mask
            );
        }
        
        // 添加所有已存在的大厅成员为节点（使用 ISteamNetworkingMessages 无需手动连接）
        CSteamID mySteamID = SteamUser()->GetSteamID();
        
        int numMembers = SteamMatchmaking()->GetNumLobbyMembers(pCallback->m_ulSteamIDLobby);
        std::cout << "Adding " << (numMembers - 1) << " lobby members as peers..." << std::endl;
        
        for (int i = 0; i < numMembers; ++i)
        {
            CSteamID memberID = SteamMatchmaking()->GetLobbyMemberByIndex(pCallback->m_ulSteamIDLobby, i);
            
            // 不添加自己
            if (memberID != mySteamID)
            {
                std::cout << "Adding peer: " << memberID.ConvertToUint64() << std::endl;
                manager_->addPeer(memberID);
            }
        }
    }
    else
    {
        std::cerr << "Failed to enter lobby" << std::endl;
    }
}

void SteamMatchmakingCallbacks::OnLobbyChatUpdate(LobbyChatUpdate_t *pCallback)
{
    // This callback is triggered when someone joins or leaves the lobby
    CSteamID affectedUser(pCallback->m_ulSteamIDUserChanged);
    CSteamID mySteamID = SteamUser()->GetSteamID();
    
    if (pCallback->m_rgfChatMemberStateChange & k_EChatMemberStateChangeEntered)
    {
        std::cout << "User " << affectedUser.ConvertToUint64() << " entered lobby" << std::endl;
        
        // 新成员加入，将其添加为节点
        if (affectedUser != mySteamID && roomManager_->getCurrentLobby().IsValid())
        {
            std::cout << "Adding new peer: " << affectedUser.ConvertToUint64() << std::endl;
            manager_->addPeer(affectedUser);
        }
    }
    else if (pCallback->m_rgfChatMemberStateChange & k_EChatMemberStateChangeLeft)
    {
        std::cout << "User " << affectedUser.ConvertToUint64() << " left lobby" << std::endl;
        // 移除离开的节点
        manager_->removePeer(affectedUser);
    }
    else if (pCallback->m_rgfChatMemberStateChange & k_EChatMemberStateChangeDisconnected)
    {
        std::cout << "User " << affectedUser.ConvertToUint64() << " disconnected from lobby" << std::endl;
        // 移除断开连接的节点
        manager_->removePeer(affectedUser);
    }
}

SteamRoomManager::SteamRoomManager(SteamNetworkingManager *networkingManager)
    : networkingManager_(networkingManager), currentLobby(k_steamIDNil),
      steamFriendsCallbacks(nullptr), steamMatchmakingCallbacks(nullptr)
{
    steamFriendsCallbacks = new SteamFriendsCallbacks(networkingManager_, this);
    steamMatchmakingCallbacks = new SteamMatchmakingCallbacks(networkingManager_, this);

    // Clear Rich Presence on initialization to prevent "Invite to game" showing when not in a lobby
    SteamFriends()->ClearRichPresence();
}

SteamRoomManager::~SteamRoomManager()
{
    delete steamFriendsCallbacks;
    delete steamMatchmakingCallbacks;
}

bool SteamRoomManager::createLobby()
{
    SteamAPICall_t hSteamAPICall = SteamMatchmaking()->CreateLobby(k_ELobbyTypePublic, 250);
    if (hSteamAPICall == k_uAPICallInvalid)
    {
        std::cerr << "Failed to create lobby" << std::endl;
        return false;
    }
    
    // 使用 ISteamNetworkingMessages 不需要创建 listen socket
    std::cout << "Creating lobby (using ISteamNetworkingMessages, no listen socket needed)" << std::endl;
    
    // Register the call result
    steamMatchmakingCallbacks->m_CallResultLobbyCreated.Set(hSteamAPICall, steamMatchmakingCallbacks, &SteamMatchmakingCallbacks::OnLobbyCreated);
    return true;
}

void SteamRoomManager::leaveLobby()
{
    if (currentLobby != k_steamIDNil)
    {
        SteamMatchmaking()->LeaveLobby(currentLobby);
        currentLobby = k_steamIDNil;
        
        // 清理所有节点
        networkingManager_->clearPeers();

        // 【新增】自动关闭VPN
        if (networkingManager_->getVpnBridge()) {
            std::cout << "Auto-stopping VPN..." << std::endl;
            networkingManager_->getVpnBridge()->stop();
        }
        
        // Clear Rich Presence when leaving lobby
        SteamFriends()->ClearRichPresence();
    }
}

bool SteamRoomManager::searchLobbies()
{
    lobbies.clear();
    SteamAPICall_t hSteamAPICall = SteamMatchmaking()->RequestLobbyList();
    if (hSteamAPICall == k_uAPICallInvalid)
    {
        std::cerr << "Failed to request lobby list" << std::endl;
        return false;
    }
    // Register the call result
    steamMatchmakingCallbacks->m_CallResultLobbyMatchList.Set(hSteamAPICall, steamMatchmakingCallbacks, &SteamMatchmakingCallbacks::OnLobbyListReceived);
    return true;
}

bool SteamRoomManager::joinLobby(CSteamID lobbyID)
{
    // 使用 ISteamNetworkingMessages 不需要创建 listen socket
    std::cout << "Joining lobby (using ISteamNetworkingMessages, no listen socket needed)" << std::endl;
    
    SteamAPICall_t hCall = SteamMatchmaking()->JoinLobby(lobbyID);
    if (hCall == k_uAPICallInvalid)
    {
        std::cerr << "Failed to join lobby" << std::endl;
        return false;
    }
    // 成员将在回调中处理
    return true;
}

std::vector<CSteamID> SteamRoomManager::getLobbyMembers() const
{
    std::vector<CSteamID> members;
    if (currentLobby != k_steamIDNil)
    {
        int numMembers = SteamMatchmaking()->GetNumLobbyMembers(currentLobby);
        for (int i = 0; i < numMembers; ++i)
        {
            CSteamID memberID = SteamMatchmaking()->GetLobbyMemberByIndex(currentLobby, i);
            members.push_back(memberID);
        }
    }
    return members;
}