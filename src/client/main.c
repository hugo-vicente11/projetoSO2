#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "parser.h"
#include "src/client/api.h"
#include "src/common/constants.h"
#include "src/common/io.h"

void *notification_thread(void *arg) {
  int notif_pipe = *(int *)arg;
  char buffer[2 * 41];

  while (1) {
    ssize_t bytes_read = read(notif_pipe, buffer, sizeof(buffer));
    if (bytes_read > 0) {
      char key[41], value[41];
      strncpy(key, buffer, 40);
      key[40] = '\0';
      strncpy(value, buffer + 41, 40);
      value[40] = '\0';
      printf("(%s,%s)\n", key, value);
    } else if (bytes_read == -1 && errno != EAGAIN) {
      perror("Failed to read from notification pipe");
      break;
    }
  }

  return NULL;
}

int main(int argc, char *argv[]) {
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <client_unique_id> <register_pipe_path>\n",
            argv[0]);
    return 1;
  }

  char req_pipe_path[256];
  char resp_pipe_path[256];
  char notif_pipe_path[256];

  snprintf(req_pipe_path, sizeof(req_pipe_path), "/tmp/req_%s", argv[1]);
  snprintf(resp_pipe_path, sizeof(resp_pipe_path), "/tmp/resp_%s", argv[1]);
  snprintf(notif_pipe_path, sizeof(notif_pipe_path), "/tmp/notif_%s", argv[1]);

  char keys[MAX_NUMBER_SUB][MAX_STRING_SIZE] = {0};
  unsigned int delay_ms;
  size_t num;

  int notif_pipe;
  int response_code = kvs_connect(req_pipe_path, resp_pipe_path, argv[2], notif_pipe_path, &notif_pipe);
  if (response_code != 0) {
    fprintf(stderr, "Failed to connect to the server\n");
    return 1;
  }

  pthread_t notif_thread;
  if (pthread_create(&notif_thread, NULL, notification_thread, &notif_pipe) != 0) {
    perror("Failed to create notification thread");
    return 1;
  }

  while (1) {
    switch (get_next(STDIN_FILENO)) {
    case CMD_DISCONNECT:
        printf("Debug: Received CMD_DISCONNECT\n");
        response_code = kvs_disconnect();
        printf("Server returned %d for operation: disconnect\n", response_code);
        if (response_code != 0) {
            fprintf(stderr, "Failed to disconnect to the server\n");
            return 1;
        }
        pthread_cancel(notif_thread);
        pthread_join(notif_thread, NULL);
        printf("Disconnected from server\n");
        return 0;

    case CMD_SUBSCRIBE:
        printf("Debug: Received CMD_SUBSCRIBE\n");
        num = parse_list(STDIN_FILENO, keys, 1, MAX_STRING_SIZE);
        if (num == 0) {
            fprintf(stderr, "Invalid command. See HELP for usage\n");
            continue;
        }

        printf("Debug: Subscribing to key: %s\n", keys[0]);
        if (kvs_subscribe(keys[0])) {
            fprintf(stderr, "Command subscribe failed\n");
        } else {
            printf("Debug: Successfully subscribed to key: %s\n", keys[0]);
        }

        break;

    case CMD_UNSUBSCRIBE:
        printf("Debug: Received CMD_UNSUBSCRIBE\n");
        num = parse_list(STDIN_FILENO, keys, 1, MAX_STRING_SIZE);
        if (num == 0) {
            fprintf(stderr, "Invalid command. See HELP for usage\n");
            continue;
        }

        printf("Debug: Unsubscribing from key: %s\n", keys[0]);
        if (kvs_unsubscribe(keys[0])) {
            fprintf(stderr, "Command unsubscribe failed\n");
        } else {
            printf("Debug: Successfully unsubscribed from key: %s\n", keys[0]);
        }

        break;

    case CMD_DELAY:
        printf("Debug: Received CMD_DELAY\n");
        if (parse_delay(STDIN_FILENO, &delay_ms) == -1) {
            fprintf(stderr, "Invalid command. See HELP for usage\n");
            continue;
        }

        if (delay_ms > 0) {
            printf("Waiting...\n");
            delay(delay_ms);
        }
        break;

    case CMD_INVALID:
        fprintf(stderr, "Invalid command. See HELP for usage\n");
        break;

    case CMD_EMPTY:
        break;

    case EOC:
        // input should end in a disconnect, or it will loop here forever
        break;
    }
}
}
