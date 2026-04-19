#define _GNU_SOURCE
#include "config.h"
#define CMD_STOP  "stop recovery ; setprop sys.usb.config adb"
#define CMD_START "start recovery ; setprop sys.usb.config adb"
#include "display.h"
#include "input.h"
#include "term.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/*  Signal flags  */
static volatile sig_atomic_t g_running  = 1;
static volatile sig_atomic_t g_sigchld  = 0;
static volatile sig_atomic_t g_vt_rel   = 0; /* deferred SIGUSR1 */
static volatile sig_atomic_t g_vt_acq   = 0; /* deferred SIGUSR2 */
static volatile sig_atomic_t g_vt_lost  = 0; /* VT fd gone (hard detach) */

/*  Saved tty state  */
static struct termios g_saved_tio;
static bool           g_tio_saved = false;

/*  Signal handlers  */
static void on_sig(int s) {
  if (s == SIGCHLD) { g_sigchld = 1; g_running = 0; }
  else                g_running = 0;
}
static void on_vt_rel(int s) { (void)s; g_vt_rel = 1; }
static void on_vt_acq(int s) { (void)s; g_vt_acq = 1; }

/*  Raw/restore stdin  */
static void stdin_raw(void) {
  if (!isatty(STDIN_FILENO)) return;
  struct termios t;
  tcgetattr(STDIN_FILENO, &t);
  g_saved_tio = t;
  g_tio_saved = true;
  cfmakeraw(&t);
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
}
static void stdin_restore(void) {
  if (g_tio_saved)
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_tio);
}

/*  --attach mode  */
static int do_attach(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) { perror("socket"); return 1; }
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("connect"); close(fd); return 1;
  }
  stdin_raw();
  uint8_t buf[IO_BUFSZ];
  while (1) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    FD_SET(fd, &rfds);
    if (select(fd+1, &rfds, NULL, NULL, NULL) < 0) break;
    if (FD_ISSET(STDIN_FILENO, &rfds)) {
      ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
      if (n <= 0) break;
      (void)write(fd, buf, (size_t)n);
    }
    if (FD_ISSET(fd, &rfds)) {
      ssize_t n = read(fd, buf, sizeof(buf));
      if (n <= 0) break;
      (void)write(STDOUT_FILENO, buf, (size_t)n);
    }
  }
  stdin_restore();
  close(fd);
  return 0;
}

/*  spawn_shell  */
static pid_t spawn_shell(int *pty_fd, int cols, int rows, const char *cmd) {
  struct winsize ws = {.ws_row = (unsigned short)rows,
                       .ws_col = (unsigned short)cols};
  pid_t pid = forkpty(pty_fd, NULL, NULL, &ws);
  if (pid < 0) { perror("forkpty"); return -1; }
  if (pid == 0) {
    setenv("TERM", TERM_ENV, 1);
    if (cmd) execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
    else     execl(DEFAULT_SHELL, DEFAULT_SHELL, NULL);
    _exit(1);
  }
  return pid;
}

/*  usage  */
static void usage(const char *prog) {
  printf("Android Recovery Console\n\n");
  printf("Usage: %s [options]\n\n", prog);
  printf("Options:\n");
  printf("  --background    Run as background daemon\n");
  printf("  --exec <cmd>    Execute specific command\n");
  printf("  --attach        Connect to background session\n");
  printf("  --help          Show this help\n");
}

/*  main  */
int main(int argc, char **argv) {
  char *exec_cmd      = NULL;
  bool  background    = false;

  for (int i = 1; i < argc; i++) {
    if      (!strcmp(argv[i], "--attach"))                    return do_attach();
    else if (!strcmp(argv[i], "--background"))                background = true;
    else if (!strcmp(argv[i], "--exec") && i+1 < argc)       exec_cmd = argv[++i];
    else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
      usage(argv[0]); return 0;
    }
  }

  /*  Input devices  */
  InputDev in = {0};
  input_init(&in);

  /*  Daemon / setsid  */
  if (background) {
    signal(SIGHUP,  SIG_IGN);
    signal(SIGINT,  SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    if (daemon(1, 0) < 0) { perror("daemon"); return 1; }
    (void)system(CMD_STOP); sleep(1);
  } else {
    setsid();
    (void)system(CMD_STOP); sleep(1);
  }

  /*  Display + terminal  */
  bool       is_service = !background && isatty(STDIN_FILENO);
  DisplayDev disp       = {0};
  Term       term       = {0};
  int        pty_fd     = -1, srv_fd = -1, cli_fd = -1;
  bool       is_blanked = false;

  if (!display_init(&disp)) return 1;
  term_init(&term, disp.width, disp.height, disp.cell_w, disp.cell_h);

  /*  Signal handlers  *
   * Install VT signals BEFORE vt_init() so there is no window where the
   * kernel could fire SIGUSR1/2 before our handlers are in place.      */
  {
    struct sigaction sa = {.sa_handler = on_sig};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);

    struct sigaction sv = {0};
    sigemptyset(&sv.sa_mask);
    sv.sa_handler = on_vt_rel; sigaction(SIGUSR1, &sv, NULL);
    sv.sa_handler = on_vt_acq; sigaction(SIGUSR2, &sv, NULL);
  }

  /*  VT init  */
  vt_init(&disp);

  /*  Per-mode signal overrides  */
  if (background) {
    signal(SIGHUP,  SIG_IGN);
    signal(SIGINT,  SIG_IGN);
    signal(SIGTERM, SIG_IGN);
  } else {
    signal(SIGINT,  SIG_IGN);  /* pass ^C to shell */
    signal(SIGTERM, SIG_IGN);
    struct sigaction sa = {.sa_handler = on_sig};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGHUP, &sa, NULL);
  }
  signal(SIGPIPE, SIG_IGN);

  /*  Unix socket server  */
  unlink(SOCKET_PATH);
  srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);
  if (bind(srv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind"); close(srv_fd); display_free(&disp);
    input_free(&in); term_free(&term); return 1;
  }
  listen(srv_fd, 1);
  (void)chmod(SOCKET_PATH, 0666);

  if (is_service) stdin_raw();
  else            LOG("starting service (no tty)");

  /*  Spawn shell  */
  pid_t child = spawn_shell(&pty_fd, term.cols, term.rows, exec_cmd);
  if (child < 0) { display_free(&disp); input_free(&in); term_free(&term); return 1; }
  fcntl(pty_fd, F_SETFL, fcntl(pty_fd, F_GETFL, 0) | O_NONBLOCK);

  /*  Main event loop  */
  while (g_running) {
    fd_set rfds, efds;
    FD_ZERO(&rfds);
    FD_ZERO(&efds);
    int maxfd = pty_fd;
    FD_SET(pty_fd, &rfds);

    if (srv_fd >= 0) {
      FD_SET(srv_fd, &rfds);
      if (srv_fd > maxfd) maxfd = srv_fd;
    }
    if (cli_fd >= 0) {
      FD_SET(cli_fd, &rfds);
      if (cli_fd > maxfd) maxfd = cli_fd;
    }
    if (is_service && !g_sigchld) {
      FD_SET(STDIN_FILENO, &rfds);
      if (STDIN_FILENO > maxfd) maxfd = STDIN_FILENO;
    }
    for (int i = 0; i < in.count; i++) {
      FD_SET(in.fds[i], &rfds);
      if (in.fds[i] > maxfd) maxfd = in.fds[i];
    }
    if (in.inotify_fd >= 0) {
      FD_SET(in.inotify_fd, &rfds);
      if (in.inotify_fd > maxfd) maxfd = in.inotify_fd;
    }
    /* Monitor VT fd for hard detachment (SIGHUP on tty, device removal). */
    int vt_fd = vt_get_fd();
    if (vt_fd >= 0) {
      FD_SET(vt_fd, &rfds);
      FD_SET(vt_fd, &efds);   /* exception: POLLHUP / error */
      if (vt_fd > maxfd) maxfd = vt_fd;
    }

    struct timeval tv = {0, SELECT_US};
    int nready = select(maxfd+1, &rfds, NULL, &efds, &tv);
    if (nready < 0) {
      if (errno == EINTR) goto handle_signals;
      break;
    }

    /*  VT fd exception → hard detachment  *
     * Kernel sent SIGHUP to the VT or the tty device went away.
     * Mark display inactive; the process keeps running (PTY/socket alive). */
    if (vt_fd >= 0 && FD_ISSET(vt_fd, &efds)) {
      g_vt_lost    = 1;
      g_vt_active  = 0;
      LOG("VT hard-detached; display suppressed");
    }

  handle_signals:
    /*  Deferred VT release (SIGUSR1)  */
    if (g_vt_rel) {
      g_vt_rel = 0;
      vt_release(&disp);
    }

    /*  Deferred VT acquire (SIGUSR2)  *
     * vt_acquire() does full framebuffer clear before setting active=1,
     * so the margin ghost artefacts are gone.                           */
    if (g_vt_acq) {
      g_vt_acq    = 0;
      g_vt_lost   = 0; /* re-acquired after a lost state resets it      */
      vt_acquire(&disp);
      input_flush(&in); /* discard keys buffered while VT was inactive   */
      for (int r = 0; r < term.rows; r++) term.dirty[r] = true;
      display_render(&disp, &term);
    }

    /*  Accept new client  */
    if (srv_fd >= 0 && FD_ISSET(srv_fd, &rfds)) {
      if (cli_fd >= 0) close(cli_fd);
      cli_fd = accept(srv_fd, NULL, NULL);
    }

    /*  Stdin (is_service mode)  */
    if (is_service && FD_ISSET(STDIN_FILENO, &rfds)) {
      uint8_t b[IO_BUFSZ];
      ssize_t n = read(STDIN_FILENO, b, sizeof(b));
      if (n > 0) { term_snap_to_bottom(&term); (void)write(pty_fd, b, (size_t)n); }
      else         is_service = false;
    }

    /*  Device hotplug  */
    if (in.inotify_fd >= 0 && FD_ISSET(in.inotify_fd, &rfds))
      input_handle_hotplug(&in);

    /*  Socket client input  */
    if (cli_fd >= 0 && FD_ISSET(cli_fd, &rfds)) {
      uint8_t b[IO_BUFSZ];
      ssize_t n = read(cli_fd, b, sizeof(b));
      if (n > 0) { term_snap_to_bottom(&term); (void)write(pty_fd, b, (size_t)n); }
      else        { close(cli_fd); cli_fd = -1; }
    }

    /*  PTY output → terminal emulator → render  */
    if (FD_ISSET(pty_fd, &rfds)) {
      uint8_t b[IO_BUFSZ];
      ssize_t n = read(pty_fd, b, sizeof(b));
      if (n > 0) {
        term_write(&term, b, (int)n);
        if (!is_blanked) display_render(&disp, &term);
        if (is_service)  (void)write(STDOUT_FILENO, b, (size_t)n);
        if (cli_fd >= 0) (void)write(cli_fd, b, (size_t)n);
      } else if (n == 0) {
        goto pty_dead;
      } else if (errno != EAGAIN && errno != EINTR) {
        goto pty_dead;
      }
    }

    /*  Keyboard / input events  */
    for (int i = 0; i < in.count; i++) {
      if (!FD_ISSET(in.fds[i], &rfds) || !g_vt_active) continue;
      struct input_event ev;
      ssize_t n = read(in.fds[i], &ev, sizeof(ev));
      if (n <= 0) {
        if (errno != EAGAIN) { input_remove_device(&in, i--); }
        continue;
      }
      if (n != sizeof(ev) || ev.type != EV_KEY) continue;

      /* Power key: toggle display blank / unblank. */
      if (ev.code == KEY_POWER && ev.value == 1) {
        is_blanked = !is_blanked;
        if (is_blanked) {
          display_blank(&disp, true);
        } else {
          display_blank(&disp, false);
          for (int r = 0; r < term.rows; r++) term.dirty[r] = true;
          display_render(&disp, &term);
        }
        continue;
      }

      /* Volume keys: scroll the terminal view. */
      if (ev.code == KEY_VOLUMEUP   && ev.value >= 1) {
        term_scroll(&term, -3); display_render(&disp, &term); continue;
      }
      if (ev.code == KEY_VOLUMEDOWN && ev.value >= 1) {
        term_scroll(&term,  3); display_render(&disp, &term); continue;
      }

      /* All other keys: translate to PTY sequences. */
      if (!is_blanked) {
        int w = input_ev_to_pty(&in, &ev, pty_fd);
        if (w > 0) term_snap_to_bottom(&term);
      }
    }
  }

pty_dead:
  if (child > 0) { kill(child, SIGTERM); waitpid(child, NULL, 0); }
  display_blank(&disp, true);   /* black screen on exit */
  stdin_restore();
  input_free(&in);
  term_free(&term);
  display_free(&disp);          /* vt_restore() called inside */
  if (cli_fd >= 0)  close(cli_fd);
  if (srv_fd >= 0)  { close(srv_fd); unlink(SOCKET_PATH); }
  if (pty_fd >= 0)  close(pty_fd);
  (void)system(CMD_START);
  return 0;
}
