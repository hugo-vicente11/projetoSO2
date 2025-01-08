#include "api.h"
#include "src/common/constants.h"
#include "src/common/protocol.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

int kvs_connect(char const *req_pipe_path, char const *resp_pipe_path,
                char const *server_pipe_path, char const *notif_pipe_path,
                int *notif_pipe) {
  // Create named pipes
  if (mkfifo(req_pipe_path, 0666) == -1 || mkfifo(resp_pipe_path, 0666) == -1 || mkfifo(notif_pipe_path, 0666) == -1) {
    perror("Failed to create named pipes");
    return 1;
  }

  // Open server pipe
  int server_fd = open(server_pipe_path, O_WRONLY);
  if (server_fd == -1) {
    perror("Failed to open server pipe");
    return 1;
  }

  // Prepare connection request message
  char message[1 + 3 * 40];
  message[0] = OP_CODE_CONNECT;
  snprintf(message + 1, 40, "%s", req_pipe_path);
  snprintf(message + 41, 40, "%s", resp_pipe_path);
  snprintf(message + 81, 40, "%s", notif_pipe_path);

  // Send connection request to server
  if (write(server_fd, message, sizeof(message)) == -1) {
    perror("Failed to send connection request");
    close(server_fd);
    return 1;
  }

  close(server_fd);

  // Open notification pipe for reading
  *notif_pipe = open(notif_pipe_path, O_RDONLY | O_NONBLOCK);
  if (*notif_pipe == -1) {
    perror("Failed to open notification pipe");
    return 1;
  }

  return 0;
}

int kvs_disconnect(void) {
  // Open request pipe
  int req_fd = open("/tmp/req", O_WRONLY);
  if (req_fd == -1) {
    perror("Failed to open request pipe");
    return 1;
  }

  // Send disconnect message
  char message[2];
  message[0] = OP_CODE_DISCONNECT;
  message[1] = '\0';
  if (write(req_fd, message, sizeof(message)) == -1) {
    perror("Failed to send disconnect message");
    close(req_fd);
    return 1;
  }

  close(req_fd);

  // Wait for server to handle the disconnect
  int resp_fd = open("/tmp/resp", O_RDONLY);
  if (resp_fd == -1) {
    perror("Failed to open response pipe");
    return 1;
  }

  char response;
  if (read(resp_fd, &response, 1) == -1) {
    perror("Failed to read disconnect response");
    close(resp_fd);
    return 1;
  }

  close(resp_fd);

  if (response != 0) {
    fprintf(stderr, "Server failed to disconnect\n");
    return 1;
  }

  return 0;
}

int kvs_subscribe(const char *key) {
  // Open request pipe
  int req_fd = open("/tmp/req", O_WRONLY);
  if (req_fd == -1) {
    perror("Failed to open request pipe");
    return 1;
  }

  // Prepare subscribe message
  char message[1 + 40];
  message[0] = OP_CODE_SUBSCRIBE;
  snprintf(message + 1, 40, "%s", key);

  // Send subscribe message
  if (write(req_fd, message, sizeof(message)) == -1) {
    perror("Failed to send subscribe message");
    close(req_fd);
    return 1;
  }

  close(req_fd);

  // Open response pipe
  int resp_fd = open("/tmp/resp", O_RDONLY);
  if (resp_fd == -1) {
    perror("Failed to open response pipe");
    return 1;
  }

  // Read response
  char response;
  if (read(resp_fd, &response, 1) == -1) {
    perror("Failed to read subscribe response");
    close(resp_fd);
    return 1;
  }

  close(resp_fd);

  // Check response
  if (response != 0 && response != 1) {
    fprintf(stderr, "Server returned an invalid response for subscribe\n");
    return 1;
  }

  return 0;
}

int kvs_unsubscribe(const char *key) {
  // Open request pipe
  int req_fd = open("/tmp/req", O_WRONLY);
  if (req_fd == -1) {
    perror("Failed to open request pipe");
    return 1;
  }

  // Prepare unsubscribe message
  char message[1 + 40];
  message[0] = OP_CODE_UNSUBSCRIBE;
  snprintf(message + 1, 40, "%s", key);

  // Send unsubscribe message
  if (write(req_fd, message, sizeof(message)) == -1) {
    perror("Failed to send unsubscribe message");
    close(req_fd);
    return 1;
  }

  close(req_fd);

  // Open response pipe
  int resp_fd = open("/tmp/resp", O_RDONLY);
  if (resp_fd == -1) {
    perror("Failed to open response pipe");
    return 1;
  }

  // Read response
  char response;
  if (read(resp_fd, &response, 1) == -1) {
    perror("Failed to read unsubscribe response");
    close(resp_fd);
    return 1;
  }

  close(resp_fd);

  // Check response
  if (response != 0 && response != 1) {
    fprintf(stderr, "Server returned an invalid response for unsubscribe\n");
    return 1;
  }

  return 0;
}
