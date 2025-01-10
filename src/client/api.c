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

  *notif_pipe = open(notif_pipe_path, O_RDONLY | O_NONBLOCK);
  if (*notif_pipe == -1) {
    perror("Failed to open notification pipe");
    return 1;
  }

  return 0;
}

int kvs_disconnect(void) {
  int req_fd = open(global_req_pipe_path, O_WRONLY);
  if (req_fd == -1) {
    perror("Failed to open request pipe");
    return 1;
  }

  char message[2];
  message[0] = OP_CODE_DISCONNECT;
  message[1] = '\0';
  if (write(req_fd, message, sizeof(message)) == -1) {
    perror("Failed to send disconnect message");
    close(req_fd);
    return 1;
  }

  close(req_fd);

  int resp_fd = open(global_resp_pipe_path, O_RDONLY);
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

  unlink(global_req_pipe_path);
  unlink(global_resp_pipe_path);
  unlink(global_notif_pipe_path);

  return 0;
}

int kvs_subscribe(const char *key) {
  int req_fd = open(global_req_pipe_path, O_WRONLY);
  if (req_fd == -1) {
    perror("Failed to open request pipe");
    return 1;
  }

  char message[1 + 40];
  message[0] = OP_CODE_SUBSCRIBE;
  snprintf(message + 1, 40, "%s", key);

  printf("Debug: Sending subscribe message: OP_CODE=%d, key=%s\n", message[0], message + 1);

  if (write(req_fd, message, sizeof(message)) == -1) {
    perror("Failed to send subscribe message");
    close(req_fd);
    return 1;
  }

  // Não fechar o req_fd aqui para manter o pipe aberto
  // close(req_fd);

  int resp_fd = open(global_resp_pipe_path, O_RDONLY);
  if (resp_fd == -1) {
    perror("Failed to open response pipe");
    close(req_fd); // Fechar o req_fd aqui se a abertura do resp_fd falhar
    return 1;
  }

  char response[2];
  ssize_t bytes_read = read(resp_fd, response, sizeof(response));
  if (bytes_read == -1) {
    perror("Failed to read subscribe response");
    close(req_fd); // Fechar o req_fd aqui se a leitura do resp_fd falhar
    close(resp_fd);
    return 1;
  }

  printf("Debug: Read %zd bytes from response pipe\n", bytes_read);
  printf("Debug: Received response: OP_CODE=%d, result=%d\n", response[0], response[1]);

  close(req_fd); // Fechar o req_fd aqui após a leitura da resposta
  close(resp_fd);

  if (response[1] != 0 && response[1] != 1) {
    fprintf(stderr, "Server returned an invalid response for subscribe\n");
    return 1;
  }

  return 0;
}

int kvs_unsubscribe(const char *key) {
  int req_fd = open(global_req_pipe_path, O_WRONLY);
  if (req_fd == -1) {
    perror("Failed to open request pipe");
    return 1;
  }

  char message[1 + 40];
  message[0] = OP_CODE_UNSUBSCRIBE;
  snprintf(message + 1, 40, "%s", key);

  if (write(req_fd, message, sizeof(message)) == -1) {
    perror("Failed to send unsubscribe message");
    close(req_fd);
    return 1;
  }

  close(req_fd);

  int resp_fd = open(global_resp_pipe_path, O_RDONLY);
  if (resp_fd == -1) {
    perror("Failed to open response pipe");
    return 1;
  }

  char response;
  if (read(resp_fd, &response, 1) == -1) {
    perror("Failed to read unsubscribe response");
    close(resp_fd);
    return 1;
  }

  close(resp_fd);

  if (response != 0 && response != 1) {
    fprintf(stderr, "Server returned an invalid response for unsubscribe\n");
    return 1;
  }

  return 0;
}
