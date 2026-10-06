/*
 * slippi_mm_probe: an interop check against the mm server using the C ENet
 * that Slippi's Dolphin bundles (refs/slippi-Ishiiruka/Externals/enet), with
 * the same host setup and message flow as SlippiMatchmaking.cpp:
 *   enet_host_create(port 41000+rand, 1 peer, 3 channels), enet_host_connect(3 channels),
 *   reliable JSON on channel 0, create-ticket -> create-ticket-resp -> get-ticket-resp.
 *
 * Build and run: see README.md next to this file. Not part of `cargo test`
 * because it needs a C compiler and the Slippi source tree.
 *
 * Usage: slippi_mm_probe <host> <port> <uid> <playKey> <targetCode> [waitSeconds]
 * Prints every JSON message received, one per line, prefixed with "RECV ".
 * Exit code: 0 if a get-ticket-resp with a matchId arrived, 1 otherwise.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <enet/enet.h>

static void send_json(ENetPeer *peer, const char *json)
{
	ENetPacket *p = enet_packet_create(json, strlen(json), ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send(peer, 0, p);
}

int main(int argc, char **argv)
{
	if (argc < 6)
	{
		fprintf(stderr, "usage: %s host port uid playKey targetCode [waitSeconds]\n", argv[0]);
		return 2;
	}
	const char *host = argv[1];
	int mmPort = atoi(argv[2]);
	const char *uid = argv[3];
	const char *playKey = argv[4];
	const char *code = argv[5];
	int waitSeconds = argc > 6 ? atoi(argv[6]) : 20;

	if (enet_initialize() != 0)
		return 2;
	srand((unsigned)time(NULL) ^ (unsigned)(size_t)&argc);

	ENetHost *client = NULL;
	int localPort = 0;
	for (int i = 0; i < 15 && !client; i++)
	{
		localPort = 41000 + rand() % 10000;
		ENetAddress clientAddr;
		clientAddr.host = ENET_HOST_ANY;
		clientAddr.port = (enet_uint16)localPort;
		client = enet_host_create(&clientAddr, 1, 3, 0, 0);
	}
	if (!client)
		return 2;

	ENetAddress addr;
	enet_address_set_host(&addr, host);
	addr.port = (enet_uint16)mmPort;
	ENetPeer *server = enet_host_connect(client, &addr, 3, 0);

	ENetEvent ev;
	int connected = 0;
	for (int i = 0; i < 20 && !connected; i++)
		if (enet_host_service(client, &ev, 500) > 0 && ev.type == ENET_EVENT_TYPE_CONNECT)
			connected = 1;
	if (!connected)
	{
		printf("ERROR could not connect\n");
		return 1;
	}

	/* search.connectCode as a JSON array of bytes, like nlohmann's std::vector<u8>. */
	char codeArr[256] = "";
	for (size_t i = 0; i < strlen(code); i++)
	{
		char b[8];
		snprintf(b, sizeof b, i ? ",%u" : "%u", (unsigned char)code[i]);
		strcat(codeArr, b);
	}
	char msg[2048];
	snprintf(msg, sizeof msg,
	         "{\"appVersion\":\"3.4.0\",\"ipAddressLan\":\"127.0.0.1:%d\",\"search\":{\"connectCode\":[%s],\"mode\":2},"
	         "\"type\":\"create-ticket\",\"user\":{\"connectCode\":\"\",\"displayName\":\"probe\",\"playKey\":\"%s\",\"uid\":\"%s\"}}",
	         localPort, codeArr, playKey, uid);
	send_json(server, msg);

	int matched = 0;
	time_t end = time(NULL) + waitSeconds;
	while (time(NULL) < end)
	{
		int n = enet_host_service(client, &ev, 250);
		if (n <= 0)
			continue;
		if (ev.type == ENET_EVENT_TYPE_RECEIVE)
		{
			printf("RECV %.*s\n", (int)ev.packet->dataLength, (const char *)ev.packet->data);
			fflush(stdout);
			if (strstr((const char *)ev.packet->data, "get-ticket-resp") && strstr((const char *)ev.packet->data, "matchId"))
				matched = 1;
			enet_packet_destroy(ev.packet);
			if (matched)
				break;
		}
		else if (ev.type == ENET_EVENT_TYPE_DISCONNECT)
		{
			printf("DISCONNECTED\n");
			break;
		}
	}
	enet_peer_disconnect(server, 0);
	while (enet_host_service(client, &ev, 1000) > 0)
		if (ev.type == ENET_EVENT_TYPE_RECEIVE)
			enet_packet_destroy(ev.packet);
	enet_host_destroy(client);
	enet_deinitialize();
	return matched ? 0 : 1;
}
