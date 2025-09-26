#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]){

	int numf = atoi(argv[1]);
	int nums = atoi(argv[3]);

	if     (strcmp(argv[2], "+")==0)
		printf("%d\n", numf+nums);

	else if(strcmp(argv[2],"-")==0)
		printf("%d\n", numf-nums);

	else if(strcmp(argv[2],"x")==0)
		printf("%d\n", numf*nums);

	else if(strcmp(argv[2],"/")==0)
		printf("%d\n", numf/nums);

	return 0;
}
	

