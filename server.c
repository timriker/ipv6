#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <unistd.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <assert.h>

#define CLIENT_QUEUE_LEN   10
#define SERVER_PORT        7002
#define BUFFERLENGTH       UINT16_MAX

/*
 *    hexdump - output a hex dump of a hexdump_buffer
 *
 *    data is pointer to the buffer
 *    length is length of buffer to convert
 *    linelen is number of chars to output per line
 *    split is number of chars in each chunk on a line
 */

char hexdump_buffer[BUFFERLENGTH / 16 * 71];
int hexdump(void const *data, size_t length, int linelen, int split)
{
    char *ptr;
    const char *inptr;
    int pos;
    int remaining = length;

    inptr = (char *)data;

    /*
     *    Assert that the hexdump_buffer is large enough. This should pretty much
     *    always be the case...
     *
     *    hex/ascii gap (2 chars) + closing \0 (1 char)
     *    split = 4 chars (2 each for hex/ascii) * number of splits
     *
     *    (hex = 3 chars, ascii = 1 char) * linelen number of chars
     */
    assert(sizeof(hexdump_buffer) >= (3 + (4 * (linelen / split)) + (linelen * 4)));
    memset(hexdump_buffer, 0, sizeof(hexdump_buffer));

    /*
     *    Loop through each line remaining
     */
    ptr = hexdump_buffer;
    while (remaining > 0) {
        int lrem;
        int splitcount;

        /*
         *    Loop through the hex chars of this line
         */
        lrem = remaining;
        splitcount = 0;
        for (pos = 0; pos < linelen; pos++) {

            /* Split hex section if required */
            if (split == splitcount++) {
                ptr += sprintf(ptr, "  ");
                splitcount = 1;
            }

            /* If remaining chars, output, else leave a space */
            if (lrem) {
                ptr += sprintf(ptr, "%.2x ", *((unsigned char *) inptr + pos));
                lrem--;
            } else {
                ptr += sprintf(ptr, "   ");
            }
        }

        *ptr++ = ' ';
        *ptr++ = ' ';

        /*
         *    Loop through the ASCII chars of this line
         */
        lrem = remaining;
        splitcount = 0;
        for (pos = 0; pos < linelen; pos++) {
            unsigned char c;

            /* Split ASCII section if required */
            if (split == splitcount++) {
                ptr += sprintf(ptr, "  ");
                splitcount = 1;
            }

            if (lrem) {
                c = *((unsigned char *) inptr + pos);
                if (c > 31 && c < 127) {
                    ptr += sprintf(ptr, "%c", c);
                } else {
                    ptr += sprintf(ptr, ".");
                }
                lrem--;
            }
        }

        *ptr++ = '\n';
        inptr += linelen;
        remaining -= linelen;
    }

    *ptr = '\0';

    return ptr - hexdump_buffer;
}

static char address[INET6_ADDRSTRLEN];
char *sockaddr2name(const struct sockaddr *sa)
{
    switch(sa->sa_family) {
        case AF_INET:
            inet_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr), address, INET6_ADDRSTRLEN);
            break;

        case AF_INET6:
            inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr), address, INET6_ADDRSTRLEN);
            break;

        default:
            strncpy(address, "Unknown AF", 11);
            return address;
    }

    return address;
}

// address + []:port
static char nameport[INET6_ADDRSTRLEN + 8];
char *sockaddr2nameport(const struct sockaddr *sa)
{
    switch(sa->sa_family) {
        case AF_INET:
            sprintf(nameport, "%s:%u", sockaddr2name(sa), ntohs(((struct sockaddr_in *)sa)->sin_port));
            break;

        case AF_INET6:
            sprintf(nameport, "[%s]:%u", sockaddr2name(sa), ntohs(((struct sockaddr_in6 *)sa)->sin6_port));
            break;

        default:
            strncpy(nameport, "Unknown AF", 11);
    }

    return nameport;
}

/* Close socket used for communication with client */
void close_client_socket(int client_sock_fd, int *max_fd, fd_set *set)
{
    int ret;

    printf("Closing connection #%d ...\n", client_sock_fd);
    ret = close(client_sock_fd);
    if (ret == -1) {
        perror("close()");
        client_sock_fd = -1;
    }
    FD_CLR(client_sock_fd, set);
    *max_fd = (client_sock_fd == *max_fd) ? client_sock_fd - 1 : *max_fd;
}

int main(int argc, char *argv[])
{
    int listen_sock_fd = -1, listen_udp_sock_fd, client_sock_fd = -1, sock_fd, max_fd = -1;
    struct sockaddr_in6 server_addr, client_addr;
    socklen_t client_addr_len;
    int ret, flag;
    fd_set sock_set, work_set;
    struct timeval timeout;
    char ch[BUFFERLENGTH];

    server_addr.sin6_family = AF_INET6;
    server_addr.sin6_addr = in6addr_any;
    server_addr.sin6_port = htons(SERVER_PORT);

    ((struct sockaddr *)&server_addr)->sa_family = AF_INET;
    ((struct sockaddr_in *)&server_addr)->sin_addr.s_addr = INADDR_ANY;
    ((struct sockaddr_in *)&server_addr)->sin_port = htons(SERVER_PORT);

     printf("Trying: %s\n", sockaddr2nameport((struct sockaddr *)&server_addr));
    printf("sizeof sockaddr %lu\n",sizeof(struct sockaddr));
    printf("sizeof sockaddr_in %lu\n",sizeof(struct sockaddr_in));
    printf("sizeof sockaddr_in6 %lu\n",sizeof(struct sockaddr_in6));
    printf("sizeof sockaddr_un %lu\n",sizeof(struct sockaddr_un));
    printf("sizeof sockaddr_storage %lu\n",sizeof(struct sockaddr_storage));

    /* Create socket for listening (client requests) */
    listen_sock_fd = socket(server_addr.sin6_family, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_fd == -1) {
        perror("tcp socket()");
        return EXIT_FAILURE;
    }

    /* Set socket to reuse address */
    flag = 1;
    ret = setsockopt(listen_sock_fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    if (ret == -1) {
        perror("setsockopt()");
        close(listen_sock_fd);
        return EXIT_FAILURE;
    }

    /* set non-blocking */
    int flags = fcntl(listen_sock_fd, F_GETFL, 0);
    fcntl(listen_sock_fd, F_SETFL, flags | O_NONBLOCK);

    /* Bind address and socket together */
    ret = bind(listen_sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret == -1) {
        perror("bind()");
        close(listen_sock_fd);
        return EXIT_FAILURE;
    }

    /* Create listening queue (client requests) */
    ret = listen(listen_sock_fd, CLIENT_QUEUE_LEN);
    if (ret == -1) {
        perror("listen()");
        close(listen_sock_fd);
        return EXIT_FAILURE;
    }

    /* Create udp socket for listening (client requests) */
    listen_udp_sock_fd = socket(server_addr.sin6_family, SOCK_DGRAM, 0);
    if (listen_udp_sock_fd == -1) {
        perror("udp socket()");
        return EXIT_FAILURE;
    }

    /* Bind address and socket together */
    ret = bind(listen_udp_sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret == -1) {
        perror("udp bind()");
        close(listen_udp_sock_fd);
        return EXIT_FAILURE;
    }

    client_addr_len = sizeof(client_addr);

    printf("Listening on tcp/udp: %s\n", sockaddr2nameport((struct sockaddr *)&server_addr));

    /* Initialize set of file descriptors */
    FD_ZERO(&sock_set);

    /* Add tcp listen socket to the set of sockets */
    FD_SET(listen_sock_fd, &sock_set);

    /* Add udp listen socket to the set of sockets */
    FD_SET(listen_udp_sock_fd, &sock_set);

    /* add stdin ? */
    //FD_SET(STDIN_FILENO, &sock_set);

    /* UDP Listen socket is the max socket */
    max_fd = listen_udp_sock_fd;

    while(1) {
        /* Set up timeout */
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        /* Copy current set of file descriptors to the working set of
         * file descriptors */
        memcpy(&work_set, &sock_set, sizeof(fd_set));

        /* Wait timeout for something to happen */
        ret = select(FD_SETSIZE, &work_set, NULL, NULL, &timeout);
        if (ret > 0) {
            /* Remember number of events on sockets */
            int count = ret;

            printf("Select %u ...\n", count);

            /* Iterate over all sockets */
            for(sock_fd = 0; sock_fd <= max_fd && count > 0; sock_fd++) {
                /* Test if event was on socket */
                if(FD_ISSET(sock_fd, &work_set)) {
                    count--;

                    /* Was event on main listen socket (new connection)? */
                    if(sock_fd == listen_sock_fd) {
                        /* Do TCP handshake with client */
                        client_sock_fd = accept(listen_sock_fd,
                                (struct sockaddr*)&client_addr,
                                &client_addr_len);
                        if (client_sock_fd == -1) {
                            perror("accept()");
                            close(listen_sock_fd);
                            return EXIT_FAILURE;
                        }

                        printf("New connection #%d from: %s ...\n",
                                client_sock_fd,
                                sockaddr2nameport((struct sockaddr *)&client_addr));

                        /* Add client socket to set of socket file descriptors */
                        FD_SET(client_sock_fd, &sock_set);
                        /* Update the biggest file descriptor */
                        max_fd = (client_sock_fd > max_fd) ? client_sock_fd : max_fd;
                    }
                    /* When event was not on listen socket, then it had to be on
                     * client socket and some data was received. */
                    else {
                        int nread, nwrite;

                        /* Is there any data to read. */
                        ret = ioctl(sock_fd, FIONREAD, &nread);
                        if (ret == -1) {
                            perror("ioctl()");
                            close(listen_sock_fd);
                            return EXIT_FAILURE;
                        }

                        /* When there is no data to read, then FIN packet was received
                         * and server should close the connection. */
                        if (nread == 0) {
                            close_client_socket(sock_fd, &max_fd, &sock_set);
                        } else {
                            /* Wait for data from client */
                            ret = recvfrom(sock_fd, ch, nread,
                                    MSG_DONTWAIT,
                                    (struct sockaddr *)&client_addr,
                                    &client_addr_len);
                            if (ret == -1) {
                                perror("recvfrom()");
                                close_client_socket(sock_fd, &max_fd, &sock_set);
                                return EXIT_FAILURE;
                            }

                            printf("Received %i bytes from #%d (%s)\n",
                                    nread,
                                    sock_fd,
                                    sockaddr2nameport((struct sockaddr *)&client_addr));

                            nwrite = hexdump(ch, nread, 16, 8);
                            //printf("sent\n%s",hexdump_buffer);
                            /* Send response to client */
                            printf("Sending %i bytes to #%d (%s)\n",
                                    nwrite,
                                    sock_fd,
                                    sockaddr2nameport((struct sockaddr *)&client_addr));
                            ret = sendto(sock_fd, hexdump_buffer, nwrite,
                                    0,
                                    (struct sockaddr *)&client_addr,
                                    client_addr_len);
                            if (ret == -1) {
                                perror("sendto()");
                                close_client_socket(sock_fd, &max_fd, &sock_set);
                                return EXIT_FAILURE;
                            }
                        }
                    }
                }
            }
        } else if(ret == 0) {
            printf("Timeout, %d fds ...\n", max_fd);
        } else {
            perror("select()");
            close(listen_sock_fd);
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}

// Local Variables: ***
// mode: C++ ***
// tab-width: 4 ***
// c-basic-offset: 4 ***
// indent-tabs-mode: nil ***
// End: ***
// ex: shiftwidth=4 tabstop=4
