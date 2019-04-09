#ifndef STATUS_H
#define STATUS_H

#include <list>
#include <string>

struct Status {
  std::string cmdline;
  double budget;
  bool merged, stdout, stderr;
  struct timeval launch;
  Status(const std::string &cmdline_, double budget_, const struct timeval &launch_)
   : cmdline(cmdline_), budget(budget_),
     merged(false), stdout(true), stderr(true),
     launch(launch_) { }
};

extern std::list<Status> status_state;

void status_init();
void status_write(int fd, const char *data, int len);
void status_refresh();
void status_finish();

void term_init(bool tty);
const char *term_red();
const char *term_normal();

#endif
