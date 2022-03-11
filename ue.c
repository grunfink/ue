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
#include <stdint.h>

#define DATA_SIZE 32000

struct {
    char *fname;                /* file name */
    int width;                  /* terminal width */
    int height;                 /* terminal height */
    int sigwinch_received;      /* sigwinch-received flag */
    int new_file;               /* file-is-new flag */
    int modified;               /* modified-since-saving flag */
    int cpos;                   /* cursor position */
    int size;                   /* size of document */
    uint8_t data[DATA_SIZE];    /* the document data */
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


void output(void)
/* paint the document to the screen */
{
    gotoxy(0, 0);

    /* new file? say it */
    if (ue.new_file) {
        printf("<new file>");
        ue.new_file = 0;
    }
    else {
        printf("cpos: %d size: %d", ue.cpos, ue.size);
        clreol();
    }

    fflush(stdout);
}


#define ctrl(k) ((k) & 31)

int input(void)
/* processes keys */
{
    char *key;
    int running = 1;

    key = read_string();

    switch (key[0]) {
    case ctrl('q'):
        /* quit and save the unmodified document to .ue.saved */
//        if (ue.modified)
//            save_file(".ue.saved");

        /* fall to ctrl-z */

    case ctrl('z'):
        /* force quit */
        running = 0;
        break;

    default:
        break;
    }

    return running;
}


int utf8_to_cpoint(uint32_t *cpoint, int *s, char c)
/* reads an utf-8 stream and decodes the codepoint */
{
    if (!*s && (c & 0x80) == 0) { /* 1 byte char */
        *cpoint = c;
    }
    else
    if (!*s && (c & 0xe0) == 0xc0) { /* 2 byte char */
        *cpoint = (c & 0x1f) << 6; *s = 1;
    }
    else
    if (!*s && (c & 0xf0) == 0xe0) { /* 3 byte char */
        *cpoint = (c & 0x0f) << 12; *s = 2;
    }
    else
    if (!*s && (c & 0xf8) == 0xf0) { /* 4 byte char */
        *cpoint = (c & 0x07) << 18; *s = 3;
    }
    else
    if (*s && (c & 0xc0) == 0x80) { /* continuation byte */
        switch (*s) {
        case 3: *cpoint |= (c & 0x3f) << 12; break;
        case 2: *cpoint |= (c & 0x3f) << 6;  break;
        case 1: *cpoint |= (c & 0x3f);       break;
        }

        (*s)--;
    }
    else {
        *cpoint = L'\xfffd';
        *s = 0;
    }

    return *s;
}


int load_file(char *fname)
/* loads the file */
{
    FILE *f;
    int ret = 0;

    /* store file name */
    ue.fname = fname;

    if ((f = fopen(fname, "rb")) != NULL) {
        int c;
        uint32_t cpoint;
        int ustate = 0;

        while (ue.size < DATA_SIZE && (c = fgetc(f)) != EOF) {
            /* keep decoding utf8 until a full codepoint is found */
            if (utf8_to_cpoint(&cpoint, &ustate, c) == 0) {
                /* treat special cases */
                if (cpoint == 0x2014)
                    cpoint = 0xac;      /* the m-dash is the not sign */
                else
                if (cpoint > 0xff)
                    cpoint = 0xa4;      /* all above iso8859-1 is an error */

                ue.data[ue.size++] = cpoint & 0xff;
            }
        }

        fclose(f);
    }
    else
        /* file is not (yet) on disk */
        ue.new_file = 1;

    if (ue.size == DATA_SIZE) {
        printf("ERROR: file too big\n");
        ret = 1;
    }

    return ret;
}


int ue_main(char *fname)
/* edits the file */
{
    struct termios tios;

    if (load_file(fname))
        goto end;

    /* startup */
    raw_tty(1, &tios);
    sigwinch_handler(0);
    startup();

    /* main loop */
    for (;;) {
        /* read terminal size, if new or changed */
        if (ue.sigwinch_received)
            get_tty_size();

        output();

        if (!input())
            break;
    }

    /* shutdown */
    raw_tty(0, &tios);
    shutdown();

end:
    return 0;
}


int main(int argc, char *argv[])
{
    if (argc != 2)
        printf("Usage: %s {file to edit}\n", argv[0]);
    else
        ue_main(argv[1]);

    return 0;
}
