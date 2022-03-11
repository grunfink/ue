/* µe - ANSI tty micro text editor by grunfink - public domain */

#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include <sys/time.h>

struct {
    int width;              /* terminal width */
    int height;             /* terminal height */
    int sigwinch_received;  /* sigwinch received flag */
} ue;


static void raw_tty(int start, struct termios *so)
/* sets/unsets stdin in raw mode */
{
    if (start) {
        struct termios o;

        /* save previous fd state */
        tcgetattr(0, so);

        /* set raw */
        o = *so;
        cfmakeraw(&o);
        tcsetattr(0, TCSANOW, &o);
    }
    else
        /* restore previously saved tty state */
        tcsetattr(0, TCSANOW, so);
}


static int something_waiting(int msecs)
/* returns yes if there is something waiting on fd */
{
    fd_set ids;
    struct timeval tv;

    /* reset */
    FD_ZERO(&ids);

    /* add fd to set */
    FD_SET(0, &ids);

    tv.tv_sec  = 0;
    tv.tv_usec = msecs * 1000;

    return select(1, &ids, NULL, NULL, &tv) > 0;
}


char *read_string(void)
/* reads an ansi string, waiting in the first char */
{
    static char buf[256];
    int n = 0;

    /* first char blocks, the (possible) next ones don't */
    do {
        char c;

        read(0, &c, sizeof(c));

        if (n < sizeof(buf))
            buf[n++] = c;
    } while (something_waiting(10));

    buf[n] = '\0';

    return n ? buf : NULL;
}


static void get_tty_size(void)
/* asks the tty for its size */
{
    char *buffer;

    /* magic line: save cursor position, move to stupid position,
       ask for current position and restore cursor position */
    printf("\0337\033[r\033[999;999H\033[6n\0338");
    fflush(stdout);

    if (something_waiting(50)) {
        buffer = read_string();

        sscanf(buffer, "\033[%d;%dR", &ue.height, &ue.width);
    }
    else {
        /* terminal didn't report; let's hope it's the default */
        ue.width  = 80;
        ue.height = 25;
    }

    ue.sigwinch_received = 0;
}


static void sigwinch_handler(int s)
/* SIGWINCH signal handler */
{
    struct sigaction sa;

    ue.sigwinch_received = 1;

    /* (re)attach signal */
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = sigwinch_handler;
    sigaction(SIGWINCH, &sa, NULL);
}


#define gotoxy(x, y) printf("\033[%d;%dH", y + 1, x + 1)
#define startup()    printf("\033[?1049h")                // enter alt screen
#define shutdown()   printf("\033[0;39;49m\033[?1049l\n") // def attr and exit alt scr
#define clreol()     printf("\033[K")
#define clrscr()     printf("\033[2J")


void paint(void)
/* paint the document to the screen */
{
    gotoxy(0, 0);
    printf("%d x %d", ue.width, ue.height);
    fflush(stdout);
}


#define ctrl(k) ((k) & 31)

void ue_main(char *fname)
/* edits the file */
{
    struct termios tios;

    /* startup */
    raw_tty(1, &tios);
    sigwinch_handler(0);
    startup();

    /* main loop */
    for (;;) {
        char *key;

        /* read terminal size, if new or changed */
        if (ue.sigwinch_received)
            get_tty_size();

        paint();

        key = read_string();

        if (key[0] == ctrl('q'))
            break;
    }

    /* shutdown */
    raw_tty(0, &tios);
    shutdown();
}


int main(int argc, char *argv[])
{
    if (argc != 2)
        printf("Usage: %s {file to edit}\n", argv[0]);
    else
        ue_main(argv[1]);

    return 0;
}
