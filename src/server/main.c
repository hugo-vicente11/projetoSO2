#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>

#include "src/common/protocol.h"
#include "src/common/constants.h"
#include "src/client/api.h"
#include "constants.h"
#include "io.h"
#include "operations.h"
#include "parser.h"

struct SharedData {
  DIR *dir;
  char *dir_name;
  pthread_mutex_t directory_mutex;
};

struct SessionData {
  char req_pipe_path[40];
  char resp_pipe_path[40];
  char notif_pipe_path[40];
  int active;
  pthread_t thread;
  char subscribed_keys[MAX_NUMBER_SUB][MAX_STRING_SIZE];
  int num_subscribed_keys;
};

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t n_current_backups_lock = PTHREAD_MUTEX_INITIALIZER;

size_t active_backups = 0; // Number of active backups
size_t max_backups;        // Maximum allowed simultaneous backups
size_t max_threads;        // Maximum allowed simultaneous threads
char *jobs_directory = NULL;
struct SessionData sessions[MAX_SESSION_COUNT];

int filter_job_files(const struct dirent *entry) {
  const char *dot = strrchr(entry->d_name, '.');
  if (dot != NULL && strcmp(dot, ".job") == 0) {
    return 1; // Keep this file (it has the .job extension)
  }
  return 0;
}

static int entry_files(const char *dir, struct dirent *entry, char *in_path,
                       char *out_path) {
  const char *dot = strrchr(entry->d_name, '.');
  if (dot == NULL || dot == entry->d_name || strlen(dot) != 4 ||
      strcmp(dot, ".job")) {
    return 1;
  }

  if (strlen(entry->d_name) + strlen(dir) + 2 > MAX_JOB_FILE_NAME_SIZE) {
    fprintf(stderr, "%s/%s\n", dir, entry->d_name);
    return 1;
  }

  strcpy(in_path, dir);
  strcat(in_path, "/");
  strcat(in_path, entry->d_name);

  strcpy(out_path, in_path);
  strcpy(strrchr(out_path, '.'), ".out");

  return 0;
}

void notify_clients(const char *key, const char *value) {
  for (int i = 0; i < MAX_SESSION_COUNT; i++) {
    if (sessions[i].active) {
      for (int j = 0; j < sessions[i].num_subscribed_keys; j++) {
        if (strcmp(sessions[i].subscribed_keys[j], key) == 0) {
          int notif_fd = open(sessions[i].notif_pipe_path, O_WRONLY);
          if (notif_fd != -1) {
            char message[2 * 41];
            snprintf(message, 41, "%s", key);
            snprintf(message + 41, 41, "%s", value);
            if (write(notif_fd, message, sizeof(message)) == -1) {
              perror("Failed to write notification");
            }
            close(notif_fd);
          }
        }
      }
    }
  }
}

static int run_job(int in_fd, int out_fd, char *filename) {
  size_t file_backups = 0;
  while (1) {
    char keys[MAX_WRITE_SIZE][MAX_STRING_SIZE] = {0};
    char values[MAX_WRITE_SIZE][MAX_STRING_SIZE] = {0};
    unsigned int delay;
    size_t num_pairs;

    switch (get_next(in_fd)) {
    case CMD_WRITE:
      num_pairs =
          parse_write(in_fd, keys, values, MAX_WRITE_SIZE, MAX_STRING_SIZE);
      if (num_pairs == 0) {
        write_str(STDERR_FILENO, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (kvs_write(num_pairs, keys, values)) {
        write_str(STDERR_FILENO, "Failed to write pair\n");
      } else {
        // Notificar clientes após a escrita bem-sucedida
        for (size_t i = 0; i < num_pairs; i++) {
          notify_clients(keys[i], values[i]);
        }
      }
      break;

    case CMD_READ:
      num_pairs =
          parse_read_delete(in_fd, keys, MAX_WRITE_SIZE, MAX_STRING_SIZE);

      if (num_pairs == 0) {
        write_str(STDERR_FILENO, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (kvs_read(num_pairs, keys, out_fd)) {
        write_str(STDERR_FILENO, "Failed to read pair\n");
      }
      break;

    case CMD_DELETE:
      num_pairs =
          parse_read_delete(in_fd, keys, MAX_WRITE_SIZE, MAX_STRING_SIZE);

      if (num_pairs == 0) {
        write_str(STDERR_FILENO, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (kvs_delete(num_pairs, keys, out_fd)) {
        write_str(STDERR_FILENO, "Failed to delete pair\n");
      } else {
        // Notificar clientes após a exclusão bem-sucedida
        for (size_t i = 0; i < num_pairs; i++) {
          notify_clients(keys[i], "DELETED");
        }
      }
      break;

    case CMD_SHOW:
      kvs_show(out_fd);
      break;

    case CMD_WAIT:
      if (parse_wait(in_fd, &delay, NULL) == -1) {
        write_str(STDERR_FILENO, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (delay > 0) {
        printf("Waiting %d seconds\n", delay / 1000);
        kvs_wait(delay);
      }
      break;

    case CMD_BACKUP:
      pthread_mutex_lock(&n_current_backups_lock);
      if (active_backups >= max_backups) {
        wait(NULL);
      } else {
        active_backups++;
      }
      pthread_mutex_unlock(&n_current_backups_lock);
      int aux = kvs_backup(++file_backups, filename, jobs_directory);

      if (aux < 0) {
        write_str(STDERR_FILENO, "Failed to do backup\n");
      } else if (aux == 1) {
        return 1;
      }
      break;

    case CMD_INVALID:
      write_str(STDERR_FILENO, "Invalid command. See HELP for usage\n");
      break;

    case CMD_HELP:
      write_str(STDOUT_FILENO,
                "Available commands:\n"
                "  WRITE [(key,value)(key2,value2),...]\n"
                "  READ [key,key2,...]\n"
                "  DELETE [key,key2,...]\n"
                "  SHOW\n"
                "  WAIT <delay_ms>\n"
                "  BACKUP\n" // Not implemented
                "  HELP\n");

      break;

    case CMD_EMPTY:
      break;

    case EOC:
      printf("EOF\n");
      return 0;
    }
  }
}

static void *handle_session(void *arg) {
  struct SessionData *session = (struct SessionData *)arg;
  int req_fd = open(session->req_pipe_path, O_RDONLY);
  int resp_fd = open(session->resp_pipe_path, O_WRONLY);
  int notif_fd = open(session->notif_pipe_path, O_WRONLY);

  if (req_fd == -1 || resp_fd == -1 || notif_fd == -1) {
    perror("Failed to open session pipes");
    session->active = 0;
    return NULL;
  }

  session->num_subscribed_keys = 0;

  char buffer[41];
  while (session->active) {
    if (read(req_fd, buffer, sizeof(buffer)) <= 0) {
      perror("Failed to read from request pipe");
      break;
    }

    char response[2]; // Response buffer with OP_CODE and result
    response[1] = 1; // Default result is failure

    switch (buffer[0]) {
    case OP_CODE_SUBSCRIBE: {
      response[0] = OP_CODE_SUBSCRIBE;
      char key[MAX_STRING_SIZE];
      strncpy(key, buffer + 1, MAX_STRING_SIZE - 1);
      key[MAX_STRING_SIZE - 1] = '\0';

      // Adicionar a chave à lista de chaves subscritas
      if (session->num_subscribed_keys < MAX_NUMBER_SUB) {
        strncpy(session->subscribed_keys[session->num_subscribed_keys], key, MAX_STRING_SIZE);
        session->num_subscribed_keys++;
        response[1] = 0; // Success
      }

      break;
    }
    case OP_CODE_UNSUBSCRIBE: {
      response[0] = OP_CODE_UNSUBSCRIBE;
      char key[MAX_STRING_SIZE];
      strncpy(key, buffer + 1, MAX_STRING_SIZE - 1);
      key[MAX_STRING_SIZE - 1] = '\0';

      // Remover a chave da lista de chaves subscritas
      for (int i = 0; i < session->num_subscribed_keys; i++) {
        if (strcmp(session->subscribed_keys[i], key) == 0) {
          for (int j = i; j < session->num_subscribed_keys - 1; j++) {
            strncpy(session->subscribed_keys[j], session->subscribed_keys[j + 1], MAX_STRING_SIZE);
          }
          session->num_subscribed_keys--;
          response[1] = 0; // Success
          break;
        }
      }

      break;
    }
    case OP_CODE_DISCONNECT:
      response[0] = OP_CODE_DISCONNECT;
      session->active = 0;
      response[1] = 0; // Success
      break;
    default:
      fprintf(stderr, "Unknown operation code\n");
      break;
    }

    if (write(resp_fd, response, sizeof(response)) == -1) {
      perror("Failed to write response to response pipe");
      break;
    }
  }

  close(req_fd);
  close(resp_fd);
  close(notif_fd);
  return NULL;
}


static void *get_file(void *arguments) {
  struct SharedData *thread_data = (struct SharedData *)arguments;
  DIR *dir = thread_data->dir;
  char *dir_name = thread_data->dir_name;

  if (pthread_mutex_lock(&thread_data->directory_mutex) != 0) {
    fprintf(stderr, "Thread failed to lock directory_mutex\n");
    return NULL;
  }

  struct dirent *entry;
  char in_path[MAX_JOB_FILE_NAME_SIZE], out_path[MAX_JOB_FILE_NAME_SIZE];
  while ((entry = readdir(dir)) != NULL) {
    if (entry_files(dir_name, entry, in_path, out_path)) {
      continue;
    }

    if (pthread_mutex_unlock(&thread_data->directory_mutex) != 0) {
      fprintf(stderr, "Thread failed to unlock directory_mutex\n");
      return NULL;
    }

    int in_fd = open(in_path, O_RDONLY);
    if (in_fd == -1) {
      write_str(STDERR_FILENO, "Failed to open input file: ");
      write_str(STDERR_FILENO, in_path);
      write_str(STDERR_FILENO, "\n");
      pthread_exit(NULL);
    }

    int out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd == -1) {
      write_str(STDERR_FILENO, "Failed to open output file: ");
      write_str(STDERR_FILENO, out_path);
      write_str(STDERR_FILENO, "\n");
      pthread_exit(NULL);
    }

    int out = run_job(in_fd, out_fd, entry->d_name);

    close(in_fd);
    close(out_fd);

    if (out) {
      if (closedir(dir) == -1) {
        fprintf(stderr, "Failed to close directory\n");
        return 0;
      }

      exit(0);
    }

    if (pthread_mutex_lock(&thread_data->directory_mutex) != 0) {
      fprintf(stderr, "Thread failed to lock directory_mutex\n");
      return NULL;
    }
  }

  if (pthread_mutex_unlock(&thread_data->directory_mutex) != 0) {
    fprintf(stderr, "Thread failed to unlock directory_mutex\n");
    return NULL;
  }

  pthread_exit(NULL);
}

static void dispatch_threads(DIR *dir) {
  pthread_t *threads = malloc(max_threads * sizeof(pthread_t));

  if (threads == NULL) {
    fprintf(stderr, "Failed to allocate memory for threads\n");
    return;
  }

  struct SharedData thread_data = {dir, jobs_directory,
                                   PTHREAD_MUTEX_INITIALIZER};

  for (size_t i = 0; i < max_threads; i++) {
    if (pthread_create(&threads[i], NULL, get_file, (void *)&thread_data) !=
        0) {
      fprintf(stderr, "Failed to create thread %zu\n", i);
      pthread_mutex_destroy(&thread_data.directory_mutex);
      free(threads);
      return;
    }
  }

  // ler do FIFO de registo

  for (unsigned int i = 0; i < max_threads; i++) {
    if (pthread_join(threads[i], NULL) != 0) {
      fprintf(stderr, "Failed to join thread %u\n", i);
      pthread_mutex_destroy(&thread_data.directory_mutex);
      free(threads);
      return;
    }
  }

  if (pthread_mutex_destroy(&thread_data.directory_mutex) != 0) {
    fprintf(stderr, "Failed to destroy directory_mutex\n");
  }

  free(threads);
}

int main(int argc, char **argv) {
  if (argc < 5) {
    write_str(STDERR_FILENO, "Usage: ");
    write_str(STDERR_FILENO, argv[0]);
    write_str(STDERR_FILENO, " <jobs_dir> <max_threads> <max_backups> <register_pipe_path>\n");
    return 1;
  }

  jobs_directory = argv[1];
  char *register_pipe_path = argv[4];

  char *endptr;
  max_backups = strtoul(argv[3], &endptr, 10);
  if (*endptr != '\0') {
    fprintf(stderr, "Invalid max_proc value\n");
    return 1;
  }

  max_threads = strtoul(argv[2], &endptr, 10);
  if (*endptr != '\0') {
    fprintf(stderr, "Invalid max_threads value\n");
    return 1;
  }

  if (max_backups <= 0) {
    write_str(STDERR_FILENO, "Invalid number of backups\n");
    return 0;
  }

  if (max_threads <= 0) {
    write_str(STDERR_FILENO, "Invalid number of threads\n");
    return 0;
  }

  if (kvs_init()) {
    write_str(STDERR_FILENO, "Failed to initialize KVS\n");
    return 1;
  }

  unlink(register_pipe_path);

  if (mkfifo(register_pipe_path, 0666) == -1) {
    perror("Failed to create register pipe");
    return 1;
  }

  int register_fd = open(register_pipe_path, O_RDONLY);
  if (register_fd == -1) {
    perror("Failed to open register pipe");
    return 1;
  }

  DIR *dir = opendir(argv[1]);
  if (dir == NULL) {
    fprintf(stderr, "Failed to open directory: %s\n", argv[1]);
    return 0;
  }

  dispatch_threads(dir);

  for (int i = 0; i < MAX_SESSION_COUNT; i++) {
    sessions[i].active = 0;
  }

  while (1) {
    char req_pipe_path[40], resp_pipe_path[40], notif_pipe_path[40];
    char buffer[1 + 3 * 40];
    if (read(register_fd, buffer, sizeof(buffer)) <= 0) {
      perror("Failed to read from register pipe");
      continue;
    }

    if (buffer[0] == OP_CODE_CONNECT) {
      strncpy(req_pipe_path, buffer + 1, 40);
      strncpy(resp_pipe_path, buffer + 41, 40);
      strncpy(notif_pipe_path, buffer + 81, 40);

      int session_index = -1;
      for (int i = 0; i < MAX_SESSION_COUNT; i++) {
        if (!sessions[i].active) {
          session_index = i;
          break;
        }
      }

      if (session_index == -1) {
        fprintf(stderr, "No available sessions\n");
        continue;
      }

      strncpy(sessions[session_index].req_pipe_path, req_pipe_path, 40);
      strncpy(sessions[session_index].resp_pipe_path, resp_pipe_path, 40);
      strncpy(sessions[session_index].notif_pipe_path, notif_pipe_path, 40);
      sessions[session_index].active = 1;

      if (pthread_create(&sessions[session_index].thread, NULL, handle_session, &sessions[session_index]) != 0) {
        perror("Failed to create session thread");
        sessions[session_index].active = 0;
      }
    }
  }

  if (closedir(dir) == -1) {
    fprintf(stderr, "Failed to close directory\n");
    return 0;
  }

  close(register_fd);
  unlink(register_pipe_path);

  while (active_backups > 0) {
    wait(NULL);
    active_backups--;
  }

  kvs_terminate();

  return 0;
}
