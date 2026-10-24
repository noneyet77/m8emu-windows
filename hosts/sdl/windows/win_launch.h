#ifndef M8_WIN_LAUNCH_H
#define M8_WIN_LAUNCH_H
/* Runs the app with a log file, error message box and crash report. */
int m8_win_launch(int (*run)(int, char **), int argc, char **argv);
/* After a successful run, start the program again with these UTF-8
 * arguments (args[0] is ignored; this executable is used). */
void m8_win_request_relaunch(int argc, char **args);
#endif
