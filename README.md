# Mini-Supervisor

A production-style **process supervisor** written in C++17, built from scratch using Linux system calls.

It reads a list of programs to run, launches them, monitors their health, automatically restarts crashed processes, captures their output to log files, and provides a live status monitor — all using the same low-level Linux APIs that tools like `systemd`, `pm2`, and Docker use internally.

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                        SUPERVISOR PROCESS                        │
│                                                                  │
│  ┌─────────────┐    ┌──────────────────┐    ┌────────────────┐  │
│  │ tasks.conf  │───▶│  parse_command() │───▶│ launch_task()  │  │
│  │  sleep 6    │    │  splits command  │    │  pipe()        │  │
│  │  false      │    │  into args[]     │    │  fork()        │  │
│  │  ls -la     │    └──────────────────┘    │  execvp()      │  │
│  └─────────────┘                            └───────┬────────┘  │
│                                                     │           │
│              ┌──────────────────────────────────────┘           │
│              │  fork() creates child processes                   │
│              ▼                                                   │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │                   CHILD PROCESSES                        │    │
│  │                                                          │    │
│  │  [sleep 6]    [false]    [ls -la]                        │    │
│  │     │            │          │                            │    │
│  │  stdout      stdout      stdout                          │    │
│  │     │            │          │                            │    │
│  │     ▼            ▼          ▼   (via dup2 → pipe)        │    │
│  └─────────────────────────────────────────────────────────┘    │
│              │            │          │                           │
│              ▼            ▼          ▼  (parent reads fd[0])     │
│         sleep.log     false.log    ls.log    (log/ folder)       │
│                                                                  │
│  ┌────────────────────────────────────────────────────────┐     │
│  │              on_child_exit() — SIGCHLD handler          │     │
│  │                                                         │     │
│  │  Child dies → kernel sends SIGCHLD to supervisor        │     │
│  │  waitpid(-1, WNOHANG) → find which PID died             │     │
│  │  Read pipe → write output to log file                   │     │
│  │  Check exit code:                                        │     │
│  │    code == 0 → "completed normally", done               │     │
│  │    code != 0 → check restart_count vs max_restarts      │     │
│  │      count < max → restart_count++, launch_task() again │     │
│  │      count >= max → "giving up", remove from task_map   │     │
│  └────────────────────────────────────────────────────────┘     │
│                                                                  │
│  ┌────────────────────────────────────────────────────────┐     │
│  │              Main monitoring loop                       │     │
│  │                                                         │     │
│  │  while task_map not empty:                              │     │
│  │    sleep(2)                                             │     │
│  │    for each pid in task_map:                            │     │
│  │      read /proc/<pid>/stat   → process state            │     │
│  │      read /proc/<pid>/status → VmRSS memory usage       │     │
│  │      print status table                                  │     │
│  └────────────────────────────────────────────────────────┘     │
│                                                                  │
│  ┌────────────────────────────────────────────────────────┐     │
│  │              on_shutdown() — SIGINT/SIGTERM handler     │     │
│  │                                                         │     │
│  │  Ctrl+C or kill <pid>:                                  │     │
│  │    1. Send SIGTERM to all children → polite stop        │     │
│  │    2. Wait 2 seconds                                     │     │
│  │    3. Send SIGKILL to any still alive → force kill      │     │
│  │    4. exit(0)                                            │     │
│  └────────────────────────────────────────────────────────┘     │
└──────────────────────────────────────────────────────────────────┘
```

---

## System Calls Used

| System Call | Where used | Why |
|---|---|---|
| `fork()` | `launch_task()` | Creates a new child process |
| `execvp()` | `launch_task()` | Replaces child with target program |
| `pipe()` | `launch_task()` | Creates log capture channel |
| `dup2()` | `launch_task()` (child) | Redirects child stdout/stderr into pipe |
| `waitpid()` | `on_child_exit()` | Reaps dead children, prevents zombies |
| `kill()` | `on_shutdown()` | Sends signals to child processes |
| `signal()` | `main()` | Registers SIGCHLD, SIGINT, SIGTERM handlers |
| `read()` | `on_child_exit()` | Drains pipe buffer into log file |
| `fopen/fscanf` | `get_process_state()` | Reads `/proc/<pid>/stat` for state |
| `fgets/sscanf` | `get_memory_kb()` | Reads `/proc/<pid>/status` for VmRSS |

---

## Project Structure

```
mini-supervisor/
├── main.cpp        — all source code (~230 lines)
├── tasks.conf      — list of programs to supervise (one per line)
├── log/            — captured output of each child process
│   ├── ls.log
│   ├── sleep.log
│   └── false.log
└── README.md       — this file
```

---

## How to Build and Run

```bash
# Build
g++ -std=c++17 main.cpp -o supervisor

# Configure tasks (edit tasks.conf)
echo "sleep 10" > tasks.conf
echo "false"   >> tasks.conf
echo "ls -la"  >> tasks.conf

# Run
./supervisor

# Stop gracefully (in another terminal)
kill $(pgrep supervisor)   # sends SIGTERM → graceful shutdown
```

---

## tasks.conf Format

One command per line. Arguments are supported.

```
sleep 10
ls -la
false
python3 my_server.py
./my_worker
```

---

## Key Concepts Demonstrated

### 1. fork() + execvp() — Process Creation
```
Supervisor forks → child gets execvp'd into target program
Parent tracks child PID in task_map
```

### 2. pipe() + dup2() — Log Capture
```
pipe(fds) creates read end (fds[0]) and write end (fds[1])
Child: dup2(fds[1], STDOUT_FILENO) → stdout goes into pipe
Parent: reads from fds[0] → writes to log file
```

### 3. SIGCHLD + waitpid() — Crash Detection
```
Child dies → kernel sends SIGCHLD to supervisor
Handler: waitpid(-1, WNOHANG) → finds which PID died
Decision: exit code 0 = done, else = restart or give up
```

### 4. /proc Virtual Filesystem — Live Monitoring
```
/proc/<pid>/stat   → 3rd field is process state (R/S/T/Z)
/proc/<pid>/status → VmRSS field is physical memory in KB
```

### 5. SIGTERM + SIGKILL — Graceful Shutdown
```
SIGTERM → polite request to stop (program can clean up)
SIGKILL → unconditional termination (cannot be caught)
Pattern: SIGTERM first, wait, then SIGKILL if still alive
```


