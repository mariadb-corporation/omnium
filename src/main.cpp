// Created by Roel Van de Paar, MariaDB
// omnium: one binary for the MariaDB QA pipeline. Verb dispatch and the child roles.
#include "verbs.h"

struct Verb { const char* name; int (*fn)(const Args&); const char* help; };
static const Verb VERBS[] = {
  {"help", cmd_help, "this text; help <verb> for one verb"},
  {"version", cmd_version, "print the version"},
  {"config", cmd_config, "show the settings (~/.omnium.conf); config KEY shows one, config KEY=VALUE sets one"},
  {"selftest", cmd_selftest, "run the built-in checks"},
  {"builds", cmd_builds, "the basedir registry /test/omnium.builds: list (after a /test scan), edit, test, report, set"},
  {"myver", cmd_myver, "version banner of the basedir in the cwd or named; Jira noformat title"},
  {"kb", cmd_kb, "known_bugs.strings: search <uid> | add <uid> <MDEV-n> | fixed <MDEV-n> | edit"},
  {"kba", cmd_kba, "the same for known_bugs.strings.SAN"},
  {"kbs", cmd_kbs, "search known_bugs.strings for a text"},
  {"kbsa", cmd_kbsa, "search known_bugs.strings.SAN for a text"},
  {"eb", cmd_eb, "open or create BUGS/MDEV-n.sql in the editor"},
  {"areas", cmd_areas, "the area table: what a trial can aim at; areas <basedir> shows what that build can run"},
  {"sql", cmd_sql, "assemble the SQL of one trial into a file: sql <basedir> [--area NAME] [--out FILE]"},
  {"build", cmd_build, "clone a version or branch and build it into /test/<name>: build 13.1 [flavours] [--tar]; build follow"},
  {"run", cmd_run, "run trials: run [basedir ...] [--trials N] [--slots N] [--area NAME]; no basedir = the registry's test set"},
  {"t", cmd_t, "the UniqueID of the trial or basedir in the cwd (new_text_string.sh)"},
  {"tt", cmd_tt, "the UniqueID plus the known-bugs verdict and the Jira search links"},
  {"els", cmd_els, "error_log_scan.sh: els {errors|lastline|top|check|clean|aggregate} <log>..."},
  {"sts", cmd_sts, "the sanitizer UniqueID of a log or trial dir (san_text_string.sh)"},
  {"fts", cmd_fts, "the fallback UniqueID of an error log (fallback_text_string.sh)"},
  {"stack", cmd_stack, "the stack or sanitizer block of the trial in the cwd, Jira noformat"},
  {"parity", cmd_parity, "the bash UID chain against this port on saved trials: parity [N|all] [--no-gdb]"},
  {"matrix", cmd_matrix, "replay a testcase on the report builds (or the builds named): matrix <sql> [build ...] [--slots N] [--out FILE]"},
  {"report", cmd_report, "the bug report of a reduced trial, into the inbox: report [<workdir>] <trial> [build ...] [--no-matrix] [--no-mtr]"},
  {"mtr", cmd_mtr, "the MTR form of a testcase, verified as a reverse gate: mtr [<workdir>] <trial> | mtr <sql> --basedir DIR"},
  {"tui", cmd_tui, "the live view of a run: panels, 1 s refresh; keys q p P s S l L r; tui [<run>] [--plain]"},
  {"cli", cmd_cli, "a shell with the omnium shortcuts (h shows them in a box, h <name> explains one); the prompt says which build or trial you are in"},
  {"fresh", cmd_fresh, "a fresh server for the basedir in the cwd or named, datadir on tmpfs: fresh [basedir] [--cl] [--keep] [--datadir DIR] [--options \"...\"]; --keep leaves a server that is already up alone"},
  {"cl", cmd_cl, "a client on the server omnium fresh started: cl [basedir] [client args]"},
  {"replay", cmd_replay, "feed a SQL file to that server: replay <file> [basedir] [--out FILE]"},
  {"ldd", cmd_ldd, "gather a server binary and the libraries it needs into a directory: ldd [basedir or dir]"},
  {"trial", cmd_trial, "what one saved trial holds: trial [<workdir>] <n>"},
  {"status", cmd_status, "every omnium run under /data: live or finished, trials, outcomes; status <run> for one"},
  {"adopt", cmd_adopt, "a pquery-run workdir's saved trials: list, --reduce the new UIDs, --report the reduced ones"},
  {"init", cmd_init, "check what the box needs, make the queues, write ~/.omnium_aliases"},
  {"inbox", cmd_inbox, "the human queue: list; --process files every .ok item; --dry-run shows the payload; --file <item>"},
  {"mail", cmd_mail, "send one mail the way a new inbox item does: mail <to> [--subject S] [--body-file F] [--mx DOMAIN] [--dry-run]"},
  {"jira", cmd_jira, "Jira over REST: jira whoami | search <uid> | versions [PROJECT] | components [PROJECT]"},
  {"reduce", cmd_reduce, "reduce a saved trial: reduce [<workdir>] <trial> [--variant quick|onethd|onethd-rnd] [--screen] [--plan]"},
};
struct Role { const char* name; int (*fn)(const Args&); };
static const Role ROLES[] = {
  {"generator", role_generator},
  {"revgen", role_revgen},
  {"trial", role_trial},
  {"hold", role_hold},
  {"jirastub", role_jirastub},
  {"smtpstub", role_smtpstub},
  {"reducer", role_reducer},
};

int cmd_version(const Args&) {
  printf("omnium %s\n", OMNIUM_VERSION);
  return 0;
}
int cmd_help(const Args& a) {
  if (!a.empty()) {
    for (auto& v : VERBS) if (a[0] == v.name) { printf("omnium %s: %s\n", v.name, v.help); return 0; }
    printf("no verb %s\n", a[0].c_str());
    return 1;
  }
  printf("omnium %s - one binary for the MariaDB QA pipeline\n\n", OMNIUM_VERSION);
  printf("usage: omnium [KEY=VALUE ...] <verb> [args]\n\n");
  for (auto& v : VERBS) printf("  %-12s %s\n", v.name, v.help);
  printf("\nSettings live in %s. Any KEY=VALUE on the command line overrides one for this call.\n",
         config_path().c_str());
  return 0;
}
int cmd_config(const Args& a) {
  if (a.empty()) { fputs(config_dump().c_str(), stdout); return 0; }
  vector<string> changed;
  for (auto& kv : a) {
    size_t eq = kv.find('=');
    if (eq == string::npos) {                                  // a bare key asks for its value
      string v;
      if (!config_get(kv, v)) { printf("no setting %s\n", kv.c_str()); return 1; }
      printf("%s=%s\n", upper(kv).c_str(), v.c_str());
      continue;
    }
    if (!config_set(kv.substr(0, eq), kv.substr(eq + 1))) { printf("%s\n", config_refusal(kv.substr(0, eq), kv.substr(eq + 1)).c_str()); return 1; }
    changed.push_back(kv.substr(0, eq));
  }
  if (!changed.empty()) { config_write_keys(changed); printf("saved %s\n", config_path().c_str()); }
  return 0;
}

// The embedded generators keep their own CLI: the role passes the arguments straight through.
static int run_embedded(const char* what, int (*fn)(int, char**), const string& fallback_bin, const Args& a) {
  vector<string> argv = {what};
  for (auto& x : a) argv.push_back(x);
  if (fn) {
    vector<char*> av;
    for (auto& s : argv) av.push_back(const_cast<char*>(s.c_str()));
    av.push_back(nullptr);
    return fn((int)av.size() - 1, av.data());
  }
  if (is_executable(fallback_bin)) {
    argv[0] = fallback_bin;
    vector<char*> av;
    for (auto& s : argv) av.push_back(const_cast<char*>(s.c_str()));
    av.push_back(nullptr);
    execv(av[0], av.data());
  }
  fprintf(stderr, "omnium: %s is not compiled in and %s is not there\n", what, fallback_bin.c_str());
  return 127;
}
// --role hold <path>: this child does nothing but sleep. The path it is given stays in its command
// line, which is what keeps the tmpfs cleaner from deleting that directory while the run uses it.
int role_hold(const Args&) {
  for (;;) {
    string line;
    if (role_cmd(line, 60000) && line == "stop") return 0;
  }
}
int role_generator(const Args& a) {
  return run_embedded("generator", omnium_generator_main, g_paths.qa + "/generatorcpp/generator", a);
}
int role_revgen(const Args& a) {
  return run_embedded("revgen", omnium_revgen_main, g_paths.qa + "/revgen/revgen", a);
}

static bool looks_like_setting(const string& s) {
  size_t eq = s.find('=');
  if (eq == string::npos || eq == 0) return false;
  for (size_t i = 0; i < eq; i++) if (!(isupper((unsigned char)s[i]) || s[i] == '_' || isdigit((unsigned char)s[i]))) return false;
  return true;
}

int main(int argc, char** argv) {
  // a subreducer: the reducer starts copies of its own executable (this binary) with its
  // REDUCER_* variables set and the input file as the only argument
  if (getenv("REDUCER_MULTI_REDUCER") && omnium_reducer_main) return omnium_reducer_main(argc, argv);
  Args all(argv + 1, argv + argc);
  if (!all.empty() && all[0] == "--role") {
    if (all.size() < 2) { fprintf(stderr, "--role needs a name\n"); return 2; }
    role_init();
    config_load(false);
    Args rest(all.begin() + 2, all.end());
    for (auto& r : ROLES) if (all[1] == r.name) return r.fn(rest);
    fprintf(stderr, "omnium: unknown role %s\n", all[1].c_str());
    return 2;
  }
  if (!all.empty() && (all[0] == "--version" || all[0] == "-V")) return cmd_version({});
  if (!all.empty() && (all[0] == "--help" || all[0] == "-h")) return cmd_help({});
  if (!all.empty() && all[0] == "--selftest") return cmd_selftest(Args(all.begin() + 1, all.end()));
  config_load(true);
  Args rest;
  const char* inherited = getenv("OMNIUM_SET");
  string set = inherited ? inherited : "";
  for (auto& a : all) {
    // a setting counts only before the verb, as the usage line says; after it the verb owns its
    // arguments, so `omnium config KEY=VALUE` reaches config and is saved
    if (rest.empty() && looks_like_setting(a)) {
      size_t eq = a.find('=');
      if (!config_set(a.substr(0, eq), a.substr(eq + 1))) { fprintf(stderr, "omnium: %s\n", config_refusal(a.substr(0, eq), a.substr(eq + 1)).c_str()); return 2; }
      set += a + "\n";
    } else {
      rest.push_back(a);
    }
  }
  // the paths built from the settings see them, and so does every omnium process this call starts
  if (!set.empty()) { setenv("OMNIUM_SET", set.c_str(), 1); paths_init(); }
  ensure_dirs();
  ensure_om_alias();
  if (rest.empty()) return cmd_help({});
  string verb = rest[0];
  Args args(rest.begin() + 1, rest.end());
  for (auto& v : VERBS) if (verb == v.name) return v.fn(args);
  fprintf(stderr, "omnium: unknown verb %s (omnium help lists them)\n", verb.c_str());
  return 2;
}
