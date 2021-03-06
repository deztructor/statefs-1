/*
 * Copyright (C) 2013 Jolla Ltd.
 * Contact: Janne Hakonen <janne.hakonen@oss.tieto.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.

 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 * 
 * http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html
 */

#include "filewatcher.h"

#include <poll.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

int openFile(char const *filepath);
struct pollfd initPollerFd(int fileFd);


Watcher createFileWatcher(char* filepaths[], int count) {
    Watcher watcher;
    watcher.filepaths = filepaths;
    watcher.count = count;
    watcher.fds = malloc(sizeof(watcher.fds[0]) * count);
    watcher.pfds = malloc(sizeof(watcher.pfds[0]) * count);
    watcher.filepaths = malloc(sizeof(char*) * count);
    memcpy(watcher.filepaths, filepaths, sizeof(char*) * count);
    return watcher;
}

bool openWatcher(Watcher* watcher) {
    int i, j;
    for (i = 0; i < watcher->count; i++) {
        watcher->fds[i] = openFile(watcher->filepaths[i]);
        if (watcher->fds[i] < 0) {
            // remove invalid filepath from 'filepaths'
            for (j = i + 1; j < watcher->count; j++) {
                watcher->filepaths[j - 1] = watcher->filepaths[j];
            }
            watcher->count--;
            i--;
            continue;
        }
        watcher->pfds[i] = initPollerFd(watcher->fds[i]);
    }
    return watcher->count > 0;
}

void listenFileChanges(Watcher* watcher, void (*callback)(char*)) {
    int rc;
    int i;

    // ignore initial state of the files
    poll(watcher->pfds, watcher->count, 0);

    while (true) {
        rc = poll(watcher->pfds, watcher->count, -1);
        if (rc < 0) {
            fprintf(stderr, "poll returned %d: %s\n", rc, strerror(errno));
            return;
        }

        if (!rc) {
            fprintf(stderr, "No events... timeout?");
            continue;
        }

        for (i = 0; i < watcher->count; ++i) {
            if (watcher->pfds[i].revents == 0)
                continue;

            if (watcher->pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                fprintf(stderr, "Poll error(E H N)=(%d %d %d)\n"
                       , watcher->pfds[i].revents & (POLLERR)
                       , watcher->pfds[i].revents & (POLLHUP)
                       , watcher->pfds[i].revents & (POLLNVAL)
                    );
                return;
            }

            callback(watcher->filepaths[i]);
        }
    }
}

int openFile(char const *filepath) {
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Can't open %s\n", filepath);
    }
    return fd;
}

struct pollfd initPollerFd(int fileFd) {
    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fileFd;
    pfd.events = POLLIN | POLLPRI;
    return pfd;
}

void deleteFileWatcher(Watcher* watcher) {
    free(watcher->pfds);
    free(watcher->fds);
    free(watcher->filepaths);
}
