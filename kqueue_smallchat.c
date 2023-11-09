/* smallchat.c -- Read clients input, send to all the other connected clients.
 *
 * Copyright (c) 2023, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the project name of nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/event.h>

/* ============================ Data structures =================================
 * The minimal stuff we can afford to have. This example must be simple
 * even for people that don't know a lot of C.
 * =========================================================================== */

#define MAX_CLIENTS 1000 // This is actually the higher file descriptor.
#define SERVER_PORT 7711

/* This structure represents a connected client. There is very little
 * info about it: the socket descriptor and the nick name, if set, otherwise
 * the first byte of the nickname is set to 0 if not set.
 * The client can set its nickname with /nick <nickname> command. */
struct client {
    int fd;     // Client socket.
    char *nick; // Nickname of the client.
};

/* This global structure encapsulates the global state of the chat. */
struct chatState {
    int serversock;     // Listening server socket.
    int numclients;     // Number of connected clients right now.
    int maxclient;      // The greatest 'clients' slot populated.
    struct client *clients[MAX_CLIENTS]; // Clients are set in the corresponding
                                         // slot of their socket descriptor.
};

struct chatState *Chat; // Initialized at startup.

/* ======================== Low level networking stuff ==========================
 * Here you will find basic socket stuff that should be part of
 * a decent standard C library, but you know... there are other
 * crazy goals for the future of C: like to make the whole language an
 * Undefined Behavior.
 * =========================================================================== */

/* Create a TCP socket listening to 'port' ready to accept connections. */
int createTCPServer(int port) {
    int s, yes = 1;
    struct sockaddr_in sa;

    if ((s = socket(AF_INET, SOCK_STREAM, 0)) == -1) return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)); // Best effort.

    memset(&sa,0,sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s,(struct sockaddr*)&sa,sizeof(sa)) == -1 ||
        listen(s, 511) == -1)
    {
        close(s);
        return -1;
    }
    return s;
}

/* Set the specified socket in non-blocking mode, with no delay flag. */
int socketSetNonBlockNoDelay(int fd) {
    int flags, yes = 1;

    /* Set the socket nonblocking.
     * Note that fcntl(2) for F_GETFL and F_SETFL can't be
     * interrupted by a signal. */
    if ((flags = fcntl(fd, F_GETFL)) == -1) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) return -1;

    /* This is best-effort. No need to check for errors. */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    return 0;
}

/* If the listening socket signaled there is a new connection ready to
 * be accepted, we accept(2) it and return -1 on error or the new client
 * socket on success. */
int acceptClient(int server_socket) {
    int s;

    while(1) {
        struct sockaddr_in sa;
        socklen_t slen = sizeof(sa);
        s = accept(server_socket,(struct sockaddr*)&sa,&slen);
        if (s == -1) {
            if (errno == EINTR)
                continue; /* Try again. */
            else
                return -1;
        }
        break;
    }
    return s;
}

/* We also define an allocator that always crashes on out of memory: you
 * will discover that in most programs designed to run for a long time, that
 * are not libraries, trying to recover from out of memory is often futile
 * and at the same time makes the whole program terrible. */
void *chatMalloc(size_t size) {
    void *ptr = malloc(size);
    if (ptr == NULL) {
        perror("Out of memory");
        exit(1);
    }
    return ptr;
}

/* Also aborting realloc(). */
void *chatRealloc(void *ptr, size_t size) {
    ptr = realloc(ptr,size);
    if (ptr == NULL) {
        perror("Out of memory");
        exit(1);
    }
    return ptr;
}

/* ====================== Small chat core implementation ========================
 * Here the idea is very simple: we accept new connections, read what clients
 * write us and fan-out (that is, send-to-all) the message to everybody
 * with the exception of the sender. And that is, of course, the most
 * simple chat system ever possible.
 * =========================================================================== */

/* Create a new client bound to 'fd'. This is called when a new client
 * connects. As a side effect updates the global Chat state. */
struct client *createClient(int fd) {
    char nick[32]; // Used to create an initial nick for the user.
    int nicklen = snprintf(nick,sizeof(nick),"user:%d",fd);
    struct client *c = chatMalloc(sizeof(*c));
    socketSetNonBlockNoDelay(fd); // Pretend this will not fail.
    c->fd = fd;
    c->nick = chatMalloc(nicklen+1);
    memcpy(c->nick,nick,nicklen);
    assert(Chat->clients[c->fd] == NULL); // This should be available.
    Chat->clients[c->fd] = c;
    /* We need to update the max client set if needed. */
    if (c->fd > Chat->maxclient) Chat->maxclient = c->fd;
    Chat->numclients++;
    return c;
}

/* Free a client, associated resources, and unbind it from the global
 * state in Chat. */
void freeClient(struct client *c) {
    free(c->nick);
    close(c->fd);
    Chat->clients[c->fd] = NULL;
    Chat->numclients--;
    if (Chat->maxclient == c->fd) {
        /* Ooops, this was the max client set. Let's find what is
         * the new highest slot used. */
        int j;
        for (j = Chat->maxclient-1; j >= 0; j--) {
            if (Chat->clients[j] != NULL) Chat->maxclient = j;
            break;
        }
        if (j == -1) Chat->maxclient = -1; // We no longer have clients.
    }
    free(c);
}

/* Allocate and init the global stuff. */
void initChat(void) {
    Chat = chatMalloc(sizeof(*Chat));
    memset(Chat,0,sizeof(*Chat));
    /* No clients at startup, of course. */
    Chat->maxclient = -1;
    Chat->numclients = 0;

    /* Create our listening socket, bound to the given port. This
     * is where our clients will connect. */
    Chat->serversock = createTCPServer(SERVER_PORT);
    if (Chat->serversock == -1) {
        perror("Creating listening socket");
        exit(1);
    }
}

/* Send the specified string to all connected clients but the one
 * having as socket descriptor 'excluded'. If you want to send something
 * to every client just set excluded to an impossible socket: -1. */
void sendMsgToAllClientsBut(int excluded, char *s, size_t len) {
    for (int j = 0; j <= Chat->maxclient; j++) {
        if (Chat->clients[j] == NULL ||
            Chat->clients[j]->fd == excluded) continue;

        /* Important: we don't do ANY BUFFERING. We just use the kernel
         * socket buffers. If the content does not fit, we don't care.
         * This is needed in order to keep this program simple. */
        write(Chat->clients[j]->fd,s,len);
    }
}

/* The main() function implements the main chat logic:
 * 1. Accept new clients connections if any.
 * 2. Check if any client sent us some new message.
 * 3. Send the message to all the other clients. */
int main() {
    initChat();
    int kq = kqueue();
    assert(-1 != kq);
    size_t maxevents = 32;
    struct kevent events[maxevents];
    struct kevent change;
    EV_SET(&change, Chat->serversock, EVFILT_READ, EV_ADD, 0, 0, NULL);
    assert(-1 != kevent(kq, &change, 1, NULL, 0, NULL));
    struct timespec tv;
    tv.tv_sec = 1;
    tv.tv_nsec = 0;
    while(1) {
        int nev = kevent(kq, NULL, 0, events, maxevents, &tv);
        for (int i = 0; i < nev; i++) {
            int fd = (int)events[i].ident;
            if (events[i].flags & EV_EOF) {
                printf("Client fd=%d disconnected\n", fd);
                freeClient(Chat->clients[fd]);
            } else if(fd == Chat->serversock) {
                printf("New client connection\n");
                int cfd = acceptClient(Chat->serversock);
                EV_SET(&change, cfd, EVFILT_READ, EV_ADD, 0, 0, NULL);
                kevent(kq, &change, 1, NULL, 0, NULL);
                if(cfd != -1) {
                    struct client *c = createClient(cfd);
                    char *welcome_msg =
                        "Welcome to Simple Chat! "
                        "Use /nick <nick> to set your nick.\n";
                    write(c->fd,welcome_msg,strlen(welcome_msg));
                    printf("Connected client fd=%d\n", cfd);
                } 
            } else if (events[i].filter == EVFILT_READ) {
                char readbuf[256];
                int nread = read(fd, readbuf, sizeof(readbuf)-1);
                if (nread <= 0) {
                    printf("Client fd=%d disconnected\n", fd);
                    freeClient(Chat->clients[fd]);
                } else {
                    struct client *c = Chat->clients[fd];
                    readbuf[nread] = 0;
                    if (readbuf[0] == '/') {
                        char *p;
                        p = strchr(readbuf, '\r'); if(p) *p = 0;
                        p = strchr(readbuf, '\n'); if(p) *p = 0;
                        char *arg = strchr(readbuf, ' ');
                        if (arg) {
                            *arg = 0;
                            arg++;
                        }
                        if (!strcmp(readbuf, "/nick") && arg) {
                            free(c->nick);
                            int nicklen = strlen(arg);
                            c->nick = chatMalloc(nicklen + 1);
                            memcpy(c->nick, arg, nicklen + 1);
                        } else if(!strcmp(readbuf, "/quit") || !strcmp(readbuf, "/exit")) {
                            printf("Client fd=%d quit\n", fd);
                            freeClient(c);
                        } else {
                            char *msg = "Unknown command\n";
                            write(c->fd, msg, strlen(msg));
                        }
                    } else {
                        char msg[256];
                        int msglen = sprintf(msg, "%s> %s", c->nick, readbuf);
                        if (msglen >= (int)sizeof(msg)) {
                            msglen = sizeof(msg)-1;
                        }
                        printf("%s", msg);
                        sendMsgToAllClientsBut(fd, msg, msglen);
                    }
                }
            }
        }
    }
    return 0;
}
