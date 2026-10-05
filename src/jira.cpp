// Created by Roel Van de Paar, MariaDB
// jira.cpp - Jira REST through libcurl with the PAT ~/jira uses (~/.config/mariadb-qa/jira.pat):
// the duplicate search behind tt's URLs, issue creation with the same payload log_jira_ticket.sh
// builds, comments and links. Plus the small JSON reader and writer that needs.
#include "verbs.h"
#include <curl/curl.h>

// ---- JSON ---------------------------------------------------------------------------------------
string json_escape(const string& s) {
  string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) o += fmt("\\u%04x", c);
        else o += (char)c;
    }
  }
  return o + "\"";
}
namespace {
struct JsonParser {
  const string& s;
  size_t i = 0;
  explicit JsonParser(const string& str) : s(str) {}
  void ws() { while (i < s.size() && isspace((unsigned char)s[i])) i++; }
  bool parse(JsonValue& v) {
    ws();
    if (i >= s.size()) return false;
    char c = s[i];
    if (c == '{') {
      v.type = JsonValue::Object; i++;
      ws();
      if (i < s.size() && s[i] == '}') { i++; return true; }
      for (;;) {
        ws();
        JsonValue k;
        if (i >= s.size() || s[i] != '"' || !parse(k)) return false;
        ws();
        if (i >= s.size() || s[i] != ':') return false;
        i++;
        JsonValue val;
        if (!parse(val)) return false;
        v.obj.push_back({k.str, val});
        ws();
        if (i < s.size() && s[i] == ',') { i++; continue; }
        if (i < s.size() && s[i] == '}') { i++; return true; }
        return false;
      }
    }
    if (c == '[') {
      v.type = JsonValue::Array; i++;
      ws();
      if (i < s.size() && s[i] == ']') { i++; return true; }
      for (;;) {
        JsonValue e;
        if (!parse(e)) return false;
        v.arr.push_back(e);
        ws();
        if (i < s.size() && s[i] == ',') { i++; continue; }
        if (i < s.size() && s[i] == ']') { i++; return true; }
        return false;
      }
    }
    if (c == '"') {
      v.type = JsonValue::String; i++;
      while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
          i++;
          switch (s[i]) {
            case 'n': v.str += '\n'; break;
            case 't': v.str += '\t'; break;
            case 'r': v.str += '\r'; break;
            case 'b': v.str += '\b'; break;
            case 'f': v.str += '\f'; break;
            case 'u': {
              if (i + 4 >= s.size()) return false;
              unsigned cp = (unsigned)strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16);
              i += 4;
              if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {
                unsigned lo = (unsigned)strtoul(s.substr(i + 3, 4).c_str(), nullptr, 16);
                if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
              }
              if (cp < 0x80) v.str += (char)cp;
              else if (cp < 0x800) { v.str += (char)(0xC0 | (cp >> 6)); v.str += (char)(0x80 | (cp & 0x3F)); }
              else if (cp < 0x10000) { v.str += (char)(0xE0 | (cp >> 12)); v.str += (char)(0x80 | ((cp >> 6) & 0x3F)); v.str += (char)(0x80 | (cp & 0x3F)); }
              else { v.str += (char)(0xF0 | (cp >> 18)); v.str += (char)(0x80 | ((cp >> 12) & 0x3F)); v.str += (char)(0x80 | ((cp >> 6) & 0x3F)); v.str += (char)(0x80 | (cp & 0x3F)); }
              break;
            }
            default: v.str += s[i];
          }
        } else {
          v.str += s[i];
        }
        i++;
      }
      if (i >= s.size()) return false;
      i++;
      return true;
    }
    if (s.compare(i, 4, "true") == 0) { v.type = JsonValue::Bool; v.b = true; i += 4; return true; }
    if (s.compare(i, 5, "false") == 0) { v.type = JsonValue::Bool; v.b = false; i += 5; return true; }
    if (s.compare(i, 4, "null") == 0) { v.type = JsonValue::Null; i += 4; return true; }
    size_t st = i;
    while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == '-' || s[i] == '+' || s[i] == '.' || s[i] == 'e' || s[i] == 'E')) i++;
    if (st == i) return false;
    v.type = JsonValue::Number;
    v.str = s.substr(st, i - st);
    return true;
  }
};
}  // namespace
bool json_parse(const string& text, JsonValue& v) {
  v = JsonValue();                                            // a value used before starts clean
  JsonParser p(text);
  return p.parse(v);
}
const JsonValue* JsonValue::get(const string& key) const {
  if (type != Object) return nullptr;
  for (auto& kv : obj) if (kv.first == key) return &kv.second;
  return nullptr;
}
string JsonValue::str_at(const string& path) const {
  const JsonValue* v = this;
  for (auto& k : split(path, '.')) { v = v->get(k); if (!v) return ""; }
  return v->type == String || v->type == Number ? v->str : v->type == Bool ? (v->b ? "true" : "false") : "";
}

// ---- HTTP ---------------------------------------------------------------------------------------
string jira_base() { return g_cfg.jira_url; }

string jira_pat() {
  if (const char* e = getenv("JIRA_PAT")) if (*e) return trim(e);
  return trim(read_file(g_cfg.pat_file));
}
namespace {
size_t curl_sink(char* ptr, size_t size, size_t n, void* ud) {
  ((string*)ud)->append(ptr, size * n);
  return size * n;
}
bool jira_call(const string& method, const string& url, const string& body, string& out, long* http, string* err, bool need_auth = true) {
  string pat = jira_pat();
  if (need_auth && pat.empty()) { if (err) *err = "no Jira PAT (" + g_cfg.pat_file + " or $JIRA_PAT)"; return false; }
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  CURL* c = curl_easy_init();
  if (!c) { if (err) *err = "curl init failed"; return false; }
  out.clear();
  struct curl_slist* hdr = nullptr;
  if (!pat.empty()) hdr = curl_slist_append(hdr, ("Authorization: Bearer " + pat).c_str());
  hdr = curl_slist_append(hdr, "Accept: application/json");
  if (!body.empty()) hdr = curl_slist_append(hdr, "Content-Type: application/json");
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_sink);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  string ua = string("omnium/") + OMNIUM_VERSION;
  curl_easy_setopt(c, CURLOPT_USERAGENT, ua.c_str());
  if (method == "POST") { curl_easy_setopt(c, CURLOPT_POST, 1L); curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str()); curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size()); }
  else if (method == "PUT") { curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "PUT"); curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str()); curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size()); }
  CURLcode rc = curl_easy_perform(c);
  long code = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  if (http) *http = code;
  bool ok = rc == CURLE_OK;
  if (!ok && err) *err = curl_easy_strerror(rc);
  curl_slist_free_all(hdr);
  curl_easy_cleanup(c);
  return ok;
}
}  // namespace
bool jira_get(const string& url, string& out, long* http, string* err) { return jira_call("GET", url, "", out, http, err); }
bool jira_post(const string& url, const string& json, string& out, long* http, string* err) { return jira_call("POST", url, json, out, http, err); }
bool jira_put(const string& url, const string& json, string& out, long* http, string* err) { return jira_call("PUT", url, json, out, http, err); }

// the JQL of one of tt's search URLs, already URL-encoded, run as a REST search
bool jira_search_url(const string& tt_url, vector<JiraHit>& hits, string* err) {
  size_t k = tt_url.find("?jql=");
  if (k == string::npos) { if (err) *err = "not a search URL: " + tt_url; return false; }
  string jql = tt_url.substr(k + 5);
  string body;
  long http = 0;
  if (!jira_get(jira_base() + "/rest/api/2/search?jql=" + jql + "&fields=key,summary,status,resolution&maxResults=25", body, &http, err)) return false;
  if (http != 200) { if (err) *err = fmt("HTTP %ld: %s", http, tail_lines(body, 3).c_str()); return false; }
  JsonValue v;
  if (!json_parse(body, v)) { if (err) *err = "search reply does not parse"; return false; }
  const JsonValue* issues = v.get("issues");
  if (!issues || issues->type != JsonValue::Array) return true;
  for (auto& is : issues->arr) {
    JiraHit h;
    h.key = is.str_at("key");
    h.summary = is.str_at("fields.summary");
    h.status = is.str_at("fields.status.name");
    h.resolution = is.str_at("fields.resolution.name");
    if (h.resolution.empty()) h.resolution = "-";
    hits.push_back(h);
  }
  return true;
}
bool jira_whoami(string& name, string* err) {
  string body;
  long http = 0;
  if (!jira_get(jira_base() + "/rest/api/2/myself", body, &http, err)) return false;
  if (http != 200) { if (err) *err = fmt("HTTP %ld", http); return false; }
  JsonValue v;
  if (!json_parse(body, v)) { if (err) *err = "reply does not parse"; return false; }
  name = v.str_at("name");
  if (name.empty()) name = v.str_at("key");
  return !name.empty();
}

// the create payload, field for field as log_jira_ticket.sh builds it
string jira_create_payload(const JiraFields& f) {
  auto named = [](const vector<string>& v, const string& key) {
    string o = "[";
    for (size_t i = 0; i < v.size(); i++) { if (i) o += ","; o += "{" + json_escape(key) + ":" + json_escape(v[i]) + "}"; }
    return o + "]";
  };
  auto plain = [](const vector<string>& v) {
    string o = "[";
    for (size_t i = 0; i < v.size(); i++) { if (i) o += ","; o += json_escape(v[i]); }
    return o + "]";
  };
  string j = "{\"fields\":{";
  j += "\"project\":{\"key\":" + json_escape(f.project) + "},";
  j += "\"issuetype\":{\"name\":" + json_escape(f.issuetype.empty() ? "Bug" : f.issuetype) + "},";
  j += "\"summary\":" + json_escape(f.summary) + ",";
  j += "\"description\":" + json_escape(f.description);
  if (!f.affects.empty()) j += ",\"versions\":" + named(f.affects, "name");
  if (!f.fix.empty()) j += ",\"fixVersions\":" + named(f.fix, "name");
  if (!f.components.empty()) j += ",\"components\":" + named(f.components, "name");
  if (!f.labels.empty()) j += ",\"labels\":" + plain(f.labels);
  if (!f.es_versions.empty()) j += ",\"customfield_13204\":" + plain(f.es_versions);
  if (!f.priority.empty()) j += ",\"priority\":{\"name\":" + json_escape(f.priority) + "}";
  if (!f.assignee.empty()) j += ",\"assignee\":{\"name\":" + json_escape(f.assignee) + "}";
  if (!f.security_id.empty()) j += ",\"security\":{\"id\":" + json_escape(f.security_id) + "}";
  j += "}}";
  return j;
}
bool jira_create(const JiraFields& f, string& key, string* err) {
  string body;
  long http = 0;
  if (!jira_post(jira_base() + "/rest/api/2/issue", jira_create_payload(f), body, &http, err)) return false;
  if (http != 201) { if (err) *err = fmt("HTTP %ld: %s", http, body.c_str()); return false; }
  JsonValue v;
  if (!json_parse(body, v)) { if (err) *err = "create reply does not parse: " + body; return false; }
  key = v.str_at("key");
  return !key.empty();
}
bool jira_comment(const string& key, const string& text, string* err) {
  string body;
  long http = 0;
  if (!jira_post(jira_base() + "/rest/api/2/issue/" + key + "/comment", "{\"body\":" + json_escape(text) + "}", body, &http, err)) return false;
  if (http != 201) { if (err) *err = fmt("HTTP %ld: %s", http, body.c_str()); return false; }
  return true;
}
bool jira_link(const string& key, const string& other, const string& type, string* err) {
  string body;
  long http = 0;
  string j = "{\"type\":{\"name\":" + json_escape(type.empty() ? "Relates" : type) + "},\"inwardIssue\":{\"key\":" + json_escape(key) +
             "},\"outwardIssue\":{\"key\":" + json_escape(other) + "}}";
  if (!jira_post(jira_base() + "/rest/api/2/issueLink", j, body, &http, err)) return false;
  if (http != 201) { if (err) *err = fmt("HTTP %ld: %s", http, body.c_str()); return false; }
  return true;
}
// the names Jira knows for a project: versions or components
bool jira_project_names(const string& project, const string& what, vector<string>& names, string* err) {
  string body;
  long http = 0;
  if (!jira_get(jira_base() + "/rest/api/2/project/" + project + "/" + what, body, &http, err)) return false;
  if (http != 200) { if (err) *err = fmt("HTTP %ld", http); return false; }
  JsonValue v;
  if (!json_parse(body, v) || v.type != JsonValue::Array) { if (err) *err = "reply does not parse"; return false; }
  for (auto& e : v.arr) { string n = e.str_at("name"); if (!n.empty()) names.push_back(n); }
  return true;
}

// omnium jira whoami | search <uid> | versions [PROJECT] | components [PROJECT]
int cmd_jira(const Args& a) {
  string err;
  if (a.empty() || a[0] == "whoami") {
    string me;
    if (!jira_whoami(me, &err)) { printf("not authenticated: %s\n", err.c_str()); return 1; }
    printf("%s\n", me.c_str());
    return 0;
  }
  if (a[0] == "search") {
    if (a.size() < 2) { fprintf(stderr, "jira search <uid>\n"); return 2; }
    for (auto& u : kb_jira_urls(a[1])) {
      vector<JiraHit> hits;
      printf("== %s\n", u.c_str());
      if (!jira_search_url(u, hits, &err)) { printf("search failed: %s\n", err.c_str()); continue; }
      for (auto& h : hits) printf("%s\t%s\t%s\t%s\n", h.key.c_str(), h.status.c_str(), h.resolution.c_str(), h.summary.c_str());
      if (hits.empty()) printf("no hits\n");
    }
    return 0;
  }
  if (a[0] == "versions" || a[0] == "components") {
    vector<string> names;
    string project = a.size() > 1 ? a[1] : "MDEV";
    if (!jira_project_names(project, a[0], names, &err)) { printf("failed: %s\n", err.c_str()); return 1; }
    for (auto& n : names) printf("%s\n", n.c_str());
    return 0;
  }
  fprintf(stderr, "omnium jira whoami | search <uid> | versions [PROJECT] | components [PROJECT]\n");
  return 2;
}
