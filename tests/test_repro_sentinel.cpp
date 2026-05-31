#include "test_common.h"
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <thread>
#include <chrono>
#include <assert.h>

// This test attempts to reproduce the "stop_waiting past sentinel gap" error.
// The error occurs due to an inconsistency between the max received packet number
// in stats and the packet gap map sentinel in SNP, specifically when a
// gap is followed by an inhibited packet (e.g., due to too many segments).

void TestRepro() {
    SteamNetworkingIdentity identSender, identRecver;
    identSender.SetGenericString("sender");
    identRecver.SetGenericString("receiver");

    HSteamNetConnection hSender, hRecver;
    TEST_Printf("Creating socket pair...\n");
    bool bSuccess = SteamNetworkingSockets()->CreateSocketPair(&hSender, &hRecver, true, &identRecver, &identSender);
    assert(bSuccess);

    // Set some short timeouts/intervals to speed things up
    SteamNetworkingUtils()->SetConnectionConfigValueInt32(hSender, k_ESteamNetworkingConfig_SendRateMin, 1024*1024);
    SteamNetworkingUtils()->SetConnectionConfigValueInt32(hSender, k_ESteamNetworkingConfig_SendRateMax, 1024*1024);

    // 1. Initial exchange
    TEST_Printf("Initial exchange...\n");
    SteamNetworkingSockets()->SendMessageToConnection(hSender, "hello", 5, k_nSteamNetworkingSend_Reliable, nullptr);

    auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool bReceived = false;
    while (std::chrono::steady_clock::now() < timeout) {
        TEST_PumpCallbacks();
        ISteamNetworkingMessage *pMsg = nullptr;
        if (SteamNetworkingSockets()->ReceiveMessagesOnConnection(hRecver, &pMsg, 1) > 0) {
            TEST_Printf("Received initial message: %s\n", (char*)pMsg->m_pData);
            pMsg->Release();
            bReceived = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(bReceived);

    // 2. Set RecvMaxSegmentsPerPacket to 1 on the receiver to easily trigger inhibition
    TEST_Printf("Setting RecvMaxSegmentsPerPacket to 1 on receiver...\n");
    SteamNetworkingUtils()->SetConnectionConfigValueInt32(hRecver, k_ESteamNetworkingConfig_RecvMaxSegmentsPerPacket, 1);

    // 3. Cause a gap.
    TEST_Printf("Causing a gap by dropping 2 packets...\n");
    SteamNetworkingUtils()->SetConnectionConfigValueFloat(hSender, k_ESteamNetworkingConfig_FakePacketLoss_Send, 100.0f);
    for (int i = 0; i < 2; ++i) {
        SteamNetworkingSockets()->SendMessageToConnection(hSender, "skip", 4, k_nSteamNetworkingSend_UnreliableNoNagle, nullptr);
        TEST_PumpCallbacks();
    }

    // 4. Send a packet that will trigger bInhibitMarkReceived and advance m_nMaxRecvPktNum.
    TEST_Printf("Sending advancing packet with multiple segments (should be inhibited)...\n");
    SteamNetworkingUtils()->SetConnectionConfigValueFloat(hSender, k_ESteamNetworkingConfig_FakePacketLoss_Send, 0.0f);
    for (int i = 0; i < 5; ++i) {
        SteamNetworkingSockets()->SendMessageToConnection(hSender, "part", 4, k_nSteamNetworkingSend_Unreliable, nullptr);
    }

    // Pump to process the advancing packet.
    for (int i = 0; i < 10; ++i) {
        TEST_PumpCallbacks();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 5. Send more packets. The sender will send a stop_waiting frame.
    TEST_Printf("Sending subsequent packets to trigger the stop_waiting error...\n");
    for (int i = 0; i < 10; ++i) {
        SteamNetworkingSockets()->SendMessageToConnection(hSender, "trigger", 7, k_nSteamNetworkingSend_UnreliableNoNagle, nullptr);
        TEST_PumpCallbacks();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    TEST_Printf("Cleaning up...\n");
    SteamNetworkingSockets()->CloseConnection(hSender, 0, nullptr, false);
    SteamNetworkingSockets()->CloseConnection(hRecver, 0, nullptr, false);
}

int main() {
    TEST_Init(nullptr);
    TEST_SetStdoutDetailLevel(k_ESteamNetworkingSocketsDebugOutputType_Verbose);
    TestRepro();
    TEST_Kill();
    TEST_Printf("Test finished successfully (if it didn't crash!)\n");
    return 0;
}
