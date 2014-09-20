/* Copyright (c) 2014 The crouton Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * WebSocket server that provides an interface to an extension running in
 * Chromium OS, used for clipboard synchronization and URL handling.
 *
 */

#define _GNU_SOURCE /* for ppoll */
#include "websocket.h"
#include <fcntl.h>
#include <sys/stat.h>

/* WebSocket constants */
#define VERSION "V1"
#define PORT 30001

/* Pipe constants */
const char* PIPE_DIR = "/tmp/crouton-ext";
const char* PIPEIN_FILENAME = "/tmp/crouton-ext/in";
const char* PIPEOUT_FILENAME = "/tmp/crouton-ext/out";
const int PIPEOUT_WRITE_TIMEOUT = 3000;

/* File descriptors */
static int pipein_fd = -1;
static int pipeout_fd = -1;

/* Open a pipe in non-blocking mode, then set it back to blocking mode. */
/* Returns fd on success, -1 if the pipe cannot be open, -2 if the O_NONBLOCK
 * flag cannot be cleared. */
static int pipe_open_block(const char* path, int oflag) {
    int fd;

    log(3, "%s", path);

    fd = open(path, oflag | O_NONBLOCK);
    if (fd < 0)
        return -1;

    /* Remove non-blocking flag */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        syserror("error in fnctl GETFL/SETFL.");
        close(fd);
        return -2;
    }

    return fd;
}

/**/
/* Pipe out functions */
/**/

/* Open the pipe out. Returns 0 on success, -1 on error. */
static int pipeout_open() {
    int i;

    log(2, "Opening pipe out...");

    /* Unfortunately, in the case where no reader is available (yet), opening
     * pipes for writing behaves as follows: In blocking mode, "open" blocks.
     * In non-blocking mode, it fails (returns -1). This means that we cannot
     * open the pipe, then use functions like poll/select to detect when a
     * reader becomes available. Waiting forever is also not an option: we do
     * want to block this server if a client "forgets" to read the answer back.
     * Therefore, we are forced to poll manually.
     * Using usleep is simpler, and probably better than measuring time elapsed:
     * If the system hangs for a while (like during large I/O writes), this will
     * still wait around PIPEOUT_WRITE_TIMEOUT ms of actual user time, instead
     * of clock time. */
    for (i = 0; i < PIPEOUT_WRITE_TIMEOUT/10; i++) {
        pipeout_fd = pipe_open_block(PIPEOUT_FILENAME, O_WRONLY);
        if (pipeout_fd >= 0)
            break;
        if (pipeout_fd == -2) /* fnctl error: this is fatal. */
            exit(1);
        usleep(10000);
    }

    if (pipeout_fd < 0) {
        error("Timeout while opening.");
        return -1;
    }

    return 0;
}

static void pipeout_close() {
    log(2, "Closing...");

    if (pipeout_fd < 0)
        return;

    close(pipeout_fd);
    pipeout_fd = -1;
}

static int pipeout_write(char* buffer, int len) {
    int n;

    log(3, "(fd=%d, len=%d)", pipeout_fd, len);

    if (pipeout_fd < 0)
        return -1;

    n = block_write(pipeout_fd, buffer, len);
    if (n != len) {
        error("Error writing to pipe.");
        pipeout_close();
    }
    return n;
}

/* Open pipe out, write a string, then close the pipe. */
static void pipeout_error(char* str) {
    pipeout_open();
    pipeout_write(str, strlen(str));
    pipeout_close();
}

/**/
/* Pipe in functions */
/**/

/* Flush the pipe (in case of error), close it, then reopen it. Reopening is
 * necessary to prevent poll from getting continuous POLLHUP when the process
 * that writes into the pipe terminates (croutonurlhandler for example).
 * This MUST be called before anything is written to pipeout to avoid race
 * condition, where we would flush out legitimate data from a second process */
static void pipein_reopen() {
    if (pipein_fd >= 0) {
        char buffer[BUFFERSIZE];
        while (read(pipein_fd, buffer, BUFFERSIZE) > 0);
        close(pipein_fd);
    }

    pipein_fd = pipe_open_block(PIPEIN_FILENAME, O_RDONLY);
    if (pipein_fd < 0) {
        syserror("Cannot open pipe in.");
        exit(1);
    }
}

/* Read data from the pipe, and forward it to the socket client. */
static void pipein_read() {
    int n;
    char buffer[FRAMEMAXHEADERSIZE+BUFFERSIZE];
    int first = 1;

    if (client_fd < 0) {
        log(1, "No client FD.");
        pipein_reopen();
        pipeout_error("EError: not connected.");
        return;
    }

    while (1) {
        n = read(pipein_fd, buffer+FRAMEMAXHEADERSIZE, BUFFERSIZE);
        log(3, "n=%d", n);

        if (n < 0) {
            /* This is very unlikely, and fatal. */
            syserror("Error reading from pipe.");
            exit(1);
        } else if (n == 0) {
            break;
        }

        /* Write a text frame for the first packet, then cont frames. */
        n = socket_client_write_frame(buffer, n,
                                  first ? WS_OPCODE_TEXT : WS_OPCODE_CONT, 0);
        if (n < 0) {
            error("Error writing frame.");
            pipein_reopen();
            pipeout_error("EError: socket write error.");
            return;
        }

        first = 0;
    }

    log(3, "EOF");

    pipein_reopen();

    /* Empty FIN frame to finish the message. */
    n = socket_client_write_frame(buffer, 0,
                                  first ? WS_OPCODE_TEXT : WS_OPCODE_CONT, 1);
    if (n < 0) {
        error("Error writing frame.");
        pipeout_error("EError: socket write error");
        return;
    }

    log(2, "Reading answer from client...");

    int fin = 0;
    uint32_t maskkey;
    int retry = 0;

    /* Ignore return value, so we still read the frame even if pipeout
     * cannot be open. */
    pipeout_open();

    /* Read possibly fragmented message from WebSocket. */
    while (fin != 1) {
        int len = socket_client_read_frame_header(&fin, &maskkey, &retry);

        log(3, "len=%d fin=%d retry=%d...", len, fin, retry);

        if (retry)
            continue;

        if (len < 0)
            break;

        /* Read the whole frame, and write it to pipeout */
        while (len > 0) {
            int rlen = (len > BUFFERSIZE) ? BUFFERSIZE: len;
            if (socket_client_read_frame_data(buffer, rlen, maskkey) < 0) {
                pipeout_close();
                return;
            }
            /* Ignore return value as well */
            pipeout_write(buffer, rlen);
            len -= rlen;
        }
    }

    pipeout_close();
}

/* Check if filename is a valid FIFO pipe. If not create it.
 * Returns 0 on success, -1 on error. */
int checkfifo(const char* filename) {
    struct stat fstat;

    /* Check if file exists: if not, create the FIFO. */
    if (access(filename, F_OK) < 0) {
        if (mkfifo(filename,
                S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH) < 0) {
            syserror("Cannot create FIFO pipe.");
            return -1;
        }
        return 0;
    }

    /* We must be able to read and write the file. (only one direction is
     * necessary in croutonwebsocket, but croutonclip needs the other direction)
     */
    if (access(filename, R_OK|W_OK) < 0) {
        error("%s exists, but not readable and writable.",
                filename);
        return -1;
    }

    if (stat(filename, &fstat) < 0) {
        syserror("Cannot stat FIFO pipe.");
        return -1;
    }

    if (!S_ISFIFO(fstat.st_mode)) {
        error("%s exists, but is not a FIFO pipe.", filename);
        return -1;
    }

    return 0;
}

/* Initialise FIFO pipes. */
void pipe_init() {
    struct stat fstat;

    /* Check if directory exists: if not, create it. */
    if (access(PIPE_DIR, F_OK) < 0) {
        if (mkdir(PIPE_DIR, S_IRWXU|S_IRWXG|S_IRWXO) < 0) {
            syserror("Cannot create FIFO pipe directory.");
            exit(1);
        }
    } else {
        if (stat(PIPE_DIR, &fstat) < 0) {
            syserror("Cannot stat FIFO pipe directory.");
            exit(1);
        }

        if (!S_ISDIR(fstat.st_mode)) {
            error("%s exists, but is not a directory.", PIPE_DIR);
            exit(1);
        }
    }

    if (checkfifo(PIPEIN_FILENAME) < 0 ||
        checkfifo(PIPEOUT_FILENAME) < 0) {
        /* checkfifo prints an error already. */
        exit(1);
    }

    pipein_reopen();
}

/* Unrequested data came in from WebSocket client. */
static void socket_client_read() {
    char buffer[BUFFERSIZE];
    int length = 0;
    int fin = 0;
    uint32_t maskkey;
    int retry = 0;
    int data = 0; /* 1 if we received some valid data */

    /* Read possible fragmented message into buffer */
    while (fin != 1) {
        int curlen = socket_client_read_frame_header(&fin, &maskkey, &retry);

        if (retry) {
            if (!data) {
                /* We only got a control frame, go back to main loop. We will
                 * get called again if there is more data waiting. */
                return;
            } else {
                /* We already read some frames of a fragmented message: wait
                 * for the rest. */
                continue;
            }
        }

        if (curlen < 0) {
            socket_client_close(0);
            return;
        }

        if (length+curlen > BUFFERSIZE) {
            error("Message too big (%d>%d).", length+curlen, BUFFERSIZE);
            socket_client_close(1);
            return;
        }

        if (socket_client_read_frame_data(buffer+length, curlen, maskkey) < 0) {
            error("Read error.");
            socket_client_close(0);
            return;
        }

        length += curlen;
        data = 1;
    }

    /* In future versions, we can process such packets here. */

    /* In the current version, this is actually never supposed to happen:
     * close the connection */
    error("Received an unexpected packet from client (%d, %c).",
          length, buffer[0]);
    socket_client_close(0);
}

static int terminate = 0;

static void signal_handler(int sig) {
    terminate = 1;
}

int main(int argc, char **argv) {
    int n;
    /* Poll array:
     * 0 - server_fd
     * 1 - pipein_fd
     * 2 - client_fd (if any)
     */
    struct pollfd fds[3];
    int nfds = 3;
    sigset_t sigmask;
    sigset_t sigmask_orig;
    struct sigaction act;
    int c;

    while ((c = getopt(argc, argv, "v:")) != -1) {
        switch (c) {
        case 'v':
            verbose = atoi(optarg);
            break;
        default:
            fprintf(stderr, "%s [-v 0-3]\n", argv[0]);
            return 1;
        }
    }

    /* Termination signal handler. */
    memset(&act, 0, sizeof(act));
    act.sa_handler = signal_handler;

    if (sigaction(SIGHUP, &act, 0) < 0 ||
        sigaction(SIGINT, &act, 0) < 0 ||
        sigaction(SIGTERM, &act, 0) < 0) {
        syserror("sigaction error.");
        return 2;
    }

    /* Ignore SIGPIPE in all cases: it may happen, since we write to pipes, but
     * it is not fatal. */
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGPIPE);

    if (sigprocmask(SIG_BLOCK, &sigmask, NULL) < 0) {
        syserror("sigprocmask error.");
        return 2;
    }

    /* Ignore terminating signals, except when ppoll is running. Save current
     * mask in sigmask_orig. */
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGHUP);
    sigaddset(&sigmask, SIGINT);
    sigaddset(&sigmask, SIGTERM);

    if (sigprocmask(SIG_BLOCK, &sigmask, &sigmask_orig) < 0) {
        syserror("sigprocmask error.");
        return 2;
    }

    /* Prepare pollfd structure. */
    memset(fds, 0, sizeof(fds));
    fds[0].events = POLLIN;
    fds[1].events = POLLIN;
    fds[2].events = POLLIN;

    /* Initialise pipe and WebSocket server */
    socket_server_init(PORT);
    pipe_init();

    while (!terminate) {
        /* Make sure fds is up to date. */
        fds[0].fd = server_fd;
        fds[1].fd = pipein_fd;
        fds[2].fd = client_fd;

        /* Only handle signals in ppoll: this makes sure we complete processing
         * the current request before bailing out. */
        n = ppoll(fds, nfds, NULL, &sigmask_orig);

        log(3, "poll ret=%d (%d, %d, %d)\n", n,
                   fds[0].revents, fds[1].revents, fds[2].revents);

        if (n < 0) {
            /* Do not print error when ppoll is interupted by a signal. */
            if (errno != EINTR || verbose >= 1)
                syserror("ppoll error.");
            break;
        }

        if (fds[0].revents & POLLIN) {
            log(1, "WebSocket accept.");
            socket_server_accept(VERSION);
            n--;
        }
        if (fds[1].revents & POLLIN) {
            log(2, "Pipe fd ready.");
            pipein_read();
            n--;
        }
        if (fds[2].revents & POLLIN) {
            log(2, "Client fd ready.");
            socket_client_read();
            n--;
        }

        if (n > 0) { /* Some events were not handled, this is a problem */
            error("Some poll events could not be handled: "
                    "ret=%d (%d, %d, %d).",
                    n, fds[0].revents, fds[1].revents, fds[2].revents);
            break;
        }
    }

    log(1, "Terminating...");

    if (client_fd)
        socket_client_close(1);

    return 0;
}
