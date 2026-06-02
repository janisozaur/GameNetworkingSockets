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
    auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < timeout) {
        TEST_PumpCallbacks();
        ISteamNetworkingMessage *pMsg = nullptr;
        if (SteamNetworkingSockets()->ReceiveMessagesOnConnection(hRecver, &pMsg, 1) > 0) {
            pMsg->Release();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (int i=0; i<10; ++i) { TEST_PumpCallbacks(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }

    // Step 2: Trigger the crash using the internal hook and crafted packet
    TEST_Printf("Step 2: Triggering crash\n");
    {
        SteamNetworkingGlobalLock scopeLock;
        ConnectionScopeLock connectionLock;
        CSteamNetworkConnectionBase *pConnRecver = GetConnectionByHandle(hRecver, connectionLock);
        assert(pConnRecver);

        int64 current_max = pConnRecver->m_statsEndToEnd.m_nMaxRecvPktNum;
        int64 current_sentinel = current_max + 1;
        TEST_Printf("State before desync: max_recv=%lld, sentinel=%lld\n", (long long)current_max, (long long)current_sentinel);

        // Manually advance max_recv to be equal to sentinel.
        pConnRecver->TEST_TriggerSentinelDesync(current_sentinel);
        TEST_Printf("State after desync: max_recv=%lld\n", (long long)pConnRecver->m_statsEndToEnd.m_nMaxRecvPktNum);

        // Crafted packet with stop_waiting = current_sentinel.
        // Lead byte 0x80 means stop_waiting with 8-bit offset.
        // stop_waiting = nPktNum - (offset+1).
        // If nPktNum = current_sentinel + 1, and we want stop_waiting = current_sentinel.
        // offset = 0.
        uint8 packet[] = { 0x80, 0x00 };

        RecvPacketContext_t ctx;
        ctx.m_usecNow = SteamNetworkingSockets_GetLocalTimestamp();
        ctx.m_pPlainText = packet;
        ctx.m_cbPlainText = sizeof(packet);
        ctx.m_nPktNum = current_sentinel + 1;
        ctx.m_pTransport = pConnRecver->m_pTransport;
        ctx.m_idxMultiPath = 0;

        // Override debug output to catch the error without exiting
        SteamNetworkingUtils()->SetDebugOutputFunction( k_ESteamNetworkingSocketsDebugOutputType_Everything, MyDebugOutput );

        pConnRecver->ProcessPlainTextDataChunk( 0, ctx );
    }

    TEST_Printf("Step 3: Checking for result\n");
    if ( g_bHitSentinelError ) {
        TEST_Printf("SUCCESS: Reproduced the issue!\n");
    } else {
        TEST_Printf("FAILED: Could not reproduce the issue.\n");
        exit(1);
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
