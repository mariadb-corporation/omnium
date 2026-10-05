// Created by Roel Van de Paar, MariaDB
// Every omnium verb and every child role. main.cpp dispatches to these.
#pragma once
#include "common.h"

using Args = vector<string>;

// verbs (main.cpp table)
int cmd_help(const Args&);
int cmd_version(const Args&);
int cmd_config(const Args&);
int cmd_selftest(const Args&);
int cmd_builds(const Args&);
int cmd_myver(const Args&);
int cmd_kb(const Args&);
int cmd_kba(const Args&);
int cmd_kbs(const Args&);
int cmd_kbsa(const Args&);
int cmd_eb(const Args&);
int cmd_build(const Args&);
int cmd_areas(const Args&);
int cmd_sql(const Args&);
int cmd_run(const Args&);
int cmd_t(const Args&);
int cmd_tt(const Args&);
int cmd_els(const Args&);
int cmd_sts(const Args&);
int cmd_fts(const Args&);
int cmd_stack(const Args&);
int cmd_parity(const Args&);
int cmd_reduce(const Args&);
int cmd_matrix(const Args&);
int cmd_report(const Args&);
int cmd_mtr(const Args&);
int cmd_tui(const Args&);
int tui_frame_lines(int rows, int cols, int builds, int slots, int inbox, int dups, int log_lines);
int cmd_mail(const Args&);
int cmd_cli(const Args&);
int cmd_fresh(const Args&);
int cmd_cl(const Args&);
int cmd_replay(const Args&);
int cmd_trial(const Args&);
int cmd_ldd(const Args&);
int cmd_jira(const Args&);
int cmd_inbox(const Args&);
int cmd_status(const Args&);
int cmd_adopt(const Args&);
int cmd_init(const Args&);

// roles: /proc/self/exe --role <name> <args>
int role_generator(const Args&);
int role_revgen(const Args&);
int role_trial(const Args&);
int role_hold(const Args&);
int role_jirastub(const Args&);                              // selftest only: a stand-in Jira on 127.0.0.1
int role_smtpstub(const Args&);                              // selftest only: a stand-in mail server on 127.0.0.1
int role_reducer(const Args&);

// the two SQL generators, compiled in from ~/mariadb-qa (weak: a NO_GENERATOR build leaves one out)
extern "C++" int omnium_generator_main(int argc, char** argv) __attribute__((weak));
extern "C++" int omnium_revgen_main(int argc, char** argv) __attribute__((weak));
// the reducer, compiled in from ~/mariadb-qa/reducercpp (weak: a NO_REDUCER build leaves it out)
extern "C++" int omnium_reducer_main(int argc, char** argv) __attribute__((weak));

// selftest.cpp
void st_check(bool cond, const string& what);
void st_eq(const string& got, const string& want, const string& what);
