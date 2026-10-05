// Created by Roel Van de Paar, MariaDB
// inbox.cpp - the human queue (/test/omnium/HUMAN-queue): omnium report puts <workdir>_bug<N>.report
// there; touch <item>.ok files it. The filing takes the edited report as it stands: the header
// lines are the Jira fields, the text under the ----- line is the ticket body. After the ticket
// exists the UID goes in known_bugs.strings (or .SAN) with the key, the testcase in BUGS/<key>.sql,
// and <item>.filed holds the key. A failure leaves <item>.error and the item stays.
#include "verbs.h"

namespace fs = std::filesystem;

namespace {
const int DAILY_CAP = 10;
struct Item {
  string dir, name, report, title, uid;
  bool ok = false, filed = false, error = false, possible_dup = false;
  string filed_key, error_text;
  int64_t age_s = 0;
};
vector<Item> inbox_items(const string& dir) {
  vector<Item> out;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(dir, ec)) {
    string n = e.path().filename().string();
    if (!ends_with(n, ".report")) continue;
    Item it;
    it.dir = dir;
    it.name = n.substr(0, n.size() - 7);
    it.report = e.path().string();
    string body;
    auto kv = report_header(read_file(it.report), &body);
    it.title = report_field(kv, "Title");
    it.uid = report_field(kv, "UID");
    string base = dir + "/" + it.name;
    it.ok = file_exists(base + ".ok");
    it.filed = file_exists(base + ".filed");
    it.error = file_exists(base + ".error");
    it.possible_dup = file_exists(base + ".possible_dup");
    if (it.filed) it.filed_key = trim(read_file(base + ".filed")).substr(0, trim(read_file(base + ".filed")).find(' '));
    if (it.error) it.error_text = trim(read_file(base + ".error"));
    auto t = fs::last_write_time(e.path(), ec);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    it.age_s = now_s() - std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count();
    out.push_back(it);
  }
  std::sort(out.begin(), out.end(), [](const Item& a, const Item& b) { return a.name < b.name; });
  return out;
}
string age_text(int64_t s) {
  if (s < 3600) return fmt("%ldm", (long)(s / 60));
  if (s < 86400) return fmt("%ldh", (long)(s / 3600));
  return fmt("%ldd", (long)(s / 86400));
}
// the sanitizer classes that stay out of an automatic filing: a possible security issue
bool held_class(const string& uid, const vector<string>& labels) {
  string s = lower(uid + " " + join(labels, " "));
  for (const char* k : {"heap-buffer-overflow", "heap-use-after-free", "stack-buffer-overflow", "stack-overflow", "stack-use-after",
                        "global-buffer-overflow", "double-free", "use-after-poison", "container-overflow", "dynamic-stack-buffer-overflow", "memcpy-param-overlap",
                        "use-of-uninitialized-value"})                  // the MSAN read Q401 holds back
    if (s.find(k) != string::npos) return true;
  return false;
}
int filed_today(const string& dir) {
  int n = 0;
  std::error_code ec;
  time_t now = (time_t)now_s();
  struct tm lt;
  localtime_r(&now, &lt);
  lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
  int64_t day_start = (int64_t)mktime(&lt);                     // local midnight, as the cap is meant per calendar day
  for (auto& e : fs::directory_iterator(dir, ec)) {
    if (!ends_with(e.path().filename().string(), ".filed")) continue;
    auto t = fs::last_write_time(e.path(), ec);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    if (std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count() >= day_start) n++;
  }
  return n;
}
// the version names Jira has. A version Jira only has as X.Y(EOL) is an end-of-life branch, and that
// goes in neither field: nothing is pushed there. A name Jira does not have is dropped as well.
void fit_versions(vector<string>& want, const vector<string>& known, vector<string>& notes, const char* what) {
  vector<string> out;
  for (auto& v : want) {
    if (std::find(known.begin(), known.end(), v) != known.end()) { out.push_back(v); continue; }
    if (std::find(known.begin(), known.end(), v + "(EOL)") != known.end()) { notes.push_back(string(what) + " " + v + " is an EOL branch; left out"); continue; }
    notes.push_back(string(what) + " " + v + " is not a Jira version name; dropped");
  }
  want = out;
}
bool file_item(Item& it, bool dry_run, string& summary) {
  string base = it.dir + "/" + it.name;
  string err;
  JiraFields f;
  string text = read_file(it.report);
  if (!report_to_fields(text, f, &err)) { write_file(base + ".error", err + "\n"); summary = "error: " + err; return false; }
  if (held_class(f.summary + " " + it.uid, f.labels)) {
    string why = "held: a memory-safety class (possible security issue) is never filed by omnium; file it by hand after review";
    write_file(base + ".error", why + "\n");
    summary = why;
    return false;
  }
  if (!dry_run && filed_today(it.dir) >= DAILY_CAP) {
    string why = fmt("held: %d tickets were filed today, the daily cap; the .ok stays and is retried tomorrow", DAILY_CAP);
    write_file(base + ".error", why + "\n");
    summary = why;
    return false;
  }
  if (f.project == "MENT") { f.affects = f.es_versions; f.es_versions.clear(); }
  vector<string> notes;
  {
    vector<string> versions, components;
    string e;
    if (jira_project_names(f.project, "versions", versions, &e)) {
      fit_versions(f.affects, versions, notes, "Affects");
      fit_versions(f.fix, versions, notes, "Fix Version");
    } else {
      notes.push_back("could not read the Jira version list (" + e + "); versions sent as written");
    }
    // an MDEV ticket never goes out with an empty Fix Version while Affects has one. For MENT the
    // Fix Version rule on ES releases is not settled, so that field stays as the report gave it.
    if (f.project == "MDEV" && f.fix.empty() && !f.affects.empty()) {
      f.fix = fix_versions(f.affects);
      notes.push_back("Fix Version was empty; taken from Affects as " + join(f.fix, ", "));
    }
    if (jira_project_names(f.project, "components", components, &e)) {
      vector<string> keep;
      for (auto& c : f.components) {
        if (std::find(components.begin(), components.end(), c) != components.end()) keep.push_back(c);
        else notes.push_back("component " + c + " is not a " + f.project + " component; dropped");
      }
      f.components = keep;
    }
  }
  string payload = jira_create_payload(f);
  write_file(base + ".preview", payload + "\n");
  if (dry_run) { summary = "dry run: payload in " + base + ".preview" + (notes.empty() ? "" : "; " + join(notes, "; ")); return true; }
  string key;
  if (!jira_create(f, key, &err)) {
    write_file(base + ".error", "filing failed: " + err + "\n");
    summary = "filing failed: " + err;
    return false;
  }
  string url = jira_base() + "/browse/" + key;
  write_file(base + ".filed", key + " " + url + "\n");
  fs::remove(base + ".ok");
  fs::remove(base + ".error");
  // registration: the UID with the key, the testcase under BUGS/
  vector<string> reg, changed;                                  // changed: the files in the mariadb-qa checkout, for git add
  if (!it.uid.empty()) {
    string e;
    if (kb_add(it.uid, key, false, &e)) { reg.push_back("kb"); changed.push_back(kb_file_for(it.uid)); } else reg.push_back("kb not added (" + e + ")");
  }
  {
    auto kv = report_header(text, nullptr);
    string tc = report_field(kv, "Testcase");
    string sql = read_file(tc);
    if (!sql.empty()) {
      string options;
      vector<string> lines = split_lines(sql);
      static const string tag = "mysqld options required for replay:";
      if (!lines.empty() && !lines[0].empty() && lines[0][0] == '#' && lower(lines[0]).find(tag) != string::npos) {
        options = trim(lines[0].substr(lower(lines[0]).find(tag) + tag.size()));
        lines.erase(lines.begin());
      }
      string e;
      if (bugs_write(key, options, join(lines, "\n") + "\n", &e)) { reg.push_back("eb"); changed.push_back(bugs_file_for(key)); } else reg.push_back("eb not written (" + e + ")");
    }
  }
  // omnium never runs git itself (Q318): the add is printed, the commit stays by hand
  summary = "filed " + key + " " + url + (reg.empty() ? "" : " [" + join(reg, ", ") + "]") + (notes.empty() ? "" : "; " + join(notes, "; ")) +
            (changed.empty() ? "" : "\ngit add " + join(changed, " "));
  return true;
}
}  // namespace

// omnium inbox [--dir DIR] [--process] [--dry-run] [--file <item>]
int cmd_inbox(const Args& a) {
  string dir = g_paths.human_queue, one;
  bool process = false, dry = false;
  for (size_t i = 0; i < a.size(); i++) {
    const string& s = a[i];
    auto val = [&](const string& name) -> string { if (i + 1 >= a.size()) { fprintf(stderr, "%s needs a value\n", name.c_str()); exit(2); } return a[++i]; };
    if (s == "--dir") dir = val(s);
    else if (s == "--process") process = true;
    else if (s == "--dry-run") { dry = true; process = true; }
    else if (s == "--file") { one = val(s); process = true; }
    else { fprintf(stderr, "omnium inbox [--dir DIR] [--process] [--dry-run] [--file <item>]\n"); return 2; }
  }
  mkdirs(dir);
  vector<Item> items = inbox_items(dir);
  if (!process) {
    if (items.empty()) { printf("inbox %s is empty\n", dir.c_str()); return 0; }
    for (auto& it : items) {
      string st = it.filed ? "filed " + it.filed_key : it.error ? "error" : it.ok ? "ok, filing pending" : it.possible_dup ? "possible dup" : "new";
      printf("%-32s %-20s %5s  %s\n", it.name.c_str(), st.c_str(), age_text(it.age_s).c_str(), it.title.c_str());
      if (it.error) printf("%-32s   %s\n", "", it.error_text.c_str());
    }
    printf("\napprove: touch %s/<item>.ok ; file: omnium inbox --process (--dry-run shows the payload first)\n", dir.c_str());
    return 0;
  }
  int rc = 0;
  bool any = false;
  for (auto& it : items) {
    if (!one.empty() && it.name != one && it.name + ".report" != one) continue;
    if (one.empty() && !it.ok) continue;
    if (it.filed) { printf("%s: already filed as %s\n", it.name.c_str(), it.filed_key.c_str()); continue; }
    if (!one.empty() && !it.ok && !dry) { printf("%s: no .ok file; touch %s/%s.ok first (or --dry-run)\n", it.name.c_str(), dir.c_str(), it.name.c_str()); rc = 1; continue; }
    any = true;
    string summary;
    bool ok = file_item(it, dry, summary);
    printf("%s: %s\n", it.name.c_str(), summary.c_str());
    if (!ok) rc = 1;
  }
  if (!any) printf("nothing to file: no .ok item%s\n", one.empty() ? "" : (" named " + one).c_str());
  return rc;
}
