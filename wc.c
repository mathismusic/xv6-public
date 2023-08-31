#include "types.h"
#include "stat.h"
#include "user.h"

char buf[512];

void
wc(int fd, char *name)
{
  int i, n; // indexing variables
  int l, w, c, inword; // l = lines, w = words, c = characters, inword = boolean to describe if we are in a word or not

  l = w = c = 0;
  inword = 0;
  while((n = read(fd, buf, sizeof(buf))) > 0){
    for(i=0; i<n; i++){
      c++; // num_chars++ (white space included)
      if(buf[i] == '\n')
        l++; // num_lines++
      if(strchr(" \r\t\n\v", buf[i]))
        inword = 0; // if we encounter a whitespace character, we are no longer in a word
      else if(!inword){
        w++; // num_words++
        inword = 1; // we are in a word till further notice
      }
    }
  }
  if(n < 0){
    printf(1, "wc: read error\n");
    exit();
  }
  // this is fprintf actually, 1 is stdout. name is name of the file that we are reading from
  printf(1, "%d %d %d %s\n", l, w, c, name);
}

int
main(int argc, char *argv[])
{
  int fd, i;

  if(argc <= 1){
    wc(0, "");
    exit();
  }

  for(i = 1; i < argc; i++){
    if((fd = open(argv[i], 0)) < 0){
      printf(1, "wc: cannot open %s\n", argv[i]);
      exit();
    }
    wc(fd, argv[i]);
    close(fd);
  }
  exit();
}
