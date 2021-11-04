/*
 * Copyright 2021 SiFive, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You should have received a copy of LICENSE.Apache2 along with
 * this software. If not, you may obtain a copy at
 *
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <poll.h>

#include <algorithm>

#include "poll.h"

#ifdef __APPLE__
#define USE_PSELECT 1
#else
#define USE_PPOLL 1
#endif

#ifdef USE_PSELECT

struct Poll::detail {
  std::vector<int> fds;
};

Poll::Poll() : imp(new Poll::detail) {
}

Poll::~Poll() {
}

void Poll::add(int fd) {
  imp->fds.push_back(fd);
}

void Poll::remove(int fd) {
  imp->fds.resize(
    std::remove(imp->fds.begin(), imp->fds.end(), fd)
    - imp->fds.begin());
}

void Poll::clear() {
  imp->fds.clear();
}

std::vector<int> Poll::wait(struct timespec *timeout, sigset_t *saved) {
  fd_set set;
  int nfds = 0;

  FD_ZERO(&set);
  for (auto fd : imp->fds) {
    if (fd >= nfds) nfds = fd + 1;
    FD_SET(fd, &set);
  }

  int retval = pselect(nfds, &set, 0, 0, timeout, saved);
  if (retval == -1 && errno != EINTR) {
    perror("pselect");
    exit(1);
  }

  std::vector<int> ready;
  if (retval > 0) {
    for (auto fd : imp->fds) {
      if (FD_ISSET(fd, &set)) {
        ready.push_back(fd);
      }
    }
  }

  return ready;
}

int Poll::max_fds() const {
  return 1024;
}

#endif

#ifdef USE_PPOLL

struct Poll::detail {
  std::vector<struct pollfd> pfds;
};

Poll::Poll() : imp(new Poll::detail) {
}

Poll::~Poll() {
}

void Poll::add(int fd) {
  imp->pfds.resize(imp->pfds.size() + 1);
  imp->pfds.back().fd = fd;
  imp->pfds.back().events = POLLIN;
}

void Poll::remove(int fd) {
  size_t i = 0, len = imp->pfds.size();
  struct pollfd *pfds = &imp->pfds[0];
  while (i < len) {
    if (pfds[i].fd == fd) {
      pfds[i].fd = pfds[len-1].fd;
      --len;
    } else {
      ++i;
    }
  }
  imp->pfds.resize(len);
}

void Poll::clear() {
  imp->pfds.clear();
}

std::vector<int> Poll::wait(struct timespec *timeout, sigset_t *saved) {
  int retval = ppoll(&imp->pfds[0], imp->pfds.size(), timeout, saved);
  if (retval == -1 && errno != EINTR) {
    perror("ppoll");
    exit(1);
  }

  std::vector<int> ready;
  if (retval > 0) {
    for (auto &pfd : imp->pfds) {
      if ((pfd.revents & (POLLIN|POLLHUP)) != 0) {
        ready.push_back(pfd.fd);
      }
    }
  }

  return ready;
}

int Poll::max_fds() const {
  return 1024;
}

#endif
