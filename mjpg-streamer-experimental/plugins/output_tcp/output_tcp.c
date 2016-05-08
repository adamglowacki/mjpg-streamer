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

#include <dirent.h>

#include "../../utils.h"
#include "../../mjpg_streamer.h"
#include "my_compiler.h"

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
static uint32_t max_frame_size;
static uint8_t *frame = NULL;
static uint32_t input_number = 0;

static uint32_t window = 10;
static char *addr = NULL;
static uint32_t port = 40405;
static uint32_t timeout_s = 30;

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
  static is_first_run = true;

  if(!is_first_run) {
    DBG("already cleaned up resources\n");
    return;
  }

  is_first_run = 0;
  OPRINT("cleaning up resources allocated by worker thread\n");

  if(frame != NULL)
    free(frame);
  close(fd);
}

void *worker_thread(void *arg) {
  /* set cleanup handler to cleanup allocated resources */
  pthread_cleanup_push(worker_cleanup, NULL);

  /* cleanup now */
  pthread_cleanup_pop(1);

  return NULL;
}

/* return zero if everything is ok */
int output_init(output_parameter *param) {
  param->argv[0] = OUTPUT_PLUGIN_NAME;
  /* show all parameters for debug purposes */
  uint32_t i;
  for(i = 0; i < param->argc; i++)
    DBG("argv[%d]=%s\n", i, param->argv[i]);

  reset_getopt();
  while (true) {
    int option_index = 0, c = 0;
    static struct option long_options[] = {
      {SHORT_HELP, no_argument, 0, 0},
      {LONG_HELP, no_argument, 0, 0},
      {SHORT_ADDR, required_argument, 0, 0},
      {LONG_ADDR, required_argument, 0, 0},
      {SHORT_PORT, required_argument, 0, 0},
      {LONG_PORT, required_argument, 0, 0}
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
    if (option_index == 0 || option_index == 1) {
    } else if (option_index 

    /* below we compare pointers, not strings; that is far more efficient */
    char *choice = long_options[option_index][0];

    if (if (strcmp(choice, short_help_param) == 0 || choice == long_help_param) {
      DBG("help param");
      help();
      return 1;
    } else if (choice == short_addr_param || choice == long_addr_param) {
      DBG("addr param");
      if (addr != NULL)
        free(addr);
      addr = malloc(strlen(optarg) + 1);
      strcpy(addr, optarg);
    } else if (choice == short_port_param || choice == long_port_param) {
      DBG("port param");
      if (sscanf(optarg, "%u", &port) != 1)
        return 1;
    } else if (choice == short_window_param || choice == long_window_param) {
      DBG("window param");
      if (sscanf(optarg, "%u", &window) != 1)
        return 1;
    } else if (choice == short_timeout_param || choice == long_timeout_param) {
      DBG("timeout param");
      if (sscanf(optarg, "%u", &timeout_s))
        return 1;
    } else if (choice == short_input_param || choice == long_input_param) {
      DBG("input param");
      if (sscanf(optarg, "%u", &input_number))
        return 1;
    }
  }

  pglobal = param->global;
  if (input_number >= pglobal->incnt) {
    OPRINT("Error: the %u input plugin number is too much for only"
        " %u plugins loaded\n", input_number, pglobal->incnt);
    return 1;
  }
  if (addr == NULL) {
    OPRINT("Error: missing recipient's address\n");
    return 1;
  }
  OPRINT("input plugin.......: (%u) %s\n", input_number, pglobal->in[input_number].plugin);
  OPRINT("address............: %s\n", addr);
  OPRINT("port...............: %u\n", port);
  OPRINT("window.............: %u frames\n", window);
  OPRINT("timeout............: %u s\n", timeout_s);

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

