// Whack-A-Mole
//
// A terminal mode whack-a-mole type game.
//
// Missing the Makefile? try: gcc -pthread -Wall -lrt -lncurses wam.c
//
// Questions/comments to: paulweaver@paulweaver.org
//

#define VERSTRING "V1.0RC16"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <error.h>
#include <ncurses.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>

//=========
// #defines
#define MAXPOPUPCOUNT   100   // Limit on max number of moles.
#define MAXDURATION     15000 // Limit on max mole cycle time (msec)
                              // This is a limit for reasonability check only, actual time is
                              // set by moletime variable in main().
#define MOLEHOLES       9     // How many holes do moles have available to choose
#define CONCURRENTMOLES 3     // How many threaded moles at once
#define GRACEPERIOD     500   // How long after mole times out (msec) before we
                              // consider its key to be a misfire.
#define SCAREDDURATION  2000  // How long moles stay scared after misfire (msec).
#define MSEC            1000000L // Handy define for use with nanosleep()

//#define AUTOPLAY        10000    // Causes input thread to start and play the game
                                // Number is the max delay between simulated keystrokes.

                            // DISP_ELE... Bits to control display_empty_playfield().
#define DISP_ELE_HOLES  1   // Indicates holes should be displayed.
#define DISP_ELE_KEYS   2   //        ...keys...
#define DISP_ELE_VERS   4   //        ...version string...
#define DISP_ELE_MSG    8   //        ...message text...
#define DISP_ELE_STAT   16  //        ...game status...
#define DISP_ELE_ALL    0xffffffff // ...all items...

//=====================================
// Functions implemented as #defines...
// Used to compress code for readability, while maintaining ability to reference
// line numbers for diagnostics.
#define lock_molecomm() \
{\
    int err;\
    if ((err = pthread_mutex_lock(&molecomm_mtx)) != 0) {\
        restore_terminal();\
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to lock molecomm mutex.");\
    }\
}

#define unlock_molecomm() \
{\
    int err;\
    if ((err = pthread_mutex_unlock(&molecomm_mtx)) != 0) {\
        restore_terminal();\
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to unlock molecomm mutex.");\
    }\
}

#define lock_ncurses() \
{\
    int err;\
    if ((err = pthread_mutex_lock(&ncurses_mtx)) != 0) {\
        restore_terminal();\
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to lock ncurses mutex.");\
    }\
}

#define unlock_ncurses() \
{\
    int err;\
    if ((err = pthread_mutex_unlock(&ncurses_mtx)) != 0) {\
        restore_terminal();\
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to unlock ncurses mutex.");\
    }\
}

#define lock_scores() \
{\
    int err;\
    if ((err = pthread_mutex_lock(&score_mtx)) != 0) {\
        restore_terminal();\
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to lock scores mutex.");\
    }\
}

#define unlock_scores() \
{\
    int err;\
    if ((err = pthread_mutex_unlock(&score_mtx)) != 0) {\
        restore_terminal();\
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to unlock scores mutex.");\
    }\
}

#define disable_thread_cancel()\
{\
    int oldstate; \
    int err;\
    if ((err = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate)) != 0) {\
        restore_terminal();\
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to disable thread cancel state.");\
    }\
}

#define enable_thread_cancel()\
{\
    int oldstate; \
    int err;\
    if ((err = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate)) != 0) {\
        restore_terminal();\
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to enable thread cancel state.");\
    }\
}

//======
// enums
enum PlayResult { WHACK, ESCAPE, MISFIRE, TOOSOON, SCAREDOFF };
                // WHACK = Mole hit successfully.
                // ESCAPE = Mole missed.
                // MISFIRE = Key hit when no mole was up in that hole.
                // TOOSOON = Key hit when mole was hiding in that hole.
                // SCAREDOFF = Mole scared off by misfire.

enum MoleStatus { AVAILABLE = 0, ASSIGNED, HIDING, UP, WHACKED, EXPIRED, SCARED, TERMINATING, COMPLETE };
                // AVAILABLE = Indicates this slot may be assigned to new thread
                // ASSIGNED = Assigned by control fuction. Available for thread.
                // HIDING = In use by thread.  Mole has not poped up yet.
                // UP = Mole is up, waiting for key.
                // WHACKED = Mole successfully hit by player.
                // EXPIRED = Mole missed, but corresponding key will not yet
                //           be considered a misfire.
                // SCARED = Mole was scared away by a misfire. (Functionally same as
                //          EXPIRED status)
                // TERMINATING = Mole thread performing final scorekeeping and cleanup.
                // COMPLETE = Thread is done and may be joined

enum GameMode { BASEGAME, TIMEDGAME };  // GameMode unimplemented. Only BASEGAME supported.
                // BASEGAME = Fixed number of moles fit into a target time.
                // TIMEDGAME = Unlimited moles in fixed amount of time.

enum AnimationType { ANIMHIDING, ANIMPOPUP, ANIMWHACKED, ANIMESCAPED, ANIMMISFIRE, ANIMMISFIRESCARED, ANIMUPSCARED, SPLASHPOPUP, INSTRPOPUP, INSTRSCARED};
                // ANIMHIDING = Mole hiding in hole, but not popped up yet.
                //              (Ears will periodically bob up and down).
                // ANIMPOPUP = Mole Quickly pops up and slowly drops until whacked.
                // ANIMWHACKED = Displays whacked panel, then score/bonus panel.
                // ANIMESCAPED = Displays escaped panel, then score panel.
                // ANIMMISFIRE = Displays misfire panel.
                // ANIMMISFIRESCARED = Displays misfire panel, then escaped panel.
                // ANIMUPSCARED = Displays Scared panel, escape panel, then score panel.
                // SPLASHPOPUP = Mole pops up, but never drops. (Used by splash page).
                // INSTRPOPUP = Mole pops up, drops, and loops. (Used by instruction page).
                // INSTRSCARED = Mole is up, scared, blank, loops (Used by instruction page).

//===========
// Structures
struct ScoreSheetRecord {
    long totaltime;         // total up time for this mole in msec
    long remainingtime;     // up time remaining in msec when mole was whacked
    int mole;               // mole #
    int hole;               // hole # where mole appeared (-1 for n/a)
    int startscore;         // starting score before changes applied from this mole
    int missedscore;        // lost points for mole getting away
    int whackedscore;       // points earned for whacking mole
    int bonusscore;         // bonus points for waiting longer to whack mole
    int penaltyscore;       // lost points for misfire
    int endscore;           // ending score after these changes are applied
    enum PlayResult playresult; // WHACK, ESCAPE, MISFIRE, TOOSOON, or SCAREDOFF
    char selection;         // Player choice (key pressed or '\x0' for timeout)
};

struct AnimationSpec {
    enum AnimationType animationtype;   // See enum definition for description.
    int hole;                           // Hole number .
    int numholes;                       // Total number of holes. (must always be MOLEHOLES for now)
    int duration;                       // Start to end duration in msec.
    int score1;                         // Main score or penalty for animations that use it.
    int score2;                         // Bonus score if needed.
    int syncpoints;                     // Number of sync points this animation contains. (Used for
                                        // determining when the animation has ended.)
    int synccount;                      // How many sync points have elapsed so far.  
                                        // Function creating the animation thread is
                                        // responsible for setting this to zero.
                                        // Animation thread sets this to 1 to indicate
                                        // that it has started, and increments it one
                                        // or more times as the animation progresses.
                                        // This is used by score calculation to determine when
                                        // player whacked the mole (and therefore their score).
                                        // Also used to determine when it is safe to kill
                                        // the POPUP animation, and keeps the mole thread in
                                        // sync with the animations.
    int mole;                           // Mole number. Not strictly needed by animation
                                        // currently, but handy for debugging.
#if defined(debug)
    int threadsn;                       // Thread serial number. also for debugging.
#endif
};

struct MoleCommRecord {// Mole thread communications
    volatile 
    enum MoleStatus molestatus; // State of this mole
    volatile
    enum MoleStatus displayack; // Display thread copies molestatus here to
                                // indicate it has handled the status change.
                                // Also used within display thread to check prion
                                // mole status.
    pthread_t thread;           // Thread ID for this moles mole_thread
    pthread_cond_t keycond;     // Thread condition variable used by input_thread
                                // to signal mole_thread that its key was pressed.
    pthread_cond_t dispcond;    // Thread condition variable used by display_thread
                                // to acknowledge mole status change.
    int threadslot;             // Index to this molecomm element, because 
                                // sometimes we only have a pointer
    int mole;                   // Mole # - aka round #
    long duration;              // Cycle time (hiding + up time) in msec
    long uptime;                // Portion of cycle time when mole will be up (not hiding)
    int hole;                   // Hole # this mole has chosen
    volatile
    char keystruck;             // Set when proper key struck. Used to catch spurious wakeups
    pthread_t animthread;       // Thread ID for this mole's animation_thread
    struct AnimationSpec animspec; // Animation Spec buffer for above thread;
    int animcancelled;          // Flag to prevent animation thread from being double-cancelled
                                // 0 = Not cancelled, 1 = Cancelled.
    int scoreidx;               // Index into scores array for this mole's score
    int scaredflag;             // Indicates this mole is scared.  Set by input_thread,
                                // used by mole_thread.
    struct timespec scaredtime; // Time mole was scared. (Used for delay before new moles start).
} molecomm[CONCURRENTMOLES];

//===========
// prototypes
//
void clear_input_buffer(void);
int compute_score(int mole, int hole, char key, int bonusstage, enum PlayResult playresult);
void display_score_sheet(int gamescore, int moles, int gametime);
void display_intro(int moles, int gametime);
void initialize_terminal(void);
int record_results(int mole, int hole, char key, int startscore, int missedscore, int whackedscore, int bonusscore, int penaltyscore, int endscore, enum PlayResult playresult);
void control_moles(int count, int duration);
void restore_terminal(void);
char waitforkey(long *msec);
long tsrandom();
pthread_t *start_input_thread(void);
pthread_t *start_display_thread(void);
void *display_thread(void *arg);
void *mole_thread(void *arg);
void *animation_thread(void *arg);
int claim_mole_hole(int molehole);
int check_mole_hole(int molehole);
void release_mole_hole(int molehole);
void assign_hole_keys(void);
void set_mole_status(struct MoleCommRecord *p, enum MoleStatus newstatus);
void set_mole_uptime(struct MoleCommRecord *p, long uptime);
void display_empty_playfield(enum GameMode gamemode, int elements, int holes, char *msg);
void show_mole(int hole, int maxholes, int level);
void show_result(int hole, int maxholes, enum PlayResult result, int score1, int score2, char *txt);
int intro_overview(int);
int intro_playfield(int);
int intro_hidingmoles(int);
int intro_popup(int);
int intro_playresults(int);
int intro_scoring(int);
int intro_penalties(int);
int intro_scoresheet(int);
void display_countdown(void);
void display_gameover(void);

// Pointers to introductory instruction page functions.
int (*intropages[])(int) = {intro_overview, intro_playfield, intro_hidingmoles, intro_popup, intro_playresults, intro_scoring, intro_penalties, intro_scoresheet};

//=================
// global variables
struct ScoreSheetRecord *scores = NULL;
int numscores = 0;
char inputkey; // From input_thread();
char holekeys[MOLEHOLES];  // Allows reassignment of keys for each mole hole
volatile int kbthread_running = 0;       // input_thread status
volatile int display_thread_running = 0; // display_thread status
const struct timespec one_msec = {0, MSEC};
int molesremaining = -1;  // Global count for main display
#if defined(debug)
int threadsn = 0;
#endif

//===========
// Animations
const struct AnimationSpec WhackedAnim = {ANIMWHACKED,0,MOLEHOLES,1500,0,0,3,0};
const struct AnimationSpec EscapedAnim = {ANIMESCAPED,0,MOLEHOLES,1500,0,0,3,0};
const struct AnimationSpec HidingAnim = {ANIMHIDING,0,MOLEHOLES,0,0,0,2,0};
const struct AnimationSpec PopupAnim = {ANIMPOPUP,0,MOLEHOLES,0,0,0,6,0};
const struct AnimationSpec MisfireScaredAnim = {ANIMMISFIRESCARED,0,MOLEHOLES,2000,0,0,2,0};
const struct AnimationSpec HideScaredAnim = {ANIMUPSCARED,0,MOLEHOLES,2000,0,0,2,0};
const struct AnimationSpec UpScaredAnim = {ANIMUPSCARED,0,MOLEHOLES,2000,0,0,2,0};
const struct AnimationSpec PopupSplash = {SPLASHPOPUP,0,MOLEHOLES,0,0,0,2,0};
const struct AnimationSpec PopupInstr = {INSTRPOPUP,0,MOLEHOLES,0,0,0,2,0};
const struct AnimationSpec ScaredInstr = {INSTRSCARED,0,MOLEHOLES,0,0,0,2,0};

//===============================
// Ascii art for animation frames
const char *asciimole[] = { " ^=--=^ ", 
                            " | oO | ", 
                            " (\"||\") ", 
                            " / \\/ \\ ", 
                            "(((  )))"};
char *asciiwhack[] = {  " *   *  ",
                        "  * *   ",
                        "*WHACK!*",
                        "  * *   ",
                        " *   *  " };

char *asciiescape[] = { "  .  .  ",
                        " . .. . ",
                        "  poof  ",
                        " . .. . ",
                        "  .  .  " };

char *asciimisfire[] = {" \\\\  // ",
                        "  \\\\//  ",
                        "   //   ",
                        "  //\\\\  ",
                        " //  \\\\ " };

char *asciiscared[] = {  " ^\\^^/^ ",
                          " |(OO)| ",
                          " ( __ ) ",
                          " /    \\ ",
                          "'''  '''" };

//========
// Mutexes
//
// To avoid deadlocks when holding multiple locks, always lock mutexes in the order 
// they appear here.  Or, use the "try, and then back off" locking strategy.
//
pthread_mutex_t start_mtx = PTHREAD_MUTEX_INITIALIZER; // Used to make sure threads for keyboard 
                                                       // and display are running before launching 
                                                       // mole threads.

pthread_cond_t start_cond = PTHREAD_COND_INITIALIZER;  // Condition variable to go along 
                                                       // with start_mtx

pthread_mutex_t hole_mtx[MOLEHOLES]; // Used to prevent two moles trying to pop up in
                                     // same hole. Need to dynamically initialize at run time

pthread_mutex_t molecomm_mtx = PTHREAD_MUTEX_INITIALIZER; // Lock for Mole communications
                                                          // buffer. Coordinates interaction 
                                                          // between keyboard, game play, 
                                                          // and display.

pthread_mutex_t random_mtx = PTHREAD_MUTEX_INITIALIZER; // random() not totally thread safe,
                                                        // so lock it.

pthread_mutex_t score_mtx = PTHREAD_MUTEX_INITIALIZER; // Scores buffer is dynamically allocated
                                                       // and moves as realloc(...) is called. 
                                                       // So, score updates need to be locked. 

pthread_mutex_t ncurses_mtx = PTHREAD_MUTEX_INITIALIZER; // Our use of ncurses is no longer
                                                         // limited to a single thread... 
                                                         // we now have multiple animation 
                                                         // threads in addition to the main 
                                                         // display thread. So, we need to 
                                                         // lock ncurses calls.

//=============================
// long tsrandom()
//
// Thread-safe wrapper for random() library call.  Created mostly as an exercise.
// Could actually just use random_r() library call if available.
//
// randbuf = buffer from calling function where random number will be stored.
//
// Returns: random number (same as in *randbuf)
//
long tsrandom() {
    int err;
    if ((err = pthread_mutex_lock(&random_mtx)) != 0) {
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to lock \"random()\" mutex.");
    }

    long randbuf = random();

    if ((err = pthread_mutex_unlock(&random_mtx)) != 0) {
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to unlock \"random()\" mutex.");
    }

    return randbuf;
};

//===================================
// int claim_mole_hole(int molehole)
//
// Assures that only one mole claims a hole at any given time
// by locking the hole_mtx for that hole.
//
// molehole = Hole number to claim (zero based). Function will
//            block until that hole is available...  
//            Or, -1 to indicate that that a random hole should be
//            assigned.  In this case, function will keep trying 
//            until a hole is available.
//
// Returns: hole number assigned (zero based).
//
int claim_mole_hole(int molehole) {
    if (molehole < -1 || molehole >= MOLEHOLES) {
        error_at_line(-1, -1, __FILE__, __LINE__, "hole number (%d) out of range.", molehole);
    }

    if (molehole == -1) { // search random holes until an available one is found
        for (;;) {
            molehole = tsrandom() % MOLEHOLES;
            int err;
            err = pthread_mutex_trylock(&hole_mtx[molehole]);
            if (err == 0) {
                break; // Lock acquired
            } else if (err == EBUSY) {
                struct timespec sleeptime = {0, 10000000L}; // 10 msec sleep so we
                nanosleep(&sleeptime, NULL);                // don't burn up the cpu
                continue;  // try again
            } else {
                restore_terminal();
                error_at_line(-1, err, __FILE__, __LINE__, "Unable to lock hole %d mutex.", molehole);
            }
        }
    } else { // Block waiting for lock on specific hole.
        int err;
        if ((err = pthread_mutex_lock(&hole_mtx[molehole])) != 0) {
            restore_terminal();
            error_at_line(-1, err, __FILE__, __LINE__, "Unable to lock hole %d mutex.", molehole);
        }
    }

    return molehole;
}

//===================================
// int check_mole_hole(int molehole)
//
// Checks to see if a mole hole mutex is locked.
//
// molehole = Hole number to check (zero based). 
//
// Returns: 1 = already claimed, 0 = available.
//
int check_mole_hole(int molehole) {
    if (molehole < 0 || molehole >= MOLEHOLES) {
        error_at_line(-1, -1, __FILE__, __LINE__, "hole number (%d) out of range.", molehole);
    }

    int err = pthread_mutex_trylock(&hole_mtx[molehole]);
    if (err == 0) {
        if ((err = pthread_mutex_unlock(&hole_mtx[molehole])) != 0) {
            error_at_line(-1, err, __FILE__, __LINE__, "Unable to unlock hole %d mutex.", molehole);
        }
        return 0; // hole is available
    } else if (err == EBUSY) {
        return 1; // hole is already claimed
    } else {
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to lock hole %d mutex.", molehole);
    }

    assert(0); // Control flow never gets here. 
}

//=====================================
// void release_mole_hole(int molehole)
//
// Releases mutex lock on a hole.
//
// molehole = hole number (zero based)
//
// Returns: void
//
void release_mole_hole(int molehole) {
    int err;
    if ((err = pthread_mutex_unlock(&hole_mtx[molehole])) != 0) {
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to unlock hole %d mutex.", molehole);
    }
}

//==============================
//void initialize_terminal(void)
//
// Set terminal to raw mode for direct access to keystrokes
//
void initialize_terminal(void) {
    initscr();  // Enter ncurses mode

    if (COLS < 80 || LINES <25) {
        restore_terminal();
        error_at_line(-1, 0, __FILE__, __LINE__, "Minimum terminal size is 80x25 (Yours is %dx%d).", COLS, LINES);
    }

    scrollok(stdscr, TRUE); // Allow scrolling
    curs_set(0);            // Disable cursor
}

//============================
// void restore_terminal(void)
//
// restores terminal settings back to the way we found them
//
//
void restore_terminal(void) {
    if (stdscr != NULL && isendwin() == FALSE) {
        endwin();  // Exit ncurses mode.
    }
}

//============================
// char waitforkey(long *msec)
//
// nonblocking keyboard input
// msec = pointer to max wait time in miliseconds
//        or NULL for blocking input
// returns key pressed, or '\0' for timeout
// also (if msec is ! NULL), sets contents of msec to time remaining 
// when key was pressed (or zero if !pressed)
//
char waitforkey(long *msec) {
    struct timeval waittime;
    fd_set stdin_fd;
    char cbuf;

    if (msec == NULL) {  // do blocking input
        int result;
        for (;;) {
            errno = 0;
            result = read(STDIN_FILENO, &cbuf, 1);
            if (errno == EINTR) { // i.e. if window resized
                lock_ncurses();
                refresh();
                unlock_ncurses();
                continue;
            } else {
                break;
            }
        }
        if (result != 1) {
            restore_terminal();
            error_at_line(-1, errno, __FILE__, __LINE__, "stdin read error.");
        }
        clear_input_buffer();  // swallow extra keys, extended key codes, etc.
        return cbuf;
    } else {           // do timed input
        if (*msec < 0L) *msec = 0L;

        FD_ZERO(&stdin_fd);
        FD_SET(STDIN_FILENO, &stdin_fd);

        waittime.tv_sec = (int)(*msec / 1000L);
        waittime.tv_usec = (long)*msec % 1000L * 1000L;

        int keyhit = select(STDIN_FILENO + 1, &stdin_fd, NULL, NULL, &waittime);
        if (keyhit == 0) { //timeout
            *msec = 0L;
            return '\0';
        } else if (keyhit > 0) { //key pressed
            int result;

            *msec = (long) waittime.tv_sec * 1000L + waittime.tv_usec / 1000L;

            for (;;) {
                errno = 0;
                result = read(STDIN_FILENO, &cbuf, 1);
                if (errno == EINTR) { // i.e. if window resized
                    lock_ncurses();
                    refresh();
                    unlock_ncurses();
                    continue;
                } else {
                    break;
                }
            }
            if (result != 1) {
                restore_terminal();
                error_at_line(-1, errno, __FILE__, __LINE__, "stdin read error.");
            }

            clear_input_buffer();  // swallow extra keys, extended key codes, etc.
            return cbuf;
        } else { // error
            restore_terminal();
            error_at_line(-1, errno, __FILE__, __LINE__, "select call error.");
        }
    }
    assert(0); // Control flow never gets here. 
    return '\0';
}

//=============================
//void clear_input_buffer(void)
//
//Swallow all keys in the buffer
//
void clear_input_buffer(void) {
    long zerotime = 0L;
    while (waitforkey(&zerotime));
}

//============================================================================================
// int compute_score(int mole, int hole, char key, int bonusstage, enum PlayResult playresult)
//
// computes score based on target hole, key pressed, how long it took player to
// press it, how long they had available, and their total score so far.
//
// mole = mole #
// hole = hole #
// key = the key pressed by player
// bonusstage = For whacked mole, how par through the pop animation was the mole?
//              (0=<20%, 1=20 to <40%, 2+40 to <60%, 3=60 to <80%, 4=80 to 100%)
// playresult = WHACK, ESCAPE, MISFIRE, TOOSOON, SCAREDOFF
//
// Returns: Index to scores buffer
//
// Score #defines...
//          Missed mole =  -10 pts per mole,
//                         multiplied by # moles missed (so far)
//                         capped at -50 per mole.
//                         Cannot cause cumulative score to go negative.
//          Whacked mole = 20 pts
//          Bonus for fast reactions or for waiting until last moment.
//              wait < 20% of up time =         25 pts
//              wait >=20% to <40% of up time =  0 pts.
//              wait >=40% to <60% of up time =  0 pts.
//              wait >=60% to <80% of up time = 20 pts.
//              wait >=80% to 100% of up time = 80 pts.
//

// NOTES ON SCORES: Care should be taken if changing scores beyond 2 digits (plus sign), 
//                  as this will probably require adjustments to some of the animations
//                  that include a score display.
#define MISSEDMOLESCORE -10
#define MISSEDMOLEMULTIPLIER 1
#define MISSEDMOLECAP -50
#define WHACKEDMOLESCORE 20
#define BONUSSLICES 5
const int BONUSPOINTS[BONUSSLICES] = {25,0,0,20,80};
int compute_score(int mole, int hole, char key, int bonusstage, enum PlayResult playresult) {
    static int missedcount = 0;
    int missedscore = 0;
    int whackedscore = 0;
    int bonusscore = 0;
    int penaltyscore = 0;
    int curscore = 0;

    lock_scores();

    if (numscores > 0) {
        curscore = scores[numscores-1].endscore;
    }

    switch(playresult) {
        case WHACK: {
            whackedscore += WHACKEDMOLESCORE;
            bonusscore = BONUSPOINTS[bonusstage];
        } break;

        case ESCAPE: 
        case SCAREDOFF: {
            missedscore = ++missedcount * MISSEDMOLESCORE * MISSEDMOLEMULTIPLIER;
            if (missedscore < MISSEDMOLECAP) missedscore = MISSEDMOLECAP;
            if (-missedscore > curscore) missedscore = -curscore;
        } break;

        case MISFIRE: 
        case TOOSOON: {
            penaltyscore = 0;  // no score penalty in this version
        } break;

        default: {
            // intentionally left empty
        };
    }

    // pass index into scores buffer back to caller
    int scorenum = record_results(mole, hole, key, curscore, missedscore, whackedscore, bonusscore, penaltyscore, curscore + missedscore + whackedscore + bonusscore + penaltyscore, playresult);

    unlock_scores();

    return scorenum;
}

//============================================================================================
//int record_results(int mole, int hole, char key, int startscore, int missedscore, int whackedscore, int bonusscore, int penaltyscore, int endscore, enum PlayResult playresult);
//
//  Records results for later use in score display
//
//  mole = mole number
//  hole = hole number
//  key = key pressed by player
//  startscore = player score before any changes from this mole
//  missedscore = score for missing mole completely (negative)
//  whackedscore = score for successfully whacking mole
//  bonus score = extra points for waiting to whack mole
//  penalty score = score for hitting wrong key (negative)
//  endscore = cumulative score after these changes are applied to startscore
//  playresult = WHACK, ESCAPE, MISFIRE, TOOSOON, or SCAREDOFF
//
//  Returns: index to scores buffer 
//
//  This function is called exclusively by compute_score(...) function, which holds a mutex lock on scores
//  buffer.  Therefore, no lock is required here.
//
int record_results(int mole, int hole, char key, int startscore, int missedscore, int whackedscore, int bonusscore, int penaltyscore, int endscore, enum PlayResult playresult){

    ++numscores;
    if (scores == NULL) {
        scores = malloc(sizeof(struct ScoreSheetRecord)); 
        if (scores == NULL) {
            restore_terminal();
            error_at_line(-1, errno, __FILE__, __LINE__, "malloc failed.");
        }
    } else {
        struct ScoreSheetRecord *temp;  // temp pointer. (In case realloc fails)
        temp = realloc(scores, numscores * sizeof(struct ScoreSheetRecord));
        if (temp == NULL) {
            free(scores);
            restore_terminal();
            error_at_line(-1, errno, __FILE__, __LINE__, "realloc failed.\n");
        }
        scores = temp;
    }

    struct ScoreSheetRecord *p = &scores[numscores-1];

    p->mole = mole;
    p->hole = hole;
    p->startscore = startscore;
    p->missedscore = missedscore;
    p->whackedscore = whackedscore;
    p->bonusscore = bonusscore;
    p->penaltyscore = penaltyscore;
    p->selection = key;
    p->playresult = playresult;
    p->endscore = endscore;

    return numscores - 1;
}

//============================================================
// void set_mole_uptime(struct MoleCommRecord *p, long uptime)
//
// Updates mole's chosen random uptime in molecomm. Wrapped with molecomm mutex 
// to prevent race conditions when interracting with display_thread().
// 
// p = pointer to the molecomm record for this thread.
// uptime = mole up time in msec
//
// Return: void
//
void set_mole_uptime(struct MoleCommRecord *p, long uptime) {
    lock_molecomm();

    p->uptime = uptime;

    unlock_molecomm();
}

//=========================================================================
//void set_mole_status(Struct MoleCommRecord *p, enum MoleStatus newstatus)
//
// Updates a mole's status.  
//
// Calling function is responsible for obtaining molecomm mutex lock before 
// calling this function.
//
// In the case of HIDING, UP, WHACKED, EXPIRED and TERMINATING moles, this function
// will wait for display_thread to acknowledge the change.
//
// p = pointer to the molecomm record for this thread.
// newstatus = New value to set p->molestatus to.
//
// Return: void
//
void set_mole_status(struct MoleCommRecord *p, enum MoleStatus newstatus) {
    int err;

    switch (newstatus) {
        case AVAILABLE: assert(p->molestatus == COMPLETE) ;  break;
        case ASSIGNED: assert(p->molestatus == AVAILABLE) ;  break;
        case HIDING: assert(p->molestatus == ASSIGNED) ;  break;
        case UP: assert(p->molestatus == HIDING) ;  break;
        case WHACKED: assert(p->molestatus == UP) ;  break;
        case EXPIRED: assert(p->molestatus == UP) ;  break;
        case SCARED: assert(p->molestatus == HIDING || p->molestatus == UP) ;  break;
        case TERMINATING: assert(p->molestatus == WHACKED || p->molestatus == EXPIRED || p->molestatus == SCARED) ;  break;
        case COMPLETE: assert(p->molestatus == TERMINATING) ;  break;
    }

    if (newstatus == AVAILABLE) {
        memset(p, 0, sizeof(struct MoleCommRecord));    // Clear out any left over data
    }

    p->molestatus = newstatus;

    if (newstatus==HIDING || newstatus==UP || newstatus==WHACKED || newstatus==EXPIRED || newstatus==TERMINATING) {
        while (p->molestatus != p->displayack) {
            if ((err = pthread_cond_wait(&p->dispcond, &molecomm_mtx)) != 0) {
                restore_terminal();
                error_at_line(-1, err, __FILE__, __LINE__, "Display thread cond wait failed.");
            }
        }
    }
}

// ============================
// void *mole_thread(void *arg)
//
// Handle a single thread-based visit by one mole.
//
// Locks mole hole.
// Selects random timing for mole.
// Sets state to HIDING, waits for hiding animation to finish.
// Sets state to UP, waits for popup animation to finish or be terminated.
// Sets state to EXPIRED or WHACKED, waits for animation to finish.
// Sets state to TERMINATED, waits for display ack
// Releases mole hole.
// sets state to COMPLETE
// 
// arg = pointer to molecomm record
//
#define MOLESTARTDELAYMIN 250  //msec
#define MOLESTARTDELAYMAX 3000 //msec
void *mole_thread(void *arg) {
    struct MoleCommRecord *p = (struct MoleCommRecord *)arg;

 #if defined(debug) && defined(_GNU_SOURCE)
    pthread_setname_np(pthread_self(), "WAM-Mole");
 #endif

    // Varying delay so they don't all start at once.
    long molestartdelay;
    if (p->mole == 1) {
        molestartdelay = MOLESTARTDELAYMIN; // Start the first one quickly ,
                                            // to avoid seeming like program locked up.
    } else {
        // subsequent moles can wait longer.
        molestartdelay = ( tsrandom() % (MOLESTARTDELAYMAX - MOLESTARTDELAYMIN) ) + MOLESTARTDELAYMIN;
    }
    struct timespec delaytime;
    delaytime.tv_sec = molestartdelay / 1000;
    delaytime.tv_nsec = molestartdelay % 1000 * 1000000L;
    nanosleep(&delaytime, NULL);

    // Claim mole hole
    int molehole; 
    molehole = claim_mole_hole(-1); // Claim random mole hole

    lock_molecomm();
    p->hole = (int)molehole;

    unlock_molecomm();

    // Set random timing for mole...
    // Duty cycle of each popup ranges from 30% to 80%
    long uptime = tsrandom();
    uptime = (uptime % 5000L + 3000L) * (long)p->duration / 10000L;
    set_mole_uptime(p, uptime);

    lock_molecomm();
    set_mole_status(p, HIDING);
    unlock_molecomm();

    for (;;) {  // Wait for HIDING animation to signal it has completed (synccount == syncpoints)
        lock_molecomm();
        if (p->animspec.syncpoints > 0 && p->animspec.synccount == p->animspec.syncpoints) {
            break;
        }

        unlock_molecomm();
        nanosleep(&one_msec, NULL);
    }
    unlock_molecomm();

    if (! p->scaredflag) {  // Pop up mole, unless it was scared. 
        struct timespec waituntil, starttime; // convert uptime to absolute time in timespec format
        clock_gettime(CLOCK_REALTIME, &starttime);

        waituntil.tv_sec = starttime.tv_sec + (uptime % 1000L * 1000000L + starttime.tv_nsec) / 1000000000L + uptime / 1000L;
        waituntil.tv_nsec = (uptime % 1000L * 1000000L + starttime.tv_nsec) % 1000000000L;

        lock_molecomm();
        set_mole_status(p, UP);

        p->keystruck = '\0'; // serves as predicate check for spurious wakeups
        int condretval = 0;

        while (p->keystruck == '\0' && condretval == 0) {
            // timed wait for input_thread to signal key was hit

            condretval = pthread_cond_timedwait(&p->keycond, &molecomm_mtx, &waituntil);
        }

        --molesremaining;

        switch (condretval) {
            case 0: {
                //Mole was either whacked or scared off

                //Check if mole was whacked ( p->keystruck matches holekeys[p->hole] )
                if (p->keystruck == holekeys[p->hole]) {
                    int ssidx;  // index into scoresheets

                    ssidx = compute_score(p->mole, p->hole, (char)p->hole + '0',p->animspec.synccount-1 , WHACK);

                    p->scoreidx = ssidx;  // Save index into scores buffer. display_thread will need it to pass to animation_thread

                    set_mole_status(p, WHACKED);

                    unlock_molecomm();

                    // Wait for GRACEPERIOD msec (in this case, it acts as a debounce
                    // since double strikes are fairly common).
                    struct timespec graceperiod;
                    graceperiod.tv_sec = GRACEPERIOD / 1000;
                    graceperiod.tv_nsec = GRACEPERIOD % 1000 * 1000000L;
                    nanosleep(&graceperiod, NULL);
                } else { // Mole was scared 
                    int ssidx;  // index into scoresheets
                    ssidx = compute_score(p->mole, p->hole, 0, 0, SCAREDOFF);
                    p->scoreidx = ssidx;  // Save index into scores buffer. display_thread will need it to pass to animation_thread
                    set_mole_status(p, SCARED);
                    unlock_molecomm();
                }
            } break; 

            case ETIMEDOUT: {
                unlock_molecomm();

                int ssidx;  // index into scores buf
                ssidx = compute_score(p->mole, p->hole, 0, 0, ESCAPE);
                lock_molecomm();

                p->scoreidx = ssidx;  // Save ndex into scores buf. display_thread will need it to pass to animation_thread

                set_mole_status(p, EXPIRED);

                unlock_molecomm();
                // Wait for GRACEPERIOD msec
                struct timespec graceperiod;
                graceperiod.tv_sec = GRACEPERIOD / 1000;
                graceperiod.tv_nsec = GRACEPERIOD % 1000 * 1000000L;
                nanosleep(&graceperiod, NULL);
            } break;

            default: {
                restore_terminal();
                error_at_line(-1, condretval, __FILE__, __LINE__, "Mole thread error on condition timedwait.");
            } break;
        }

        for (;;) {  // wait for WHACKED/ESCAPED animation to signal it has completed (synccount == syncpoints)
            lock_molecomm();
            if (p->animspec.syncpoints > 0 && p->animspec.synccount == p->animspec.syncpoints) {
                unlock_molecomm();
                break;
            }
            unlock_molecomm();
            nanosleep(&one_msec, NULL);
        }
    } else { // mole was scared, so no popup.  Set status to SCARED and release molecomm lock.
        compute_score(p->mole, p->hole, 0, 0, SCAREDOFF);
        set_mole_status(p, SCARED);

        --molesremaining;

        unlock_molecomm();
    }

    if (p->molestatus == SCARED) {
        for (;;) {  // wait for SCARED animation to signal it has completed (synccount == syncpoints)
            lock_molecomm();
            if (p->animspec.animationtype == ANIMMISFIRESCARED || p->animspec.animationtype == ANIMUPSCARED) {
                if (p->animspec.syncpoints > 0 && p->animspec.synccount == p->animspec.syncpoints) {
                    unlock_molecomm();
                    break;
                }
            }
            unlock_molecomm();
            nanosleep(&one_msec, NULL);
        }
    }

    lock_molecomm();
    release_mole_hole((int)molehole);
    set_mole_status(p, TERMINATING);
    set_mole_status(p, COMPLETE);
    unlock_molecomm();

    return NULL;
}

//============================================
// void control_moles(int count, int duration)
//
// Creates threads for moles. Runs up to CONCURRENTMOLES threads at a time
// until "count" mole threads have been completed.
//
//     count = number of popups (range 1 to MAXPOPUPCOUNT), 
//     duration = Mole cycle time in msec.
//
//     return: void
//
void control_moles(int count, int duration) {
    int err;

    // force args to sane values
    if (count < 1) count = 1;
    if (count > MAXPOPUPCOUNT) count = MAXPOPUPCOUNT;
    if (duration < 1000) duration = 1000;
    if (duration > MAXDURATION) {
        duration = MAXDURATION;
    }

    int molesstarted = 0;
    int molescompleted = 0;
    int idx = 0;
    molesremaining = count;

    while (molescompleted < count) {
        // p is pointer to the MoleCommRecord for this thread slot
        struct MoleCommRecord *p = &molecomm[idx];

        // If prior moles were scared off by a misfire, wait until creating new moles.
        struct timespec tsnow, tsexp;
        clock_gettime(CLOCK_MONOTONIC, &tsnow);
        tsexp = p->scaredtime;
        tsexp.tv_nsec += (SCAREDDURATION % 1000) * MSEC;
        tsexp.tv_sec += SCAREDDURATION / 1000;
        if (tsexp.tv_nsec >=  1000000000L) {
            tsexp.tv_nsec -= 1000000000L;
            tsexp.tv_sec++;
        }
        if (tsnow.tv_sec > tsexp.tv_sec || (tsnow.tv_sec == tsexp.tv_sec && tsnow.tv_nsec > tsexp.tv_nsec )) {
            switch (p->molestatus) {
                case AVAILABLE: { // empty slot, create a thread
                    if (molesstarted < count) {
                        lock_molecomm();
                        p->mole = molesstarted + 1;
                        p->threadslot = idx;
                        p->duration = duration;

                        unlock_molecomm();
                        set_mole_status(p, ASSIGNED);

                        if ((err = pthread_cond_init(&molecomm[idx].dispcond, NULL)) != 0) {
                            restore_terminal();
                            error_at_line(-1, err, __FILE__, __LINE__, "Unable to initialize mole display condition %d.",idx);
                        }
                        if ((err = pthread_cond_init(&molecomm[idx].keycond, NULL)) != 0) {
                            restore_terminal();
                            error_at_line(-1, err, __FILE__, __LINE__, "Unable to initialize mole key condition %d.",idx);
                        }

                        if ((err = pthread_create(&molecomm[idx].thread, NULL, mole_thread, p)) != 0) {
                            restore_terminal();
                            error_at_line(-1, err, __FILE__, __LINE__, "Unable to create mole thread %d.", idx);
                        }

                        ++molesstarted;
                    }
                } break;

                case COMPLETE: { // mole thread ready to be joined
                    void *retval;
                    if ((err = pthread_join(p->thread, &retval)) != 0) { // join mole thread
                        restore_terminal();
                        error_at_line(-1, err, __FILE__, __LINE__, "Unable to join mole thread %d. Error=%d", idx, err);
                    }

                    set_mole_status(p, AVAILABLE);
                    ++molescompleted;
                } break;

                default: {
                    // intentionally left empty
                };
            }
        }

        if (++idx == CONCURRENTMOLES) {
            idx = 0;
            struct timespec sleeptime = {0, 100000000L}; // 100 msec sleep so we
            nanosleep(&sleeptime, NULL);                 // don't burn up the cpu
        }
    }
}

//==============================
// void intro_splashscreen(void)
//
// Part of the instructions presented by display_intro()
//
void intro_splashscreen(void) {
    struct AnimationSpec anim[MOLEHOLES];
    pthread_t animthread[MOLEHOLES];
    display_empty_playfield(BASEGAME, DISP_ELE_HOLES, MOLEHOLES, NULL);

    int linenum = 3;
    const int startcol = 43;
    lock_ncurses();
    mvprintw(++linenum,startcol,"        Whack-A-Mole %s\n", VERSTRING);
    refresh();
    unlock_ncurses();

    int i;
    for (i=0; i<MOLEHOLES; i++) {
        anim[i] = PopupSplash;
        anim[i].hole = i;

        int err;
        if ((err = pthread_create(&animthread[i], NULL, animation_thread, &anim[i])) != 0) {
            restore_terminal();
            error_at_line(-1, err, __FILE__, __LINE__, "Unable to create animation thread %d.", i);
        }

        struct timespec sleeptime = {0, 150000000L}; // 150 msec sleep 
        nanosleep(&sleeptime, NULL);
    }

    for (i=0; i<MOLEHOLES; i++) {
        int err;
        void *retval;
        if ((err = pthread_join(animthread[i], &retval)) != 0) { // Join animation thread
            restore_terminal();
            error_at_line(-1, err, __FILE__, __LINE__, "Unable to join animation thread %d Error=%d.", i,err);
        }
    }

    lock_ncurses();
    linenum += 2;
    mvprintw(++linenum,startcol,"   A Linux / ncurses implementation  ");
    mvprintw(++linenum,startcol,"   of the classic electromechanical  ");
    mvprintw(++linenum,startcol,"   arcade game, using POSIX threads. ");
    linenum += 2;
    mvprintw(++linenum,startcol,"         ==================          ");
    mvprintw(++linenum,startcol,"         Please select one:          ");
    ++linenum;
    mvprintw(++linenum,startcol,"           I)nstructions             ");
    mvprintw(++linenum,startcol,"           P)lay                     ");
    ++linenum;
    mvprintw(++linenum,startcol,"         ==================          ");
    refresh();
    unlock_ncurses();
}

//============================
//int intro_overview(int page)
//
// Part of the instructions presented by display_intro()
//
int intro_overview(int page) {
    lock_ncurses();
    clear();
    int linenum = 0;
    const int startcol = 22;
    mvprintw(linenum,0,"Whack-A-Mole %s", VERSTRING);
    mvprintw(0, 80 - 18,"[Instructions %d/%d]", page+1, sizeof(intropages) / sizeof(int(*)()));
    linenum +=2;
    mvprintw(++linenum,startcol,"              OVERVIEW               ");
    mvprintw(++linenum,startcol,"                                     ");
    mvprintw(++linenum,startcol,"   Score points by whacking the      ");
    mvprintw(++linenum,startcol,"   moles when they pop up in the     ");
    mvprintw(++linenum,startcol,"   holes.                            ");
    mvprintw(++linenum,startcol,"                                     ");
    mvprintw(++linenum,startcol,"   A penalty score is assessed for   ");
    mvprintw(++linenum,startcol,"   any missed moles.                 ");
    mvprintw(++linenum,startcol,"                                     ");
    mvprintw(++linenum,startcol,"   Up to %d moles may be active at   ", CONCURRENTMOLES);
    mvprintw(++linenum,startcol,"   the same time.                    ");
    linenum += 2;
    mvprintw(++linenum,startcol,"   ===============================   ");
    mvprintw(++linenum,startcol,"        Options: (N)ext pg,          ");
    mvprintw(++linenum,startcol,"                 (S)tart game        ");
    mvprintw(++linenum,startcol,"   ===============================   ");
    refresh();
    unlock_ncurses();

    return linenum;
}

//=============================
//int intro_playfield(int page)
//
// Part of the instructions presented by display_intro()
//
int intro_playfield(int page) {
    lock_ncurses();
    clear();
    display_empty_playfield(BASEGAME, DISP_ELE_HOLES | DISP_ELE_KEYS, MOLEHOLES, NULL);
    mvprintw(0,0,"Whack-A-Mole %s", VERSTRING);
    mvprintw(0, 80 - 18,"[Instructions %d/%d]", page+1, sizeof(intropages) / sizeof(int(*)()));
    int linenum = 0;
    const int startcol = 43;
    linenum +=2;
    mvprintw(++linenum,startcol,"              PLAYFIELD              ");
    mvprintw(++linenum,startcol,"                                     ");
    mvprintw(++linenum,startcol,"   This is the playfield for the     ");
    mvprintw(++linenum,startcol,"   game.                             ");
    ++linenum;
    mvprintw(++linenum,startcol,"   The key assigned to each hole is  ");
    mvprintw(++linenum,startcol,"   displayed to the upper right of   ");
    mvprintw(++linenum,startcol,"   the hole.                         ");
    ++linenum;
    mvprintw(++linenum,startcol,"   Press that key to swing your      ");
    mvprintw(++linenum,startcol,"   virtual hammer at the hole.       ");
    ++linenum;
    mvprintw(++linenum,startcol,"   HINT: Make sure numlock is on.    ");
    linenum += 2;
    mvprintw(++linenum,startcol,"   ==============================    ");
    mvprintw(++linenum,startcol,"   Options: (N)ext pg, (P)rev pg,    ");
    mvprintw(++linenum,startcol,"            (S)tart game             ");
    mvprintw(++linenum,startcol,"   ==============================    ");
    refresh();
    unlock_ncurses();

    return linenum;
}

//===============================
//int intro_hidingmoles(int page)
//
// Part of the instructions presented by display_intro()
//
int intro_hidingmoles(int page) {
    lock_ncurses();
    clear();
    display_empty_playfield(BASEGAME, DISP_ELE_HOLES | DISP_ELE_KEYS, MOLEHOLES, NULL);
    mvprintw(0,0,"Whack-A-Mole %s", VERSTRING);
    mvprintw(0, 80 - 18,"[Instructions %d/%d]", page+1, sizeof(intropages) / sizeof(int(*)()));
    int linenum = 0;
    const int startcol = 43;
    linenum +=2;
    mvprintw(++linenum,startcol,"              GAMEPLAY               ");
    mvprintw(++linenum,startcol,"            Hiding Moles             ");
    mvprintw(++linenum,startcol,"                                     ");
    mvprintw(++linenum,startcol,"   Each mole starts out by choosing  ");
    mvprintw(++linenum,startcol,"   a hole and hiding.  Look closely, ");
    mvprintw(++linenum,startcol,"   and you can occasionally see the  ");
    mvprintw(++linenum,startcol,"   mole's ears in the hole.          ");
    mvprintw(++linenum,startcol,"                                     ");
    mvprintw(++linenum,startcol,"   You CANNOT whack a mole while it  ");
    mvprintw(++linenum,startcol,"   is hiding.  You must wait for it  ");
    mvprintw(++linenum,startcol,"   to pop up.                        ");
    mvprintw(++linenum,startcol,"                                     ");
    mvprintw(++linenum,startcol,"   Hole 5 on the left shows an       ");
    mvprintw(++linenum,startcol,"   example of a mole hiding.         ");
    linenum += 2;
    mvprintw(++linenum,startcol,"   ==============================    ");
    mvprintw(++linenum,startcol,"   Options: (N)ext pg, (P)rev pg,    ");
    mvprintw(++linenum,startcol,"            (S)tart game             ");
    mvprintw(++linenum,startcol,"   ==============================    ");
    refresh();
    unlock_ncurses();

    struct AnimationSpec anim = HidingAnim;
    pthread_t thread;
    anim.hole = 4;
    anim.duration = -1; //run until cancelled
    int err;
    if ((err = pthread_create(&thread, NULL, animation_thread, &anim)) != 0) {
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to create animation thread.");
    }

    // now wait for key to be hit
    const struct timeval onesecond = {1, 0L};
    struct timeval waittime;
    fd_set stdin_fd;
    do {
        waittime = onesecond;
        FD_ZERO(&stdin_fd);
        FD_SET(STDIN_FILENO, &stdin_fd);
    } while (0 == select(STDIN_FILENO + 1, &stdin_fd, NULL, NULL, &waittime)); //wait for keypress

    // Now kill the animation thread and join it
    if ((err = pthread_cancel(thread)) != 0) {
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to cancel animation thread.");
    }
    void *retval;
    if ((err = pthread_join(thread, &retval)) != 0) { 
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to join animation thread. Error=%d.", err);
    }

    return linenum;
}

//=========================
//int intro_popup(int page)
//
// Part of the instructions presented by display_intro()
//
int intro_popup(int page) {
    lock_ncurses();
    clear();
    display_empty_playfield(BASEGAME, DISP_ELE_HOLES | DISP_ELE_KEYS, MOLEHOLES, NULL);
    mvprintw(0,0,"Whack-A-Mole %s", VERSTRING);
    mvprintw(0, 80 - 18,"[Instructions %d/%d]", page+1, sizeof(intropages) / sizeof(int(*)()));
    int linenum = 0;
    const int startcol = 43;
    linenum +=2;
    mvprintw(++linenum,startcol,"              GAMEPLAY               ");
    mvprintw(++linenum,startcol,"           Popped Up Moles           ");
    mvprintw(++linenum,startcol,"                                     ");
    mvprintw(++linenum,startcol,"   When a mole is ready, it pops its ");
    mvprintw(++linenum,startcol,"   head up in the hole.              ");
    mvprintw(++linenum,startcol,"                                     ");
    mvprintw(++linenum,startcol,"   Press the key for that hole       ");
    mvprintw(++linenum,startcol,"   before the mole gets away.        ");
    mvprintw(++linenum,startcol,"                                     ");
    mvprintw(++linenum,startcol,"   Hole 5 on the left shows a mole   ");
    mvprintw(++linenum,startcol,"   popping up and getting away.      ");
    linenum += 2;
    mvprintw(++linenum,startcol,"   ==============================    ");
    mvprintw(++linenum,startcol,"   Options: (N)ext pg, (P)rev pg,    ");
    mvprintw(++linenum,startcol,"            (S)tart game             ");
    mvprintw(++linenum,startcol,"   ==============================    ");
    refresh();
    unlock_ncurses();

    struct AnimationSpec anim = PopupInstr;
    pthread_t thread;
    anim.hole = 4;
    anim.duration = 3000;
    int err;
    if ((err = pthread_create(&thread, NULL, animation_thread, &anim)) != 0) {
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to create animation thread.");
    }

    // now wait for key to be hit
    const struct timeval onesecond = {1, 0L};
    struct timeval waittime;
    fd_set stdin_fd;
    do {
        waittime = onesecond;
        FD_ZERO(&stdin_fd);
        FD_SET(STDIN_FILENO, &stdin_fd);
    } while (0 == select(STDIN_FILENO + 1, &stdin_fd, NULL, NULL, &waittime)); //wait for keypress

    // Now kill the animation thread and join it
    if ((err = pthread_cancel(thread)) != 0) {
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to cancel animation thread.");
    }
    void *retval;
    if ((err = pthread_join(thread, &retval)) != 0) { 
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to join animation thread. Error=%d.", err);
    }

    return linenum;
}

//===============================
//int intro_playresults(int page)
//
// Part of the instructions presented by display_intro()
//
int intro_playresults(int page) {
    lock_ncurses();
    clear();
    int linenum = 0;
    const int startcol = 0;
    mvprintw(linenum,0,"Whack-A-Mole %s", VERSTRING);
    mvprintw(0, 80 - 18,"[Instructions %d/%d]", page+1, sizeof(intropages) / sizeof(int(*)()));
    ++linenum;
    mvprintw(++linenum,startcol,"                                                                 ________");
    mvprintw(++linenum,startcol,"                                 +----------------------------- /%8.8s\\", asciiwhack[0]);
    mvprintw(++linenum,startcol,"           PLAY RESULTS          | If all goes well and you    / %8.8s \\", asciiwhack[1]);
    mvprintw(++linenum,startcol,"                                 | press the correct key in    | %8.8s |", asciiwhack[2]);
    mvprintw(++linenum,startcol,"    ________                     | time, you WHACK the mole.   | %8.8s |", asciiwhack[3]);
    mvprintw(++linenum,startcol,"   /%8.8s\\ -------------------+------------+--------------- \\ %8.8s /", asciiescape[0], asciiwhack[4]);
    mvprintw(++linenum,startcol,"  / %8.8s \\   If you're too slow, the mole |                 \\________/", asciiescape[1]);
    mvprintw(++linenum,startcol,"  | %8.8s |   will ESCAPE and disappear in |                 ", asciiescape[2]);
    mvprintw(++linenum,startcol,"  | %8.8s |   a \"poof\" of dust.            |                  ________", asciiescape[3]);
    mvprintw(++linenum,startcol,"  \\ %8.8s / -----------------+-------------+---------------- /%8.8s\\", asciiescape[4], asciimisfire[0]);
    mvprintw(++linenum,startcol,"   \\________/                   | If you have BAD AIM and      / %8.8s \\", asciimisfire[1]);
    mvprintw(++linenum,startcol,"                                | hit the wrong key, or you    | %8.8s |", asciimisfire[2]);
    mvprintw(++linenum,startcol,"                                | swing TOO SOON, the hammer   | %8.8s |", asciimisfire[3]);
    mvprintw(++linenum,startcol,"    ________                    | will slam into the ground.   \\ %8.8s /",asciimisfire[4]);
    mvprintw(++linenum,startcol,"   /        \\ ------------------+-----------------------+------ \\________/");
    mvprintw(++linenum,startcol,"  /          \\   When the hammer slams the ground, all  |");
    mvprintw(++linenum,startcol,"  |          |   moles that are up or hiding are SCARED |");
    mvprintw(++linenum,startcol,"  |          |   OFF and can no longer be whacked.      |");
    mvprintw(++linenum,startcol,"  \\          / -----------------------------------------+");
    mvprintw(++linenum,startcol,"   \\________/");
    mvprintw(++linenum,startcol,"                   ===========================================");
    mvprintw(++linenum,startcol,"                   Options: (N)ext pg, (P)rev pg, (S)tart game");
    mvprintw(++linenum,startcol,"                   ===========================================");
    refresh();
    unlock_ncurses();

    struct AnimationSpec anim = ScaredInstr;
    pthread_t thread;
    anim.hole = 6;
    anim.duration = 3000;
    int err;
    if ((err = pthread_create(&thread, NULL, animation_thread, &anim)) != 0) {
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to create animation thread.");
    }

    // now wait for key to be hit
    const struct timeval onesecond = {1, 0L};
    struct timeval waittime;
    fd_set stdin_fd;
    do {
        waittime = onesecond;
        FD_ZERO(&stdin_fd);
        FD_SET(STDIN_FILENO, &stdin_fd);
    } while (0 == select(STDIN_FILENO + 1, &stdin_fd, NULL, NULL, &waittime)); //wait for keypress

    // Now kill the animation thread and join it
    if ((err = pthread_cancel(thread)) != 0) {
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to cancel animation thread.");
    }
    void *retval;
    if ((err = pthread_join(thread, &retval)) != 0) { 
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to join animation thread. Error=%d.", err);
    }

    return linenum;
}

//===========================
//int intro_scoring(int page)
//
// Part of the instructions presented by display_intro()
//
int intro_scoring(int page) {
    lock_ncurses();
    clear();
    int linenum = 0;
    const int startcol = 0;
    mvprintw(linenum,0,"Whack-A-Mole %s", VERSTRING);
    mvprintw(0, 80 - 18,"[Instructions %d/%d]", page+1, sizeof(intropages) / sizeof(int(*)()));
    ++linenum;
    mvprintw(++linenum,startcol,"                                    SCORING");
    ++linenum;
    mvprintw(++linenum,startcol,"              Each successfully whacked mole earns you %d points. ", WHACKEDMOLESCORE);
    mvprintw(++linenum,startcol,"                                                                                ");
    mvprintw(++linenum,startcol,"              You can also earn a speed or skill bonus.  Whack the mole");
    mvprintw(++linenum,startcol,"              at the follwing times to earn one of these bonuses.");
    mvprintw(++linenum,startcol,"  +--------------+-----------------------------+-----------------------------+  ");
    mvprintw(++linenum,startcol,"  |   Lightning  |            Meh...           |       Nerves of steel.      |  ");
    mvprintw(++linenum,startcol,"  |   Reflexes.  |     Thanks for playing.     |   (Push it to the limit!)   |  ");
    mvprintw(++linenum,startcol,"  |   ________   |   ________       ________   |   ________       ________   |  ");
    mvprintw(++linenum,startcol,"  |  /%8.8s\\  |  /        \\     /        \\  |  /        \\     /        \\  |  ", asciimole[0]);
    mvprintw(++linenum,startcol,"  | / %8.8s \\ | / %8.8s \\   /          \\ | /          \\   /          \\ |  ", asciimole[1], asciimole[0]);
    mvprintw(++linenum,startcol,"  | | %8.8s | | | %8.8s |   | %8.8s | | |          |   |          | |  ", asciimole[2], asciimole[1], asciimole[0]);
    mvprintw(++linenum,startcol,"  | | %8.8s | | | %8.8s |   | %8.8s | | | %8.8s |   |          | |  ", asciimole[3], asciimole[2], asciimole[1], asciimole[0]);
    mvprintw(++linenum,startcol,"  | \\ %8.8s / | \\ %8.8s /   \\ %8.8s / | \\ %8.8s /   \\ %8.8s / |  ", asciimole[4], asciimole[3], asciimole[2], asciimole[1], asciimole[0]);
    mvprintw(++linenum,startcol,"  |  \\________/  |  \\________/     \\________/  |  \\________/     \\________/  |  ");
    mvprintw(++linenum,startcol,"  |              |                             |                             |  ");
    mvprintw(++linenum,startcol,"  |   Bonus: %-2d  |   Bonus: %-2d      Bonus: %-2d  |   Bonus: %-2d      Bonus:%-2d   |  ", BONUSPOINTS[0], BONUSPOINTS[1], BONUSPOINTS[2], BONUSPOINTS[3], BONUSPOINTS[4]);
    mvprintw(++linenum,startcol,"  +--------------+-----------------------------+-----------------------------+  ");
    ++linenum;
    mvprintw(++linenum,startcol,"                   ===========================================");
    mvprintw(++linenum,startcol,"                   Options: (N)ext pg, (P)rev pg, (S)tart game");
    mvprintw(++linenum,startcol,"                   ===========================================");
    refresh();
    unlock_ncurses();

    return linenum;
}

//=============================
//int intro_penalties(int page)
//
// Part of the instructions presented by display_intro()
//
int intro_penalties(int page) {
    lock_ncurses();
    clear();
    int linenum = 0;
    const int startcol = 0;
    mvprintw(linenum,0,"Whack-A-Mole %s", VERSTRING);
    mvprintw(0, 80 - 18,"[Instructions %d/%d]", page+1, sizeof(intropages) / sizeof(int(*)()));
    ++linenum;
    mvprintw(++linenum,startcol,"                                   PENALTIES");
    ++linenum;
    mvprintw(++linenum,startcol,"             You will be assessed a penalty for each mole that escapes.");
    ++linenum;
    mvprintw(++linenum,startcol,"             The first mole to escape will cost you a %d point penalty.", abs(MISSEDMOLESCORE));
    ++linenum;
    mvprintw(++linenum,startcol,"             Each additional escaped mole costs another penalty AND ");
    mvprintw(++linenum,startcol,"             increases the size of the penalty by %d points.  (So the ", abs(MISSEDMOLESCORE) * MISSEDMOLEMULTIPLIER);
    mvprintw(++linenum,startcol,"             first costs you %d points, the second costs %d, third", abs(MISSEDMOLESCORE) * MISSEDMOLEMULTIPLIER, abs(MISSEDMOLESCORE) * MISSEDMOLEMULTIPLIER * 2);
    mvprintw(++linenum,startcol,"             costs %d, etc.)", abs(MISSEDMOLESCORE) * MISSEDMOLEMULTIPLIER * 3);
    ++linenum;
    mvprintw(++linenum,startcol,"             The size of the penalty is capped at %d points.  A penalty", abs(MISSEDMOLECAP));
    mvprintw(++linenum,startcol,"             will never make your accumulated score go below 0.");
    ++linenum;
    mvprintw(++linenum,startcol,"             Scared moles (caused by your hammer slamming the ground)");
    mvprintw(++linenum,startcol,"             count as missed, and recieve all escaped-mole penalties.");
    ++linenum;
    mvprintw(++linenum,startcol,"                   ===========================================");
    mvprintw(++linenum,startcol,"                   Options: (N)ext pg, (P)rev pg, (S)tart game");
    mvprintw(++linenum,startcol,"                   ===========================================");
    refresh();
    unlock_ncurses();

    return linenum;
}

//==============================
//int intro_scoresheet(int page)
//
// Part of the instructions presented by display_intro()
//
int intro_scoresheet(int page) {
    lock_ncurses();
    clear();
    int linenum = 0;
    const int startcol = 0;
    mvprintw(linenum,0,"Whack-A-Mole %s", VERSTRING);
    mvprintw(0, 80 - 18,"[Instructions %d/%d]", page+1, sizeof(intropages) / sizeof(int(*)()));
    ++linenum;
    mvprintw(++linenum,startcol,"                                   SCORE SHEET");
    ++linenum;
    mvprintw(++linenum,startcol,"           When you finish the game, you will see a score sheet with the");
    mvprintw(++linenum,startcol,"           details of your game.  The score sheet events are:");
    ++linenum;
    mvprintw(++linenum,startcol,"           Whacked Mole!     - Success!  You whacked a mole, earned a");
    mvprintw(++linenum,startcol,"                               score, and possibly a bonus.");
    ++linenum;
    mvprintw(++linenum,startcol,"           Mole Escaped      - The mole got away, costing you a penalty.");
    ++linenum;
    mvprintw(++linenum,startcol,"           Bad Aim           - You hit a hole with no mole present.");
    ++linenum;
    mvprintw(++linenum,startcol,"           Hit Too Soon      - You hit a hole when the mole was still");
    mvprintw(++linenum,startcol,"                               hiding.");
    ++linenum;
    mvprintw(++linenum,startcol,"           Mole Scared Away  - This mole was scared away by the \"Bad Aim\"");
    mvprintw(++linenum,startcol,"                               or \"Hit Too Soon\" event above. (Costing you");
    mvprintw(++linenum,startcol,"                               an escaped-mole penalty)");
    ++linenum;
    mvprintw(++linenum,startcol,"                         ================================");
    mvprintw(++linenum,startcol,"                         Options: (P)rev pg, (S)tart game");
    mvprintw(++linenum,startcol,"                         ================================");
    refresh();
    unlock_ncurses();

    return linenum;
}

//=========================
// void display_intro(int moles, int gametime)
//
// Shows introduction, rules, etc.
//
// Locks curses_mutex out of an abundance of caution. Display_thread is not
// even running yet, when this function ir called.  Locking the mutex is just
// done to prevent problems if future updates change this. 
void display_intro(int moles, int gametime) {
    char key;

    intro_splashscreen();
    int playselected = 0;
    while (! playselected) {
        clear_input_buffer();
        key = waitforkey(NULL);
        switch(toupper(key)) {
            case 'I': {
                int page = 0;
                while (! playselected) {
                    (*intropages[page])(page);
                    key = waitforkey(NULL);
                    switch (toupper(key)) {
                        case 'N': { // Next page
                            if (page < sizeof(intropages) / sizeof(int(*)()) -1) {
                                ++page;
                            }
                        } break;

                        case 'P': { // Prev page
                            if (page > 0) {
                                --page;
                            }
                        } break;

                        case 'S': { // Start Game
                            playselected = 1; 
                        } break;
                    }
                }
            } break;

            case 'P': {
                playselected = 1; // Start the game
            } break;
        }
    }
    display_countdown();
}

//============================
//void display_countdown(void)
//
// Displays a brief countdown so player can get ready.
//
//
void display_countdown() {
    int row = 8;
    int col = 32;
    struct timespec sleeptime = {0, 300000000L}; // 300 msec sleep 
    lock_ncurses();
    clear();
    mvprintw(row, col, "===============");
    mvprintw(row+1, col, "GAME STARTS IN:");
    mvprintw(row+5, col, "===============");
    refresh();
    unlock_ncurses();

    int i;
    for (i=5; i>0; i--) {
        lock_ncurses();
        mvprintw(row+2,col+5,"+---+");
        mvprintw(row+3,col+5,"| %d |", i);
        mvprintw(row+4,col+5,"+---+");
        refresh();
        unlock_ncurses();
        nanosleep(&sleeptime, NULL);
        lock_ncurses();
        mvprintw(row+2,col+5,"     ");
        mvprintw(row+3,col+5,"  %d  ", i);
        mvprintw(row+4,col+5,"     ");
        refresh();
        unlock_ncurses();
        nanosleep(&sleeptime, NULL);
    }
 }

//===========================
//void display_gameover(void)
//
// Displays the GAME OVER message
//
//
void display_gameover() {
    int row = 13;
    int col = 53;

    lock_ncurses();
    mvprintw(row, col, "===============");
    mvprintw(row+1, col, "   GAME OVER");
    mvprintw(row+2, col, "===============");
    mvprintw(row+3, col, " Press any key");
    refresh();
    unlock_ncurses();

    const struct timeval halfsecond = {0, 500000L};
    struct timeval waittime;
    fd_set stdin_fd;
    int i = 0;
    do {
        lock_ncurses();
        if (i<11) {
            if (i % 2 == 0) {
                mvprintw(row, col, "===============");
                mvprintw(row+1, col, "   GAME OVER");
                mvprintw(row+2, col, "===============");
            } else {
                mvprintw(row, col, "               ");
                mvprintw(row+1, col, "            ");
                mvprintw(row+2, col, "               ");
            }
        } else {
            if (i % 2 == 0) {
                mvprintw(row+3, col, " Press any key");
            } else {
                mvprintw(row+3, col, "              ");
            }
        }
        refresh();
        unlock_ncurses();

        ++i;
        waittime = halfsecond;
        FD_ZERO(&stdin_fd);
        FD_SET(STDIN_FILENO, &stdin_fd);
    } while (0 == select(STDIN_FILENO + 1, &stdin_fd, NULL, NULL, &waittime)); //wait for keypress

    clear_input_buffer();
 }

//=================================================================
// void display_score_sheet(int gamescore, int moles, int gametime)
//
// gamescore = total score for entire game
//
// Shows game results and any other closing thoughts.
// 
// Uses ncurses for navigable paginated display.
// Locks curses_mutex out of an abundance of caution. Display_thread should
// be dead (or at least no longer actively outputting) when this is called.
// Locking the mutex is just done to prevent problems if future updates change
// that behavior.
//
#define EXTRALINES      12  // For headers and footers in scoresheet pad
#define DATALINESTART   9   // Number of lines in page heading
void display_score_sheet (int gamescore, int moles, int gametime){
    int i;
    int molenum = 1;

    // Mole numbers in scores[] were assigned at thread creation, which may result in
    // them being out of numerical order within the array.  Here, we reassign them
    // so as not to confuse the player.
    for (i=0; i<numscores; i++) {
        if (scores[i].mole > 0) {
            scores[i].mole = molenum++;
        }
    }

    // Static Display
    lock_ncurses();
    clear();
    mvprintw(0,0,"===================");
    mvprintw(1,0,"Your score was %d", gamescore);
    mvprintw(2,0,moles == 1 ? "for 1 mole" : "for %d moles",moles);
    mvprintw(3,0,"===================");
    mvprintw(4,35, "Score Sheet:");
    mvprintw(6,0,"\t\t\t\t\t\t\tBonus\tRunning");
    mvprintw(7,0,"\tMole\tHole\tEvent\t\t\tScore\tScore\tTotal");
    mvprintw(1,27,"Thank you for playing Whack-A-Mole %s", VERSTRING);

    // Paginated score display
    int pagesize = (LINES - EXTRALINES);
    int pages = (numscores + (pagesize -1))/ (LINES - EXTRALINES);
    int currentpage = 0;
    char cmd = '1';

    while(toupper(cmd) != 'Q') {
        int startat = currentpage * pagesize;
        move(DATALINESTART, 0);
        clrtobot();
        unlock_ncurses();  // just to maintain mutex lock order
        lock_scores();
        lock_ncurses();
        int linenum;
        for (i=startat, linenum=DATALINESTART; i<numscores && i<startat+pagesize; i++, linenum++) {
            struct ScoreSheetRecord *p = &scores[i];
            mvprintw(linenum, 0, p->mole <= 0 ? "\t\t" : "\t%d\t",p->mole);
            printw(p->hole == -1 ? "\t" : "%c\t",holekeys[p->hole]);
            printw(p->playresult == WHACK ? "Whacked Mole!\t\t" : p->playresult == ESCAPE ? "Mole Escaped\t\t" : p->playresult == MISFIRE ? "Bad Aim\t\t\t" : p->playresult == TOOSOON ? "Hit Too Soon\t\t" : "Mole Scared Away\t");
            printw(p->missedscore + p->whackedscore + p->penaltyscore == 0 ? "\t" : "% 3d\t", p->missedscore + p->whackedscore + p->penaltyscore);
            //
            // only display bonus if mole was whacked (bonus could be 0)
            printw(p->bonusscore == 0 ? "\t" : "%d\t", p->bonusscore);
            printw("%d", p->startscore + p->missedscore + p->whackedscore + p->bonusscore + p->penaltyscore);
        }
        unlock_scores();

        if (pages > 1) {
            mvprintw(LINES-1,0,"[Page %d/%d]\tCommand: (Q)uit, (1)st pg, (P)rev pg, (N)ext pg, (L)ast pg.", currentpage+1, pages);
        } else {
            mvprintw(LINES-1,0,"Press Q to quit.");
        }
        refresh();
        unlock_ncurses();
        cmd = toupper(waitforkey(NULL));
        lock_ncurses();
        switch (cmd) {
            case '1': {
               currentpage = 0; 
            } break;

            case 'P': {
                if (--currentpage < 0) currentpage = 0;
            } break;

            case 'N': {
                if (++currentpage >= pages) currentpage = pages - 1;
            } break;

            case 'L': {
                currentpage = pages - 1;
            } break;
        }
    }
    unlock_ncurses();
}

//==================================================
// void show_mole(int hole, int maxholes, int level)
//
// Displays one mole within one hole at a specified level.
// Doesn't actually call refresh() to draw screen.
//
// hole: Hole number to show mole in (zero based)
//
// maxholes: Determines hole geometry. As of V1.0, this must be MOLEHOLES
//
// level: 0 = No mole
//        1-4 = Partial moles
//        5 = Full mole
//
// This function does NOT lock the curses_mutex because this is a low level
// function and will have no way of knowing which other mutex locks may
// be in effect. That would be bad, since this program relies on mutexes 
// being locked in a defined order to prevent deadlocks.
// The calling function definitely should have a lock in effect when
// it calls this function.
//
const struct HoleScreenCoords {
    int top[5];     // Top row for moles at level 1-5
    int height[5];  // Height for moles at level 1-5
    int left;       // Column of left side of mole
} holescreencoords[MOLEHOLES] = {{{6,5,4,3,2},{1,2,3,4,5},4},
                         {{6,5,4,3,2},{1,2,3,4,5},18},
                         {{6,5,4,3,2},{1,2,3,4,5},32},
                         {{13,12,11,10,9},{1,2,3,4,5},4},
                         {{13,12,11,10,9},{1,2,3,4,5},18},
                         {{13,12,11,10,9},{1,2,3,4,5},32},
                         {{20,19,18,17,16},{1,2,3,4,5},4},
                         {{20,19,18,17,16},{1,2,3,4,5},18},
                         {{20,19,18,17,16},{1,2,3,4,5},32}};

int moleheight = sizeof(asciimole) / sizeof(char*); // Lines per mole

void show_mole(int hole, int maxholes, int level) {
    struct HoleScreenCoords *hsc = (struct HoleScreenCoords *)&holescreencoords[hole];

    if (maxholes == MOLEHOLES) { // currently, MOLEHOLES is the only valid choice
        int i;
        // First for loop blanks hole
        for (i=0; i < moleheight; i++) {
            mvprintw(hsc->top[moleheight - 1] + i, hsc->left, "        ");
        }
        // level zero = clear hole, so don't paint mole
        if (level > 0) {
            // Second for loop paints mole
            for (i=0; i < hsc->height[level-1]; i++) {
                mvprintw(hsc->top[level-1] + i, hsc->left, asciimole[i]);
            }
        }
        refresh();
    } else {
        restore_terminal();
        error_at_line(-1, 0, __FILE__, __LINE__, "Unsupported number of mole holes (%d).", maxholes);
    }
}

//=========================================================================================
// void show_result(int hole, int maxholes, enum PlayResult result, int score1, int score2, char *txt)
//
// Shows play result on playfield in place of mole.  Related to show_mole() function
// above, and re-uses its holescreencoords data for screen positioning.
//
// hole: Hole number to show result in (zero based)
//
// maxholes: Determines hole geometry. As of V1.0, this must be MOLEHOLES
//
// result: Game play result: WHACK, ESCAPE, MISFIRE, TOOSOON, SCAREDOFF (or -1 to blank and display txt)
//
// score1: Score for WHACK, penalty for ESCAPE.  If score1 is zero,
//         the ascii graphic will be displayed. Otherwise, the score graphic
//         (see char *[]'s below)
//
// score2: Bonus score for WHACK, ignored for others
//
// txt: Text msg to display when PlayResult == -1
//
// This function does NOT lock the curses_mutex because this is a low level
// function and will have no way of knowing which other mutex locks may
// be in effect. That would be bad, since this program relies on mutexes 
// being locked in a defined order to prevent deadlocks.
// The calling function definitely should have a lock in effect when
// it calls this function.
//
char scorewhackpat[5][9] = {"        ",
                           " WHACK! ",
                           "        ",
                           "Score:#1",     // #1 is macro that expands to score1
                           "Bonus:#2" };   // #2 is macro that expands to score2
char scorewhackbuf[5][9];
char *scorewhack[]={(char *)&scorewhackbuf[0],(char *)&scorewhackbuf[1],(char *)&scorewhackbuf[2],(char *)&scorewhackbuf[3],(char *)&scorewhackbuf[4]};

char scoreescapebuf[5][9];
char scoreescapepat[5][9] ={"        ",
                           " ESCAPE ",
                           "        ",      
                           " Score  ",
                           "  #1    " };   // #1 is macro that expands to score1
char *scoreescape[]={(char *)&scoreescapebuf[0],(char *)&scoreescapebuf[1],(char *)&scoreescapebuf[2],(char *)&scoreescapebuf[3],(char *)&scoreescapebuf[4]};

char asciiblankbuf[5][9] = {  "        ",
                           "        ",
                           "        ",
                           "        ",
                           "        " };
char *asciiblank[]={(char *)&asciiblankbuf[0],(char *)&asciiblankbuf[1],(char *)&asciiblankbuf[2],(char *)&asciiblankbuf[3],(char *)&asciiblankbuf[4]};

void show_result(int hole, int maxholes, enum PlayResult result, int score1, int score2, char *txt) {
    struct HoleScreenCoords *hsc = (struct HoleScreenCoords *)&holescreencoords[hole];
    char **ascii;
    int height;

    if (score1 < -99 || score1 > 99 || score2 <- 99 || score2 > 99) {
        restore_terminal();
        error_at_line(-1, 0, __FILE__, __LINE__, "Score (%d/%d) outside range.", score1, score2);
    }

    if (maxholes == MOLEHOLES) { // currently, MOLEHOLES is the only valid choice
        switch (result) {
            case WHACK: {
                if (score1 == 0) {
                    ascii = asciiwhack;
                    height = sizeof(asciiwhack) / sizeof(char*); // Lines in ascii graphic
                } else {
                    int i;
                    ascii = (char **)scorewhack;
                    height = sizeof(scorewhack)/sizeof(scorewhack[0]);// Lines in ascii graphic

                    memcpy(scorewhackbuf, scorewhackpat, sizeof(scorewhackbuf));
                    for (i=0; i<height; i++) {
                        char *c;
                        c = strstr(scorewhack[i],"#1");
                        if (c != NULL) {
                            sprintf(c, "%d", score1);
                        }
                        c = strstr(scorewhack[i],"#2");
                        if (c != NULL) {
                            sprintf(c, "%d", score2);
                        }
                    }
                }
            break; }

            case ESCAPE: {
                if (score1 == 0) {
                    ascii = asciiescape;
                    height = sizeof(asciiescape) / sizeof(char*); // Lines in ascii graphic
                } else {
                    int i;
                    ascii = (char **)scoreescape;
                    height = sizeof(scoreescape)/sizeof(scoreescape[0]);// Lines in ascii graphic

                    memcpy(scoreescapebuf, scoreescapepat, sizeof(scoreescapebuf));
                    for (i=0; i<height; i++) {
                        char *c;
                        c = strstr(scoreescape[i],"#1");
                        if (c != NULL) {
                            sprintf(c, "%d ", score1);
                        }
                    }
                }
            break; }

            case MISFIRE: 
            case TOOSOON: {
                ascii = asciimisfire;
                height = sizeof(asciimisfire) / sizeof(char*); // Lines in ascii graphic
            break; }

            case SCAREDOFF: {
                ascii = asciiscared;
                height = sizeof(asciiscared) / sizeof(char*); // Lines in ascii graphic
            break; }

            default: {
                sprintf(asciiblankbuf[2], "%8.8s",txt);
                ascii = asciiblank;
                height = sizeof(asciiblank) / sizeof(char*); // Lines in ascii graphic
            } break;
        }

        int i;
        for (i=0; i < height; i++) {
            mvprintw(hsc->top[height - 1] + i, hsc->left, ascii[i]);
        }
        refresh();
    } else {
        restore_terminal();
        error_at_line(-1, 0, __FILE__, __LINE__, "Unsupported number of mole holes (%d).", maxholes);
    }
}

//=========================================================================================
// void display_empty_playfield(enum GameMode gamemode, int elements, int holes, char *msg)
//
// Shows the play field including holes, welcome message, score area, etc.
//
// gamemode: BASEGAME = Fixed number of moles
//           TIMEDGAME = Fixed Duration
//
// holes: As of V1.0, this must be MOLEHOLES
//
// msg: Welcome message
//
// This function does NOT lock the curses_mutex because this is a low level
// function and will have no way of knowing which other mutex locks may
// be in effect. That would be bad, since this program relies on mutexes 
// being locked in a defined order to prevent deadlocks.
// The calling function definitely should have a lock in effect when
// it calls this function.
//
void display_empty_playfield(enum GameMode gamemode, int elements, int holes, char *msg) {
    clear();
    if (elements & DISP_ELE_VERS) {
        mvprintw(0,0,"Whack-A-Mole %s ",VERSTRING);
    }

    if (holes != MOLEHOLES) {
        restore_terminal();
        error_at_line(-1, 0, __FILE__, __LINE__, "Unsupported number of mole holes (%d).", holes);
    }

    if (elements & DISP_ELE_HOLES) {
        mvprintw(1,2,"  ________      ________      ________   ");
        mvprintw(2,2,elements & DISP_ELE_KEYS ? " /        \\%c   /        \\%c   /        \\%c " : " /        \\    /        \\    /        \\  ",holekeys[0],holekeys[1],holekeys[2]);
        mvprintw(3,2,"/          \\  /          \\  /          \\ ");
        mvprintw(4,2,"|          |  |          |  |          | ");
        mvprintw(5,2,"|          |  |          |  |          | ");
        mvprintw(6,2,"\\          /  \\          /  \\          / ");
        mvprintw(7,2," \\________/    \\________/    \\________/  ");
        mvprintw(8,2,"  ________      ________      ________   ");
        mvprintw(9,2,elements & DISP_ELE_KEYS ? " /        \\%c   /        \\%c   /        \\%c " : " /        \\    /        \\    /        \\  ",holekeys[3],holekeys[4],holekeys[5]);
        mvprintw(10,2,"/          \\  /          \\  /          \\ ");
        mvprintw(11,2,"|          |  |          |  |          | ");
        mvprintw(12,2,"|          |  |          |  |          | ");
        mvprintw(13,2,"\\          /  \\          /  \\          / ");
        mvprintw(14,2," \\________/    \\________/    \\________/  ");
        mvprintw(15,2,"  ________      ________      ________   ");
        mvprintw(16,2,elements & DISP_ELE_KEYS ? " /        \\%c   /        \\%c   /        \\%c " : " /        \\    /        \\    /        \\  ",holekeys[6],holekeys[7],holekeys[8]);
        mvprintw(17,2,"/          \\  /          \\  /          \\ ");
        mvprintw(18,2,"|          |  |          |  |          | ");
        mvprintw(19,2,"|          |  |          |  |          | ");
        mvprintw(20,2,"\\          /  \\          /  \\          / ");
        mvprintw(21,2," \\________/    \\________/    \\________/  ");
    }

    if (elements & DISP_ELE_MSG && msg != NULL) {
        mvprintw(2,60-strlen(msg)/2,msg);
    }

    if (elements & DISP_ELE_STAT) {
        mvprintw(9,53,"===============");
        mvprintw(10,53,"   SCORE: %d", 0);
        mvprintw(11,53,"===============");

        if (gamemode == BASEGAME) {
            mvprintw(6,53,"   MOLES:   "); 
        } else {
            mvprintw(6,53,"   TIME:    "); 
            restore_terminal();
            error_at_line(-1, 0, __FILE__, __LINE__, "Unsupported game mode.");
        }
    }

    refresh();
}

//=================================
//void *animation_thread(void *arg)
//
// arg = pointer to AnimationSpec structure. (See struct tag for more info)
// 
// These animations are code driven. A data driven animation would be
// more flexible, but not worth it at this point as there are only two
// animations right now.
//
void *animation_thread(void *arg) {
    struct AnimationSpec *aspec = (struct AnimationSpec *)arg;
    struct timespec sleeptime = {0, 0}; 
 #if defined(debug) && defined(_GNU_SOURCE)
    pthread_setname_np(pthread_self(), "WAM-Animation");
 #endif

    switch (aspec->animationtype) {
        case ANIMHIDING: {
            // The mole is hiding...
            // Animation behavior: 1) 200msec up
            //                     2) 200msec 1/3 chance up, 2/3 chance down
            //                     3) 200msec 1/3 chance up, 2/3 chance down
            //                     (so either short, medium or long single pop
            //                     or double pop)
            //                     4) Random down time from 800 to 2000 msec
            //                     Above steps repead until duration expires.
            //                     A duration of -1 indicates "run forever".

 #if defined(debug) && defined(_GNU_SOURCE)
            pthread_setname_np(pthread_self(), "WAM-Anim-Hiding");
 #endif
            lock_molecomm();
            aspec->synccount = 1;  // Indicate animation running
            unlock_molecomm();

            int timeremaining = aspec->duration;
            while (timeremaining > 0 || aspec->duration == -1) {
                if (timeremaining < 600 && aspec->duration != -1) {  // < 600msec left?
                    sleeptime.tv_sec = 0;
                    sleeptime.tv_nsec = timeremaining * MSEC;
                    nanosleep(&sleeptime, NULL);
                    break;
                } else {
                    disable_thread_cancel(); // don't get cancelled while holding a lock
                    lock_ncurses();
                    show_mole(aspec->hole, aspec->numholes, 1); // show the ears
                    refresh();
                    unlock_ncurses();
                    enable_thread_cancel();
                    // Ears up for 200 msec
                    sleeptime.tv_sec = 0;
                    sleeptime.tv_nsec = 200*MSEC; 
                    nanosleep(&sleeptime, NULL);

                    disable_thread_cancel(); // don't get cancelled while holding a lock
                    lock_ncurses();
                    show_mole(aspec->hole, aspec->numholes, tsrandom()%3?0:1); // 1/3 chance for extended bounce
                    refresh();
                    unlock_ncurses();
                    enable_thread_cancel();
                    // Ears up for 200 msec
                    sleeptime.tv_sec = 0;
                    sleeptime.tv_nsec = 200*MSEC; 
                    nanosleep(&sleeptime, NULL);

                    disable_thread_cancel(); // don't get cancelled while holding a lock
                    lock_ncurses();
                    show_mole(aspec->hole, aspec->numholes, tsrandom()%3?0:1); // 1/3 chance for extended or double bounce
                    refresh();
                    unlock_ncurses();
                    enable_thread_cancel();
                    // Ears up for 200 msec
                    sleeptime.tv_sec = 0;
                    sleeptime.tv_nsec = 200*MSEC; 
                    nanosleep(&sleeptime, NULL);

                    timeremaining -= 600;
                }

                disable_thread_cancel(); // don't get cancelled while holding a lock
                lock_ncurses();
                show_mole(aspec->hole, aspec->numholes, 0); // blank hole
                refresh();
                unlock_ncurses();
                enable_thread_cancel();
                long targettime;
                targettime = tsrandom();
                targettime = targettime % 1200 + 800;
                if (targettime > timeremaining && aspec->duration != -1) {
                    targettime = timeremaining;
                }
                sleeptime.tv_sec = targettime / 1000; 
                sleeptime.tv_nsec = (targettime % 1000) * MSEC; 
                nanosleep(&sleeptime, NULL);
                timeremaining -= targettime;
            }
            lock_molecomm();
            aspec->synccount = 2;  // Indicate animation complete
            unlock_molecomm();
        } break;

        case ANIMPOPUP: 
        case INSTRPOPUP: 
        case SPLASHPOPUP: {
            // The mole is up!
            // Animation behavior: 1) Mole rises 5 steps. 30msec after each. (150msec total)
            //                        This counts as part of the initial stage when the
            //                        player is eligible for "lightning reflexes" bonus.
            //                     2) Mole stays up at level 5 for (duration/5) -150 msec.
            //                     3) Mole drops one level each (duration/5) msec.
            //
            //                     For SPLASHPOPUP, only part 1 is displayed.
            //                     For INSTRPOPUP, animation repeats until cancelled.

 #if defined(debug) && defined(_GNU_SOURCE)
            pthread_setname_np(pthread_self(), "WAM-Anim-PopUp");
 #endif
            do {    // INSTRPOPUP loops forever, others just once through
                int synccount = 0;
                disable_thread_cancel(); // don't get cancelled while holding a lock
                lock_molecomm();
                aspec->synccount = ++synccount;
                unlock_molecomm();
                enable_thread_cancel();
                sleeptime.tv_sec = 0;
                sleeptime.tv_nsec = 30*MSEC; 
                int i;
                for (i=1; i<=5; i++) {
                    disable_thread_cancel(); // don't get cancelled while holding a lock
                    lock_ncurses();
                    show_mole(aspec->hole, aspec->numholes, i); // Mole popping up
                    refresh();
                    unlock_ncurses();
                    enable_thread_cancel();
                    nanosleep(&sleeptime, NULL);
                }

                if (aspec->animationtype == ANIMPOPUP || aspec->animationtype == INSTRPOPUP) {
                    int leveltime = aspec->duration / 5; // amount of time to keep mole at each level
                    sleeptime.tv_sec = (leveltime - 150) / 1000;
                    sleeptime.tv_nsec = (leveltime - 150) % 1000 * 1000000L;

                    nanosleep(&sleeptime, NULL);

                    sleeptime.tv_sec = leveltime / 1000;
                    sleeptime.tv_nsec = (leveltime % 1000) * 1000000L;
                    for (i=4; i>=1; i--) {
                        disable_thread_cancel(); // don't get cancelled while holding a lock
                        lock_ncurses();
                        show_mole(aspec->hole, aspec->numholes, i); // Mole going back down
                        refresh();
                        unlock_ncurses();
                        lock_molecomm();
                        aspec->synccount = ++synccount; // synccounts 2-5
                        unlock_molecomm();
                        enable_thread_cancel();

                        nanosleep(&sleeptime, NULL);
                    }

                    disable_thread_cancel(); // don't get cancelled while holding a lock
                    lock_molecomm();
                    aspec->synccount = ++synccount; 
                    unlock_molecomm();
                    lock_ncurses();
                    show_mole(aspec->hole, aspec->numholes, 0); // Blank out the hole
                    unlock_ncurses();
                    enable_thread_cancel();

                    if (aspec->animationtype == INSTRPOPUP) {
                        sleeptime.tv_sec = 0;
                        sleeptime.tv_nsec = 500L * MSEC;
                        nanosleep(&sleeptime, NULL);
                    }
                }
            } while (aspec->animationtype == INSTRPOPUP);
        } break;

        case ANIMWHACKED: {
 #if defined(debug) && defined(_GNU_SOURCE)
            pthread_setname_np(pthread_self(), "WAM-Anim-Whack");
 #endif
            lock_molecomm();
            aspec->synccount = 1;  // Indicate animation running
            unlock_molecomm();
            // Frame 1
            const int frame1time = 500; //msec
            sleeptime.tv_sec = 0;
            sleeptime.tv_nsec = frame1time * MSEC; 
            disable_thread_cancel(); // don't get cancelled while holding a lock
            lock_ncurses();

            show_result(aspec->hole, aspec->numholes, WHACK, 0, 0, NULL);
            refresh();
            unlock_ncurses();
            enable_thread_cancel();
            nanosleep(&sleeptime, NULL);

            // Frame 2
            sleeptime.tv_sec = (int)((aspec->duration - frame1time) / 1000L);
            sleeptime.tv_nsec = (long)((aspec->duration - frame1time) % 1000L) * MSEC;
            disable_thread_cancel(); // don't get cancelled while holding a lock
            lock_ncurses();

            show_result(aspec->hole, aspec->numholes, WHACK, aspec->score1, aspec->score2, NULL);
            refresh();
            unlock_ncurses();
            enable_thread_cancel();
            lock_molecomm();
            aspec->synccount = 2;  // Indicate animation progressing
            unlock_molecomm();
            nanosleep(&sleeptime, NULL);

            // Blank after animation
            disable_thread_cancel(); // don't get cancelled while holding a lock
            lock_ncurses();

            show_result(aspec->hole, aspec->numholes, -1, 0, 0, "");  // Blank out hole
            refresh();
            unlock_ncurses();
            enable_thread_cancel();
            lock_molecomm();
            aspec->synccount = 3;  // Indicate animation complete
            unlock_molecomm();
        } break;

        case ANIMESCAPED: {
 #if defined(debug) && defined(_GNU_SOURCE)
            pthread_setname_np(pthread_self(), "WAM-Anim-Escape");
 #endif
            lock_molecomm();
            aspec->synccount = 1;  // Indicate animation running
            unlock_molecomm();
            // Blank at start (makes it look better)
            const int blanktime = 250; //msec 
            sleeptime.tv_sec = 0;
            sleeptime.tv_nsec = blanktime * MSEC; 
            disable_thread_cancel(); // don't get cancelled while holding a lock
            lock_ncurses();

            show_result(aspec->hole, aspec->numholes, -1, 0, 0, "");  // Blank out hole
            refresh();
            unlock_ncurses();
            enable_thread_cancel();
            nanosleep(&sleeptime, NULL);

            // Frame 1
            const int frame1time = 500; //msec 
            sleeptime.tv_sec = 0;
            sleeptime.tv_nsec = frame1time * MSEC; 
            disable_thread_cancel(); // don't get cancelled while holding a lock
            lock_ncurses();

            show_result(aspec->hole, aspec->numholes, ESCAPE, 0, 0, NULL);
            refresh();
            unlock_ncurses();
            enable_thread_cancel();
            nanosleep(&sleeptime, NULL);

            // Frame 2
            sleeptime.tv_sec = (int)((aspec->duration - frame1time - blanktime) / 1000L);
            sleeptime.tv_nsec = (long)((aspec->duration - frame1time - blanktime) % 1000L) * MSEC;
            disable_thread_cancel(); // don't get cancelled while holding a lock
            lock_ncurses();

            show_result(aspec->hole, aspec->numholes, ESCAPE, aspec->score1, aspec->score2, NULL);
            refresh();
            unlock_ncurses();
            enable_thread_cancel();
            lock_molecomm();
            aspec->synccount = 2;  // Indicate animation progressing
            unlock_molecomm();
            nanosleep(&sleeptime, NULL);

            // Blank after animation
            disable_thread_cancel(); // don't get cancelled while holding a lock
            lock_ncurses();

            show_result(aspec->hole, aspec->numholes, -1, 0, 0, "");  // Blank out hole
            refresh();
            unlock_ncurses();
            enable_thread_cancel();
            lock_molecomm();
            aspec->synccount = 3;  // Indicate animation complete
            unlock_molecomm();
        } break;

        case ANIMMISFIRE: {  // Shows misfire animation on hole not occupied by mole
            // Currently unimplemented.  display_thread handles "misfire" frame directly.
        } break;

        case ANIMMISFIRESCARED: { // Shows misfire on hole where mole was hiding
 #if defined(debug) && defined(_GNU_SOURCE)
            pthread_setname_np(pthread_self(), "WAM-Anim-Scare1");
 #endif
            lock_molecomm();
            aspec->synccount = 1;  // Indicate animation running
            unlock_molecomm();

            int frametime = aspec->duration / 4;
            sleeptime.tv_sec = frametime / 1000;
            sleeptime.tv_nsec = (frametime % 1000) * MSEC; 
            disable_thread_cancel(); // don't get cancelled while holding a lock
            lock_ncurses();

            show_result(aspec->hole, aspec->numholes, MISFIRE, 0, 0, NULL);
            refresh();
            unlock_ncurses();
            enable_thread_cancel();
            nanosleep(&sleeptime, NULL);

            frametime = aspec->duration / 20;
            sleeptime.tv_sec = frametime / 1000;
            sleeptime.tv_nsec = (frametime % 1000) * MSEC; 
            int i;
            for (i=0; i<3; i++) {
                disable_thread_cancel(); // don't get cancelled while holding a lock
                lock_ncurses();

                show_result(aspec->hole, aspec->numholes, -1, 0, 0, "!SCARED!");
                refresh();
                unlock_ncurses();
                enable_thread_cancel();
                nanosleep(&sleeptime, NULL);

                disable_thread_cancel(); // don't get cancelled while holding a lock
                lock_ncurses();

                show_result(aspec->hole, aspec->numholes, -1, 0, 0, "");
                refresh();
                unlock_ncurses();
                enable_thread_cancel();
                nanosleep(&sleeptime, NULL);
            }

            frametime = aspec->duration / 4;
            sleeptime.tv_sec = frametime / 1000;
            sleeptime.tv_nsec = (frametime % 1000) * MSEC; 
            disable_thread_cancel(); // don't get cancelled while holding a lock
            lock_ncurses();

            show_result(aspec->hole, aspec->numholes, SCAREDOFF, 0, 0, NULL);
            refresh();
            unlock_ncurses();
            enable_thread_cancel();
            nanosleep(&sleeptime, NULL);

            frametime = aspec->duration * 2 / 10;
            sleeptime.tv_sec = frametime / 1000;
            sleeptime.tv_nsec = (frametime % 1000) * MSEC; 
            disable_thread_cancel(); // don't get cancelled while holding a lock
            lock_ncurses();

            show_result(aspec->hole, aspec->numholes, -1, 0, 0, "!SCARED!");
            refresh();
            unlock_ncurses();
            enable_thread_cancel();
            nanosleep(&sleeptime, NULL);

            // Blank after animation
            disable_thread_cancel(); // don't get cancelled while holding a lock
            lock_ncurses();

            show_result(aspec->hole, aspec->numholes, -1, 0, 0, "");  // Blank out hole

            refresh();
            unlock_ncurses();
            enable_thread_cancel();
            lock_molecomm();
            aspec->synccount = 2;  // Indicate animation complete
            unlock_molecomm();
        } break;

        case INSTRSCARED:    // Scared animation from instructions page.
        case ANIMUPSCARED: { // UP mole scared off by missfire on other hole.
 #if defined(debug) && defined(_GNU_SOURCE)
            pthread_setname_np(pthread_self(), "WAM-Anim-Scare2");
 #endif
            lock_molecomm();
            aspec->synccount = 1;  // Indicate animation running
            unlock_molecomm();

            do {    // ANIMUPSCARED does this once, INSTRSCARED loops until cancelled

                // INTRSCARED starts out displaying an UP mole
                if (aspec->animationtype == INSTRSCARED) {
                    disable_thread_cancel(); // don't get cancelled while holding a lock
                    lock_ncurses();
                    show_mole(aspec->hole, aspec->numholes, 5);
                    refresh();
                    unlock_ncurses();
                    enable_thread_cancel();
                    sleeptime.tv_sec = 3;
                    sleeptime.tv_nsec = 0l;
                    nanosleep(&sleeptime, NULL);

                    disable_thread_cancel(); // don't get cancelled while holding a lock
                    lock_ncurses();
                    show_result(aspec->hole, aspec->numholes, SCAREDOFF, 0, 0, NULL);
                    refresh();
                    unlock_ncurses();
                    enable_thread_cancel();
                    sleeptime.tv_sec = 0;
                    sleeptime.tv_nsec = 750000000L;
                    nanosleep(&sleeptime, NULL);
                }

                int frametime = aspec->duration / 20;
                sleeptime.tv_sec = frametime / 1000;
                sleeptime.tv_nsec = (frametime % 1000) * MSEC; 
                int i;
                for (i=0; i<3; i++) {
                    disable_thread_cancel(); // don't get cancelled while holding a lock
                    lock_ncurses();

                    show_result(aspec->hole, aspec->numholes, -1, 0, 0, "!SCARED!");
                    refresh();
                    unlock_ncurses();
                    enable_thread_cancel();
                    nanosleep(&sleeptime, NULL);

                    disable_thread_cancel(); // don't get cancelled while holding a lock
                    lock_ncurses();

                    show_result(aspec->hole, aspec->numholes, -1, 0, 0, "");
                    refresh();
                    unlock_ncurses();
                    enable_thread_cancel();
                    nanosleep(&sleeptime, NULL);
                }

                frametime = aspec->duration * 5 / 10;
                sleeptime.tv_sec = frametime / 1000;
                sleeptime.tv_nsec = (frametime % 1000) * MSEC; 
                disable_thread_cancel(); // don't get cancelled while holding a lock
                lock_ncurses();

                show_result(aspec->hole, aspec->numholes, SCAREDOFF, 0, 0, NULL);
                refresh();
                unlock_ncurses();
                enable_thread_cancel();
                nanosleep(&sleeptime, NULL);

                frametime = aspec->duration * 2 / 10;
                sleeptime.tv_sec = frametime / 1000;
                sleeptime.tv_nsec = (frametime % 1000) * MSEC; 
                disable_thread_cancel(); // don't get cancelled while holding a lock
                lock_ncurses();

                show_result(aspec->hole, aspec->numholes, -1, 0, 0, "!SCARED!");
                refresh();
                unlock_ncurses();
                enable_thread_cancel();
                nanosleep(&sleeptime, NULL);

                // Blank after animation
                disable_thread_cancel(); // don't get cancelled while holding a lock
                lock_ncurses();

                show_result(aspec->hole, aspec->numholes, -1, 0, 0, "");  // Blank out hole

                refresh();
                unlock_ncurses();
                enable_thread_cancel();

                if (aspec->animationtype == INSTRSCARED) {
                    sleeptime.tv_sec = 2;
                    sleeptime.tv_nsec = 500000000l;
                    nanosleep(&sleeptime, NULL);
                }
            } while (aspec->animationtype == INSTRSCARED);
            lock_molecomm();
            aspec->synccount = 2;  // Indicate animation complete
            unlock_molecomm();
        } break;

        default: {
            // intentionally left empty
        };
    }
    return NULL;
}

//================================
// void *display_thread(void *arg)
// Display management thread. 
//
// Looks for changes to molecomm and scores buffers and updates display accordingly.
// Ack's status changes back to mole_thread.
//
// Also looks for new entries in the scores buffer and displays results.
//
// This function makes extensive use of the ncurses_mutex to help it
// play nice with the animation_thread.
//
void *display_thread(void *arg){
    static struct {
        int status; // 1=misfire active (displayed), 0=not
        struct timespec timer;
    } misfires[MOLEHOLES]; // Used to track misfire display for each hole.
    struct MoleCommRecord newmolecomm[CONCURRENTMOLES];
    struct MoleCommRecord oldmolecomm[CONCURRENTMOLES];
    int knownscores = 0;
    int err;

 #if defined(debug) && defined(_GNU_SOURCE)
    pthread_setname_np(pthread_self(), "WAM-Display");
 #endif

    memset(newmolecomm, 0, sizeof(newmolecomm));
    memset(oldmolecomm, 0, sizeof(oldmolecomm));

    lock_ncurses();
    display_empty_playfield(BASEGAME, DISP_ELE_ALL, MOLEHOLES, "Good luck and have fun!!!");
    unlock_ncurses();

    struct timespec sleeptime = {0, 500000000L}; // 500 msec sleep to give player a chance
    nanosleep(&sleeptime, NULL);                 // to get ready for the moles.

    if ((err = pthread_mutex_lock(&start_mtx)) != 0) {   // set up mutex for cond wait
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to lock start mutex.");
    }

    display_thread_running = 1;

    if ((err = pthread_mutex_unlock(&start_mtx)) != 0) {
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to lock start mutex.");
    }

    if ((err = pthread_cond_signal(&start_cond)) != 0) {
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to send start_cond signal to main thread.");
    }

    int misfirepending = 0;  // dont allow thread to be cancelled if misfire display pending

    for (;;) {
        disable_thread_cancel(); // don't get cancelled while holding a lock
        lock_molecomm();

        // Lock and snapshot live molecomm buffer, then work from the snaphot
        memcpy(newmolecomm, molecomm, sizeof(newmolecomm));

        unlock_molecomm();
        lock_ncurses();
        if (molesremaining >= 0) {
            mvprintw(6, 63, "%-4d ", molesremaining);
        }
        unlock_ncurses();

        int i;
        for (i=0; i<CONCURRENTMOLES; i++) { // check each mole slot
            struct MoleCommRecord *pnew = &newmolecomm[i];
            struct MoleCommRecord *pold = &oldmolecomm[i];

            if (pnew->molestatus == pold->molestatus) continue;  // No Change

            lock_molecomm();
            switch (pnew->molestatus) {
                case HIDING: {

                    molecomm[i].animspec = HidingAnim;
                    molecomm[i].animspec.hole = pnew->hole;
                    molecomm[i].animspec.duration = pnew->duration - pnew->uptime;
                    molecomm[i].animspec.mole = pnew->mole;
                    molecomm[i].animcancelled = 0;
#if defined(debug)
                    molecomm[i].animspec.threadsn = ++threadsn;
#endif

                    if ((err = pthread_create(&molecomm[i].animthread, NULL, animation_thread, &molecomm[i].animspec)) != 0) {
                        restore_terminal();
                        error_at_line(-1, err, __FILE__, __LINE__, "Unable to create animation thread %d.", i);
                    }
                } break;

                case UP: {
                    // First, join with terminated HIDING animation thread (cleanup zombie)

                    pthread_t pttemp = molecomm[i].animthread; // Snapshot this. We have to unlock
                                                               // molecomm while we wait on joining
                                                               // the animation thread. But without the 
                                                               // lock, some other thread could mess
                                                               // with it.
                    unlock_molecomm();

                    void *retval;
                    if ((err = pthread_join(pttemp, &retval)) != 0) { // Join hiding animation thread
                        restore_terminal();
                        error_at_line(-1, err, __FILE__, __LINE__, "Unable to join animation thread %d Error=%d.", i,err);
                    }

                    lock_ncurses();
                    show_mole(molecomm[i].hole, MOLEHOLES, 0); // blank hole
                    refresh();
                    unlock_ncurses();

                    lock_molecomm();
                    memset(&molecomm[i].animspec, 0, sizeof(molecomm[i].animspec));
                    molecomm[i].animspec = PopupAnim;
                    molecomm[i].animspec.hole = pnew->hole;
                    molecomm[i].animspec.duration = pnew->uptime;
                    molecomm[i].animspec.mole = pnew->mole;
                    molecomm[i].animcancelled = 0;
#if defined(debug)
                    molecomm[i].animspec.threadsn = ++threadsn;
#endif

                    if ((err = pthread_create(&molecomm[i].animthread, NULL, animation_thread, &molecomm[i].animspec)) != 0) {
                        restore_terminal();
                        error_at_line(-1, err, __FILE__, __LINE__, "Unable to create animation thread %d.", i);
                    }
                } break;

                case WHACKED: {

                    // First, join with terminated UP animation thread (cleanup zombie)

                    pthread_t pttemp = molecomm[i].animthread; // Snapshot this. We have to unlock
                                                               // molecomm while we wait on joining
                                                               // the animation thread. But without the 
                                                               // lock, some other thread could mess
                                                               // with it.

                    unlock_molecomm();

                    void *retval;

                    if ((err = pthread_join(pttemp, &retval)) != 0) { // Join the UP animation thread
                        restore_terminal();
                        error_at_line(-1, err, __FILE__, __LINE__, "Unable to join animation thread %d. Error=%d.", i, err);
                    }

                    lock_ncurses();
                    show_mole(molecomm[i].hole, MOLEHOLES, 0); // blank hole
                    refresh();
                    unlock_ncurses();

                    lock_molecomm();
                    memset(&molecomm[i].animspec, 0, sizeof(molecomm[i].animspec));
                    lock_ncurses();
                    show_mole(molecomm[i].hole, 9, 0); // Clear out mole hole
                    refresh();
                    molecomm[i].animspec = WhackedAnim;
                    molecomm[i].animspec.hole = pnew->hole;
                    unlock_ncurses();   // maintain proper lock order
                    lock_scores(); //prevent scores from moving due to asyncronous realloc call
                    lock_ncurses();
                    molecomm[i].animspec.score1 = scores[molecomm[i].scoreidx].whackedscore;
                    molecomm[i].animspec.score2 = scores[molecomm[i].scoreidx].bonusscore;

                    unlock_scores();
                    molecomm[i].animspec.mole = pnew->mole;
                    molecomm[i].animcancelled = 0;
#if defined(debug)
                    molecomm[i].animspec.threadsn = ++threadsn;
#endif

                    if ((err = pthread_create(&molecomm[i].animthread, NULL, animation_thread, &molecomm[i].animspec)) != 0) {
                        restore_terminal();
                        error_at_line(-1, err, __FILE__, __LINE__, "Unable to create animation thread %d.", i);
                    }
                    unlock_ncurses();
                } break;

                case EXPIRED: {

                    // First, join with terminated UP animation thread (cleanup zombie)

                    pthread_t pttemp = molecomm[i].animthread; // Snapshot this. We have to unlock
                                                               // molecomm while we wait on joining
                                                               // the animation thread. But without the 
                                                               // lock, some other thread could mess
                                                               // with it.

                    unlock_molecomm();

                    void *retval;

                    if ((err = pthread_join(pttemp, &retval)) != 0) { // Join the UP animation thread
                        restore_terminal();
                        error_at_line(-1, err, __FILE__, __LINE__, "Unable to join animation thread %d. Error=%d.", i, err);
                    }

                    lock_ncurses();
                    show_mole(molecomm[i].hole, MOLEHOLES, 0); // blank hole
                    refresh();
                    unlock_ncurses();

                    lock_molecomm();
                    memset(&molecomm[i].animspec, 0, sizeof(molecomm[i].animspec));
                    molecomm[i].animspec = EscapedAnim;
                    molecomm[i].animspec.hole = pnew->hole;
                    lock_scores(); //prevent scores from moving due to asyncronous realloc call
                    lock_ncurses();
                    molecomm[i].animspec.score1 = scores[molecomm[i].scoreidx].missedscore;
                    unlock_scores();
                    molecomm[i].animspec.score2 = 0;
                    molecomm[i].animspec.mole = pnew->mole;
                    molecomm[i].animcancelled = 0;
#if defined(debug)
                    molecomm[i].animspec.threadsn = ++threadsn;
#endif

                    if ((err = pthread_create(&molecomm[i].animthread, NULL, animation_thread, &molecomm[i].animspec)) != 0) {
                        restore_terminal();
                        error_at_line(-1, err, __FILE__, __LINE__, "Unable to create animation thread %d.", i);
                    }
                    unlock_ncurses();
                } break;

                case TERMINATING: {
                    // Join the animation thread from prev status
                    //
                    pthread_t pttemp = molecomm[i].animthread; // Snapshot this. We have to unlock
                                                               // molecomm while we wait on joining
                                                               // the animation thread. But without the 
                                                               // lock, some other thread could mess
                                                               // with it.

                    unlock_molecomm();
                    void *retval;

                    if ((err = pthread_join(pttemp, &retval)) != 0) { // Join whacked/missed/scared anim thread
                        restore_terminal();
                        error_at_line(-1, err, __FILE__, __LINE__, "Unable to join animation thread %d. Error=%d.", i, err);
                    }

                    lock_ncurses();
                    show_mole(molecomm[i].hole, MOLEHOLES, 0); // blank hole
                    refresh();
                    unlock_ncurses();

                    lock_molecomm();
                    memset(&molecomm[i].animspec, 0, sizeof(molecomm[i].animspec));
                } break;

                case SCARED: {
                    // Join the UP or HIDING animation thread from prev status
                    pthread_t pttemp = molecomm[i].animthread; // Snapshot this. We have to unlock
                                                               // molecomm while we wait on joining
                                                               // the animation thread. But without the
                                                               // lock, some other thread could mess
                                                               // with it.
                    unlock_molecomm();
                    void *retval;

                    if ((err = pthread_join(pttemp, &retval)) != 0) { // Join up/hiding anim thread
                        restore_terminal();
                        error_at_line(-1, err, __FILE__, __LINE__, "Unable to join animation thread %d. Error=%d.", i, err);
                    }

                    lock_ncurses();
                    show_mole(molecomm[i].hole, MOLEHOLES, 0); // blank hole
                    refresh();
                    unlock_ncurses();

                    lock_molecomm();
                    memset(&molecomm[i].animspec, 0, sizeof(molecomm[i].animspec));

                    // Select one of three animations, depending on mole state when scared
                    if (molecomm[i].displayack == UP) {
                        molecomm[i].animspec = UpScaredAnim;
                        molecomm[i].animspec.hole = pnew->hole;
                        molecomm[i].animspec.mole = pnew->mole;
                        molecomm[i].animcancelled = 0;
#if defined(debug)
                        molecomm[i].animspec.threadsn = ++threadsn;
#endif

                        if ((err = pthread_create(&molecomm[i].animthread, NULL, animation_thread, &molecomm[i].animspec)) != 0) {
                            restore_terminal();
                            error_at_line(-1, err, __FILE__, __LINE__, "Unable to create animation thread %d.", i);
                        }
                    } else if (molecomm[i].displayack == HIDING) {
                        if (molecomm[i].keystruck == holekeys[molecomm[i].hole]) {
                            molecomm[i].animspec = MisfireScaredAnim;
                            molecomm[i].animspec.hole = pnew->hole;
                            molecomm[i].animspec.mole = pnew->mole;
                            molecomm[i].animcancelled = 0;
#if defined(debug)
                            molecomm[i].animspec.threadsn = ++threadsn;
#endif

                            if ((err = pthread_create(&molecomm[i].animthread, NULL, animation_thread, &molecomm[i].animspec)) != 0) {
                                restore_terminal();
                                error_at_line(-1, err, __FILE__, __LINE__, "Unable to create animation thread %d.", i);
                            }
                        } else {
                            molecomm[i].animspec = HideScaredAnim;
                            molecomm[i].animspec.hole = pnew->hole;
                            molecomm[i].animspec.mole = pnew->mole;
                            molecomm[i].animcancelled = 0;
#if defined(debug)
                            molecomm[i].animspec.threadsn = ++threadsn;
#endif

                            if ((err = pthread_create(&molecomm[i].animthread, NULL, animation_thread, &molecomm[i].animspec)) != 0) {
                                restore_terminal();
                                error_at_line(-1, err, __FILE__, __LINE__, "Unable to create animation thread %d.", i);
                            }
                        }
                    } else {
                    }
                } break;

                default: {
                    // intentionally left empty
                } break;
            }

            molecomm[i].displayack = molecomm[i].molestatus; // Ack mole_thread
            if ((err = pthread_cond_signal(&molecomm[i].dispcond)) != 0) {
                restore_terminal();
                error_at_line(-1, err, __FILE__, __LINE__, "Unable to send display cond signal to mole thread %d.", i);
            }
            unlock_molecomm();
        }

        memcpy(oldmolecomm, newmolecomm, sizeof(oldmolecomm)); // save copy 

        // Now look for a new scoresheet record and handle it

        lock_scores(); 

        if (numscores > knownscores) {

            // make temp copy, since scores can be realloc'd and move on us
            struct ScoreSheetRecord tscore = scores[knownscores];
            unlock_scores();
            if (tscore.playresult == MISFIRE || tscore.playresult == TOOSOON) {
                // If we get here, we have a misfire.

                lock_molecomm();
                int i;
                // First step with misfire is to cancel all active animation threads
                for (i=0; i<CONCURRENTMOLES; i++) { // Check each mole thread for active animation

                    if ((molecomm[i].animspec.animationtype == ANIMHIDING ||
                         molecomm[i].animspec.animationtype == ANIMPOPUP)
                        && molecomm[i].animspec.synccount > 0     // animation must have started
                        && molecomm[i].animspec.synccount < molecomm[i].animspec.syncpoints // and animation can't be ending
                        && molecomm[i].animcancelled == 0) {      // and animation not already cancelled

                        if ((err = pthread_cancel(molecomm[i].animthread)) != 0) { //kill animation
                            restore_terminal();
                            error_at_line(-1, err, __FILE__, __LINE__, "Unable to cancel animation thread.");
                        }

                        molecomm[i].animcancelled = 1;
                        // flag animation as finished
                        molecomm[i].animspec.synccount =  molecomm[i].animspec.syncpoints;
                    }
                }

                // Next step is to let each mole thread proceed, if it was waiting on key press
                for (i=0; i<CONCURRENTMOLES; i++) {

                    molecomm[i].keystruck = tscore.selection;
                    if ((err = pthread_cond_signal(&molecomm[i].keycond)) != 0) {
                        restore_terminal();
                        error_at_line(-1, err, __FILE__, __LINE__, "Unable to send cond signal to thread slot %d",i);
                    }
                }

                unlock_molecomm();

                const long MisfireDisplayTime = 1500; //(msec)
                struct timespec tsnow, tsexp;
                clock_gettime(CLOCK_MONOTONIC, &tsnow);
                tsexp = tsnow;
                tsexp.tv_nsec += (MisfireDisplayTime % 1000) * MSEC;
                tsexp.tv_sec += MisfireDisplayTime / 1000;
                if (tsexp.tv_nsec >=  1000000000L) {
                    tsexp.tv_nsec -= 1000000000L;
                    tsexp.tv_sec++;
                }

                misfires[tscore.hole].timer = tsexp;  // notes misfire status for later
            }

            lock_ncurses();

            mvprintw(10,53, "   SCORE: %d ", tscore.endscore);
            refresh();
            unlock_ncurses();

            ++knownscores;
        } else {

            unlock_scores();
        }

        // Handle misfire display.  If timer has not expired, misfire needs to be displayed
        // (if it isn't already up).  If timer has expires, take down the display if needed.
        // Also, we lock out the hole during the misfire display so any mole than needs it
        // will be forced to wait.
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        misfirepending = 0;  // Flag indicates one or more misfires pending, 
                             // so don't let thread be cancelled if set.
        for (i=0; i<MOLEHOLES; i++) {

            if (misfires[i].timer.tv_sec > now.tv_sec || (misfires[i].timer.tv_sec == now.tv_sec && misfires[i].timer.tv_nsec > now.tv_nsec)) {

                // make sure misfire is displayed.
                // If mole hole is already locked, it meand the misfire was on a
                // hiding mole. Thius will be handled by an animation, so no need
                // to do the misfile frame here.
                if (misfires[i].status == 0 && check_mole_hole(i) == 0) {
                    claim_mole_hole(i); // Lock hole

                    misfires[i].status = 1;
                    lock_ncurses();

                    show_result(i, MOLEHOLES, MISFIRE, 0, 0, NULL);
                    unlock_ncurses();
                }
            } else {
                // make sure misfire is NOT displayed
                if (misfires[i].status == 1) {
                    // Clear misfire display
                    misfires[i].status = 0;
                    lock_ncurses();

                    show_result(i, MOLEHOLES, -1, 0, 0, "");
                    unlock_ncurses();

                    release_mole_hole(i); 
                }
            }

            if (misfires[i].status == 1) {
                    misfirepending = 1;     // Causes thread to be uncancellable
            }
        }

        if (! misfirepending) {  // can't cancel thread if any misfires pending

            enable_thread_cancel(); // Give main() a chance to cancel the thread
        }

        struct timespec sleeptime = {0, 10000000L}; // 10 msec sleep so we
        nanosleep(&sleeptime, NULL);                // don't burn up the cpu
    }

    return NULL;
}

//===============================
// void *input_thread(void *arg)
//
// Keyboard input thread.
//
// Scans for keyboard input and communicates key presses to
// mole threads through global MoleComm structure.
//
// If no mole thread is expecting the key, a scoresheet record is
// created with a misfire record.  Misfire also sets scaredflag for each
// molecomm record.
//
void *input_thread(void *arg) {
    char inputkey;
    long msec;
    int err;
 #if defined(debug) && defined(_GNU_SOURCE)
    pthread_setname_np(pthread_self(), "WAM-Input");
 #endif

    if ((err = pthread_mutex_lock(&start_mtx)) != 0) {   // set up mutex for cond wait
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to lock start mutex.");
    }

    kbthread_running = 1;

    if ((err = pthread_mutex_unlock(&start_mtx)) != 0) {
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to lock start mutex.");
    }

    if ((err = pthread_cond_signal(&start_cond)) != 0) {
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to send start_cond signal to main thread.");
    }

    for (;;) {
        msec = 1L;   // wait a msec so as not to slam cpu
        inputkey = waitforkey(&msec);

#if defined(AUTOPLAY)
        if (inputkey == '\0') {
            struct timespec chaos;
            int autoplaymsec = tsrandom() % AUTOPLAY;
            chaos.tv_sec = autoplaymsec / 1000;
            chaos.tv_nsec = autoplaymsec % 1000 * 1000000L;
            nanosleep(&chaos, NULL);
            inputkey = holekeys[tsrandom() % sizeof(holekeys)];
        }
#endif

        if (inputkey != '\0') {
            // make sure this key is even a valid selection
            if (memchr((const void *)holekeys, (int)inputkey, sizeof(holekeys)) == NULL) {
                continue;
            }

            // Some key was hit. Lock molecomm mutex while we figure it out.
            disable_thread_cancel(); // don't get cancelled while holding a lock
            lock_molecomm();

            int whackflag = 0;
            int i;
            for (i=0; i<CONCURRENTMOLES; i++) { // Check each mole thread

                if (molecomm[i].molestatus == UP              // Mole must be UP
                    && molecomm[i].displayack == UP           // and display thread must agree it's up
                    && holekeys[molecomm[i].hole] == inputkey // and correct key must be hit
                    && molecomm[i].keystruck != inputkey      // and this is first time key struck
                    && molecomm[i].animspec.synccount > 0     // and animation must have started
                    && molecomm[i].animspec.synccount <       // and animation can't be ending
                            molecomm[i].animspec.syncpoints   
                    && molecomm[i].animcancelled == 0) {      // and animation not already cancelled

                    if ((err = pthread_cancel(molecomm[i].animthread)) != 0) { //kill animation
                        restore_terminal();
                        error_at_line(-1, err, __FILE__, __LINE__, "Unable to cancel animation thread.");
                    }
                    molecomm[i].animcancelled = 1;
                    whackflag = 1;
                    molecomm[i].keystruck = inputkey;

                    // let mole thread proceed
                    if ((err = pthread_cond_signal(&molecomm[i].keycond)) != 0) {
                        restore_terminal();
                        error_at_line(-1, err, __FILE__, __LINE__, "Unable to send cond signal to thread slot %d",i);
                    }
                } else if ((molecomm[i].molestatus == EXPIRED || molecomm[i].molestatus == WHACKED || (molecomm[i].molestatus == UP && molecomm[i].animspec.synccount == molecomm[i].animspec.syncpoints)) && holekeys[molecomm[i].hole] == inputkey){
                    // Both these conditions are considered near miss (no score or penalty).
                    // First is a recendly expired mole, second takes care of double strike.
                    whackflag = 1; 
                } else {
                }
            }

            // unlock molecomm mutex
            unlock_molecomm();

            if (!whackflag) {  // This is a misfire!
                int i;
                int misfirehole;
                enum PlayResult misfiretype = MISFIRE; // default, unless we set to TOOSOON later
                lock_molecomm();
                for(i=0; i<CONCURRENTMOLES; i++) {  // Set scaredflag for each Hiding/Up mole 
                                                    // so mole_thread can exit early

                    if (molecomm[i].molestatus == HIDING || molecomm[i].molestatus == UP ) {
                        molecomm[i].scaredflag = 1;
                        clock_gettime(CLOCK_MONOTONIC, &molecomm[i].scaredtime);
                    }

                    if (molecomm[i].molestatus == HIDING && inputkey == holekeys[molecomm[i].hole]) {
                        misfiretype = TOOSOON;
                    }
                }
                unlock_molecomm();

                for (i=0; i<MOLEHOLES; i++) {  // search holekeys to find misfire hole
                    if (inputkey == holekeys[i]) {
                        break;
                    }
                }
                misfirehole = i;
                // Log the misfire in the scores buffer. (triggers display_thread to handle it) 
                compute_score(-1, misfirehole, inputkey, 0, misfiretype);
            }

            enable_thread_cancel(); 
        }
    }

    return NULL;
}

//===================================
//pthread_t *start_input_thread(void)
//
// Starts up the input monitoring thread.
//
// returns the thread ID
//
pthread_t *start_input_thread(void) {
    static pthread_t tid;   // thread ID
    int err;

    clear_input_buffer();

    if ((err = pthread_mutex_lock(&start_mtx)) != 0) {   // set up mutex for cond wait
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to lock start mutex.");
    }

    if ((err = pthread_create(&tid, NULL, input_thread, NULL)) != 0) {
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to set create input thread.");
    }

    while (!kbthread_running) {
        if ((err = pthread_cond_wait(&start_cond, &start_mtx)) != 0) {
            restore_terminal();
            error_at_line(-1, err, __FILE__, __LINE__, "Input thread cond wait failed.");
        }
    }

    if ((err = pthread_mutex_unlock(&start_mtx)) != 0) {
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to lock start mutex.");
    }

    return &tid;
}

//====================================
//pthread_t *start_display_thread(void)
//
// Starts up the display monitoring thread.
//
// returns the thread ID
//
pthread_t *start_display_thread(void) {
    static pthread_t tid;   // thread ID
    pthread_attr_t tattr;   // thread attributes
    int err;

    if ((err = pthread_attr_init(&tattr)) != 0) {  // Start with default thread attributes.
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to assign default display thread attributes.\n");
    }

    if ((err = pthread_mutex_lock(&start_mtx) != 0)) {   // set up mutex for cond wait
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to lock start mutex.");
    }

    if ((err = pthread_create(&tid, &tattr, display_thread, NULL)) != 0) {
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to set create display thread.");
    }

    if ((err = pthread_attr_destroy(&tattr)) != 0) {     // attributes no longer needed
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to set release display thread attributes.");
    }

    while (!display_thread_running) {
        if ((err = pthread_cond_wait(&start_cond, &start_mtx)) != 0) {
            restore_terminal();
            error_at_line(-1, err, __FILE__, __LINE__, "Input thread cond wait failed.");
        }
    }

    if ((err = pthread_mutex_unlock(&start_mtx)) != 0) {
            restore_terminal();
            error_at_line(-1, err, __FILE__, __LINE__, "Unable to unlock start mutex.");
    }

    return &tid;
}

//============================
// void assign_hole_keys(void)
//
// Assigns a key to each mole hole.
// Currently just "1" to "9" for the 9 holes, but could expanded in the future
// or randomized to provide additional challenge, changed after each hit, etc.
//
void assign_hole_keys(void) {
    memcpy(holekeys,"789456123", sizeof(holekeys));
}

//===============================
// MAIN
int main(int argc, char *argv[]) {
    int err;
    const int moles = 20;      // How many moles in this game.
    const int moletime = 6500; // Time for each mole in msec.
                               // (Split randomly between HIDING and UP time)
    pthread_t *kbinput_tid;
    pthread_t *display_tid;

    long seed = time(NULL);
    srandom(seed);
    int i;
    for (i=0; i<MOLEHOLES; i++) {    // Initialize hole_mtx[]
        if ((err = pthread_mutex_init(&hole_mtx[i], NULL)) != 0) {
            restore_terminal();
            error_at_line(-1, err, __FILE__, __LINE__, "Unable to initialize hole mutex.\n");
        }
    }

    assign_hole_keys();   // Assign a key to each mole hole

    initialize_terminal();

#if !defined(AUTOPLAY)
    display_intro(moles, moles * (moletime + GRACEPERIOD) / 1000);
#endif

    kbinput_tid = start_input_thread();
    display_tid = start_display_thread();

    control_moles(moles, moletime);

    if ((err = pthread_cancel(*kbinput_tid)) != 0) {
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to cancel input thread.");
    }

    void *retval;
    if ((err = pthread_join(*kbinput_tid, &retval)) != 0) { // join input thread
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to join input thread. Error=%d.", err);
    }

    if ((err = pthread_cancel(*display_tid)) != 0) { 
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to cancel display thread.");
    }

    if ((err = pthread_join(*display_tid, &retval)) != 0) { // join display thread
        restore_terminal();
        error_at_line(-1, err, __FILE__, __LINE__, "Unable to join display thread. Error=%d.", err);
    }

#if !defined(AUTOPLAY)
    display_gameover();

    if (numscores > 0) {
        display_score_sheet(scores[numscores - 1].endscore, moles, moles * (moletime + GRACEPERIOD) / 1000);
    }
#endif

    for (i=0; i<MOLEHOLES; i++) {    // Destroy dynamically initialized hole_mtx[]
        if ((err = pthread_mutex_destroy(&hole_mtx[i])) != 0) {
            restore_terminal();
            error_at_line(-1, err, __FILE__, __LINE__, "Unable to destroy hole mutex %d.", i);
        }
    }

    clear_input_buffer();
    lock_scores();

    if (scores != NULL) free(scores);
    unlock_scores();
    restore_terminal();

    return 0;
}
