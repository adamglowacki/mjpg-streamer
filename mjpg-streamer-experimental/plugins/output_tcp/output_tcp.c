/* This output plugin sends frames via TCP. Every frame receipt must be
 * acknowledged by the receiving app with a single byte. There is
 * a configurable window --- maximum number of frames that can be sent without
 * ACK. TCP connection is closed if the receiver does not send any ACKs for
 * a long period of time. */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <resolv.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <getopt.h>
#include <pthread.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>
#include <netdb.h>

#include <dirent.h>

#include "../../utils.h"
#include "../../mjpg_streamer.h"
#include "my_compiler.h"
#include "stse.h"

#define PLUGIN_NAME     "TCP output plugin"
#define SHORT_HELP      "h"
#define LONG_HELP       "help"
#define SHORT_ADDR      "a"
#define LONG_ADDR       "address"
#define SHORT_PORT      "p"
#define LONG_PORT       "port"
#define SHORT_WINDOW    "w"
#define LONG_WINDOW     "window"
#define SHORT_TIMEOUT   "t"
#define LONG_TIMEOUT    "timeout"
#define SHORT_INPUT     "i"
#define LONG_INPUT      "input"

static pthread_t worker;
static globals *pglobal;

static struct {
  uint32_t input_number;
  uint32_t window;
  char *addr;
  uint32_t port;
  uint32_t timeout_s;
} params;

static struct {
  struct addrinfo *rcv_info;
  int sock;
} net;

static struct {
  uint8_t *bytes;
  uint32_t size;
  uint32_t used;
} raw_buf;

stse_buf_t encoded_buf;

void help(void) {
  fprintf(stderr,
      " ---------------------------------------------------------------\n" \
      " Help for output plugin "PLUGIN_NAME"\n" \
      " ---------------------------------------------------------------\n" \
      " The following parameters can be passed to this plugin:\n" \
      " [-"SHORT_HELP" | --"LONG_HELP" ] show help and exit\n" \
      " [-"SHORT_ADDR" | --"LONG_ADDR" ] IP/DNS address of recipient\n" \
      " [-"SHORT_PORT" | --"LONG_PORT" ] TCP port of recipient\n" \
      " [-"SHORT_WINDOW" | --"LONG_WINDOW"] maximum number of pictures to be"
        " sent without ACK\n" \
      " [-"SHORT_TIMEOUT" | --"LONG_TIMEOUT"] maximum amount of seconds to"
        " wait for ACK\n" \
      " [-"SHORT_INPUT" | --"LONG_INPUT" ] read frames from the specified"
        " input plugin (first input plugin is the 0th)\n" \
      " ---------------------------------------------------------------\n");
}

void worker_cleanup(void *arg) {
  static bool is_first_run = true;

  if (!is_first_run) {
    DBG("already cleaned up resources\n");
    return;
  }

  is_first_run = 0;
  OPRINT("cleaning up resources allocated by worker thread\n");

  if (net.sock >= 0)
    close(net.sock);

  if (net.rcv_info != NULL)
    freeaddrinfo(net.rcv_info);

  if (raw_buf.bytes != NULL)
    free(raw_buf.bytes);

  if (encoded_buf.bytes != NULL)
    free(encoded_buf.bytes);
}

static inline bool resize_buffers(uint32_t new_frame_size) {
  uint32_t prev_buf_size = raw_buf.size;
  raw_buf.size = 2 * new_frame_size;
  DBG("increasing buffer size from %u to %u\n", prev_buf_size, raw_buf.size);
  uint8_t *tmp = realloc(raw_buf.bytes, raw_buf.size);
  if (tmp == NULL)
    return false;
  raw_buf.bytes = tmp;

  encoded_buf.size = 2 * raw_buf.size + 2;
  tmp = realloc(encoded_buf.bytes, encoded_buf.size);
  if (tmp == NULL)
    return false;
  encoded_buf.bytes = tmp;

  return true;
}

bool grab_frame(void) {
  uint32_t in_num = params.input_number;
  pthread_mutex_lock(&pglobal->in[in_num].db);
  pthread_cond_wait(&pglobal->in[in_num].db_update, &pglobal->in[in_num].db);
  uint32_t frame_size = pglobal->in[in_num].size;
  if (frame_size > raw_buf.size && !resize_buffers(frame_size)) {
    pthread_mutex_unlock(&pglobal->in[in_num].db);
    LOG("not enough memory\n");
    return false;
  }
  memcpy(raw_buf.bytes, pglobal->in[in_num].buf, frame_size);
  raw_buf.used = frame_size;
  pthread_mutex_unlock(&pglobal->in[in_num].db);

  return true;
}

bool transmit_frame(void) {
  encoded_buf.used = 0;
  if (!stse_start(&encoded_buf)) {
    DBG("stse_start(..) failed\n");
    return false;
  }
  if (!stse_append(&encoded_buf, raw_buf.bytes, raw_buf.used)) {
    DBG("stse_append(..) failed\n");
    return false;
  }
  if (!stse_end(&encoded_buf)) {
    DBG("stse_end(..) failed\n");
    return false;
  }
  ssize_t x;
  x = send(net.sock, encoded_buf.bytes, encoded_buf.used, 0);
  if (x == -1) {
    perror("send");
    return false;
  } else if (x < encoded_buf.used) {
    LOG("can't transmit the whole frame\n");
    return false;
  }

  return true;
}

void *worker_thread(void *arg) {
  /* set cleanup handler to cleanup allocated resources */
  pthread_cleanup_push(worker_cleanup, NULL);

  struct addrinfo hints = {0};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  int x = 0;
  do {
    x = getaddrinfo(params.addr, NULL, &hints, &net.rcv_info);
  } while (x != 0 && x == EAI_AGAIN);
  if (x != 0) {
    if (x == EAI_SYSTEM)
      perror("getaddrinfo");
    else
      fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(x));
    return NULL;
  }
  if (net.rcv_info->ai_addr->sa_family != AF_INET) {
    DBG("getaddrinfo returned a protocol family different from AF_INET");
    return NULL;
  }
  struct sockaddr_in *ip4 = (struct sockaddr_in *)(net.rcv_info->ai_addr);
  ip4->sin_port = htons((uint16_t)params.port);
  net.sock = socket(net.rcv_info->ai_family, net.rcv_info->ai_socktype,
      net.rcv_info->ai_protocol);
  if (net.sock < 0) {
    perror("socket");
    return NULL;
  }
  if (connect(net.sock, net.rcv_info->ai_addr,
        net.rcv_info->ai_addrlen) != 0) {
    perror("connect");
    return NULL;
  }
  
  uint32_t confirmed = 0, sent = 0;
  while (true) {
    while (confirmed + params.window < sent + 1) {
      DBG("waiting for acks\n");
      char buf[64];
      ssize_t x = recv(net.sock, buf, sizeof(buf), 0);
      if (x == 0) {
        DBG("socket closed\n");
        return NULL; /* socket closed */
      }
      if (x < 0) {
        perror("recv");
        return NULL;
      }
      DBG("received %d acks\n, x");
      confirmed += x;
    }
    DBG("wait for new frame\n");
    if (!grab_frame()) {
      DBG("grab_frame() failed\n");
      return NULL;
    }
    DBG("transmit the frame\n");
    if (!transmit_frame()) {
      DBG("transmit_frame() failed\n");
      return NULL;
    }
    DBG("frame transmitted\n");
    sent += 1;
  }

  /* cleanup now */
  pthread_cleanup_pop(1);

  return NULL;
}

static inline bool is_arg(const char *chosen, const char *short_option,
    const char *long_option) {
  if (strcmp(chosen, short_option) == 0)
    return true;
  if (strcmp(chosen, long_option) == 0)
    return true;
  return false;
}

/* return zero if everything is ok */
int output_init(output_parameter *param) {
  param->argv[0] = PLUGIN_NAME;

  /* show all parameters for debug purposes */
  uint32_t i;
  for(i = 0; i < param->argc; i++)
    DBG("argv[%u]=%s\n", i, param->argv[i]);

  /* default parameters */
  params.input_number = 0;
  params.window = 10;
  params.addr = NULL;
  params.port = 40405;
  params.timeout_s = 30;

  net.rcv_info = NULL;
  net.sock = -1;

  reset_getopt();
  while (true) {
    int option_index = 0, c = 0;
    static struct option long_options[] = {
      {SHORT_HELP, no_argument, 0, 0},
      {LONG_HELP, no_argument, 0, 0},
      {SHORT_ADDR, required_argument, 0, 0},
      {LONG_ADDR, required_argument, 0, 0},
      {SHORT_PORT, required_argument, 0, 0},
      {LONG_PORT, required_argument, 0, 0},
      {SHORT_WINDOW, required_argument, 0, 0},
      {LONG_WINDOW, required_argument, 0, 0},
      {SHORT_TIMEOUT, required_argument, 0, 0},
      {LONG_TIMEOUT, required_argument, 0, 0},
      {SHORT_INPUT, required_argument, 0, 0},
      {LONG_INPUT, required_argument, 0, 0},
      {0, 0, 0, 0}
    };
    c = getopt_long_only(param->argc, param->argv, "", long_options, &option_index);
    /* no more options */
    if (c == -1)
      break;
    /* unrecognized option */
    if (c == '?') {
      help();
      return 1;
    }
    /* internal bug */
    if (!(option_index + 1 < array_size(long_options)))
      return 1;
    const char *choice = long_options[option_index].name;
    if (is_arg(choice, SHORT_HELP, LONG_HELP)) {
      DBG("help param\n");
      help();
      return 1;
    } else if (is_arg(choice, SHORT_ADDR, LONG_ADDR)) {
      DBG("addr param\n");
      if (params.addr != NULL)
        free(params.addr);
      params.addr = malloc(strlen(optarg) + 1);
      if (params.addr == 0) {
        perror("malloc");
        return 1;
      }
      strcpy(params.addr, optarg);
    } else if (is_arg(choice, SHORT_PORT, LONG_PORT)) {
      DBG("port param\n");
      if (sscanf(optarg, "%u", &params.port) != 1)
        return 1;
    } else if (is_arg(choice, SHORT_WINDOW, LONG_WINDOW)) {
      DBG("window param\n");
      if (sscanf(optarg, "%u", &params.window) != 1)
        return 1;
    } else if (is_arg(choice, SHORT_TIMEOUT, LONG_TIMEOUT)) {
      DBG("timeout param\n");
      if (sscanf(optarg, "%u", &params.timeout_s))
        return 1;
    } else if (is_arg(choice, SHORT_INPUT, LONG_INPUT)) {
      DBG("input param\n");
      if (sscanf(optarg, "%u", &params.input_number))
        return 1;
    }
  }

  pglobal = param->global;
  if (params.input_number >= pglobal->incnt) {
    OPRINT("Error: the %u input plugin number is too large for only"
        " %u plugins loaded\n", params.input_number, pglobal->incnt);
    return 1;
  }
  if (params.addr == NULL) {
    OPRINT("Error: missing recipient's address\n");
    return 1;
  }
  /* buffers will be allocated when the first frame is ready */
  raw_buf.bytes = NULL;
  raw_buf.size = 0;
  encoded_buf.bytes = NULL;
  encoded_buf.size = 0;
  OPRINT("input plugin....: (%u) %s\n", params.input_number,
      pglobal->in[params.input_number].plugin);
  OPRINT("address.........: %s\n", params.addr);
  OPRINT("port............: %u\n", params.port);
  OPRINT("window..........: %u frames\n", params.window);
  OPRINT("timeout.........: %u s\n", params.timeout_s);

  return 0;
}

/* return zero always */
int output_stop(int id) {
    DBG("will cancel worker thread\n");
    pthread_cancel(worker);
    return 0;
}

/* return zero always */
int output_run(int id) {
    DBG("launching worker thread\n");
    pthread_create(&worker, 0, worker_thread, NULL);
    pthread_detach(worker);
    return 0;
}

