char ask_yes_or_quit(const char *question)
{
    char c;
    int attempts = 0;

    while (1) {
        printf("%s (y/q): ", question);
        fflush(stdout);

        if (scanf(" %c", &c) != 1) {
            attempts++;
        } else if (c == 'y' || c == 'Y') {
            return 'y';
        } else if (c == 'q' || c == 'Q') {
            return 'q';
        } else {
            attempts++;
        }

        if (attempts >= 3) {
            printf("Too many invalid attempts. Quitting.\n");
            return 'q';
        }

        printf("Invalid input. Try again (%d/3).\n", attempts);
    }
}
