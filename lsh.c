#define _POSIX_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <readline/readline.h>
#include <readline/history.h>

#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

#include "parse.h"

/* We require C99 for VARARGS macros here. */
#ifdef DEBUG
#define DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINT(...) do { } while (0)
#endif

const mode_t DEFAULT_MODE = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

int count_programs(const Pgm*);
const Pgm** reverse_array(const Pgm*, int);
void safe_close(int fd);
void handle_command(const Command*);
void strip_white(char*);

/**
 * When non-zero, this global means the user is done using this program.
 */
int done = 0;

int main(void)
{
  Command cmd;

  signal(SIGINT, SIG_IGN);

  while (!done) {

    char* line;
    line = readline("> ");

    if (!line)
    {
      done = 1;
    }
    else
    {
      strip_white(line);

      if (*line)
      {
        add_history(line);
        int n = parse(line, &cmd);
        if (n == 1)
          handle_command(&cmd);
        else
          fprintf(stderr, "error: could not parse %s\n", line);
      }
    }
    free(line);

    /* This is to make sure no zombies escape */
    int pid = 1;
    while (pid > 0)
    {
      int status;
      pid = waitpid(-1, &status, WNOHANG);
      if (pid > 0)
        DEBUG_PRINT("main: zombie catched with pid=%d exited with %d\n", pid, status);
    }
  }
  return 0;
}

void handle_command(const Command* cmd)
{
  int count              = 0;
  int fdin               = 0;
  int fdout              = 0;
  int previous_read_end  = fdin;
  int previous_write_end = -1;
  const Pgm** pgms       = NULL;

  /* the parser does not support more than 20 pipe commands anyway */
  pid_t pids[20];

  assert(cmd != NULL);
  assert(cmd->pgm != NULL);

  DEBUG_PRINT("handle_command(%p)\n", (void*) cmd);

  fdin  = STDIN_FILENO;
  fdout = STDOUT_FILENO;

  if (cmd->rstdin != NULL)
    fdin = open(cmd->rstdin, O_RDONLY);

  if (cmd->rstdout != NULL)
    fdout = open(cmd->rstdout, O_WRONLY | O_CREAT, DEFAULT_MODE);

  if (fdin == -1)
  {
    fprintf(stderr, "error: could not open %s for reading\n", cmd->rstdin);
    goto cleanup;
  }

  if (fdout == -1)
  {
    fprintf(stderr, "error: could not open %s for writing\n", cmd->rstdout);
    goto cleanup;
  }

  DEBUG_PRINT("handle_command: stdin=%d, stdout=%d\n", fdin, fdout);

  count = count_programs(cmd->pgm);

  DEBUG_PRINT("handle_command: count=%d\n", count);

  pgms = reverse_array(cmd->pgm, count);

  previous_read_end  = fdin;
  previous_write_end = -1;
  for (int i = 0; i < count; ++i)
  {
    const Pgm* current = pgms[i];

    DEBUG_PRINT("handle_command: i=%d, current=%p\n", i, (void*) current);

    if (strcmp(current->pgmlist[0],"exit") == 0)
      done = 1;
    else if (strcmp(current->pgmlist[0], "cd") == 0)
    {
      int err = chdir(current->pgmlist[1]);
      if (err == -1) {
        fprintf(stderr, "error: could not cd to %s\n", current->pgmlist[1]);
      }
    }
    else
    {
      int pipefds[2];
      int next_read_end;
      int next_write_end;

      if (i == count-1)
      {
        next_read_end  = -1;
        next_write_end = fdout;
      }
      else
      {
        int err = pipe(pipefds);
        if (err == -1)
        {
          fprintf(stderr, "error: pipe failure, watch out for floods.\n");
          goto cleanup;
        }

        next_read_end  = pipefds[0];
        next_write_end = pipefds[1];
      }

      DEBUG_PRINT("handle_command: i=%d, previous_read_end=%d, previous_write_end=%d\n", i, previous_read_end, previous_write_end);
      DEBUG_PRINT("handle_command: i=%d, next_read_end=%d, next_write_end=%d\n", i, next_read_end, next_write_end);
      DEBUG_PRINT("handle_command: i=%d, path=%s\n", i, current->pgmlist[0]);

      int pid = fork();

      if (pid == 0)
      {
        /* This makes sure ctrl-c does not kill background processes */
        if (cmd->bakground)
          setpgid(0, 0);
        else
          signal(SIGINT, SIG_DFL);

        /* Always close previous write end as it is not used by this process */
        safe_close(previous_write_end);

        /* Connect previous write end with our stdin */
        dup2(previous_read_end, STDIN_FILENO);
        safe_close(previous_read_end);

        /* Close next read end because it used by next process and not us */
        safe_close(next_read_end);

        /* Connect next write end with our stdout */
        dup2(next_write_end, STDOUT_FILENO);
        safe_close(next_write_end);

        execvp(current->pgmlist[0], current->pgmlist);
        fprintf(stderr, "error: could not execute %s\n", current->pgmlist[0]);
        exit(1);
      }
      else if (pid > 0)
      {
        /* Close both of the previous as they are used by the child and not the
         * parent.
         */
        DEBUG_PRINT("handle_command: i=%d, pid=%d\n", i, pid);

        safe_close(previous_read_end);
        safe_close(previous_write_end);

        previous_read_end  = next_read_end;
        previous_write_end = next_write_end;

        pids[i] = pid;
      }
      else
      {
        fprintf(stderr, "error: fork failed with %d\n", pid);
        goto cleanup;
      }
    }

    current = current->next;
  }

  /* Only wait for children if they are foreground processes */
  if (!cmd->bakground)
  {
    for (int i = 0; i < count; ++i)
    {
      pid_t pid = pids[i];
      int status;
      DEBUG_PRINT("handle_command: waiting for child index=%d, pid=%d\n", i, pid);
      waitpid(pid, &status, 0);
      DEBUG_PRINT("handle_command: child index=%d, pid=%d exited with %d\n", i, pid, status);
    }
  }

cleanup:
  free(pgms);

  safe_close(fdin);
  safe_close(fdout);
  safe_close(previous_read_end);
  safe_close(previous_write_end);
}

/**
 * Count the number of programs in a pipe.
 */
int count_programs(const Pgm* pgm)
{
  int count = 0;
  while (pgm)
  {
    ++count;
    pgm = pgm->next;
  }

  return count;
}

/**
 * Reverse the pgm structure and store pointers to each elements into a
 * reversed array.
 */
const Pgm** reverse_array(const Pgm* pgm, int count)
{
  const Pgm** pgms = calloc(count, sizeof(Pgm*));
  for (int i = count-1; i >= 0; --i)
  {
    pgms[i] = pgm;
    pgm = pgm->next;
  }

  return pgms;
}

/**
 * Safe wrapper to close call.
 *
 * Does not close negative file descriptors and does not close standard in or
 * standard out. This makes sure we never accidentally close standard out or
 *standard in.
 */
void safe_close(int fd)
{
  if (fd > 1)
    close(fd);
}

/**
 * Strip whitespace from the start and end of string.
 */
void strip_white(char* string)
{
  int i = 0;

  while (whitespace(string[i])) {
    i++;
  }

  if (i) {
    strcpy(string, string + i);
  }

  i = strlen(string) - 1;
  while (i > 0 && whitespace(string[i])) {
    i--;
  }

  string [++i] = '\0';
}
