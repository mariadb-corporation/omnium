// Created by Roel Van de Paar, MariaDB
// winterm.h - what of the terminal window the user can see
#pragma once

// How many of the `rows` text rows of a terminal window lie below the work area of its screen (the
// screen less the taskbar). The window's client area starts at client_top and is client_h high, and
// the work area ends at work_bottom, all in pixels. A row that is cut in half counts as hidden.
int rows_below_work_area(long client_top, long client_h, long work_bottom, int rows);

// The same for the window this process runs in: the rows under the taskbar, or off the screen's
// bottom edge. 0 when there is no window to ask (not Windows, a terminal that is no window of this
// desktop, a minimized window), which takes every row as visible.
int term_rows_hidden(int rows);
