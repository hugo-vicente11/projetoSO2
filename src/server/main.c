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

#define BUFFER_SIZE MAX_SESSION_COUNT

typedef struct {
    char req_pipe_path[40];
    char resp_pipe_path[40];
    char notif_pipe_path[40];
} ConnectionRequest;

ConnectionRequest buffer[BUFFER_SIZE];
volatile sig_atomic_t sigusr1_received = 0;

static pthread_t job_thread;

struct SharedData {
  DIR *dir;
  char *dir_name;
  pthread_mutex_t directory_mutex;
};

struct SessionData {
  char req_pipe_path[40];
  char resp_pipe_path[40];
  char notif_pipe_path[40];
  int req_fd;
  int resp_fd;
  int notif_fd;
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
  (void) sig;
  printf("Debug: RECEBI O SINAL!\n");

  // Eliminar todas as subscrições e encerrar os FIFOs
  pthread_mutex_lock(&buffer_mutex);
  for (int i = 0; i < MAX_SESSION_COUNT; i++) {
    if (sessions[i].active) {
      close(sessions[i].req_fd);
      close(sessions[i].resp_fd);
      close(sessions[i].notif_fd);
      sessions[i].active = 0;
      sessions[i].num_subscribed_keys = 0; // Remover todas as subscrições
    }
  }
  pthread_mutex_unlock(&buffer_mutex);
  printf("Debug: Sessões terminadas\n");
}

void notify_clients(const char *key, const char *value) {
  printf("Debug: Notifying clients about key: %s, value: %s\n", key, value);
  for (int i = 0; i < MAX_SESSION_COUNT; i++) {
    printf("Debug: Checking session %d, active: %d, num_subscribed_keys: %d\n", i, sessions[i].active, sessions[i].num_subscribed_keys);
    if (sessions[i].active) {
      for (int j = 0; j < sessions[i].num_subscribed_keys; j++) {
        printf("Debug: Comparing subscribed key: %s with changed key: %s\n", sessions[i].subscribed_keys[j], key);
        if (strcmp(sessions[i].subscribed_keys[j], key) == 0) {
          printf("Debug: Client %d subscribed to key: %s\n", i, key);
          printf("Debug: Trying to open notification pipe: %s\n", sessions[i].notif_pipe_path);
          int notif_fd = sessions[i].notif_fd;
          if (notif_fd != -1) {
            printf("CONSEGUI ABRIR O PIPE DE NOTIS DESTA SESSAO!\n");
            char message[2 * 41];
            snprintf(message, 41, "%s", key);
            snprintf(message + 41, 41, "%s", value);
            if (write(notif_fd, message, sizeof(message)) == -1) {
              perror("Failed to write notification");
            } else {
              printf("Debug: Notification sent to client %d\n", i);
            }
            close(notif_fd);
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
    printf("Debug: NOTIF PIPE: %s\n", session->notif_pipe_path);
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

    char local_buffer[41]; // Renomear para evitar sombreamento
    while (session->active) {
        ssize_t bytes_read = read(session->req_fd, local_buffer, sizeof(local_buffer));
        if (bytes_read <= 0) {
            if (bytes_read == 0) {
                printf("Debug: Request pipe closed by client\n");
            } else {
                perror("Failed to read from request pipe");
            }
            break;
        }

        printf("Debug: Read %zd bytes from request pipe\n", bytes_read);
        printf("Debug: Received message: OP_CODE=%d, key=%s\n", local_buffer[0], local_buffer + 1);

        response[0] = local_buffer[0];
        response[1] = 1;

        switch (local_buffer[0]) {
        case OP_CODE_SUBSCRIBE: {
            char key[MAX_STRING_SIZE];
            strncpy(key, local_buffer + 1, MAX_STRING_SIZE - 1);
            key[MAX_STRING_SIZE - 1] = '\0';

            printf("Debug: Checking if key exists: %s\n", key);
            // Verificar se a chave existe na hashtable
            if (kvs_key_exists(key)) {
                // Adicionar a chave à lista de chaves subscritas
                if (session->num_subscribed_keys < MAX_NUMBER_SUB) {
                    strncpy(session->subscribed_keys[session->num_subscribed_keys], key, MAX_STRING_SIZE);
                    session->num_subscribed_keys++;
                    response[1] = 1; // Key exists
                    printf("Debug: Subscribed to key: %s\n", key);
                    printf("Debug: Total subscribed keys: %d\n", session->num_subscribed_keys);
                } else {
                    printf("Debug: Maximum number of subscriptions reached\n");
                }
            } else {
                response[1] = 0; // Key does not exist
                printf("Debug: Key does not exist: %s\n", key);
            }

            break;
        }
        case OP_CODE_UNSUBSCRIBE: {
            char key[MAX_STRING_SIZE];
            strncpy(key, local_buffer + 1, MAX_STRING_SIZE - 1);
            key[MAX_STRING_SIZE - 1] = '\0';

            // Remover a chave da lista de chaves subscritas
            int found = 0;
            for (int i = 0; i < session->num_subscribed_keys; i++) {
                if (strcmp(session->subscribed_keys[i], key) == 0) {
                    for (int j = i; j < session->num_subscribed_keys - 1; j++) {
                        strncpy(session->subscribed_keys[j], session->subscribed_keys[j + 1], MAX_STRING_SIZE);
                    }
                    session->num_subscribed_keys--;
                    response[1] = 0; // Success
                    found = 1;
                    printf("Debug: Unsubscribed from key: %s\n", key);
                    printf("Debug: Total subscribed keys: %d\n", session->num_subscribed_keys);
                    break;
                }
            }
            if (!found) {
                response[1] = 1;
                printf("Debug: Subscription did not exist for key: %s\n", key);
            }

            break;
        }
        case OP_CODE_DISCONNECT:
            session->active = 0;
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

        printf("Debug: Sent response: OP_CODE=%d, result=%d\n", response[0], response[1]);
    }

    //close(session->req_fd);
    //close(session->resp_fd);
    //close(session->notif_fd);

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
    // REMOVER ANTES DE ENVIAR
    run_job(STDIN_FILENO, STDOUT_FILENO, "stdin");
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

static void *job_dispatcher(void *arg) {
    DIR *dir = (DIR *)arg;
    dispatch_threads(dir);
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
        char read_buffer[1 + 3 * 40];
        ssize_t bytes_read = read(register_fd, read_buffer, sizeof(read_buffer));
        if (bytes_read <= 0) {
            if (bytes_read == -1) {
                perror("Failed to read from register pipe");
            }
            continue;
        }

        if (read_buffer[0] == OP_CODE_CONNECT) {
            sem_wait(&semEmpty);
            pthread_mutex_lock(&buffer_mutex);
            sem_getvalue(&semFull, &index);
            strncpy(buffer[index].req_pipe_path, read_buffer + 1, 40);
            strncpy(buffer[index].resp_pipe_path, read_buffer + 41, 40);
            strncpy(buffer[index].notif_pipe_path, read_buffer + 81, 40);

            pthread_mutex_unlock(&buffer_mutex);
            sem_post(&semFull);
        }
    }
    return NULL;
}

void *manager_task(void *arg) {
    (void)arg; // Ignorar o parâmetro não utilizado
    int index;
    while (1) {
        sem_wait(&semFull);
        pthread_mutex_lock(&buffer_mutex);
        sem_getvalue(&semFull, &index);
        ConnectionRequest request = buffer[index];

        pthread_mutex_unlock(&buffer_mutex);
        sem_post(&semEmpty);

        // Handle the connection request
        int session_index = -1;
        for (int i = 0; i < MAX_SESSION_COUNT; i++) {
            if (!sessions[i].active) {
                session_index = i;
                break;
            }
        }

        if (session_index != -1) {
            strncpy(sessions[session_index].req_pipe_path, request.req_pipe_path, 40);
            strncpy(sessions[session_index].resp_pipe_path, request.resp_pipe_path, 40);
            strncpy(sessions[session_index].notif_pipe_path, request.notif_pipe_path, 40);
            sessions[session_index].active = 1;


            handle_session(&sessions[session_index]);
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

  printf("Debug: Initializing KVS\n");
  if (kvs_init()) {
    write_str(STDERR_FILENO, "Failed to initialize KVS\n");
    return 1;
  }

  printf("Debug: Unlinking register pipe path\n");
  unlink(register_pipe_path);

  printf("Debug: Creating register pipe\n");
  if (mkfifo(register_pipe_path, 0666) == -1) {
    perror("Failed to create register pipe");
    return 1;
  }

  printf("Debug: Opening register pipe\n");
  int register_fd = open(register_pipe_path, O_RDONLY);
  if (register_fd == -1) {
    perror("Failed to open register pipe");
    return 1;
  }

  printf("Debug: Opening jobs directory\n");
  DIR *dir = opendir(argv[1]);
  if (dir == NULL) {
    fprintf(stderr, "Failed to open directory: %s\n", argv[1]);
    return 0;
  }

  // Dispara thread para tratar dos .job
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

  if (pthread_create(&host_thread, NULL, host_task, &register_fd) != 0) {
    perror("Failed to create host thread");
    return 1;
  }

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
  unlink(register_pipe_path);

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
