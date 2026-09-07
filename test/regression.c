#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "kaddr.h"
#include "kconnection.h"
#include "kselector_manager.h"
#include "kselectable.h"
#include "ksocket.h"
#include "ksync.h"

extern bool get_addr(const char *hostname, kgl_addr_type addr_type, struct addrinfo **res);

static int64_t now_msec(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void test_condition_events(void)
{
	kcond *manual = kcond_init(false);
	kcond_notice(manual);
	assert(kcond_try_wait(manual, 0));
	assert(kcond_try_wait(manual, 0));
	kcond_reset(manual);
	assert(!kcond_try_wait(manual, 0));
	kcond_destroy(manual);

	kcond *automatic = kcond_init(true);
	kcond_notice(automatic);
	assert(kcond_try_wait(automatic, 0));
	assert(!kcond_try_wait(automatic, 0));
	int64_t start = now_msec();
	assert(!kcond_try_wait(automatic, 50));
	assert(now_msec() - start >= 40);
	kcond_destroy(automatic);
}

static void test_address_helpers(void)
{
	struct addrinfo *res = NULL;
	assert(!get_addr("invalid host name", kgl_addr_ip, &res));
	assert(res == NULL);

	assert(get_addr("127.0.0.1", kgl_addr_ip, &res));
	struct sockaddr_in *source = (struct sockaddr_in *)res->ai_addr;
	uint16_t old_port = source->sin_port;
	sockaddr_i converted;
	ksocket_addrinfo_sockaddr(res, 12345, &converted);
	assert(source->sin_port == old_port);
	assert(ksocket_addr_port(&converted) == 12345);
	freeaddrinfo(res);

#ifdef KSOCKET_UNIX
	char long_path[sizeof(((struct sockaddr_un *)0)->sun_path) + 1];
	memset(long_path, 'x', sizeof(long_path) - 1);
	long_path[sizeof(long_path) - 1] = '\0';
	struct sockaddr_un unix_addr;
	assert(ksocket_unix_addr(long_path, &unix_addr) == -1);
#endif
}

static void test_socket_helpers(void)
{
	SOCKET fd = socket(AF_INET, SOCK_DGRAM, 0);
	assert(ksocket_opened(fd));
	assert(ksocket_is_block(fd));
	ksocket_no_block(fd);
	assert(!ksocket_is_block(fd));
	ksocket_close(fd);
}

static void test_udp_without_pktinfo(void)
{
	SOCKET receiver = socket(AF_INET, SOCK_DGRAM, 0);
	SOCKET sender = socket(AF_INET, SOCK_DGRAM, 0);
	assert(ksocket_opened(receiver) && ksocket_opened(sender));

	struct sockaddr_in bind_addr;
	memset(&bind_addr, 0, sizeof(bind_addr));
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(bind(receiver, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) == 0);
	socklen_t bind_len = sizeof(bind_addr);
	assert(getsockname(receiver, (struct sockaddr *)&bind_addr, &bind_len) == 0);
	assert(sendto(sender, "x", 1, 0, (struct sockaddr *)&bind_addr, bind_len) == 1);

	char byte = 0;
	kgl_iovec iov;
	iov.iov_base = &byte;
	iov.iov_len = 1;
	kgl_iovec buffers;
	buffers.iov_base = (char *)&iov;
	buffers.iov_len = 1;
	kconnection connection;
	memset(&connection, 0, sizeof(connection));
	connection.st.fd = receiver;
	connection.st.e[OP_READ].buffer = &buffers;
	assert(selectable_recvmsg(&connection.st) == 1);
	assert(byte == 'x');

	ksocket_close(sender);
	ksocket_close(receiver);
}

static void test_selector_shutdown(void)
{
	kasync_init();
	selector_manager_init(1, false);
	selector_manager_start(NULL, true);
#ifdef _WIN32
	Sleep(100);
#else
	usleep(100000);
#endif
	selector_manager_close();
	assert(!is_selector_manager_init());
}

int main(void)
{
	test_condition_events();
	test_address_helpers();
	test_socket_helpers();
	test_udp_without_pktinfo();
	test_selector_shutdown();
	puts("kasync regression tests passed");
	return 0;
}
