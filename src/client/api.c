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

static char global_req_pipe_path[MAX_PIPE_PATH_LENGTH];
static char global_resp_pipe_path[MAX_PIPE_PATH_LENGTH];
static char global_notif_pipe_path[MAX_PIPE_PATH_LENGTH];
static int req_fd = -1;
static int resp_fd = -1;
static int notif_fd = -1;

int kvs_connect(char const *req_pipe_path, char const *resp_pipe_path,
                char const *server_pipe_path, char const *notif_pipe_path,
                int *notif_pipe) {
  // Remover pipes existentes
  unlink(req_pipe_path);
  unlink(resp_pipe_path);
  unlink(notif_pipe_path);

  if (mkfifo(req_pipe_path, 0666) == -1 || mkfifo(resp_pipe_path, 0666) == -1 || mkfifo(notif_pipe_path, 0666) == -1) {
    perror("Failed to create named pipes");
    return 1;
  }

  strncpy(global_req_pipe_path, req_pipe_path, sizeof(global_req_pipe_path) - 1);
  strncpy(global_resp_pipe_path, resp_pipe_path, sizeof(global_resp_pipe_path) - 1);
  strncpy(global_notif_pipe_path, notif_pipe_path, sizeof(global_notif_pipe_path) - 1);

  int server_fd = open(server_pipe_path, O_WRONLY);
  if (server_fd == -1) {
    perror("Failed to open server pipe");
    return 1;
  }

  char message[1 + 3 * 40];
  message[0] = OP_CODE_CONNECT;
  snprintf(message + 1, 40, "%s", req_pipe_path);
  snprintf(message + 41, 40, "%s", resp_pipe_path);
  snprintf(message + 81, 40, "%s", notif_pipe_path);

  if (write(server_fd, message, sizeof(message)) == -1) {
    perror("Failed to send connection request");
    close(server_fd);
    return 1;
  }

  close(server_fd);


  req_fd = open(global_req_pipe_path, O_WRONLY);
  if (req_fd == -1) {
    perror("Failed to open request pipe");
    return 1;
  }
  printf("Debug: Waiting for server response on %s\n", resp_pipe_path);
  resp_fd = open(global_resp_pipe_path, O_RDONLY);
  if (resp_fd == -1) {
    perror("Failed to open response pipe");
    return 1;
  }
  notif_fd = open(global_notif_pipe_path, O_RDONLY);
  if (notif_fd == -1) {
    perror("Failed to open notification pipe");
    return 1;
  }
  *notif_pipe = notif_fd;
  printf("PAREI DE ESPERAR\n");
  char response[2];
  ssize_t bytes_read = read(resp_fd, response, sizeof(response));
  if (bytes_read == -1) {
    perror("Failed to read connect response");
    close(resp_fd);
    resp_fd = -1;
    return 1;
  }
  printf("Debug: Read %zd bytes from response pipe\n", bytes_read);
  printf("Debug: Received response: OP_CODE=%d, result=%d\n", response[0], response[1]);

  printf("Server returned %d for operation: connect\n", response[1]);

  return response[1];
}

int kvs_disconnect(void) {
  printf("Debug: kvs_disconnect called\n");

  // Abrir o req_fd se ainda não estiver aberto
  if (req_fd == -1) {
    req_fd = open(global_req_pipe_path, O_WRONLY);
    if (req_fd == -1) {
      perror("Failed to open request pipe");
      return 1;
    }
    printf("Debug: Request pipe opened successfully\n");
  }

  char message[1];
  message[0] = OP_CODE_DISCONNECT;

  printf("Debug: Sending disconnect message: OP_CODE=%d\n", message[0]);

  if (write(req_fd, message, sizeof(message)) == -1) {
    perror("Failed to send disconnect message");
    return 1;
  }
  printf("Debug: Disconnect message sent successfully\n");

  // Abrir o resp_fd se ainda não estiver aberto
  if (resp_fd == -1) {
    resp_fd = open(global_resp_pipe_path, O_RDONLY);
    if (resp_fd == -1) {
      perror("Failed to open response pipe");
      return 1;
    }
    printf("Debug: Response pipe opened successfully\n");
  }

  char response[2];
  ssize_t bytes_read = read(resp_fd, response, sizeof(response));
  if (bytes_read == -1) {
    perror("Failed to read disconnect response");
    return 1;
  }
  printf("Debug: Read %zd bytes from response pipe\n", bytes_read);
  printf("Debug: Received response: OP_CODE=%d, result=%d\n", response[0], response[1]);

  printf("Server returned %d for operation: disconnect\n", response[1]);

  if (response[1] != 0) {
    fprintf(stderr, "Server failed to disconnect\n");
    return 1;
  }

  close(req_fd);
  close(resp_fd);
  req_fd = -1;
  resp_fd = -1;

  unlink(global_req_pipe_path);
  unlink(global_resp_pipe_path);
  unlink(global_notif_pipe_path);

  printf("Debug: Disconnected successfully\n");
  return 0;
}

int kvs_subscribe(const char *key) {
  printf("Debug: kvs_subscribe called with key: %s\n", key);

  // Abrir o req_fd se ainda não estiver aberto
  if (req_fd == -1) {
    req_fd = open(global_req_pipe_path, O_WRONLY);
    if (req_fd == -1) {
      perror("Failed to open request pipe");
      return 1;
    }
    printf("Debug: Request pipe opened successfully\n");
  }

  char message[1 + 40];
  message[0] = OP_CODE_SUBSCRIBE;
  snprintf(message + 1, 40, "%s", key);

  printf("Debug: Sending subscribe message: OP_CODE=%d, key=%s\n", message[0], message + 1);

  if (write(req_fd, message, sizeof(message)) == -1) {
    perror("Failed to send subscribe message");
    return 1;
  }
  printf("Debug: Subscribe message sent successfully\n");

  // Abrir o resp_fd se ainda não estiver aberto
  if (resp_fd == -1) {
    resp_fd = open(global_resp_pipe_path, O_RDONLY);
    if (resp_fd == -1) {
      perror("Failed to open response pipe");
      return 1;
    }
    printf("Debug: Response pipe opened successfully\n");
  }

  char response[2];
  ssize_t bytes_read = read(resp_fd, response, sizeof(response));
  if (bytes_read == -1) {
    perror("Failed to read subscribe response");
    return 1;
  }
  printf("Debug: Read %zd bytes from response pipe\n", bytes_read);
  printf("Debug: Received response: OP_CODE=%d, result=%d\n", response[0], response[1]);

  if (response[1] != 0 && response[1] != 1) {
    fprintf(stderr, "Server returned an invalid response for subscribe\n");
    return 1;
  }

  printf("Server returned %d for operation: subscribe\n", response[1]);
  return 0;
}

int kvs_unsubscribe(const char *key) {
  printf("Debug: kvs_unsubscribe called with key: %s\n", key);

  // Abrir o req_fd se ainda não estiver aberto
  if (req_fd == -1) {
    req_fd = open(global_req_pipe_path, O_WRONLY);
    if (req_fd == -1) {
      perror("Failed to open request pipe");
      return 1;
    }
    printf("Debug: Request pipe opened successfully\n");
  }

  char message[1 + 40];
  message[0] = OP_CODE_UNSUBSCRIBE;
  snprintf(message + 1, 40, "%s", key);

  printf("Debug: Sending unsubscribe message: OP_CODE=%d, key=%s\n", message[0], message + 1);

  if (write(req_fd, message, sizeof(message)) == -1) {
    perror("Failed to send unsubscribe message");
    return 1;
  }
  printf("Debug: Unsubscribe message sent successfully\n");

  // Abrir o resp_fd se ainda não estiver aberto
  if (resp_fd == -1) {
    resp_fd = open(global_resp_pipe_path, O_RDONLY);
    if (resp_fd == -1) {
      perror("Failed to open response pipe");
      return 1;
    }
    printf("Debug: Response pipe opened successfully\n");
  }

  char response[2];
  ssize_t bytes_read = read(resp_fd, response, sizeof(response));
  if (bytes_read == -1) {
    perror("Failed to read unsubscribe response");
    return 1;
  }
  printf("Debug: Read %zd bytes from response pipe\n", bytes_read);
  printf("Debug: Received response: OP_CODE=%d, result=%d\n", response[0], response[1]);

  if (response[1] != 0 && response[1] != 1) {
    fprintf(stderr, "Server returned an invalid response for unsubscribe\n");
    return 1;
  }

  printf("Server returned %d for operation: unsubscribe\n", response[1]);
  return 0;
}
