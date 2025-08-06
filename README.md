# IST-KVS: Concurrent Key-Value Store with Client Subscription over Named Pipes

A multithreaded client-server Key-Value Store system implemented in C, enabling real-time key updates via named pipes and session-based client subscriptions. Built as part of an academic project with a focus on **concurrent systems**, **IPC (Inter-Process Communication)**, **thread synchronization**, and **signal handling**.

---

## 🚀 Features

- ✅ **Named Pipe-based Client-Server Architecture**
- 🔄 **Real-Time Key Change Notifications** via client-specific notification pipes
- 🧵 **Multithreaded Server** with thread pools for:
  - Handling `.job` file execution
  - Managing concurrent client sessions (subscription/commands)
- 🗂️ **Concurrent Key-Value Store (KVS)** with read/write locks
- 🧠 **Session-Based Subscriptions** (subscribe/unsubscribe keys)
- 🧼 **Signal Handling with SIGUSR1** to forcefully disconnect all clients
- 🧪 Includes testable architecture and interactive client interface

---

## 📁 Project Structure

```
.
├── src/
│   ├── client/         # Client API and interaction logic
│   ├── server/         # Server logic and job/thread management
│   ├── common/         # Shared protocol, constants, and IO utils
├── main.c              # Entry point for server and client
├── Makefile            # Build system
├── enunciado.md        # Project specification (academic)
```

---

## 🧠 How It Works

### Architecture Overview

- The **server** creates a named pipe (`register_fifo`) to accept session requests.
- Clients use the API (`kvs_connect`) to send connection metadata including three pipes:
  - **Request pipe** (commands)
  - **Response pipe** (operation results)
  - **Notification pipe** (key-value change notifications)
- Server uses:
  - **Host Thread** to handle registration and SIGUSR1 signals
  - **Session Manager Threads** to handle up to `MAX_SESSION_COUNT` client sessions
  - **Job Dispatcher Threads** to process `.job` files in parallel
- Clients interact via two threads:
  - Command sender (from `stdin`)
  - Notification listener (from notification pipe)

---

## 🧪 Example Session

```bash
# Launch server
./kvs jobs_dir 4 2 /tmp/register_fifo

# Launch client (with test commands from file)
./client client_id /tmp/register_fifo < test_client.txt
```

Sample `test_client.txt`:
```
SUBSCRIBE [key1]
DELAY 1000
UNSUBSCRIBE [key1]
DISCONNECT
```

Client output on receiving updates:
```
(key1,new_value)
(key2,DELETED)
```

---

## 🔧 Build Instructions

```bash
make all
```

### Executables

- `kvs` – server process
- `client` – client process
- Can be executed with:
  - `./kvs <jobs_dir> <max_threads> <max_backups> <register_pipe>`
  - `./client <client_id> <register_pipe>`

---

## 🧵 Concurrency Details

- **Producer-Consumer Buffer**: For session dispatching, synchronized with semaphores and mutexes
- **Reader-Writer Locks**: For consistent access to the central hash table
- **Signal Blocking with `pthread_sigmask`**: Non-host threads ignore SIGUSR1 safely
- **Thread Isolation**: Client disconnects or crashes do not crash the server

---

## 🧠 Skills Demonstrated

- Advanced C Programming (POSIX)
- Systems Programming (pipes, signals, file descriptors)
- Thread Synchronization (mutexes, semaphores, condition variables)
- Resource Cleanup and Robust Session Management
- Clean separation of concerns (modular structure)
- Debugging concurrent systems and handling edge cases

---

## 📚 Academic Context

This project was developed as part of the Operating Systems course at [Instituto Superior Técnico (IST)](https://tecnico.ulisboa.pt/), focused on low-level concurrency, IPC, and real-time client-server architecture.
