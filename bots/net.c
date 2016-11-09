#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#ifndef SERVER
#include <SDL.h>
#endif
#include "littlesms.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <unistd.h>
#include <fcntl.h>

#define BACKLOG_CAP 4096
#define PLAYER_MAX 2

static int sockfd;
#ifdef SERVER
#define CLIENT_MAX 256
static uint32_t cli_player[CLIENT_MAX];
static struct sockaddr *cli_addr[CLIENT_MAX];
static socklen_t cli_addrlen[CLIENT_MAX];
static int32_t player_cli[PLAYER_MAX] = {-1, -1};
static uint32_t player_initial_frame_idx[PLAYER_MAX];
static uint32_t player_frame_idx[PLAYER_MAX];
uint32_t player_input_beg[PLAYER_MAX];
uint32_t player_input_end[PLAYER_MAX];
uint32_t player_input_gap_beg[PLAYER_MAX];
uint32_t player_input_gap_end[PLAYER_MAX];
#else
static struct sockaddr *serv_addr;
static socklen_t serv_addrlen;
uint32_t serv_frame_idx;
uint32_t serv_frame_arrived[BACKLOG_CAP];
uint32_t serv_input_beg = 0;
uint32_t serv_input_end = 0;
uint32_t serv_input_gap_beg = 0;
uint32_t serv_input_gap_end = 0;
static uint32_t initial_backlog = 0;
#endif

// a minute should be long enough
#define BLWRAP(i) ((i) & (BACKLOG_CAP-1))
static struct SMS backlog[BACKLOG_CAP];
static uint8_t input_log[BACKLOG_CAP][2];
uint16_t input_pmask[BACKLOG_CAP];
static uint32_t backlog_end = 0;

static uint8_t net_intro_packet[13];

const uint8_t player_masks[PLAYER_MAX][2] = {
	{0x3F, 0x00},
	{0xC0, 0x0F},
};

int player_control = 0;
int32_t player_id = -2;
uint32_t sms_rom_crc = 0;

static uint32_t crc32_sms_net(uint8_t *data, size_t len, uint32_t crc)
{
	crc = ~crc;

	for(size_t i = 0; i < len; i++) {
		crc ^= (uint32_t)(data[i]);
		for(int j = 0; j < 8; j++) {
			crc = (crc>>1)^((crc&1)*0xEDB88320);
		}
	}

	return ~crc;
}

#ifdef SERVER
static void detach_client(int cidx)
{
	// Don't try to detach unconnected clients
	if(cidx < 0 || cidx >= CLIENT_MAX) {
		return;
	}

	printf("Detaching client %5d\n", cidx);

	int pidx = cli_player[cidx];
	if(pidx != -1) {
		player_cli[pidx] = -1;
	}
	cli_addrlen[cidx] = 0;
	free(cli_addr[cidx]);
	cli_addr[cidx] = NULL;

}

static void kick_client(const char *reason, int cidx, void *maddr, socklen_t maddr_len)
{
	assert(reason[0] == '\x02');

	printf("Kicking: \"%s\"\n", reason+1);

	// Send message
	sendto(sockfd, reason, strlen(reason)+1, 0,
		(struct sockaddr *)maddr, maddr_len);

	// Check if client needs detachment
	detach_client(cidx);
}
#endif

uint8_t bot_hook_input(struct SMS *sms, uint64_t timestamp, int port)
{
#ifndef SERVER
	SDL_Event ev;
	if(!sms->no_draw) {
	while(SDL_PollEvent(&ev)) {
		switch(ev.type) {
			case SDL_KEYDOWN:
				if((player_control&1) != 0) {
				switch(ev.key.keysym.sym)
				{
					case SDLK_w: sms->joy[0] &= ~0x01; break;
					case SDLK_s: sms->joy[0] &= ~0x02; break;
					case SDLK_a: sms->joy[0] &= ~0x04; break;
					case SDLK_d: sms->joy[0] &= ~0x08; break;
					case SDLK_KP_2: sms->joy[0] &= ~0x10; break;
					case SDLK_KP_3: sms->joy[0] &= ~0x20; break;
					default:
						break;
				}
				}

				if((player_control&2) != 0) {
				switch(ev.key.keysym.sym)
				{
					case SDLK_w: sms->joy[0] &= ~0x40; break;
					case SDLK_s: sms->joy[0] &= ~0x80; break;
					case SDLK_a: sms->joy[1] &= ~0x01; break;
					case SDLK_d: sms->joy[1] &= ~0x02; break;
					case SDLK_KP_2: sms->joy[1] &= ~0x04; break;
					case SDLK_KP_3: sms->joy[1] &= ~0x08; break;
					default:
						break;
				}
				}
				break;

			case SDL_KEYUP:
				if((player_control&1) != 0) {
				switch(ev.key.keysym.sym)
				{
					case SDLK_w: sms->joy[0] |= 0x01; break;
					case SDLK_s: sms->joy[0] |= 0x02; break;
					case SDLK_a: sms->joy[0] |= 0x04; break;
					case SDLK_d: sms->joy[0] |= 0x08; break;
					case SDLK_KP_2: sms->joy[0] |= 0x10; break;
					case SDLK_KP_3: sms->joy[0] |= 0x20; break;
					default:
						break;
				}
				}

				if((player_control&2) != 0) {
				switch(ev.key.keysym.sym)
				{
					case SDLK_w: sms->joy[0] |= 0x40; break;
					case SDLK_s: sms->joy[0] |= 0x80; break;
					case SDLK_a: sms->joy[1] |= 0x01; break;
					case SDLK_d: sms->joy[1] |= 0x02; break;
					case SDLK_KP_2: sms->joy[1] |= 0x04; break;
					case SDLK_KP_3: sms->joy[1] |= 0x08; break;
					default:
						break;
				}
				}
				break;

			case SDL_QUIT:
				exit(0);
				break;
			default:
				break;
		}
	}
	}
#endif

	return sms->joy[port&1];
}

void bot_update()
{
	// Save backlog frame (for sync purposes)
	sms_copy(&backlog[BLWRAP(backlog_end)], &sms_current);

	// Save frame input
#ifdef SERVER
	input_log[BLWRAP(backlog_end)][0] = sms_current.joy[0];
	input_log[BLWRAP(backlog_end)][1] = sms_current.joy[1];
	input_pmask[BLWRAP(backlog_end)] |= player_control;
#else
	if(player_id >= 0) {
		input_log[BLWRAP(backlog_end)][0] = sms_current.joy[0];
		input_log[BLWRAP(backlog_end)][1] = sms_current.joy[1];
	}
	input_pmask[BLWRAP(backlog_end)] = 0;
#endif

#ifdef SERVER
	// Do a quick check
	if(player_cli[0] < 0) {
		player_frame_idx[0] = backlog_end;
	}
	if(player_cli[1] < 0) {
		player_frame_idx[1] = backlog_end;
	}

	// Get messages
	printf("=== SFrame %d\n", backlog_end);
	for(;;)
	{
		bool jump_out = false;

		// Ensure that we are ahead of both players
		// Unless we have no players, in which case wait until we have players
		int32_t wldiff0 = (int32_t)(backlog_end-player_frame_idx[0]);
		int32_t wldiff1 = (int32_t)(backlog_end-player_frame_idx[1]);
		//printf("wldiffs %d %d\n", wldiff0, wldiff1);
		if((wldiff0 < 0 || wldiff1 < 0) && (player_cli[0] >= 0 || player_cli[1] >= 0)) {
			//printf("jump-out activated\n");
			jump_out = true;
		}

		for(;;) {
			uint8_t mbuf[2048];
			uint8_t maddr[128];
			socklen_t maddr_len = sizeof(maddr);
			memset(mbuf, 0, sizeof(mbuf));
			ssize_t rlen = recvfrom(sockfd, mbuf, sizeof(mbuf),
				//(jump_out ? MSG_DONTWAIT : 0),
				MSG_DONTWAIT,
				(struct sockaddr *)&maddr, &maddr_len);

			if(rlen < 0) {
				break;
			}

			// Try to find client

			int cidx = -1;
			for(int i = 0; i < CLIENT_MAX; i++) {
				if(cli_addrlen[i] == maddr_len
					&& !memcmp(cli_addr[i], maddr, cli_addrlen[i])) {

					cidx = i;
					break;
				}
			}

			//printf("SMSG %5d %02X %d\n", (int)rlen, mbuf[0], cidx);

			if(cidx == -1 && mbuf[0] == '\x01') {
				// Someone wants to connect!
				printf("Client trying to connect\n");

				// Validate data
				if(rlen != sizeof(net_intro_packet)) {
					kick_client("\x02""Bad intro", cidx, maddr, maddr_len);
					continue;

				} else if(0 != memcmp(net_intro_packet, mbuf, rlen)) {
					kick_client("\x02""Bad emu state", cidx, maddr, maddr_len);
					continue;

				}

				// Find a slot
				for(int i = 0; i < CLIENT_MAX; i++) {
					if(cli_addrlen[i] == 0) {
						cidx = i;
						break;
					}
				}

				if(cidx < 0 || cidx >= CLIENT_MAX) {
					kick_client("\x02Server full", cidx, maddr, maddr_len);
					continue;
				}

				// Find a suitable player slot
				int pidx = -1;
				if(player_cli[0] < 0) {
					pidx = 0;
				} else if(player_cli[1] < 0) {
					// TODO: get nonspecs to sync
					//pidx = 1;
				}

				cli_player[cidx] = pidx;
				printf("client %5d -> player %5d\n", cidx, pidx);
				if(pidx != -1) {
					player_cli[pidx] = cidx;
					player_frame_idx[pidx] = backlog_end;
					player_initial_frame_idx[pidx] = backlog_end;
					player_input_beg[pidx] = backlog_end;
					player_input_end[pidx] = backlog_end;
					player_input_gap_beg[pidx] = backlog_end;
					player_input_gap_end[pidx] = backlog_end;
				}
				cli_addrlen[cidx] = maddr_len;
				cli_addr[cidx] = malloc(maddr_len);
				memcpy(cli_addr[cidx], maddr, maddr_len);

				// Send intro
				uint8_t sintro_buf[9];
				sintro_buf[0] = 0x01;
				((uint32_t *)(sintro_buf+1))[0] = (pidx == -1
					? backlog_end
					: player_frame_idx[pidx]);
				((int32_t *)(sintro_buf+1))[1] = pidx;
				sendto(sockfd, sintro_buf, sizeof(sintro_buf), 0,
					(struct sockaddr *)&maddr, maddr_len);

			} else if(cidx == -1) {
				// We don't have this client, kick it
				kick_client("\x02Not connected", cidx, maddr, maddr_len);

			} else if(mbuf[0] == '\x01') {
				// Did they not get the intro? Resend the intro.
				int pidx = cli_player[cidx];

				uint8_t sintro_buf[9];
				sintro_buf[0] = 0x01;
				((uint32_t *)(sintro_buf+1))[0] = (pidx == -1
					? backlog_end
					: player_frame_idx[pidx]);
				((int32_t *)(sintro_buf+1))[1] = pidx;
				sendto(sockfd, sintro_buf, sizeof(sintro_buf), 0,
					(struct sockaddr *)&maddr, maddr_len);

			} else if(mbuf[0] == '\x02') {
				// Someone's sick of us!
				mbuf[sizeof(mbuf)-1] = '\x00';
				printf("Client disconnected: \"%s\"\n", mbuf+1);
				detach_client(cidx);

			} else if(mbuf[0] == '\x03') {
				// Do they need a given state? Send it!
				uint32_t frame_idx = *((uint32_t *)(mbuf+1));
				uint32_t offs = *((uint32_t *)(mbuf+5));
				uint32_t len = *((uint16_t *)(mbuf+9));
				if(offs > sizeof(struct SMS) || offs+len > sizeof(struct SMS)) {
					kick_client("\x02""Out of bounds state read", cidx, maddr, maddr_len);
					continue;
				}
				if(len > 1024 || len < 1) {
					kick_client("\x02""Sync length bad", cidx, maddr, maddr_len);
					continue;
				}

				mbuf[0] = 0x04;
				uint8_t *ps = mbuf+11;
				uint8_t *pd = (uint8_t *)&backlog[BLWRAP(frame_idx)];
				memcpy(ps, pd+offs, len);
				sendto(sockfd, mbuf, 11+len, 0,
					(struct sockaddr *)&maddr, maddr_len);

			} else if(mbuf[0] == '\x05') {
				// Did they not get a set of inputs? Resend it!
				uint32_t in_beg = ((uint32_t *)(mbuf+1))[0];

				// Of course, we have to have the inputs available...
				if((backlog_end-in_beg) >= BACKLOG_CAP-1) {
					kick_client("\x02""End of backlog reached", cidx, maddr, maddr_len);
					continue;
				}

				// Get length and clamp max at now / clamp max at max
				uint32_t in_len = mbuf[5];
				uint32_t in_end = in_beg+in_len;
				if((int32_t)(backlog_end-in_end) < 0) {
					in_len = backlog_end-in_beg;
				}

				// Skip if length == 0
				if(in_len == 0) {
					continue;
				}

				// Now produce backlog!
				mbuf[0] = 0x06;
				((uint32_t *)(mbuf+1))[0] = in_beg;
				assert(in_len >= 1 && in_len <= 255);
				mbuf[5] = in_len;
				uint8_t *p = &mbuf[6];
				for(uint32_t i = 0; i < in_len; i++) {
					p[i*2+0] = input_log[BLWRAP((in_beg+i))][0];
					p[i*2+1] = input_log[BLWRAP((in_beg+i))][1];
				}

				// Send it!
				sendto(sockfd, mbuf, 6+2*in_len, 0,
					(struct sockaddr *)&maddr, maddr_len);

			} else if(mbuf[0] == '\x06') {
				int pidx = cli_player[cidx];
				if(pidx < 0 || pidx >= PLAYER_MAX) {
					// Extraneous packet - spectators do NOT send input
					continue;
				}

				// WE HAVE INPUTS
				uint32_t in_beg = *((uint32_t *)(mbuf+1));
				uint32_t in_len = mbuf[5];
				uint32_t in_end = in_beg+in_len;

				// Ensure we are in range
				int32_t fdelta0 = (int32_t)(in_beg-backlog_end);
				int32_t fdelta1 = fdelta0+in_len;
				if(fdelta0 >= BACKLOG_CAP/4 || -fdelta0 >= BACKLOG_CAP-1) {
					kick_client("\x02""Frame out of range", cidx, maddr, maddr_len);
					continue;
				}
				if(fdelta1 >= BACKLOG_CAP/4 || -fdelta1 >= BACKLOG_CAP-1) {
					kick_client("\x02""Frame out of range", cidx, maddr, maddr_len);
					continue;
				}

				// Check if we fill gaps
				if(player_input_gap_beg[pidx] != player_input_gap_end[pidx]) {
					int32_t gdbeg = (int32_t)(in_beg-player_input_gap_beg[pidx]);
					int32_t gdend = (int32_t)(in_end-player_input_gap_end[pidx]);
					int32_t gdxbeg = (int32_t)(in_beg-player_input_gap_end[pidx]);
					int32_t gdxend = (int32_t)(in_end-player_input_gap_beg[pidx]);

					if(gdbeg <= 0 && gdxend >= 0) {
						player_input_gap_beg[pidx] = in_end;
					}

					if(gdend >= 0 && gdxbeg <= 0) {
						player_input_gap_end[pidx] = in_beg;
					}

					int32_t gdelta = (int32_t)(player_input_gap_end[pidx]-player_input_gap_beg[pidx]);
					if(gdelta <= 0) {
						player_input_gap_end[pidx] = player_input_gap_beg[pidx];
					}
				}

				// Check for gaps
				if(in_beg > backlog_end) {
					// Check if we've detected a gap already
					if(player_input_gap_beg[pidx] == player_input_gap_end[pidx]) {
						player_input_gap_beg[pidx] = backlog_end;
						player_input_gap_end[pidx] = in_beg;
					} else {
						player_input_gap_end[pidx] = in_beg;
					}

					uint32_t gapoffs = player_input_gap_beg[pidx];
					uint32_t gaplen = player_input_gap_end[pidx]-player_input_gap_beg[pidx];
					printf("GAP %10d %3d\n", gapoffs, gaplen);
					assert(gaplen >= 1 && gaplen <= 255);
					uint8_t fillgappkt[6];
					fillgappkt[0] = 0x05;
					((uint32_t *)(fillgappkt+1))[0] = gapoffs;
					fillgappkt[5] = gaplen;
					sendto(sockfd, fillgappkt, sizeof(fillgappkt), 0,
						cli_addr[cidx], cli_addrlen[cidx]);

					// SKIP
					continue;
				}

				// Advance if necessary
				int32_t pfdelta = (int32_t)(in_end-player_frame_idx[pidx]);
				if(pfdelta > 0) {
					player_frame_idx[pidx] = in_end;
					//printf("Advance %d\n", in_end);
				}

				// Set inputs
				mbuf[0] = 0x06;
				uint8_t *p = mbuf+6;
				for(int i = 0; i < in_len; i++) {
					int idx = BLWRAP(in_beg+i);
					uint8_t old0 = input_log[idx][0];
					uint8_t old1 = input_log[idx][1];
					uint8_t m0 = player_masks[pidx][0];
					uint8_t m1 = player_masks[pidx][1];
					uint8_t new0 = p[2*i+0];
					uint8_t new1 = p[2*i+1];
					new0 = old0^((old0^new0)&m0);
					new1 = old1^((old1^new1)&m1);
					//p[2*i+0] = input_log[idx][0] = backlog[idx].joy[0] = new0;
					//p[2*i+1] = input_log[idx][1] = backlog[idx].joy[1] = new1;
					p[2*i+0] = input_log[idx][0] = new0;
					p[2*i+1] = input_log[idx][1] = new1;
					input_pmask[idx] |= (1<<pidx);
				}
				//printf("%3d INPUT\n", pidx);

				// Send it to the other clients!
				for(int ci = 0; ci < CLIENT_MAX; ci++) {
					//if(cli_addrlen[ci] != 0 && ci != cidx) {
					if(cli_addrlen[ci] != 0 && ci != cidx) {
						sendto(sockfd, mbuf, 6+2*in_len, 0,
							cli_addr[ci], cli_addrlen[ci]);
					}
				}

			}
		}

		if(jump_out) {
			break;
		} else {
			usleep(10000);
		}
	}
#else

	// Get messages
	for(;;)
	{
		uint8_t mbuf[2048];
		uint8_t maddr[128];
		socklen_t maddr_len = sizeof(maddr);
		memset(mbuf, 0, sizeof(mbuf));
		ssize_t rlen = recvfrom(sockfd, mbuf, sizeof(mbuf),
			MSG_DONTWAIT, (struct sockaddr *)&maddr, &maddr_len);

		if(rlen < 0) {
			if(player_id < 0) {
				// Wait for messages
				if((int32_t)(backlog_end-serv_frame_idx) >= 0) {
					usleep(1000);
					continue;
				}
			}

			// Check if we need to keep going
			if(false) {
				usleep(1000);
				continue;
			}
			break;
		}

		//printf("CMSG %d %02X\n", (int)rlen, mbuf[0]);

		if(mbuf[0] == '\x01') {
			// We connected! Again!
			printf("Acknowledged! Again!\n");
			initial_backlog = ((uint32_t *)(mbuf+1))[0];
			player_id = ((int32_t *)(mbuf+1))[1];
			backlog_end = initial_backlog;
			player_control = ((player_id >= 0 && player_id <= 31)
				? (1<<player_id) : 0);

		} else if(mbuf[0] == '\x02') {
			// The server hates us!
			mbuf[sizeof(mbuf)-1] = '\x00';
			printf("KICKED: \"%s\"\n", mbuf+1);
			fflush(stdout);
			abort();

		} else if(mbuf[0] == '\x04') {
			// New state!
			// XXX: do we really want to bother with this?
			// It can be sent extraneously in theory,
			// but that's all really
			//printf("STATE\n");

		} else if(mbuf[0] == '\x05') {
			// Server missed our inputs - resend them!
			uint32_t in_beg = ((uint32_t *)(mbuf+1))[0];

			// Of course, we have to have the inputs available...
			assert(!((backlog_end-in_beg) >= BACKLOG_CAP-1));

			// Get length and clamp max at now / clamp max at max
			uint32_t in_len = mbuf[5];
			if((int32_t)(backlog_end-(in_beg+in_len)) < 0) {
				in_len = backlog_end-in_beg;
			}

			// Skip if length == 0
			if(in_len == 0) {
				continue;
			}

			// Now produce backlog!
			mbuf[0] = 0x06;
			((uint32_t *)(mbuf+1))[0] = in_beg;
			assert(in_len >= 1 && in_len <= 255);
			mbuf[5] = in_len;
			uint8_t *p = &mbuf[6];
			for(uint32_t i = 0; i < in_len; i++) {
				p[i*2+0] = input_log[BLWRAP((in_beg+i))][0];
				p[i*2+1] = input_log[BLWRAP((in_beg+i))][1];
			}

			// Send it!
			sendto(sockfd, mbuf, 6+2*in_len, 0,
				(struct sockaddr *)&maddr, maddr_len);

		} else if(mbuf[0] == '\x06') {
			// New inputs!
			uint32_t in_beg = ((uint32_t *)(mbuf+1))[0];
			uint32_t in_len = mbuf[5];
			uint32_t in_end = in_beg+in_len;
			uint8_t *p = &mbuf[6];
			printf("INPUT %10d %3d %10d %10d\n"
				, in_beg, in_len, in_end, backlog_end);

			// Check if we fill gaps
			if(serv_input_gap_beg != serv_input_gap_end) {
				int32_t gdbeg = (int32_t)(in_beg-serv_input_gap_beg);
				int32_t gdend = (int32_t)(in_end-serv_input_gap_end);
				int32_t gdxbeg = (int32_t)(in_beg-serv_input_gap_end);
				int32_t gdxend = (int32_t)(in_end-serv_input_gap_beg);

				if(gdbeg <= 0 && gdxend >= 0) {
					serv_input_gap_beg = in_end;
				}

				if(gdend >= 0 && gdxbeg <= 0) {
					serv_input_gap_end = in_beg;
				}

				int32_t gdelta = (int32_t)(serv_input_gap_end-serv_input_gap_beg);
				if(gdelta <= 0) {
					serv_input_gap_end = serv_input_gap_beg;
				}
			}

			// Check for gaps
			if(in_beg > backlog_end) {
				// Check if we've detected a gap already
				if(serv_input_gap_beg == serv_input_gap_end) {
					serv_input_gap_beg = backlog_end;
					serv_input_gap_end = in_beg;
				} else {
					serv_input_gap_end = in_beg;
				}

				uint32_t gapoffs = serv_input_gap_beg;
				uint32_t gaplen = serv_input_gap_end-serv_input_gap_beg;
				printf("GAP %10d %3d\n", gapoffs, gaplen);
				assert(gaplen >= 1 && gaplen <= 255);
				uint8_t fillgappkt[6];
				fillgappkt[0] = 0x05;
				((uint32_t *)(fillgappkt+1))[0] = gapoffs;
				fillgappkt[5] = gaplen;
				sendto(sockfd, fillgappkt, sizeof(fillgappkt), 0,
					serv_addr, serv_addrlen);
			}

			// Check for differences
			uint32_t resim_beg = backlog_end;
			uint8_t m0 = (player_id >= 0 ? player_masks[player_id][0] : 0x00);
			uint8_t m1 = (player_id >= 0 ? player_masks[player_id][1] : 0x00);
			for(int i = 0; i < in_len; i++) {
				int idx = BLWRAP(in_beg+i);
				/*
				input_log[idx][0] &= m0;
				input_log[idx][1] &= m1;
				input_log[idx][0] |= p[i*2+0] & ~m0;
				input_log[idx][1] |= p[i*2+1] & ~m1;
				*/
				if(input_log[idx][0] != p[i*2+0]
					|| input_log[idx][1] != p[i*2+1]) {

					if(in_beg+i < resim_beg) {
						resim_beg = in_beg+i;
					}
				}
				input_log[idx][0] = p[i*2+0];
				input_log[idx][1] = p[i*2+1];
				backlog[idx].joy[0] = input_log[idx][0];
				backlog[idx].joy[1] = input_log[idx][1];
			}

			//assert(!((backlog_end-in_beg) >= BACKLOG_CAP-1));

			// FIXME do this properly to avoid desyncs
			/*
			int idx = BLWRAP(backlog_end);
			input_log[idx][0] &= m0;
			input_log[idx][1] &= m1;
			input_log[idx][0] |= p[(in_len-1)*2+0] & ~m0;
			input_log[idx][1] |= p[(in_len-1)*2+1] & ~m1;
			backlog[idx].joy[0] = input_log[idx][0];
			backlog[idx].joy[1] = input_log[idx][1];
			printf("INPUT %02X %02X\n"
				, backlog[idx].joy[0]
				, backlog[idx].joy[1]
				);
			*/

			// Also, server is now ahead
			serv_frame_idx = in_end;
		}
	}
#endif

	uint32_t idx = BLWRAP(backlog_end);

	// Restore frame input
	sms_current.joy[0] = input_log[idx][0];
	sms_current.joy[1] = input_log[idx][1];

#ifndef SERVER
	if(player_id >= 0) {
		// Send frame input to server
		// TODO: batch these!
		uint8_t myinputpkt[8];
		myinputpkt[0] = 0x06;
		((uint32_t *)(myinputpkt+1))[0] = backlog_end;
		myinputpkt[5] = 0x01;
		myinputpkt[6] = input_log[idx][0];
		myinputpkt[7] = input_log[idx][1];
		sendto(sockfd, myinputpkt, sizeof(myinputpkt), 0,
			serv_addr, serv_addrlen);
	}
#endif

	// Load frame backup
	backlog[idx].joy[0] = input_log[idx][0];
	backlog[idx].joy[1] = input_log[idx][1];
	sms_copy(&sms_current, &backlog[idx]);
	backlog_end++;

#ifndef SERVER

	// If spectator, play catchup if too far behind
	if(player_id < 0) {
		printf("***** SPEC %d\n", backlog_end);
		if((int32_t)(serv_frame_idx-backlog_end) >= 20) {
			twait = time_now()-FRAME_WAIT*15;
		}
	}

	// Disable no_draw
	sms_current.no_draw = false;
#endif

#ifdef SERVER

	// If server... don't wait.
	twait = time_now()-FRAME_WAIT;
#endif
}

void bot_init(int argc, char *argv[])
{
	sms_current.joy[0] = 0xFF;
	sms_current.joy[1] = 0xFF;

	memset(input_log, 0xFF, sizeof(input_log));

	sms_rom_crc = crc32_sms_net(sms_rom, sizeof(sms_rom), 0);
	//sms_rom_crc = crc32_sms_net(sms_rom, 512*1024, 0);

	net_intro_packet[0] = 0x01;
	((uint32_t *)(net_intro_packet+1))[0] = (uint32_t)sizeof(struct SMS);
	((uint32_t *)(net_intro_packet+1))[1] = sms_rom_crc;
	((uint32_t *)(net_intro_packet+1))[2] = 0
		| (sms_rom_is_banked ? 0x01 : 0)
		| (USE_NTSC ? 0x02 : 0)
		|0;

#ifdef SERVER
	if(argc <= 1) {
		fprintf(stderr, "usage: %s port\n", argv[0]);
		fflush(stderr);
		abort();
	}

	printf("Starting server\n");

	// Get info
	struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_DGRAM,
		.ai_flags = AI_PASSIVE,
	};
	struct addrinfo *ai;
	int gai_err = getaddrinfo(NULL, argv[1], &hints, &ai);

	if(gai_err != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(gai_err));
		abort();
	}

	printf("Info for localhost:\n");
	struct addrinfo *ai_in4 = NULL;
	struct addrinfo *ai_in6 = NULL;
	for(struct addrinfo *p = ai; p != NULL; p = p->ai_next) {
		char adsbuf[128];
		adsbuf[0] = '\x00';
		switch(p->ai_family) {
			case AF_INET:
				ai_in4 = p;
				inet_ntop(p->ai_family,
					&(((struct sockaddr_in *)(p->ai_addr))->sin_addr),
					adsbuf, sizeof(adsbuf));
				printf(" - IPv4: \"%s\"\n", adsbuf);
				break;

			case AF_INET6:
				ai_in6 = p;
				inet_ntop(p->ai_family,
					&(((struct sockaddr_in6 *)(p->ai_addr))->sin6_addr),
					adsbuf, sizeof(adsbuf));
				printf(" - IPv6: \"%s\"\n", adsbuf);
				break;

			default:
				printf(" - ??? (%d)\n", p->ai_family);
				break;
		}

	}
	printf("\n");

	// Get socket
	struct addrinfo *ai_best = (ai_in6 != NULL ? ai_in6 : ai_in4);
	assert(ai_best != NULL);
	sockfd = socket(ai_best->ai_family, ai_best->ai_socktype, ai_best->ai_protocol);
	assert(sockfd > 0);

	// Bind port
	int yes = 1;
	int sso_err = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	assert(sso_err != -1);
	int bind_err = bind(sockfd, ai_best->ai_addr, ai_best->ai_addrlen);
	assert(bind_err == 0);

	// Free info
	freeaddrinfo(ai);

	printf("Server is now online!\n\n");
#else
	if(argc <= 2) {
		fprintf(stderr, "usage: %s hostname port\n", argv[0]);
		fflush(stderr);
		abort();
	}

	printf("Starting client\n");

	// Get info
	struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_DGRAM,
	};
	struct addrinfo *ai;
	int gai_err = getaddrinfo(argv[1], argv[2], &hints, &ai);

	if(gai_err != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(gai_err));
		abort();
	}

	// Get socket
	struct addrinfo *ai_best = ai;
	assert(ai_best != NULL);
	printf("%d %d %d\n", ai_best->ai_family, ai_best->ai_socktype, ai_best->ai_protocol);
	sockfd = socket(ai_best->ai_family, ai_best->ai_socktype, ai_best->ai_protocol);
	assert(sockfd > 0);

	// Copy addr
	serv_addrlen = ai_best->ai_addrlen;
	serv_addr = malloc(serv_addrlen);
	memcpy(serv_addr, ai_best->ai_addr, serv_addrlen);

	// Free info
	freeaddrinfo(ai);

	printf("Client is now online!\n\n");

	// Try connecting to server
	printf("Establishing connection...\n");
	bool connected = false;

	for(int attempts = 20; attempts > 0 && !connected; attempts--) {
		sendto(sockfd, net_intro_packet, sizeof(net_intro_packet), 0, serv_addr, serv_addrlen);
		usleep(200000);
		for(;;) {
			uint8_t mbuf[2048];
			uint8_t maddr[128];
			socklen_t maddr_len = sizeof(maddr);
			memset(mbuf, 0, sizeof(mbuf));
			ssize_t rlen = recvfrom(sockfd, mbuf, sizeof(mbuf),
				MSG_DONTWAIT, (struct sockaddr *)&maddr, &maddr_len);

			if(rlen < 0) {
				break;
			}

			printf("CMSG %d %02X\n", (int)rlen, mbuf[0]);

			if(mbuf[0] == '\x01') {
				// We connected!
				assert(rlen == 9);
				initial_backlog = ((uint32_t *)(mbuf+1))[0];
				player_id = ((int32_t *)(mbuf+1))[1];
				backlog_end = initial_backlog;
				serv_input_beg = backlog_end;
				serv_input_end = backlog_end;
				serv_input_gap_beg = backlog_end;
				serv_input_gap_end = backlog_end;
				player_control = ((player_id >= 0 && player_id <= 31)
					? (1<<player_id) : 0);
				printf("Acknowledged! id=%02d control=%08X\n"
					, player_id, player_control);
				connected = true;
				break;

			} else if(mbuf[0] == '\x02') {
				// The server hates us!
				mbuf[sizeof(mbuf)-1] = '\x00';
				printf("KICKED: \"%s\"\n", mbuf+1);
				connected = false;
				break;
			}
		}
	}
	assert(connected);

	// Sync state
	uint8_t has_byte[sizeof(struct SMS)];
	uint32_t bytes_remain = sizeof(struct SMS);
	memset(has_byte, 0x00, sizeof(has_byte));

	printf("Sending sync requests...\n");
	for(uint32_t i = 0; i < sizeof(struct SMS); ) {
		uint32_t nlen = sizeof(struct SMS)-i;
		if(nlen > 1024) {
			nlen = 1024;
		}

		uint8_t pktbuf[11];
		pktbuf[0] = 0x03;
		*((uint32_t *)(pktbuf+1)) = backlog_end;
		*((uint32_t *)(pktbuf+5)) = i;
		*((uint16_t *)(pktbuf+9)) = nlen;
		sendto(sockfd, pktbuf, sizeof(pktbuf), 0, serv_addr, serv_addrlen);

		i += nlen;
	}

	printf("Syncing state...\n");
	int sleeps_until_resync = 10;
	while(bytes_remain > 0) {
		usleep(100000);
		bool had_packet = false;
		for(;;) {
			uint8_t mbuf[2048];
			uint8_t maddr[128];
			socklen_t maddr_len = sizeof(maddr);
			memset(mbuf, 0, sizeof(mbuf));
			ssize_t rlen = recvfrom(sockfd, mbuf, sizeof(mbuf),
				MSG_DONTWAIT, (struct sockaddr *)&maddr, &maddr_len);

			if(rlen < 0) {
				break;
			}

			//printf("CMSG %d %02X\n", (int)rlen, mbuf[0]);

			if(mbuf[0] == '\x04') {
				had_packet = true;
				// Synced state has arrived!
				assert(rlen >= 11);
				uint32_t frame_idx = *((uint32_t *)(mbuf+1));
				uint32_t offs = *((uint32_t *)(mbuf+5));
				uint32_t len = *((uint16_t *)(mbuf+9));
				printf("Sync packet: frame_idx=%08X, offs=%08X, len=%04X\n"
					, frame_idx, offs, len);
				uint8_t *ps = mbuf+11;
				uint8_t *pd = (uint8_t *)&sms_current;
				assert(offs <= sizeof(struct SMS));
				assert(offs+len <= sizeof(struct SMS));
				memcpy(pd+offs, ps, len);
				for(int i = 0; i < len; i++) {
					if(has_byte[offs+i] == 0){
						has_byte[offs+i] = 0xFF;
						bytes_remain--;
					}
				}

			} else if(mbuf[0] == '\x02') {
				// The server hates us!
				mbuf[sizeof(mbuf)-1] = '\x00';
				printf("KICKED: \"%s\"\n", mbuf+1);
				fflush(stdout);
				abort();
			}
		}

		if(!had_packet) {
			sleeps_until_resync--;
			if(sleeps_until_resync <= 0) {
				printf("Sending resync (%d remain)\n", bytes_remain);
				sleeps_until_resync = 10;

				for(uint32_t i = 0; i < sizeof(struct SMS); ) {
					uint32_t nlen = sizeof(struct SMS)-i;
					if(nlen > 1024) {
						nlen = 1024;
					}

					bool need_byte = false;
					for(int j = 0; j < nlen; j++) {
						if(has_byte[i+j] == 0) {
							need_byte = true;
							break;
						}
					}

					if(need_byte) {
						uint8_t pktbuf[11];
						pktbuf[0] = 0x03;
						*((uint32_t *)(pktbuf+1)) = backlog_end;
						*((uint32_t *)(pktbuf+5)) = i;
						*((uint16_t *)(pktbuf+9)) = nlen;
						sendto(sockfd, pktbuf, sizeof(pktbuf), 0, serv_addr, serv_addrlen);
					}

					i += nlen;
				}
			}
		}
	}

	printf("State is now synced. Let's go.\n");
#endif
	printf("ROM CRC: %04X\n", sms_rom_crc);

}
