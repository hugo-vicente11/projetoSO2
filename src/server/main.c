#include <signal.h>
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
#include <semaphore.h>
#include <errno.h>

#include "src/common/protocol.h"
#include "src/common/constants.h"
#include "src/client/api.h"
#include "constants.h"
#include "io.h"
#include "operations.h"
#include "parser.h"

typedef struct {
    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char resp_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];
} ConnectionRequest;

ConnectionRequest buffer[MAX_SESSION_COUNT];
static pthread_t job_thread;

struct SharedData {
  DIR *dir;
  char *dir_name;
  pthread_mutex_t directory_mutex;
};

struct SessionData {
  char req_pipe_path[MAX_PIPE_PATH_LENGTH];
  char resp_pipe_path[MAX_PIPE_PATH_LENGTH];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH];
  int req_fd;
  int resp_fd;
  int notif_fd;
  int active;
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
sem_t semEmpty;
sem_t semFull;
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;

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

void handle_sigusr1(int sig) {
  (void) sig; // Ignorar o parâmetro sig
  pthread_mutex_lock(&buffer_mutex); // Bloquear o mutex para proteger a seção crítica
  for (int i = 0; i < MAX_SESSION_COUNT; i++) {
    if (sessions[i].active) {
      // Fechar pipes da sessão ativa
      close(sessions[i].req_fd);
      close(sessions[i].resp_fd);
      close(sessions[i].notif_fd);
      sessions[i].active = 0;
      sessions[i].num_subscribed_keys = 0; // Remover todas as subscrições
    }
  }
  pthread_mutex_unlock(&buffer_mutex); // Desbloquear o mutex
}

void notify_clients(const char *key, const char *value) {
  for (int i = 0; i < MAX_SESSION_COUNT; i++) {
    if (sessions[i].active) {
      for (int j = 0; j < sessions[i].num_subscribed_keys; j++) {
        if (strcmp(sessions[i].subscribed_keys[j], key) == 0) {
          if (sessions[i].notif_fd != -1) {
            // Preparar mensagem de notificação
            char message[2 * (MAX_STRING_SIZE+1)];
            snprintf(message, (MAX_STRING_SIZE+1), "%s", key);
            snprintf(message + (MAX_STRING_SIZE+1), (MAX_STRING_SIZE+1), "%s", value);
            // Enviar notificação ao cliente
            if (write(sessions[i].notif_fd, message, sizeof(message)) == -1) {
              perror("Failed to write notification");
            }
          } else {
            perror("Failed to open notification pipe");
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

static void handle_session(void *arg) {
    struct SessionData *session = (struct SessionData *)arg;

    // Bloquear o sinal SIGUSR1
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    // Abrir pipes da sessão
    session->req_fd = open(session->req_pipe_path, O_RDONLY);
    session->resp_fd = open(session->resp_pipe_path, O_WRONLY);
    session->notif_fd = open(session->notif_pipe_path, O_WRONLY);

    if (session->req_fd == -1 || session->resp_fd == -1 || session->notif_fd == -1) {
        perror("Failed to open session pipes");
        session->active = 0;
        return;
    }

    // Enviar resposta de conexão
    char response[2];
    response[0] = OP_CODE_CONNECT;
    response[1] = 0; // Sucesso
    if (write(session->resp_fd, response, sizeof(response)) == -1) {
        perror("Failed to write connection response");
        session->active = 0;
        return;
    }

    session->num_subscribed_keys = 0;

    char local_buffer[MAX_PIPE_PATH_LENGTH+1]; // Renomear para evitar sombreamento
    while (session->active) {
        ssize_t bytes_read = read(session->req_fd, local_buffer, sizeof(local_buffer));
        if (bytes_read == -1) {
            perror("Failed to read from request pipe");
            break;
        }

        response[0] = local_buffer[0];
        response[1] = 1;

        switch (local_buffer[0]) {
        case OP_CODE_SUBSCRIBE: {
            char key[MAX_STRING_SIZE];
            strncpy(key, local_buffer + 1, MAX_STRING_SIZE - 1);
            key[MAX_STRING_SIZE - 1] = '\0';

            if (kvs_key_exists(key)) {
                if (session->num_subscribed_keys < MAX_NUMBER_SUB) {
                    strncpy(session->subscribed_keys[session->num_subscribed_keys], key, MAX_STRING_SIZE);
                    session->num_subscribed_keys++;
                    response[1] = 1; // Key exists
                } else {
                    fprintf(stderr, "Maximum number of subscriptions reached\n");
                }
            } else {
                response[1] = 0; // Key does not exist
            }

            break;
        }
        case OP_CODE_UNSUBSCRIBE: {
            char key[MAX_STRING_SIZE];
            strncpy(key, local_buffer + 1, MAX_STRING_SIZE - 1);
            key[MAX_STRING_SIZE - 1] = '\0';

            int found = 0;
            for (int i = 0; i < session->num_subscribed_keys; i++) {
                if (strcmp(session->subscribed_keys[i], key) == 0) {
                    for (int j = i; j < session->num_subscribed_keys - 1; j++) {
                        strncpy(session->subscribed_keys[j], session->subscribed_keys[j + 1], MAX_STRING_SIZE);
                    }
                    session->num_subscribed_keys--;
                    response[1] = 0; // Success
                    found = 1;
                    break;
                }
            }
            if (!found) {
                response[1] = 1;
            }
            break;
        }
        case OP_CODE_DISCONNECT:
            session->active = 0;
            session->num_subscribed_keys = 0; // Remover todas as subscrições
            response[1] = 0; // Success
            break;
        default:
            fprintf(stderr, "Unknown operation code\n");
            break;
        }

        if (write(session->resp_fd, response, sizeof(response)) == -1) {
            perror("Failed to write response to response pipe");
            break;
        }
    }

    // Marcar a sessão como inativa e notificar a thread gestora
    pthread_mutex_lock(&buffer_mutex);
    session->active = 0;
    pthread_mutex_unlock(&buffer_mutex);
    sem_post(&semEmpty);

    return;
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

    int result = run_job(in_fd, out_fd, entry->d_name); // Renomear para evitar sombreamento

    close(in_fd);
    close(out_fd);

    if (result) {
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

static void *job_dispatcher(void *arg) {
    DIR *dir = (DIR *)arg;
    dispatch_threads(dir); // Despachar threads para processar jobs
    return NULL;
}

void *host_task(void *arg) {
    int register_fd = *(int *)arg;
    int index;

    // Configurar o tratamento de sinal
    struct sigaction sa;
    sa.sa_handler = handle_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    while (1) {
        char read_buffer[1 + 3 * MAX_PIPE_PATH_LENGTH];
        ssize_t bytes_read = read(register_fd, read_buffer, sizeof(read_buffer));
        if (bytes_read <= 0) {
            if (bytes_read == -1) {
                perror("Failed to read from register pipe");
            }
            continue;
        }

        if (read_buffer[0] == OP_CODE_CONNECT) {
            sem_wait(&semEmpty); // Esperar por espaço no buffer
            pthread_mutex_lock(&buffer_mutex); // Bloquear o mutex para proteger o buffer
            sem_getvalue(&semFull, &index);
            strncpy(buffer[index].req_pipe_path, read_buffer + 1, MAX_PIPE_PATH_LENGTH);
            strncpy(buffer[index].resp_pipe_path, read_buffer + (MAX_PIPE_PATH_LENGTH+1), MAX_PIPE_PATH_LENGTH);
            strncpy(buffer[index].notif_pipe_path, read_buffer + (2*MAX_PIPE_PATH_LENGTH+1), MAX_PIPE_PATH_LENGTH);
            pthread_mutex_unlock(&buffer_mutex); // Desbloquear o mutex
            sem_post(&semFull); // Sinalizar que há um item no buffer
        }
    }
    return NULL;
}

void *manager_task(void *arg) {
    (void)arg; // Ignorar o parâmetro não utilizado
    int index;
    while (1) {
        sem_wait(&semFull); // Esperar por um item no buffer
        pthread_mutex_lock(&buffer_mutex); // Bloquear o mutex para proteger o buffer
        sem_getvalue(&semFull, &index);
        ConnectionRequest request = buffer[index];
        pthread_mutex_unlock(&buffer_mutex); // Desbloquear o mutex
        sem_post(&semEmpty); // Sinalizar que há espaço no buffer

        // Lidar com o pedido de conexão
        int session_index = -1;
        for (int i = 0; i < MAX_SESSION_COUNT; i++) {
            if (!sessions[i].active) {
                session_index = i;
                break;
            }
        }

        if (session_index != -1) {
            // Configurar a sessão com os caminhos dos pipes
            strncpy(sessions[session_index].req_pipe_path, request.req_pipe_path, MAX_PIPE_PATH_LENGTH);
            strncpy(sessions[session_index].resp_pipe_path, request.resp_pipe_path, MAX_PIPE_PATH_LENGTH);
            strncpy(sessions[session_index].notif_pipe_path, request.notif_pipe_path, MAX_PIPE_PATH_LENGTH);
            sessions[session_index].active = 1;

            handle_session(&sessions[session_index]); // Iniciar a sessão
        }
    }
    return NULL;
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

  unlink(register_pipe_path); // Remover pipe de registo existente

  if (mkfifo(register_pipe_path, 0666) == -1) {
    perror("Failed to create register pipe");
    return 1;
  }

  DIR *dir = opendir(argv[1]);
  if (dir == NULL) {
    fprintf(stderr, "Failed to open directory: %s\n", argv[1]);
    return 0;
  }

  // Criar thread para despachar jobs
  if (pthread_create(&job_thread, NULL, job_dispatcher, (void *)dir) != 0) {
    perror("Failed to create job dispatcher thread");
    return 1;
  }

  sem_init(&semEmpty, 0, MAX_SESSION_COUNT);
  sem_init(&semFull, 0, 0);

  for (int i = 0; i < MAX_SESSION_COUNT; i++) {
    sessions[i].active = 0;
  }

  pthread_t host_thread;
  pthread_t manager_threads[MAX_SESSION_COUNT];

  int register_fd = open(register_pipe_path, O_RDONLY);
  if (register_fd == -1) {
    perror("Failed to open register pipe");
    return 1;
  }

  // Criar thread para gerir conexões de clientes
  if (pthread_create(&host_thread, NULL, host_task, &register_fd) != 0) {
    perror("Failed to create host thread");
    return 1;
  }

  // Criar threads para gerir sessões
  for (int i = 0; i < MAX_SESSION_COUNT; i++) {
    if (pthread_create(&manager_threads[i], NULL, manager_task, NULL) != 0) {
      perror("Failed to create manager thread");
      return 1;
    }
  }

  pthread_join(host_thread, NULL);
  for (int i = 0; i < MAX_SESSION_COUNT; i++) {
    pthread_join(manager_threads[i], NULL);
  }

  if (closedir(dir) == -1) {
    fprintf(stderr, "Failed to close directory\n");
    return 0;
  }

  close(register_fd);
  unlink(register_pipe_path); // Remover pipe de registo

  // Esperar que todos os backups terminem
  while (active_backups > 0) {
    wait(NULL);
    active_backups--;
  }

  kvs_terminate();
  pthread_join(job_thread, NULL);
  sem_destroy(&semEmpty);
  sem_destroy(&semFull);
  return 0;
}
