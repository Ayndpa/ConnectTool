#include "steam_room_manager.h"
#include "steam_networking_manager.h"
#include "steam_vpn_bridge.h"
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
            std::cout << "Auto-starting VPN with default settings (10.0.0.0/8)..." << std::endl;
            manager_->getVpnBridge()->start("", "10.0.0.0", "255.0.0.0");
        }
        
        // Connect to all existing members in the lobby (except ourselves)
        CSteamID mySteamID = SteamUser()->GetSteamID();
        
        int numMembers = SteamMatchmaking()->GetNumLobbyMembers(pCallback->m_ulSteamIDLobby);
        std::cout << "Connecting to " << (numMembers - 1) << " lobby members..." << std::endl;
        
        for (int i = 0; i < numMembers; ++i)
        {
            CSteamID memberID = SteamMatchmaking()->GetLobbyMemberByIndex(pCallback->m_ulSteamIDLobby, i);
            
            // Don't connect to ourselves
            if (memberID != mySteamID)
            {
                std::cout << "Connecting to member " << memberID.ConvertToUint64() << std::endl;
                manager_->connectToPeer(memberID);
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
        
        // If it's not us and we're already in the lobby, connect to the new member
        if (affectedUser != mySteamID && roomManager_->getCurrentLobby().IsValid())
        {
            std::cout << "New member joined: " << affectedUser.ConvertToUint64() << " (Waiting for them to connect)" << std::endl;
            // manager_->connectToPeer(affectedUser);
        }
    }
    else if (pCallback->m_rgfChatMemberStateChange & k_EChatMemberStateChangeLeft)
    {
        std::cout << "User " << affectedUser.ConvertToUint64() << " left lobby" << std::endl;
        // Connection cleanup will be handled by Steam networking callbacks
    }
    else if (pCallback->m_rgfChatMemberStateChange & k_EChatMemberStateChangeDisconnected)
    {
        std::cout << "User " << affectedUser.ConvertToUint64() << " disconnected from lobby" << std::endl;
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
    
    // Create P2P listen socket
    networkingManager_->getListenSock() = networkingManager_->getInterface()->CreateListenSocketP2P(0, 0, nullptr);
    if (networkingManager_->getListenSock() == k_HSteamListenSocket_Invalid)
    {
        std::cerr << "Failed to create listen socket" << std::endl;
        return false;
    }
    std::cout << "Created P2P listen socket" << std::endl;
    
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
        
        // Disconnect all P2P connections and close listen socket
        // This properly cleans up all network state
        networkingManager_->disconnect();

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
    // Create P2P listen socket BEFORE joining lobby
    // This is needed to receive incoming connections from other lobby members
    if (networkingManager_->getListenSock() == k_HSteamListenSocket_Invalid)
    {
        networkingManager_->getListenSock() = networkingManager_->getInterface()->CreateListenSocketP2P(0, 0, nullptr);
        if (networkingManager_->getListenSock() == k_HSteamListenSocket_Invalid)
        {
            std::cerr << "Failed to create listen socket for joining lobby" << std::endl;
            return false;
        }
        std::cout << "Created P2P listen socket for joining lobby" << std::endl;
    }
    
    SteamAPICall_t hCall = SteamMatchmaking()->JoinLobby(lobbyID);
    if (hCall == k_uAPICallInvalid)
    {
        std::cerr << "Failed to join lobby" << std::endl;
        return false;
    }
    // Connection will be handled by callback
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