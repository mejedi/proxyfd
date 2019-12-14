/* A program interleaving stdout and stderr output.
 * The output ordering when invoked directly and under ./prun should match. */
#include <stdio.h>
int main()
{
	fputs("1\n", stdout);
	fputs("2\n", stdout);
	fputs("3\n", stdout);
	fputs("4",   stderr);
	fputs("5\n", stdout);
	fputs("\n",   stderr);
	return 0;
}
