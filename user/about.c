// Jayden Sorter
// This program is placed into the Makefile so when I run make qemu it will be able to have the word after "about" be appended to the 
// argument if statement

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main (int argc, char *argv[]){

printf("COM S 3520 Project 1A \n");
printf("Student: Jayden Sorter \n");
if (argc > 1)
printf("Argument: %s \n", argv[1]);
exit (0);

}