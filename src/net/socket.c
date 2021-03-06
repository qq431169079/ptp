/* Copyright (c) 2017-2019 Foudil Brétel.  All rights reserved. */
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include "log.h"

static int sock_geterr(int fd) {
   int err = 1;
   socklen_t len = sizeof err;
   if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len) == -1)
       log_perror(LOG_ERR, "Failed getsockopt: %s.", errno);
   if (err)
      errno = err;              // set errno to the socket SO_ERROR
   return err;
}

/**
 * Endeavor to close a socket cleanly.
 * http://stackoverflow.com/a/12730776/421846
 */
bool sock_close(int fd) {      // *not* the Windows closesocket()
   if (fd < 0) {
       log_error("sock_close() got negative sock.");
       return false;
   }

   sock_geterr(fd);    // first clear any errors, which can cause close to fail
   if (shutdown(fd, SHUT_RDWR) < 0) // secondly, terminate the 'reliable' delivery
       if (errno != ENOTCONN && errno != EINVAL) // SGI causes EINVAL
           log_perror(LOG_ERR, "Failed shutdown: %s.", errno);
   if (close(fd) < 0) {         // finally call close()
       log_perror(LOG_ERR, "Failed close: %s.", errno);
       return false;
   }

   return true;
}

static int sock_setnonblock(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1) {
        log_perror(LOG_ERR, "Failed get fcntl: %s.", errno);
        return errno;
    }
    if (fcntl(sock, F_SETFL, flags |= O_NONBLOCK) == -1) {
        log_perror(LOG_ERR, "Failed set fcntl: %s.", errno);
        return errno;
    }

    return 0;
}

static int sock_setopts(int sock, const int family, const int socktype) {
    const int so_false = 0, so_true = 1;
    if ((setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&so_true,
                   sizeof(so_true)) < 0) ||
        (family == AF_INET6 &&
         setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&so_false,
                   sizeof(so_true)) < 0)) {
        log_perror(LOG_ERR, "Failed setsockopt: %s.", errno);
        return errno;
    }

    if (socktype == SOCK_DGRAM) {
        int n = 1024 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &n, sizeof(n));
        socklen_t len = sizeof(n);
        if (getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &n, &len) != -1)
            log_debug("UDP socket SO_RCVBUF=%d", n);
    }

    return 0;
}

/**
 * Returns the listening socket fd, or -1 if failure.
 */
int socket_init(const int socktype, const char bind_addr[], const char bind_port[])
{
    int sockfd = -1;
    if (socktype != SOCK_STREAM && socktype != SOCK_DGRAM) {
        log_error("Server init with unsupported socket type.");
        return sockfd;
    }

    // getaddrinfo for host
    struct addrinfo hints, *addrs;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = socktype;
    hints.ai_flags    = AI_PASSIVE;
    if (getaddrinfo(bind_addr, bind_port, &hints, &addrs)) {
        log_perror(LOG_ERR, "Failed getaddrinfo: %s.", errno);
        return -1;
    }

    // socket and bind
    struct addrinfo *it;
    for (it = addrs; it; it=it->ai_next) {
        sockfd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sockfd == -1)
            continue;

        if (sock_setopts(sockfd, it->ai_family, it->ai_socktype))
            return -1;

        if (sock_setnonblock(sockfd))
            return -1;

        if (bind(sockfd, it->ai_addr, it->ai_addrlen) == 0)
            break;

        sock_close(sockfd);
    }

    freeaddrinfo(addrs);

    if (!it) {
        log_perror(LOG_ERR, "Failed connect: %s.", errno);
        return -1;
    }

    if (socktype == SOCK_STREAM && listen(sockfd, 32)) {
        log_perror(LOG_ERR, "Failed listen: %s.", errno);
        return -1;
    }

    return sockfd;
}

bool socket_shutdown(int sock)
{
    if (!sock_close(sock))
        return false;
    log_info("Socket closed.");
    return true;
}

// https://beej.us/guide/bgnet/html/multi/sockaddr_inman.html
bool sockaddr_storage_fmt(char str[], const struct sockaddr_storage *ss)
{
    char *p = str;
    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *sa = (struct sockaddr_in *)ss;
        for (int i=0; i<4; i++) {
            snprintf(p, 3, "%02x",((unsigned char*)(&sa->sin_addr))[i]);
            p += 2;
        }
        snprintf(p, 6, ":%02x%02x",
                 ((unsigned char*)(&sa->sin_port))[0],
                 ((unsigned char*)(&sa->sin_port))[1]);
    }
    else if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *sa = (struct sockaddr_in6 *)ss;
        for (int i=0; i<16; i++) {
            snprintf(p, 3, "%02x",((unsigned char*)(&sa->sin6_addr))[i]);
            p += 2;
        }
        snprintf(p, 6, ":%02x%02x",
                 ((unsigned char*)(&sa->sin6_port))[0],
                 ((unsigned char*)(&sa->sin6_port))[1]);
    }
    else {
        return false;
    }
    return true;
}

bool sockaddr_storage_cmp4(const struct sockaddr_storage *a,
                           const struct sockaddr_storage *b)
{
    return
        ((struct sockaddr_in*)a)->sin_addr.s_addr ==
        ((struct sockaddr_in*)b)->sin_addr.s_addr &&
        ((struct sockaddr_in*)a)->sin_port ==
        ((struct sockaddr_in*)b)->sin_port;
}

bool sockaddr_storage_cmp6(const struct sockaddr_storage *a,
                           const struct sockaddr_storage *b)
{
    return
        (0 == memcmp(&((struct sockaddr_in6*)a)->sin6_addr.s6_addr,
                     &((struct sockaddr_in6*)b)->sin6_addr.s6_addr, 16) &&
         ((struct sockaddr_in6*)a)->sin6_port ==
         ((struct sockaddr_in6*)b)->sin6_port);
}
