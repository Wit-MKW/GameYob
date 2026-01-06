#include <nds.h>
#include <dswifi9.h>
#include <nds/cothread.h>
#include <sys/socket.h>
#include <errno.h>
#include "nifi.h"
#include "mmu.h"
#include "main.h"
#include "gameboy.h"
#include "gbcpu.h"
#include "console.h"
#include <mobile.h>

bool nifiInit = false;
uint8_t linkReceivedData;
unsigned nifiTime;

static struct nifiUser {
	struct mobile_adapter *adapter;
	bool serial, wifi;
	char config[MOBILE_CONFIG_SIZE];
	unsigned time[MOBILE_MAX_TIMERS];
	int sock[MOBILE_MAX_CONNECTIONS];
	char sockOpen, sockUdp, sockConn;
	char number[2][MOBILE_MAX_NUMBER_SIZE+1];
} nifiState;

void debug_log(struct nifiUser *user, const char *line) {
	(void)user;
	printLog("%s\n", line);
}

void serial_disable(struct nifiUser *user) {
	user->serial = false;
}

void serial_enable(struct nifiUser *user, bool mode_32bit) {
	user->serial = !mode_32bit;
}

bool config_read(struct nifiUser *user, void *dest, uintptr_t offset, size_t size) {
	memcpy(dest, user->config + offset, size);
	return true;
}

bool config_write(struct nifiUser *user, const void *src, uintptr_t offset, size_t size) {
	memcpy(user->config + offset, src, size);
	return true;
}

void time_latch(struct nifiUser *user, unsigned timer) {
	user->time[timer] = nifiTime;
}

bool time_check_ms(struct nifiUser *user, unsigned timer, unsigned ms) {
	return (unsigned)(((uint64_t)(nifiTime - user->time[timer]) * 125ull) >> 19) >= ms;
}

bool sock_open(struct nifiUser *user, unsigned conn, enum mobile_socktype type,
		enum mobile_addrtype addrtype, unsigned bindport) {
	if (!user->wifi) {
		printLog("Connecting to Wi-Fi...\n");
		if (!Wifi_CheckInit())
			Wifi_InitDefault(WIFI_ATTEMPT_DSI_MODE);
		Wifi_EnableWifi();
		Wifi_InternetMode();
		do {
			cothread_yield();
		} while (!Wifi_LibraryModeReady());
		Wifi_AutoConnect();
		int status;
		do {
			cothread_yield();
			status = Wifi_AssocStatus();
			if (status == ASSOCSTATUS_CANNOTCONNECT) {
				printLog("Failed!\n");
				return false;
			}
		} while (status != ASSOCSTATUS_ASSOCIATED);
		user->wifi = true;
		printLog("Success!\n");
	}
	int sock = socket(
		(addrtype == MOBILE_ADDRTYPE_IPV6) ? AF_INET6 : AF_INET,
		(type == MOBILE_SOCKTYPE_UDP) ? SOCK_DGRAM : SOCK_STREAM,
		(type == MOBILE_SOCKTYPE_UDP) ? IPPROTO_UDP : IPPROTO_TCP
	);
	if (sock == -1)
		return false;
	if (bindport) {
		int rc;
		if (addrtype == MOBILE_ADDRTYPE_IPV6) {
			struct sockaddr_in6 addr = {
				.sin6_len = sizeof(addr),
				.sin6_family = AF_INET6,
				.sin6_port = htons(bindport),
				.sin6_flowinfo = 0,
				.sin6_addr = {0},
				.sin6_scope_id = 0
			};
			rc = bind(sock, (struct sockaddr*)&addr, sizeof(addr));
		} else {
			struct sockaddr_in addr = {
				.sin_len = sizeof(addr),
				.sin_family = AF_INET,
				.sin_port = htons(bindport),
				.sin_addr = {0},
				.sin_zero = {0}
			};
			rc = bind(sock, (struct sockaddr*)&addr, sizeof(addr));
		}
		if (rc == -1) {
			close(sock);
			return false;
		}
	}
	int nbio = 1;
	if (ioctl(sock, FIONBIO, (char*)&nbio) == -1) {
		close(sock);
		return false;
	}
	user->sock[conn] = sock;
	user->sockOpen |= (1 << conn);
	user->sockConn &= ~(1 << conn);
	if (type == MOBILE_SOCKTYPE_UDP)
		user->sockUdp |= (1 << conn);
	else
		user->sockUdp &= ~(1 << conn);
	return true;
}

void sock_close(struct nifiUser *user, unsigned conn) {
	close(user->sock[conn]);
}

int sock_connect(struct nifiUser *user, unsigned conn, const struct mobile_addr *addr) {
	int rc;
	if (user->sockConn & (1 << conn)) {
		struct pollfd pfd = {
			.fd = user->sock[conn],
			.events = POLLOUT
		};
		rc = poll(&pfd, 1, 0);
		if (rc == -1)
			return -(errno != EAGAIN && errno != EINTR);
		return rc && (pfd.revents == POLLOUT);
	}
	user->sockConn |= (1 << conn);
	if (addr->type == MOBILE_ADDRTYPE_IPV6) {
		struct mobile_addr6 *addr6 = (struct mobile_addr6*)addr;
		struct sockaddr_in6 sockaddr = {
			.sin6_len = sizeof(sockaddr),
			.sin6_family = AF_INET6,
			.sin6_port = htons(addr6->port),
			.sin6_flowinfo = 0,
			.sin6_scope_id = 0
		};
		memcpy(&sockaddr.sin6_addr.s6_addr, addr6->host, MOBILE_HOSTLEN_IPV6);
		rc = connect(user->sock[conn], (struct sockaddr*)&sockaddr, sizeof(sockaddr));
	} else {
		struct mobile_addr4 *addr4 = (struct mobile_addr4*)addr;
		struct sockaddr_in sockaddr = {
			.sin_len = sizeof(sockaddr),
			.sin_family = AF_INET,
			.sin_port = htons(addr4->port),
			.sin_zero = {0}
		};
		memcpy(&sockaddr.sin_addr.s_addr, addr4->host, MOBILE_HOSTLEN_IPV4);
		rc = connect(user->sock[conn], (struct sockaddr*)&sockaddr, sizeof(sockaddr));
	}
	return (rc == -1) ? -(errno != EINPROGRESS) : 1;
}

bool sock_listen(struct nifiUser *user, unsigned conn) {
	return !listen(user->sock[conn], 1);
}

bool sock_accept(struct nifiUser *user, unsigned conn) {
	struct sockaddr_in6 address;
	socklen_t address_len = sizeof(address);
	int sock = accept(user->sock[conn], (struct sockaddr*)&address, &address_len);
	if (sock == -1)
		return false;
	close(user->sock[conn]);
	user->sock[conn] = sock;
	return true;
}

int sock_send(struct nifiUser *user, unsigned conn, const void *data,
		unsigned size, const struct mobile_addr *addr) {
	ssize_t rc;
	if (addr) {
		if (addr->type == MOBILE_ADDRTYPE_IPV6) {
			struct mobile_addr6 *addr6 = (struct mobile_addr6*)addr;
			struct sockaddr_in6 sockaddr = {
				.sin6_len = sizeof(sockaddr),
				.sin6_family = AF_INET6,
				.sin6_port = htons(addr6->port),
				.sin6_flowinfo = 0,
				.sin6_scope_id = 0
			};
			memcpy(&sockaddr.sin6_addr.s6_addr, addr6->host, MOBILE_HOSTLEN_IPV6);
			rc = sendto(user->sock[conn], data, size, 0,
				(struct sockaddr*)&sockaddr, sizeof(sockaddr));
		} else {
			struct mobile_addr4 *addr4 = (struct mobile_addr4*)addr;
			struct sockaddr_in sockaddr = {
				.sin_len = sizeof(sockaddr),
				.sin_family = AF_INET,
				.sin_port = htons(addr4->port),
				.sin_zero = {0}
			};
			memcpy(&sockaddr.sin_addr.s_addr, addr4->host, MOBILE_HOSTLEN_IPV4);
			rc = sendto(user->sock[conn], data, size, 0,
				(struct sockaddr*)&sockaddr, sizeof(sockaddr));
		}
	} else {
		rc = send(user->sock[conn], data, size, 0);
	}
	return (rc == -1) ? -(errno != EAGAIN) : rc;
}

int sock_recv(struct nifiUser *user, unsigned conn, void *data,
		unsigned size, struct mobile_addr *addr) {
	ssize_t rc;
	if (addr) {
		struct sockaddr_in6 sockaddr;
		socklen_t sockaddr_len = sizeof(sockaddr);
		if (data && size) {
			rc = recvfrom(user->sock[conn], data, size, 0,
				(struct sockaddr*)&sockaddr, &sockaddr_len);
		} else {
			char buffer;
			rc = recvfrom(user->sock[conn], &buffer, 1, MSG_PEEK,
				(struct sockaddr*)&sockaddr, &sockaddr_len);
		}
		if (rc != -1) {
			if (sockaddr.sin6_family == AF_INET6) {
				struct mobile_addr6 *addr6 = (struct mobile_addr6*)addr;
				addr6->type = MOBILE_ADDRTYPE_IPV6;
				addr6->port = ntohs(sockaddr.sin6_port);
				memcpy(addr6->host, &sockaddr.sin6_addr.s6_addr, MOBILE_HOSTLEN_IPV6);
			} else {
				struct sockaddr_in *sockaddr4 = (struct sockaddr_in*)&sockaddr;
				struct mobile_addr4 *addr4 = (struct mobile_addr4*)addr;
				addr4->type = MOBILE_ADDRTYPE_IPV4;
				addr4->port = ntohs(sockaddr4->sin_port);
				memcpy(addr4->host, &sockaddr4->sin_addr.s_addr, MOBILE_HOSTLEN_IPV4);
			}
		}
	} else {
		if (data && size) {
			rc = recv(user->sock[conn], data, size, 0);
		} else {
			char buffer;
			rc = recv(user->sock[conn], &buffer, 1, MSG_PEEK);
		}
	}
	switch (rc) {
	case -1:
		if (errno == ECONNRESET)
			return -2;
		return -(errno != EAGAIN && errno != EINTR);
	case 0:
		return (user->sockUdp & (1 << conn)) ? 0 : -2;
	case 1:
		return (data && size);
	}
	return rc;
}

void update_number(struct nifiUser *user, enum mobile_number type, const char *number) {
	strncpy(user->number[type], number ?: "<none>", MOBILE_MAX_NUMBER_SIZE+1);
	user->number[type][MOBILE_MAX_NUMBER_SIZE] = 0;
}

void saveNifi() {
	FILE *fp = fopen("/libmobile_config.bin", "wb");
	if (fp) {
		fwrite(nifiState.config, 1, MOBILE_CONFIG_SIZE, fp);
		fclose(fp);
	}
}

void enableNifi() {
	if (nifiInit)
		return;
	nifiState.serial = false;
	nifiState.wifi = false;
	nifiState.sockOpen = 0;
	strcpy(nifiState.number[0], "<none>");
	strcpy(nifiState.number[1], "<none>");
	FILE *fp = fopen("/libmobile_config.bin", "rb");
	if (fp) {
		fread(nifiState.config, 1, MOBILE_CONFIG_SIZE, fp);
		fclose(fp);
	}
	nifiState.adapter = mobile_new(&nifiState);
	nifiInit = true;
#define DEF_CB(NAME) mobile_def_##NAME(nifiState.adapter, (mobile_func_##NAME)NAME);
	DEF_CB(debug_log);
	DEF_CB(serial_disable);
	DEF_CB(serial_enable);
	DEF_CB(config_read);
	DEF_CB(config_write);
	DEF_CB(time_latch);
	DEF_CB(time_check_ms);
	DEF_CB(sock_open);
	DEF_CB(sock_close);
	DEF_CB(sock_connect);
	DEF_CB(sock_listen);
	DEF_CB(sock_accept);
	DEF_CB(sock_send);
	DEF_CB(sock_recv);
	DEF_CB(update_number);
#undef DEF_CB
	mobile_start(nifiState.adapter);
}

void disableNifi() {
	if (nifiInit) {
		mobile_stop(nifiState.adapter);
		if (nifiState.wifi) {
			Wifi_DisconnectAP();
			Wifi_DisableWifi();
			printLog("Disconnected from Wi-Fi\n");
		}
		nifiInit = false;
		free(nifiState.adapter);
		saveNifi();
	}
}

void updateNifi(int cycles) {
	nifiTime += cycles;
	cothread_yield();
	bool noSocketsBefore = (nifiState.wifi && !nifiState.sockOpen);
	mobile_loop(nifiState.adapter);
	if (noSocketsBefore && !nifiState.sockOpen) {
		Wifi_DisconnectAP();
		Wifi_DisableWifi();
		nifiState.wifi = false;
		printLog("Disconnected from Wi-Fi\n");
	}
}

void sendPacketByte(uint8_t byte) {
	static uint8_t next = 0xFF;
	linkReceivedData = next;
	if (nifiState.serial)
		next = mobile_transfer(nifiState.adapter, byte);
	else
		next = 0xFF;
}
