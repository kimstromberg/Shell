struct c
{
  char** pgmlist;
  struct c* next;
};

typedef struct c Pgm;

struct node
{
  Pgm* pgm;
  char* rstdin;
  char* rstdout;
  char* rstderr;
  int bakground;
};

typedef struct node Command;

extern int parse(char*, Command *);
