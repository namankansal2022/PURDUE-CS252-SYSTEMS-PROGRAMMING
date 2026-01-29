#include <stdlib.h>
#include <stdio.h>
#include <termios.h>
#include <string.h>

static struct termios original_tty;
static int tty_saved = 0;

/* 
 * Sets terminal into raw mode. 
 * This causes having the characters available
 * immediately instead of waiting for a newline. 
 * Also there is no automatic echo.
 */
void tty_raw_mode(void)
{
    struct termios tty_attr;
    
    if (!tty_saved) {
        tcgetattr(0, &original_tty);
        tty_saved = 1;
    }
    
    tcgetattr(0, &tty_attr);
    
    /* Set raw mode. */
    tty_attr.c_lflag &= (~(ICANON|ECHO));
    tty_attr.c_cc[VTIME] = 0;
    tty_attr.c_cc[VMIN] = 1;
    
    tcsetattr(0, TCSANOW, &tty_attr);
}

/* 
 * Restores terminal to original mode.
 */
void tty_reset(void)
{
    if (tty_saved) {
        tcsetattr(0, TCSANOW, &original_tty);
    }
}