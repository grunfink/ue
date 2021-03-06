/* µe - ANSI tty micro text editor by grunfink - public domain */

#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define VERSION "1.02"

#ifndef TAB_SIZE
#define TAB_SIZE 4
#endif

#ifndef DATA_SIZE
#define DATA_SIZE 32768
#endif

#ifndef UNDO_LEVELS
#define UNDO_LEVELS 64
#endif

struct snap {
    uint8_t data[DATA_SIZE];    /* the document data */
    int vpos;                   /* visual position (first byte shown) */
    int cpos;                   /* cursor position */
    int size;                   /* size of document */
    int mark_s;                 /* selection mark start */
    int mark_e;                 /* selection mark end */
    int modified;               /* modified-since-saving flag */
};

struct {
    struct snap e;              /* working snap */
    struct snap s[UNDO_LEVELS]; /* previous snapshots of e */
    int history;                /* snap history */
    int undo;                   /* levels available to undo */
    uint8_t clip[DATA_SIZE];    /* clipboard */
    int clip_size;              /* clipboard size */
    char *fname;                /* file name */
    int width;                  /* terminal width */
    int height;                 /* terminal height */
    int sigwinch_received;      /* sigwinch-received flag */
    int new_file;               /* file-is-new flag */
    int refuse_quit;            /* refuse-quit-because-of-file-modified flag */
    int *ac0;                   /* array of column #0 positions */
} ue;


/** ANSI **/

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


static char *read_string(void)
/* reads an ansi string, waiting in the first char */
{
    static char buf[256];
    int n = 0;

    /* first char blocks, the (possible) next ones don't */
    do {
        read(0, &buf[n++], sizeof(char));
    } while (something_waiting(10) && n < sizeof(buf) - 1);

    buf[n] = '\0';

    return buf;
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

    /* redim the array of column #0 addresses */
    ue.ac0 = realloc(ue.ac0, ue.height * 2 * sizeof(int));

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
#define clreol()     printf("\033[m\033[K")               // no reverse attr and clear to eol
#define clrscr()     printf("\033[2J")


/** encoding **/

/* Unicode codepoint to internal representation conversion table,
   which is mostly iso8859-1 with some windows-1252 cherry-picks */
struct _uc2int {
    uint16_t cpoint;
    uint16_t isochar;
} uc2int[] = {
    { 0x2014,  0x97 },      /* M-DASH */
    { 0x20ac,  0x80 },      /* EURO SIGN */
    { 0x0160,  0x8a },      /* S WITH CARON */
    { 0x0161,  0x9a },      /* s WITH CARON */
    { 0x017d,  0x8e },      /* Z WITH CARON */
    { 0x017e,  0x9e },      /* z WITH CARON */
    { 0x0152,  0x8c },      /* OE LIGATURE */
    { 0x0153,  0x9c },      /* oe LIGATURE */
    { 0x0178,  0x9f },      /* Y WITH DIAERESIS */
    { 0x2018,  0x91 },      /* LEFT SINGLE QUOTATION MARK */
    { 0x2019,  0x92 },      /* RIGHT SINGLE QUOTATION MARK */
    { 0x201c,  0x93 },      /* LEFT DOUBLE QUOTATION MARK */
    { 0x201d,  0x94 },      /* RIGHT DOUBLE QUOTATION MARK */
    { 0x2026,  0x85 },      /* ELLIPSIS */
    { 0xfffd,  0x15 },      /* ASCII NAK (0x15) is used for REPLACEMENT CHAR */
};


int utf8_to_internal(uint32_t *cpoint, int *s, char c)
/* reads an utf-8 stream, decodes the codepoint and converts to internal */
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
    if (*s && (c & 0xc0) == 0x80) { /* continuation byte */
        switch (*s) {
        case 3: *cpoint |= (c & 0x3f) << 12; break;
        case 2: *cpoint |= (c & 0x3f) << 6;  break;
        case 1: *cpoint |= (c & 0x3f);       break;
        }

        (*s)--;
    }
    else {
        *cpoint = 0xfffd;        /* Error; replacement character */
        *s = 0;
    }

    /* convert to internal if ready */
    if (*s == 0) {
        int n;

        for (n = 0; n < sizeof(uc2int) / sizeof(struct _uc2int); n++) {
            if (*cpoint == uc2int[n].cpoint) {
                *cpoint = uc2int[n].isochar;
                break;
            }
        }

        /* set anything not converted to an error */
        if (*cpoint > 0xff)
            *cpoint = 0x15;
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

        while (ue.e.size < DATA_SIZE && (c = fgetc(f)) != EOF) {
            /* keep decoding utf8 until a full codepoint is found */
            if (utf8_to_internal(&cpoint, &ustate, c) == 0)
                ue.e.data[ue.e.size++] = cpoint;
        }

        fclose(f);
    }
    else
        /* file is not (yet) on disk */
        ue.new_file = 1;

    if (ue.e.size == DATA_SIZE) {
        printf("ERROR: file too big\n");
        ret = 1;
    }

    return ret;
}


void put_internal_to_file(uint32_t cpoint, FILE *f)
/* puts an internal char into a file, converting to utf-8 */
{
    int n;

    /* convert non-standard characters */
    for (n = 0; n < sizeof(uc2int) / sizeof(struct _uc2int); n++) {
        if (cpoint == uc2int[n].isochar) {
            cpoint = uc2int[n].cpoint;
            break;
        }
    }

    if (cpoint < 0x80)
        fputc((char)cpoint, f);
    else {
        if (cpoint < 0x800)
            fputc((char) (0xc0 | (cpoint >> 6)),   f);
        else
        if (cpoint < 0x10000) {
            fputc((char) (0xe0 | (cpoint >> 12)),         f);
            fputc((char) (0x80 | ((cpoint >> 6) & 0x3f)), f);
        }

        fputc((char) (0x80 | (cpoint & 0x3f)), f);
    }
}


void save_file(char *fname)
/* saves the file */
{
    FILE *f;

    if ((f = fopen(fname, "wb")) != NULL) {
        int n;

        for (n = 0; n < ue.e.size; n++)
            put_internal_to_file(ue.e.data[n], f);

        fclose(f);

        ue.e.modified = 0;
    }
}


/** tools **/

int ue_find_bol(int pos)
/* finds the beginning of the line */
{
    if (pos) {
        /* if it's over an EOL, move backwards */
        if (ue.e.data[pos] == '\n')
            pos--;

        /* find it */
        while (pos && ue.e.data[pos] != '\n')
            pos--;
    }

    return pos;
}


int ue_row_size(int pos)
/* returns the size of the row from pos */
{
    int size = 0;
    int bpos = -1;

    while (pos < ue.e.size && ue.e.data[pos] != '\n' && size < ue.width) {
        /* remember the position of a blank */
        if (ue.e.data[pos] == ' ')
            bpos = size;

        size++;
        pos++;
    }

    /* if full size and a blank was seen, set it */
    if (size == ue.width && bpos != -1)
        size = bpos;

    return size;
}


int ue_find_col_0(int pos)
/* returns the position of column #0 of this row */
{
    int col0;

    /* find the beginning of the real line */
    col0 = ue_find_bol(pos);

    while (col0 < ue.e.size) {
        /* get row size from here */
        int size = ue_row_size(col0) + 1;

        /* between column #0 and end-of-row? done */
        if (col0 <= pos && pos < col0 + size)
            break;

        /* move to next line */
        col0 += size;
    }

    return col0;
}


void ue_fix_vpos(void)
/* fixes the visual position */
{
    if (ue.e.cpos < ue.e.vpos) {
        /* cursor above first character? just get column #0 */
        ue.e.vpos = ue_find_col_0(ue.e.cpos);
    }
    else {
        int n;

        /* fill the first half with current vpos */
        for (n = 0; n < ue.height; n++)
            ue.ac0[n] = ue.e.vpos;

        for (n = 0;; n++) {
            int size;

            /* store */
            ue.ac0[(n + ue.height - 2) % (ue.height * 2)] = ue.e.vpos;

            /* get the row size */
            size = ue_row_size(ue.e.vpos) + 1;

            /* if cpos is in this range, done */
            if (ue.e.vpos <= ue.e.cpos && ue.e.cpos <= ue.e.vpos + size)
                break;

            ue.e.vpos += size;
        }

        /* finally get from the buffer */
        ue.e.vpos = ue.ac0[n % (ue.height * 2)];
    }
}


/** painting **/

void ue_output(void)
/* paint the document to the screen */
{
    ue_fix_vpos();

    gotoxy(0, 0);

    if (ue.new_file) {
        /* new file? say it */
        printf("<new file>");
        ue.new_file = 0;
    }
    else
    if (ue.refuse_quit) {
        /* refuse quit? say it */
        if (ue.refuse_quit == 1) {
            printf("ctrl-q again to force quit");
            clreol();
            ue.refuse_quit = 2;
        }
        else
        /* already notified but still here? user didn't quit */
        if (ue.refuse_quit == 2)
            ue.refuse_quit = 0;
    }
    else {
        int n, p;
        int cx, cy;

        cx = cy = -1;

        for (n = 0, p = ue.e.vpos; n < ue.height; n++) {
            int m, size, rev = 0;

            gotoxy(0, n);

            if (p <= ue.e.size) {
                /* get size of row */
                size = ue_row_size(p);

                for (m = 0; m <= size; m++) {
                    /* cursor position? store coords */
                    if (p == ue.e.cpos) {
                        cx = m;
                        cy = n;
                    }

                    /* inside selection block? */
                    if (ue.e.mark_e != -1) {
                        int r = ue.e.mark_s <= p && p < ue.e.mark_e;

                        if (r != rev) {
                            printf(r ? "\033[7m" : "\033[m");
                            rev = r;
                        }
                    }

                    /* put char */
                    int c = ue.e.data[p++];
                    put_internal_to_file(c == '\n' ? ' ' : c, stdout);
                }
            }

            clreol();
        }

        if (cx != -1)
            gotoxy(cx, cy);
    }

    fflush(stdout);
}


/** editing **/

void ue_snap(void)
/* takes a new snapshot (for undoing) */
{
    /* copy */
    ue.s[ue.history % UNDO_LEVELS] = ue.e;

    /* move forward */
    ue.history++;

    /* one more level 'undoable' */
    if (ue.undo < UNDO_LEVELS)
        ue.undo++;
}


void ue_undo(void)
/* recovers the edition state from a previous snapshot */
{
    if (ue.undo > 0) {
        /* one less level available */
        ue.undo--;

        /* move back in history */
        ue.history--;

        /* copy */
        ue.e = ue.s[ue.history % UNDO_LEVELS];
    }
}


void ue_delete(int count)
/* deletes count bytes from the cursor position */
{
    /* if there is a selection block, delete it */
    if (ue.e.mark_e != -1) {
        ue.e.cpos = ue.e.mark_s;
        count = ue.e.mark_e - ue.e.mark_s;

        ue.e.mark_s = ue.e.mark_e = -1;
    }

    if (ue.e.cpos < ue.e.size) {
        int n;

        /* move memory 'down' */
        for (n = 0; n < ue.e.size - ue.e.cpos; n++)
            ue.e.data[ue.e.cpos + n] = ue.e.data[ue.e.cpos + n + count];

        /* decrease size */
        ue.e.size -= count;

        ue.e.modified++;
    }
}


int ue_expand(int size)
/* opens room in the cursor position */
{
    int ret = 0;

    /* if there is a block, delete it */
    if (ue.e.mark_e != -1)
        ue_delete(0);

    if (ue.e.size + size < DATA_SIZE) {
        int n;

        /* move memory 'up' */
        for (n = ue.e.size - ue.e.cpos; n > 0; n--)
            ue.e.data[ue.e.cpos + n] = ue.e.data[ue.e.cpos + n - size];

        /* increase size */
        ue.e.size += size;

        ue.e.modified++;
        ret++;
    }

    return ret;
}


void ue_insert(char c)
/* inserts a byte into the cursor position */
{
    if (ue_expand(1))
        ue.e.data[ue.e.cpos++] = c;
}


/** input **/

#define ctrl(k) ((k) & 31)

int ue_input(char *key)
/* processes keys */
{
    int n = 0;          /* general-purpose variable (must be 0) */
    int running = 1;

    /* ANSI sequence? do a crude conversion from this table:
        \033[A     ctrl('k')    up
        \033[B     ctrl('j')    down
        \033[C     ctrl('l')    right
        \033[D     ctrl('h')    left
        \033[5~    ctrl('p')    pgup
        \033[6~    ctrl('n')    pgdn
        \033[H     ctrl('a')    home
        \033[F     ctrl('e')    end
        \033[3~    ctrl('d')    delete
    */
    if (key[0] == 0x1b) {
        char *key1 = "ABCD56HF3";
        char *key2 = "kjlhpnaed";
        char *p;

        if ((p = strchr(key1, key[2])))
            key[0] = ctrl(key2[p - key1]);
    }

    switch (key[0]) {
    case ctrl('l'):
        /* move right */
        if (ue.e.cpos < ue.e.size)
            ue.e.cpos++;
        break;

    case ctrl('h'):
        /* move left */
        if (ue.e.cpos > 0)
            ue.e.cpos--;
        break;

    case ctrl('a'):
        /* beginning of row */
        ue.e.cpos = ue_find_col_0(ue.e.cpos);

        break;

    case ctrl('e'):
        /* end of row */
        ue.e.cpos = ue_find_col_0(ue.e.cpos);
        ue.e.cpos += ue_row_size(ue.e.cpos);

        break;

    case ctrl('k'):
        /* move up */
        {
            int col0;

            /* not at BOF? */
            if ((col0 = ue_find_col_0(ue.e.cpos))) {
                int col = ue.e.cpos - col0;
                int size;

                /* find the col0 of the previous row */
                col0 = ue_find_col_0(col0 - 1);

                /* move to previous column or end of row */
                size = ue_row_size(col0);
                ue.e.cpos = col0 + (col < size ? col : size);
            }
        }

        break;

    case ctrl('j'):
        /* move down */
        {
            int col0, col, size;

            col0 = ue_find_col_0(ue.e.cpos);
            col  = ue.e.cpos - col0;
            size = ue_row_size(col0);

            /* not at EOF? */
            if (col0 + size < ue.e.size) {
                /* move to the beginning of the next line */
                ue.e.cpos = col0 + size + 1;

                /* move to previous column or end of row */
                size = ue_row_size(ue.e.cpos);
                ue.e.cpos += (col < size ? col : size);
            }
        }

        break;

    case ctrl('p'):
        /* page up */
        for (n = 0; n < ue.height - 1; n++)
            ue_input("\x0b");

        break;

    case ctrl('n'):
        /* page down */
        for (n = 0; n < ue.height - 1; n++)
            ue_input("\x0a");

        break;

    case ctrl('s'):
        /* save file */
        save_file(ue.fname);
        break;

    case ctrl('x'):
        /* cut block */
        ue_snap();

        n = 1;

        /* fallthrough */

    case ctrl('c'):
        /* copy block */
        if (ue.e.mark_e != -1) {
            /* alloc space into clipboard */
            ue.clip_size = ue.e.mark_e - ue.e.mark_s;
            memcpy(ue.clip, &ue.e.data[ue.e.mark_s], ue.clip_size);

            /* cut? delete block */
            if (n)
                ue_input("\x04");
        }

        /* fallthrough */

    case ctrl('u'):
        /* unmark selection */
        ue.e.mark_s = ue.e.mark_e = -1;
        break;

    case ctrl('v'):
        /* paste block */
        ue_snap();

        if (ue_expand(ue.clip_size)) {
            memcpy(&ue.e.data[ue.e.cpos], ue.clip, ue.clip_size);
            ue.e.cpos += ue.clip_size;
        }

        break;

    case ctrl('b'):
        /* mark beginning / end of selection */
        if (ue.e.mark_s == -1)
            ue.e.mark_s = ue.e.cpos;
        else
        if (ue.e.mark_e == -1)
            ue.e.mark_e = ue.e.cpos;

        break;

    case ctrl('q'):
        /* quit (if not modified) */
        if (ue.e.modified) {
            if (ue.refuse_quit == 2)
                running = 0;
            else
                ue.refuse_quit = 1;
        }
        else
            running = 0;

        break;

    case ctrl('y'):
        /* delete line */
        ue_snap();

        ue.e.cpos = ue_find_col_0(ue.e.cpos);
        ue_delete(ue_row_size(ue.e.cpos) + 1);
        break;

    case '\177':
        /* backspace */
        if (ue.e.cpos == 0)
            break;

        ue.e.cpos--;

        /* fall through */

    case ctrl('d'):
        /* delete char under the cursor (or the selected block) */
        ue_snap();
        ue_delete(1);

        break;

    case ctrl('f'):
    case ctrl('g'):
    case ctrl('o'):
    case ctrl('r'):
    case ctrl('t'):
    case ctrl('w'):
        /* unused keys */
        break;

    case '\t':
        /* tab */
        n = TAB_SIZE - (ue.e.cpos - ue_find_col_0(ue.e.cpos)) % TAB_SIZE;
        while (n--)
            ue_insert(' ');

        break;

    case ctrl('z'):
        /* undo */
        ue_undo();

        break;

    default:
        if (key[0] != '\x1b') {
            uint32_t cpoint;
            int s = 0;

            ue_snap();

            while (*key) {
                if (*key == '\r')
                    *key = '\n';

                /* decode utf-8 and insert char by char */
                if (utf8_to_internal(&cpoint, &s, *key++) == 0)
                    ue_insert(cpoint);
            }
        }
        break;
    }

    return running;
}


int main(int argc, char *argv[])
{
    struct termios tios;

    if (argc != 2) {
        printf("Usage: ue {file to edit}\n");
        goto end;
    }

    if (load_file(argv[1]))
        goto end;

    /* startup */
    raw_tty(1, &tios);
    sigwinch_handler(0);
    startup();

    /* unmark selection */
    ue_input("\x15");

    /* main loop */
    for (;;) {
        /* read terminal size, if new or changed */
        if (ue.sigwinch_received)
            get_tty_size();

        ue_output();

        if (!ue_input(read_string()))
            break;
    }

    /* shutdown */
    raw_tty(0, &tios);
    shutdown();

end:
    return 0;
}
