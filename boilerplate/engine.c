/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * ========================================================================
 * TODO 1: bounded_buffer_push() - Producer thread adds log items to buffer
 * ========================================================================
 * 
 * Requirements:
 *   - Lock the mutex to protect shared state
 *   - Wait (block) if buffer is full, unless shutting down
 *   - Copy the log item to the tail position
 *   - Increment count and advance tail pointer (circular)
 *   - Signal waiting consumers (pthread_cond_signal)
 *   - Return 0 on success, -1 if rejected (e.g., during shutdown when full)
 *
 * Logic flow:
 *   Lock → Check shutdown/full → Wait if needed → Add item → Signal → Unlock
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    int rc;

    pthread_mutex_lock(&buffer->mutex);

    if (buffer->shutting_down && buffer->count >= LOG_BUFFER_CAPACITY) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    while (buffer->count >= LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    if (buffer->shutting_down && buffer->count >= LOG_BUFFER_CAPACITY) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    memcpy(&buffer->items[buffer->tail], item, sizeof(*item));
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);

    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * ========================================================================
 * TODO 2: bounded_buffer_pop() - Consumer thread removes log items from buffer
 * ========================================================================
 * 
 * Requirements:
 *   - Lock the mutex
 *   - Wait if buffer is empty AND not shutting down
 *   - Return 0 (EOF) if buffer is empty AND shutting down
 *   - Copy item from head position
 *   - Decrement count and advance head pointer (circular)
 *   - Signal waiting producers (pthread_cond_signal)
 *   - Return 1 on success, 0 on EOF
 *
 * Return values:
 *   1 = successfully got an item
 *   0 = EOF (shutdown + empty) - time to exit thread
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return 0;  /* EOF signal */
    }

    memcpy(item, &buffer->items[buffer->head], sizeof(*item));
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);

    pthread_mutex_unlock(&buffer->mutex);
    return 1;  /* Success */
}

/*
 * ========================================================================
 * TODO 3: logging_thread() - Consumer thread that writes logs to files
 * ========================================================================
 * 
 * Responsibilities:
 *   1. Loop forever, popping log items from the bounded buffer
 *   2. When container ID changes, close old file and open new one
 *   3. Create logs/ directory if it doesn't exist
 *   4. Write log chunks to the per-container log file (logs/container_id.log)
 *   5. Flush after each write for real-time visibility
 *   6. Exit cleanly when bounded_buffer_pop() returns 0 (EOF during shutdown)
 *
 * Flow:
 *   Initialize → Loop pop() → Check container_id → Open/close files → Write → Repeat
 *   When pop() returns 0 (EOF), break and cleanup
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    int rc;
    FILE *log_file = NULL;
    char current_container_id[CONTAINER_ID_LEN] = {0};

    while (1) {
        /* Pop from buffer - returns 1 if got item, 0 if EOF */
        rc = bounded_buffer_pop(&ctx->log_buffer, &item);

        if (rc == 0) {
            /* EOF - shutdown signal, time to exit */
            break;
        }

        /* If container ID changed, switch to new log file */
        if (strcmp(current_container_id, item.container_id) != 0) {
            if (log_file != NULL)
                fclose(log_file);

            /* Create logs directory */
            mkdir(LOG_DIR, 0755);

            /* Build log path: logs/container_id.log */
            char log_path[PATH_MAX];
            snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, item.container_id);

            /* Open/append to log file */
            log_file = fopen(log_path, "a");
            if (log_file == NULL) {
                perror("fopen");
                continue;
            }

            strncpy(current_container_id, item.container_id, sizeof(current_container_id) - 1);
        }

        /* Write log chunk to file */
        if (log_file != NULL) {
            fwrite(item.data, 1, item.length, log_file);
            fflush(log_file);  /* Ensure it's written immediately */
        }
    }

    /* Cleanup */
    if (log_file != NULL)
        fclose(log_file);

    return NULL;
}

/*
 * ========================================================================
 * TODO 4: child_fn() - Container entrypoint (runs in clone'd process)
 * ========================================================================
 * 
 * This function runs INSIDE the cloned child process with isolated namespaces.
 * It sets up the container environment and executes the shell.
 *
 * Required actions:
 *   1. Change root filesystem to container's rootfs via chroot()
 *   2. Change working directory to / (inside the new root)
 *   3. Mount /proc inside container (so ps, top, etc. work)
 *   4. Optionally mount /sys
 *   5. Apply nice value if specified (CPU priority)
 *   6. Redirect stdout/stderr to log file if provided
 *   7. Execute /bin/sh inside the container
 *
 * Return values:
 *   Returns 1 on any error (child won't continue)
 *   If execvp succeeds, process is replaced and function never returns
 */
int child_fn(void *arg)
{
    child_config_t *config = (child_config_t *)arg;
    char *shell[] = {"/bin/sh", NULL};

    /* Step 1: Chroot into the container's root filesystem */
    if (chroot(config->rootfs) < 0) {
        perror("chroot");
        return 1;
    }

    /* Step 2: Change to root directory (now inside container rootfs) */
    if (chdir("/") < 0) {
        perror("chdir");
        return 1;
    }

    /* Step 3: Mount /proc (essential for shell commands) */
    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("mount /proc");
        return 1;
    }

    /* Step 4: Mount /sys (optional, but helpful) */
    if (mount("sysfs", "/sys", "sysfs", 0, NULL) < 0) {
        perror("mount /sys");
        /* Non-fatal: continue even if /sys mount fails */
    }

    /* Step 5: Set CPU priority (nice value) if specified */
    if (config->nice_value != 0) {
        if (nice(config->nice_value) < 0) {
            perror("nice");
        }
    }

    /* Step 6: Redirect stdout and stderr to log file */
    if (config->log_write_fd >= 0) {
        dup2(config->log_write_fd, STDOUT_FILENO);
        dup2(config->log_write_fd, STDERR_FILENO);
        close(config->log_write_fd);
    }

    /* Step 7: Execute /bin/sh inside the container */
    execvp(shell[0], shell);

    /* If execvp returns, something went wrong */
    perror("execvp");
    return 1;
}

/*
 * Helper: Register a container with the kernel monitor module
 * Sends an ioctl to /dev/container_monitor with PID and memory limits
 */
int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

/*
 * Helper: Unregister a container from the kernel monitor module
 */
int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/* Global context for signal handlers */
static supervisor_ctx_t *g_ctx = NULL;

/*
 * Signal handler for SIGCHLD (child process died)
 * Just interrupts the accept() call so main loop can check status
 */
static void handle_sigchld(int sig)
{
    (void)sig;
    /* Interrupt accept() in the event loop */
}

/*
 * Signal handler for SIGINT and SIGTERM (shutdown signals)
 * Sets flag to exit supervisor loop
 */
static void handle_sigint(int sig)
{
    (void)sig;
    if (g_ctx != NULL) {
        g_ctx->should_stop = 1;
    }
}

/*
 * ========================================================================
 * TODO 5 & 6: run_supervisor() - Main event loop for the supervisor
 * ========================================================================
 * 
 * This is the heart of the project. The supervisor:
 *
 *   1. Opens /dev/container_monitor (kernel module IPC)
 *   2. Creates a UNIX domain socket at /tmp/mini_runtime.sock (control plane)
 *   3. Installs signal handlers (SIGCHLD, SIGINT, SIGTERM)
 *   4. Spawns the logging thread
 *   5. Enters an event loop:
 *      - Accepts client connections
 *      - Receives control requests (START, PS, STOP, etc.)
 *      - For START: clones a new container process
 *      - Registers container with kernel monitor
 *      - Tracks container metadata in a linked list
 *      - For PS: returns list of running containers
 *      - For STOP: kills container and unregisters from kernel
 *   6. On shutdown: waits for logger thread, cleans up resources
 */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;
    struct sockaddr_un addr;
    int client_sock;
    control_request_t client_req;
    control_response_t response;
    ssize_t nread;
    char stack[STACK_SIZE];
    container_record_t *container;
    char log_path[PATH_MAX];
    int log_fd;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    /* Initialize mutexes and bounded buffer */
    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* ===== Step 1: Open kernel monitor device ===== */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) {
        perror("open /dev/container_monitor");
        fprintf(stderr, "Is the kernel module loaded? (sudo insmod monitor.ko)\n");
        goto cleanup;
    }

    printf("[supervisor] Connected to kernel monitor\n");

    /* ===== Step 2: Create UNIX domain socket ===== */
    unlink(CONTROL_PATH);  /* Remove stale socket */
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        goto cleanup;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto cleanup;
    }

    if (listen(ctx.server_fd, 5) < 0) {
        perror("listen");
        goto cleanup;
    }

    printf("[supervisor] Control socket ready at %s\n", CONTROL_PATH);

    /* ===== Step 3: Install signal handlers ===== */
    signal(SIGCHLD, handle_sigchld);
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    /* ===== Step 4: Start logging thread ===== */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create (logger)");
        goto cleanup;
    }

    printf("[supervisor] Logger thread started\n");
    printf("[supervisor] Ready. Rootfs: %s\n", rootfs);

    /* ===== Step 5: Event loop - handle client requests ===== */
    while (!ctx.should_stop) {
        /* Accept client connection (with timeout via SIGCHLD) */
        client_sock = accept(ctx.server_fd, NULL, NULL);
        if (client_sock < 0) {
            if (errno == EINTR) {
                continue;  /* Interrupted by signal, try again */
            }
            perror("accept");
            continue;
        }

        /* Receive request from client */
        nread = recv(client_sock, &client_req, sizeof(client_req), 0);
        if (nread != sizeof(client_req)) {
            fprintf(stderr, "Invalid request size\n");
            close(client_sock);
            continue;
        }

        memset(&response, 0, sizeof(response));

        /* Process the request */
        switch (client_req.kind) {
        case CMD_START:
        case CMD_RUN: {
            /* Validate container ID */
            if (strlen(client_req.container_id) == 0) {
                snprintf(response.message, sizeof(response.message), "Invalid container ID");
                response.status = 1;
                break;
            }

            /* Check if container already exists */
            pthread_mutex_lock(&ctx.metadata_lock);
            container = ctx.containers;
            while (container != NULL) {
                if (strcmp(container->id, client_req.container_id) == 0) {
                    snprintf(response.message, sizeof(response.message), "Container already exists");
                    response.status = 1;
                    pthread_mutex_unlock(&ctx.metadata_lock);
                    goto send_response;
                }
                container = container->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);

            /* Create log file for this container */
            mkdir(LOG_DIR, 0755);
            snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, client_req.container_id);
            log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (log_fd < 0) {
                snprintf(response.message, sizeof(response.message), "Cannot create log file");
                response.status = 1;
                break;
            }

            /* Prepare child process configuration */
            child_config_t child_config;
            memset(&child_config, 0, sizeof(child_config));
            strncpy(child_config.id, client_req.container_id, sizeof(child_config.id) - 1);
            strncpy(child_config.rootfs, client_req.rootfs, sizeof(child_config.rootfs) - 1);
            strncpy(child_config.command, client_req.command, sizeof(child_config.command) - 1);
            child_config.nice_value = client_req.nice_value;
            child_config.log_write_fd = log_fd;

            /* Clone new container process with isolated namespaces */
            pid_t child_pid = clone(
                child_fn,
                stack + STACK_SIZE,
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                &child_config
            );

            close(log_fd);

            if (child_pid < 0) {
                snprintf(response.message, sizeof(response.message), "clone() failed");
                response.status = 1;
                break;
            }

            /* Register container with kernel monitor module */
            if (register_with_monitor(ctx.monitor_fd, client_req.container_id, child_pid,
                                     client_req.soft_limit_bytes, client_req.hard_limit_bytes) < 0) {
                fprintf(stderr, "Warning: Failed to register with monitor\n");
            }

            /* Add to container metadata list */
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *new_record = malloc(sizeof(*new_record));
            if (new_record != NULL) {
                memset(new_record, 0, sizeof(*new_record));
                strncpy(new_record->id, client_req.container_id, sizeof(new_record->id) - 1);
                new_record->host_pid = child_pid;
                new_record->state = CONTAINER_RUNNING;
                new_record->started_at = time(NULL);
                new_record->soft_limit_bytes = client_req.soft_limit_bytes;
                new_record->hard_limit_bytes = client_req.hard_limit_bytes;
                new_record->next = ctx.containers;
                ctx.containers = new_record;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);

            snprintf(response.message, sizeof(response.message), "Container started (PID %d)", child_pid);
            response.status = 0;
            break;
        }

        case CMD_PS: {
            /* List all containers */
            pthread_mutex_lock(&ctx.metadata_lock);
            snprintf(response.message, sizeof(response.message), "ID\tPID\tState\n");
            container = ctx.containers;
            while (container != NULL) {
                char *msg = response.message + strlen(response.message);
                snprintf(msg, sizeof(response.message) - strlen(response.message),
                        "%s\t%d\t%s\n",
                        container->id, container->host_pid,
                        state_to_string(container->state));
                container = container->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
            response.status = 0;
            break;
        }

        case CMD_STOP: {
            /* Stop (kill) a container */
            pthread_mutex_lock(&ctx.metadata_lock);
            container = ctx.containers;
            while (container != NULL) {
                if (strcmp(container->id, client_req.container_id) == 0) {
                    kill(container->host_pid, SIGKILL);
                    unregister_from_monitor(ctx.monitor_fd, client_req.container_id, container->host_pid);
                    container->state = CONTAINER_KILLED;
                    snprintf(response.message, sizeof(response.message), "Container stopped");
                    response.status = 0;
                    pthread_mutex_unlock(&ctx.metadata_lock);
                    goto send_response;
                }
                container = container->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
            snprintf(response.message, sizeof(response.message), "Container not found");
            response.status = 1;
            break;
        }

        default:
            snprintf(response.message, sizeof(response.message), "Unknown command");
            response.status = 1;
        }

    send_response:
        /* Send response to client */
        send(client_sock, &response, sizeof(response), 0);
        close(client_sock);
    }

cleanup:
    printf("[supervisor] Shutting down...\n");

    /* Signal logger thread to exit */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    /* Close file descriptors */
    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);
    if (ctx.server_fd >= 0)
        close(ctx.server_fd);
    unlink(CONTROL_PATH);

    /* Free container list */
    pthread_mutex_lock(&ctx.metadata_lock);
    container = ctx.containers;
    while (container != NULL) {
        container_record_t *next = container->next;
        free(container);
        container = next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Destroy synchronization primitives */
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    return 0;
}

/*
 * ========================================================================
 * TODO 5b: send_control_request() - Client-side IPC
 * ========================================================================
 * 
 * This is called by CLI commands (start, stop, ps, etc.) to send requests
 * to the supervisor via the UNIX domain socket.
 *
 * Flow:
 *   1. Create a UNIX domain socket
 *   2. Connect to supervisor at /tmp/mini_runtime.sock
 *   3. Send the control request
 *   4. Receive response
 *   5. Print result or error
 */
static int send_control_request(const control_request_t *req)
{
    int sock;
    struct sockaddr_un addr;
    control_response_t response;
    ssize_t nread;

    /* Create UNIX domain socket */
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    /* Connect to supervisor */
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect to supervisor");
        fprintf(stderr, "Is the supervisor running? (supervisor <rootfs>)\n");
        close(sock);
        return 1;
    }

    /* Send request to supervisor */
    if (send(sock, req, sizeof(*req), 0) < 0) {
        perror("send");
        close(sock);
        return 1;
    }

    /* Receive response from supervisor */
    nread = recv(sock, &response, sizeof(response), 0);
    if (nread < 0) {
        perror("recv");
        close(sock);
        return 1;
    }

    /* Print response */
    if (response.status == 0) {
        printf("%s\n", response.message);
        close(sock);
        return 0;
    } else {
        fprintf(stderr, "Error: %s\n", response.message);
        close(sock);
        return 1;
    }
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    printf("Expected states include: %s, %s, %s, %s, %s\n",
           state_to_string(CONTAINER_STARTING),
           state_to_string(CONTAINER_RUNNING),
           state_to_string(CONTAINER_STOPPED),
           state_to_string(CONTAINER_KILLED),
           state_to_string(CONTAINER_EXITED));
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
