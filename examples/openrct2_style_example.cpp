// OpenRCT2-style GameNetworkingSockets Integration Example
//
// This example demonstrates how to use GameNetworkingSockets to:
// 1. Establish a Client-Server connection with P2P NAT traversal.
// 2. Map deterministic simulation ticks to network messages.
// 3. Handle reliable command synchronization.
//
// To use this with the C++ signaling server:
//   ./cpp_signaling_server 10000 &
//   ./openrct2_style_example server
//   ./openrct2_style_example client

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <chrono>
#include <mutex>
#include <cstring>
#include <arpa/inet.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingcustomsignaling.h>
#include "trivial_signaling_client.h" // Re-using the example signaling client logic

// Simulation settings
const int TICK_RATE_MS = 33; // ~30 FPS

struct GameCommand {
    uint32_t tick;
    uint32_t player_id;
    std::string command_data;
};

class GameNetworkManager {
public:
    GameNetworkManager(const char* identity) {
        SteamNetworkingIdentity self;
        self.SetGenericString(identity);

        SteamDatagramErrMsg errMsg;
        if (!GameNetworkingSockets_Init(&self, errMsg)) {
            std::cerr << "Failed to init GameNetworkingSockets: " << errMsg << std::endl;
            exit(1);
        }
        m_pInterface = SteamNetworkingSockets();
    }

    ~GameNetworkManager() {
        // Release the signaling client (created by CreateTrivialSignalingClient) before shutting down
        if (m_pSignalingClient) {
            m_pSignalingClient->Release();
            m_pSignalingClient = nullptr;
        }
        GameNetworkingSockets_Kill();
    }

    void InitSignaling(const char* signaling_server_addr) {
        SteamDatagramErrMsg errMsg;
        m_pSignalingClient = CreateTrivialSignalingClient(signaling_server_addr, m_pInterface, errMsg);
        if (!m_pSignalingClient) {
            std::cerr << "Failed to connect to signaling server: " << errMsg << std::endl;
            exit(1);
        }
    }

    void StartServer() {
        m_is_server = true;
        SteamNetworkingConfigValue_t opt;
        opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)OnStatusChangedStatic);

        // Open a P2P listen socket. Clients will connect via signaling.
        m_listen_socket = m_pInterface->CreateListenSocketP2P(0, 1, &opt);

        // Validate that the listen socket was created successfully
        if (m_listen_socket == k_HSteamListenSocket_Invalid) {
            std::cerr << "Failed to create P2P listen socket (returned k_HSteamListenSocket_Invalid)" << std::endl;
            m_is_server = false;
            return;
        }

        std::cout << "Server listening for P2P connections..." << std::endl;
    }

    void ConnectToServer(const char* server_identity) {
        m_is_server = false;
        SteamNetworkingIdentity server_id;
        if (!server_id.ParseString(server_identity)) {
            std::cerr << "Failed to parse server identity: " << server_identity << std::endl;
            m_connection = k_HSteamNetConnection_Invalid;
            return;
        }

        SteamNetworkingConfigValue_t opt;
        opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)OnStatusChangedStatic);

        SteamDatagramErrMsg errMsg;
        ISteamNetworkingConnectionSignaling* pSignaling = m_pSignalingClient->CreateSignalingForConnection(server_id, errMsg);

        // Validate that signaling was created successfully
        if (!pSignaling) {
            std::cerr << "Failed to create signaling for connection to " << server_identity << ": " << errMsg << std::endl;
            m_connection = k_HSteamNetConnection_Invalid;
            return;
        }

        m_connection = m_pInterface->ConnectP2PCustomSignaling(pSignaling, &server_id, 0, 1, &opt);
        std::cout << "Connecting to server: " << server_identity << std::endl;
    }

    void Update() {
        m_pSignalingClient->Poll();
        SteamNetworkingSockets()->RunCallbacks();
        ReceiveMessages();
    }

    void SendCommand(const GameCommand& cmd) {
        // In a deterministic simulation, commands must be reliable and ordered.
        // We use ReliableNoNagle to ensure they are sent immediately at the end of a tick.
        // Packet format: [tick:uint32_be, player_id:uint32_be, command_data:bytes]
        std::string packet;
        packet.resize(8 + cmd.command_data.size());

        // Serialize fields in network byte order (big-endian) to be endian-safe
        uint32_t tick_be = htonl(cmd.tick);
        uint32_t player_id_be = htonl(cmd.player_id);
        memcpy(&packet[0], &tick_be, 4);
        memcpy(&packet[4], &player_id_be, 4);
        memcpy(&packet[8], cmd.command_data.data(), cmd.command_data.size());

        if (m_is_server) {
            for (auto const& [conn, info] : m_clients) {
                m_pInterface->SendMessageToConnection(conn, packet.data(), packet.size(), k_nSteamNetworkingSend_ReliableNoNagle, nullptr);
            }
        } else if (m_connection != k_HSteamNetConnection_Invalid) {
            m_pInterface->SendMessageToConnection(m_connection, packet.data(), packet.size(), k_nSteamNetworkingSend_ReliableNoNagle, nullptr);
        }
    }

public:
    static GameNetworkManager* s_instance;
private:
    ISteamNetworkingSockets* m_pInterface;
    ITrivialSignalingClient* m_pSignalingClient;
    HSteamListenSocket m_listen_socket = k_HSteamListenSocket_Invalid;
    HSteamNetConnection m_connection = k_HSteamNetConnection_Invalid;
    bool m_is_server = false;
    std::map<HSteamNetConnection, SteamNetConnectionInfo_t> m_clients;

    static void OnStatusChangedStatic(SteamNetConnectionStatusChangedCallback_t* pInfo) {
        s_instance->OnStatusChanged(pInfo);
    }

    void OnStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo) {
        switch (pInfo->m_info.m_eState) {
            case k_ESteamNetworkingConnectionState_Connecting:
                if (m_is_server) {
                    m_pInterface->AcceptConnection(pInfo->m_hConn);
                }
                break;
            case k_ESteamNetworkingConnectionState_Connected:
                std::cout << "Connected: " << pInfo->m_info.m_szConnectionDescription << std::endl;
                if (m_is_server) m_clients[pInfo->m_hConn] = pInfo->m_info;
                else m_connection = pInfo->m_hConn;
                break;
            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
                std::cout << "Disconnected: " << pInfo->m_info.m_szEndDebug << std::endl;
                m_pInterface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                if (m_is_server) m_clients.erase(pInfo->m_hConn);
                else m_connection = k_HSteamNetConnection_Invalid;
                break;
            default:
                break;
        }
    }

    void ReceiveMessages() {
        SteamNetworkingMessage_t* pMsgs[10];
        int numMsgs;
        if (m_is_server) {
            for (auto const& [conn, info] : m_clients) {
                numMsgs = m_pInterface->ReceiveMessagesOnConnection(conn, pMsgs, 10);
                for (int i = 0; i < numMsgs; ++i) {
                    HandleMessage(pMsgs[i]);
                    pMsgs[i]->Release();
                }
            }
        } else if (m_connection != k_HSteamNetConnection_Invalid) {
            numMsgs = m_pInterface->ReceiveMessagesOnConnection(m_connection, pMsgs, 10);
            for (int i = 0; i < numMsgs; ++i) {
                HandleMessage(pMsgs[i]);
                pMsgs[i]->Release();
            }
        }
    }

    void HandleMessage(SteamNetworkingMessage_t* pMsg) {
        if (pMsg->m_cbSize < 8) return;

        // Deserialize fields from network byte order (big-endian) to avoid alignment and endianness issues
        // Packet format: [tick:uint32_be, player_id:uint32_be, command_data:bytes]
        uint32_t tick_be, player_id_be;
        memcpy(&tick_be, pMsg->m_pData, 4);
        memcpy(&player_id_be, (char*)pMsg->m_pData + 4, 4);
        uint32_t tick = ntohl(tick_be);
        uint32_t player_id = ntohl(player_id_be);
        std::string data((char*)pMsg->m_pData + 8, pMsg->m_cbSize - 8);

        std::cout << "Received Command: Tick=" << tick << " Player=" << player_id << " Data=" << data << std::endl;

        // If server, rebroadcast to all other clients (skip the origin client)
        if (m_is_server) {
            GameCommand cmd = {tick, player_id, data};
            // Serialize the command once for rebroadcast
            std::string packet;
            packet.resize(8 + cmd.command_data.size());
            uint32_t tick_out = htonl(cmd.tick);
            uint32_t player_id_out = htonl(cmd.player_id);
            memcpy(&packet[0], &tick_out, 4);
            memcpy(&packet[4], &player_id_out, 4);
            memcpy(&packet[8], cmd.command_data.data(), cmd.command_data.size());

            // Iterate over all clients and skip the sender
            for (auto const& [conn, info] : m_clients) {
                if (conn != pMsg->m_conn) {
                    m_pInterface->SendMessageToConnection(conn, packet.data(), packet.size(), k_nSteamNetworkingSend_ReliableNoNagle, nullptr);
                }
            }
        }
    }
};

GameNetworkManager* GameNetworkManager::s_instance = nullptr;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " [server|client]" << std::endl;
        return 1;
    }

    std::string role = argv[1];
    const char* identity = (role == "server") ? "generic:server" : "generic:client1";

    GameNetworkManager mgr(identity);
    GameNetworkManager::s_instance = &mgr;

    if (role == "server") {
        mgr.InitSignaling("127.0.0.1:10000");
        mgr.StartServer();
    } else {
        mgr.InitSignaling("127.0.0.1:10000");
        mgr.ConnectToServer("generic:server");
    }

    uint32_t tick = 0;
    while (true) {
        auto start = std::chrono::steady_clock::now();

        mgr.Update();

        // Simulate sending a command every 100 ticks
        if (tick % 100 == 0) {
            GameCommand cmd = {tick, (role == "server" ? 0U : 1U), "Build Coaster"};
            mgr.SendCommand(cmd);
        }

        tick++;

        auto end = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        if (elapsed.count() < TICK_RATE_MS) {
            std::this_thread::sleep_for(std::chrono::milliseconds(TICK_RATE_MS - elapsed.count()));
        }
    }

    return 0;
}
