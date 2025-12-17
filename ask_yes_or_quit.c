#include <stdio.h>

char ask_yes_or_quit(const char *question)
{
    char c;

    while (1) {
        printf("%s (y/q): ", question);
        fflush(stdout);

        if (scanf(" %c", &c) != 1)
            continue;

        if (c == 'y' || c == 'Y')
            return 'y';
        if (c == 'q' || c == 'Q')
            return 'q';

        printf("Invalid input. Try again.\n");
    }
}
