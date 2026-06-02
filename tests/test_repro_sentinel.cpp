#include "test_common.h"
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#define protected public
#define private public
#include <steamnetworkingsockets_connections.h>
#include <steamnetworkingsockets_snp.h>
#undef protected
#undef private

#include <thread>
#include <chrono>
#include <assert.h>
#include <string.h>

using namespace SteamNetworkingSocketsLib;

namespace SteamNetworkingSocketsLib {
    extern CSteamNetworkConnectionBase *GetConnectionByHandle( HSteamNetConnection sock, ConnectionScopeLock &scopeLock );
}

bool g_bHitSentinelError = false;
void MyDebugOutput( ESteamNetworkingSocketsDebugOutputType eType, const char *pszMsg )
{
    if ( strstr( pszMsg, "stop_waiting past sentinel gap" ) )
        g_bHitSentinelError = true;
    printf( "%s\n", pszMsg );
}

void TestRepro() {
    SteamNetworkingIdentity identSender, identRecver;
    identSender.SetGenericString("sender");
    identRecver.SetGenericString("receiver");

    HSteamNetConnection hSender, hRecver;
    TEST_Printf("Creating socket pair...\n");
    bool bSuccess = SteamNetworkingSockets()->CreateSocketPair(&hSender, &hRecver, true, &identSender, &identRecver);
    assert(bSuccess);

    // Step 1: Baseline exchange
    TEST_Printf("Step 1: Baseline exchange\n");
    SteamNetworkingSockets()->SendMessageToConnection(hSender, "1", 1, k_nSteamNetworkingSend_Reliable, nullptr);
    auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool message_received = false;
    while (std::chrono::steady_clock::now() < timeout) {
        TEST_PumpCallbacks();
        ISteamNetworkingMessage *pMsg = nullptr;
        if (SteamNetworkingSockets()->ReceiveMessagesOnConnection(hRecver, &pMsg, 1) > 0) {
            pMsg->Release();
            message_received = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if ( !message_received ) {
        TEST_Printf("FAILED: Baseline message never received within timeout\n");
        exit(1);
    }

    // Give it a bit more time to settle and make sure all acks are processed
    for (int i=0; i<20; ++i) { TEST_PumpCallbacks(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }

    // Step 2: Trigger the crash using inhibited packet path
    TEST_Printf("Step 2: Triggering inhibited packet path\n");
    {
        SteamNetworkingGlobalLock scopeLock;
        ConnectionScopeLock connectionLock;
        CSteamNetworkConnectionBase *pConnRecver = GetConnectionByHandle(hRecver, connectionLock);
        assert(pConnRecver);

        // Set segment limit to 1 to trigger inhibition on a multi-segment packet
        pConnRecver->m_connectionConfig.RecvMaxSegmentsPerPacket.Set(1);

        int64 current_max = pConnRecver->m_statsEndToEnd.m_nMaxRecvPktNum;
        int64 pkt_inhibited = current_max + 1;
        int64 pkt_trigger = current_max + 2;

        TEST_Printf("Current max_recv=%lld. Sending inhibited packet %lld...\n", (long long)current_max, (long long)pkt_inhibited);

        // Crafted packet with 2 unreliable segments.
        // Seg 1: Lead 0x00, msgnum 1, offset 0, size 1, data 0xAA
        // Seg 2: Lead 0x00, offset 0, size 1, data 0xBB
        uint8 packet_inhibited[] = { 0x00, 0x01, 0x00, 0x01, 0xAA, 0x00, 0x00, 0x01, 0xBB };

        RecvPacketContext_t ctx;
        ctx.m_usecNow = SteamNetworkingSockets_GetLocalTimestamp();
        ctx.m_pPlainText = packet_inhibited;
        ctx.m_cbPlainText = sizeof(packet_inhibited);
        ctx.m_nPktNum = pkt_inhibited;
        ctx.m_pTransport = pConnRecver->m_pTransport;
        ctx.m_idxMultiPath = 0;

        // Override debug output to catch the error
        SteamNetworkingUtils()->SetDebugOutputFunction( k_ESteamNetworkingSocketsDebugOutputType_Everything, MyDebugOutput );

        pConnRecver->ProcessPlainTextDataChunk( 0, ctx );

        TEST_Printf("Sending trigger packet %lld with stop_waiting targeting %lld...\n", (long long)pkt_trigger, (long long)pkt_inhibited);

        // Crafted packet with stop_waiting frame.
        // offset = 0 means stop_waiting = nPktNum - 1 = pkt_trigger - 1 = pkt_inhibited.
        uint8 packet_trigger[] = { 0x80, 0x00 };

        ctx.m_pPlainText = packet_trigger;
        ctx.m_cbPlainText = sizeof(packet_trigger);
        ctx.m_nPktNum = pkt_trigger;

        pConnRecver->ProcessPlainTextDataChunk( 0, ctx );
    }

    TEST_Printf("Step 3: Checking for result\n");
    if ( g_bHitSentinelError ) {
        TEST_Printf("REPRODUCED: 'stop_waiting past sentinel gap' error detected!\n");
        // For a regression test, hitting the bug means failure.
        exit(1);
    } else {
        TEST_Printf("SUCCESS: Issue not detected.\n");
    }

    SteamNetworkingSockets()->CloseConnection(hSender, 0, nullptr, false);
    SteamNetworkingSockets()->CloseConnection(hRecver, 0, nullptr, false);
}

int main() {
    TEST_Init(nullptr);
    TestRepro();
    TEST_Kill();
    return 0;
}
