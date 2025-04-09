#include <cstring>       // strdup()
#include <fstream>       // ifstream (file reading)
#include <iostream>      // cout, cerr
#include <sstream>       // stringstream (string splitting)
#include <string>        // string
#include <sys/wait.h>    // waitpid(), WIFEXITED(), WEXITSTATUS()
#include <unistd.h>      // fork(), execvp(), sleep()
#include <unordered_map> // unordered_map
#include <vector>        // vector

using namespace std;

// ─────────────────────────────────────────────
// DATA STRUCTURES
// ─────────────────────────────────────────────

struct Task {
  vector<char *> args;  // command + arguments, e.g. ["sleep", "10", NULL]
  int restart_count;    // how many times this task has been restarted
  int max_restarts = 3; // give up after this many restarts
  int log_fd;
};

// Global map: PID → Task
// When SIGCHLD fires and tells us "PID 1234 died",
// we look up task_map[1234] to find what command it was running.
unordered_map<pid_t, Task> task_map;
bool shutting_down = false;


// ─────────────────────────────────────────────
// HELPERS
// ─────────────────────────────────────────────

// Splits a string like "sleep 10" into ["sleep", "10", NULL]
// execvp() needs a raw C-style char** array, not a C++ string.
// strdup() makes a heap copy of each word so the pointer stays valid.
vector<char *> parse_command(const string &line) {
  vector<char *> args;
  stringstream ss(line);
  string word;
  while (ss >> word) {
    args.push_back(
        strdup(word.c_str())); // c_str() → char*, strdup() → heap copy
  }
  args.push_back(NULL); // execvp() requires NULL as the last element
  return args;
}

// Forks a new child and runs the given task.
// Returns the new child's PID, or -1 on error.
pid_t launch_task(Task &task) {
  int fds[2];
  pipe(fds);
  pid_t pid = fork();
  if (pid == -1) {
    perror("[Supervisor] fork failed");
    return -1;
  }
  if (pid == 0) {
    // CHILD: replace this process image with the actual program
    close(fds[0]);               // child doesn't read
    dup2(fds[1], STDOUT_FILENO); // stdout → pipe
    dup2(fds[1], STDERR_FILENO); // stderr → pipe too
    close(fds[1]);               // original fd no longer needed
    execvp(task.args[0], task.args.data());
    perror("[Supervisor] execvp failed"); // only reached if execvp fails
    exit(1);
  }
  // PARENT: return the child's PID
  close(fds[1]);
  task.log_fd = fds[0];
  return pid;
}

// ─────────────────────────────────────────────
// SIGNAL HANDLER — single place for all child deaths
// ─────────────────────────────────────────────

// Called automatically by the kernel when ANY child process dies.
// This is the ONLY place where child deaths are handled.
// The kernel passes the signal number (SIGCHLD = 17) in 'sig'.
void on_child_exit(int sig) {
  int status;
  pid_t pid;

  // Loop: multiple children can die simultaneously, but the kernel may
  // deliver only ONE SIGCHLD. So drain all dead children in one handler call.
  // waitpid(-1)  → check ANY child (not a specific PID)
  // WNOHANG      → don't block; return 0 immediately if no dead child ready
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {

    // Look up which command this PID was running
    Task task = task_map[pid];
    task_map.erase(pid); // remove dead PID; whoever claimed it owns the erase
                         // ── Read child's output and save to log file ──
    string log_path = "log/" + string(task.args[0]) + ".log";
    ofstream logfile(log_path, ios::app); // ios::app = append (don't overwrite)

    char buf[4096];
    ssize_t bytes_read;
    while ((bytes_read = read(task.log_fd, buf, sizeof(buf))) > 0) {
      logfile.write(buf, bytes_read); // write exactly bytes_read bytes
    }
    close(task.log_fd); // done with the read end of this pipe

    // ── CASE 1: Normal completion (exit code 0) → task is done, no restart
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
      cout << "[Supervisor] PID " << pid << " ('" << task.args[0]
           << "') completed successfully." << endl;
      continue; // don't restart
    }

    // ── CASE 2: Crashed or killed → decide whether to restart
    if (WIFEXITED(status)) {
      cout << "[Supervisor] PID " << pid << " ('" << task.args[0]
           << "') exited with error code " << WEXITSTATUS(status) << endl;
    } else {
      cout << "[Supervisor] PID " << pid << " ('" << task.args[0]
           << "') was killed by a signal." << endl;
    }
    if (shutting_down) return;
    if (task.restart_count >= task.max_restarts) {
      cout << "[Supervisor] '" << task.args[0] << "' failed "
           << task.max_restarts << " times. Giving up." << endl;
      continue; // don't restart; task_map already erased → task is gone
    }

    // Restart the task with an incremented restart count
    task.restart_count++;
    cout << "[Supervisor] Restarting '" << task.args[0] << "' (attempt "
         << task.restart_count << "/" << task.max_restarts << ")..." << endl;

    pid_t pid_new = launch_task(task);
    if (pid_new > 0) {
      task_map[pid_new] = task; // track new child with updated restart count
    }
  }
}

string get_process_state(pid_t pid) {
  char path[64];
  sprintf(path, "/proc/%d/stat", pid); // build path string

  FILE *f = fopen(path, "r"); // open it (C-style, same as proclore.c)
  if (!f)
    return "Unknown";

  int pid_r;
  char comm[256];
  char state;
  fscanf(f, "%d %s %c", &pid_r, comm, &state); // read first 3 fields
  fclose(f);

  if (state == 'R')
    return "Running";
  if (state == 'S')
    return "Sleeping"; // waiting (most common)
  if (state == 'T')
    return "Stopped"; // Ctrl+Z
  if (state == 'Z')
    return "Zombie";
  return "Unknown";
}

long get_memory_kb(pid_t pid) {
  char path[64];
  sprintf(path, "/proc/%d/status", pid);

  FILE *f = fopen(path, "r");
  if (!f)
    return -1;

  char line[256];
  while (fgets(line, sizeof(line), f)) {
    long kb;
    if (sscanf(line, "VmRSS: %ld kB", &kb) == 1) {
      fclose(f);
      return kb;
    }
  }
  fclose(f);
  return -1; // not found (process may have just died)
}

void on_shutdown(int sig) {
  shutting_down=true;
  cout << "\n[Supervisor] Shutting down gracefully..." << endl;
  for (auto &[pid, task] : task_map) {
    kill(pid, SIGTERM); // politely ask each child to stop
  }
  // wait a moment, then kill any that didn't stop
  sleep(2);
  for (auto &[pid, task] : task_map) {
    kill(pid, SIGKILL); // force kill stragglers
  }
  exit(0);
}

// ─────────────────────────────────────────────
// MAIN
// ─────────────────────────────────────────────

int main() {
  // Register BEFORE launching any task.
  // A child could die instantly; if handler isn't registered yet, we'd miss it.
  signal(SIGCHLD, on_child_exit);
  signal(SIGINT, on_shutdown);
  signal(SIGTERM, on_shutdown);
  cout << "[Supervisor] Starting up..." << endl;

  // Open the config file listing programs to run
  ifstream file("tasks.conf");
  if (!file.is_open()) {
    cerr << "[Supervisor] Error: Could not open tasks.conf" << endl;
    return 1;
  }

  // Launch each task listed in tasks.conf
  string line;
  while (getline(file, line)) {
    if (line.empty())
      continue;

    Task task{parse_command(line), 0};
    pid_t pid = launch_task(task);
    if (pid > 0) {
      task_map[pid] = task;
      cout << "[Supervisor] Launched '" << line << "' → PID " << pid << endl;
    }
  }

  // Keep running until all tasks have completed or been given up on.
  // on_child_exit() erases tasks from task_map as they finish.
  // When task_map is empty, every task is done.
  cout << "[Supervisor] All tasks launched. Monitoring..." << endl;
  while (!task_map.empty()) {
    sleep(2); // print status every 2 seconds

    if (task_map.empty())
      break; // tasks may have finished during sleep

    cout << "\n[Supervisor] ══ Status ═══════════════════════════" << endl;
    for (auto &[pid, task] : task_map) {
      string state = get_process_state(pid);
      long mem = get_memory_kb(pid);
      cout << "  PID " << pid << " | " << task.args[0] << " | " << state
           << " | " << (mem > 0 ? to_string(mem) + " KB" : "N/A") << endl;
    }
    cout << "[Supervisor] ══════════════════════════════════════" << endl;
  }

  cout << "[Supervisor] All tasks completed. Shutting down." << endl;
  return 0;
}
