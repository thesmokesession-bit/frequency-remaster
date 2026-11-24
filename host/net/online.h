#pragma once

// Online play: up to four machines in one room. The room code (FREQ_ONLINE_ROOM) names a topic on
// a free ntfy server where the machines leave their addresses, and keys the tag every datagram
// carries, so only machines given the code take part. The machine with FREQ_ONLINE_HOST=1 holds
// slot 0, the others link to it directly over UDP (hole punching, with a public STUN server's view
// of each address), and the host passes each guest's messages on to the other guests. Messages
// are reliable and arrive in the order their sender sent them.

#ifdef __cplusplus
extern "C" {
#endif

/** The largest message HostOnlineSend() takes, in bytes. */
#define HOST_ONLINE_MAX_MESSAGE 1024

/** Start the session the settings ask for on its own thread. From main(), before the game. */
void HostOnlineStart(void);

/**
 * Exit any room and open the room of the code pszRoom, as its host or as a guest. Returns 0 when
 * the UDP port cannot be opened.
 */
int HostOnlineOpen(const char *pszRoom, int bHost);

/** Exit the room. The other machines see this machine drop at once. */
void HostOnlineClose(void);

/** Whether a room is open on this machine. */
int HostOnlineActive(void);

/** Whether the last polls of the signalling server failed (no network, or the server is down). */
int HostOnlineSignalFailing(void);

/** This machine's slot: 0 on the host, 1 to 3 on a linked guest, and -1 before that. */
int HostOnlineSlot(void);

/** The slots this machine is linked to now, one bit each (a guest has only bit 0). */
unsigned HostOnlineLinked(void);

/** The round trip to the machine in nSlot, in milliseconds. */
int HostOnlineRttMs(int nSlot);

/** Whether the host takes new guests (it stops while a song plays). */
void HostOnlineSetOpen(int bOpen);

/** Queue a message for every other machine in the room. Returns 0 when it is too long. */
int HostOnlineSend(const void *pData, int nSize);

/**
 * Take the next message another machine sent: its length (0 when there is none), with its slot in
 * *pnFrom. A message longer than nCapacity is dropped.
 */
int HostOnlineReceive(void *pBuffer, int nCapacity, int *pnFrom);

#ifdef __cplusplus
}
#endif
