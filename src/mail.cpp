// Created by Roel Van de Paar, MariaDB
// mail.cpp - one mail per new inbox item, when EMAIL is set. This box has no local mail program, so
// omnium talks SMTP to the recipient domain's MX itself (libcurl, STARTTLS where the MX offers it).
// A Date and a Message-ID are required by the big providers, so both are always set.
#include "verbs.h"
#include <curl/curl.h>
#include <resolv.h>
#include <netdb.h>

namespace {
struct Reader { string text; size_t pos = 0; };
size_t read_cb(char* buf, size_t size, size_t n, void* userp) {
  Reader* r = (Reader*)userp;
  size_t want = size * n;
  size_t have = r->text.size() - r->pos;
  size_t take = have < want ? have : want;
  if (take) { memcpy(buf, r->text.data() + r->pos, take); r->pos += take; }
  return take;
}
string hostname_of() {
  char h[256] = {0};
  if (gethostname(h, sizeof(h) - 1) != 0) return "localhost";
  return h;
}
string rfc_date() {
  time_t t = time(nullptr);
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[64];
  strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S %z", &tmv);
  return buf;
}
}  // namespace

// The answer section of a DNS reply, read by hand: the header, the question skipped, then each
// record's name, type, class, TTL and data; an MX record's data is the preference and the host.
// Only dn_expand is used, which every resolver has (Cygwin's has no ns_* parsing calls).
bool mx_parse(const unsigned char* answer, int len, vector<string>& hosts) {
  hosts.clear();
  if (len < 12) return false;
  const unsigned char* end = answer + len;
  auto u16 = [](const unsigned char* p) { return (int)((p[0] << 8) | p[1]); };
  int qd = u16(answer + 4), an = u16(answer + 6);
  const unsigned char* p = answer + 12;
  char name[1025];
  for (int i = 0; i < qd; i++) {                                // the question: name, type, class
    int n = dn_expand(answer, end, p, name, sizeof(name));
    if (n < 0 || p + n + 4 > end) return false;
    p += n + 4;
  }
  vector<std::pair<int, string>> mx;
  for (int i = 0; i < an; i++) {
    int n = dn_expand(answer, end, p, name, sizeof(name));
    if (n < 0 || p + n + 10 > end) return false;
    p += n;
    int type = u16(p), rdlen = u16(p + 8);
    const unsigned char* rd = p + 10;
    if (rd + rdlen > end) return false;
    if (type == 15 && rdlen >= 3) {                             // MX: preference, then the host name
      int pref = u16(rd);
      if (dn_expand(answer, end, rd + 2, name, sizeof(name)) >= 0) mx.push_back({pref, name});
    }
    p = rd + rdlen;
  }
  std::stable_sort(mx.begin(), mx.end(), [](auto& a, auto& b) { return a.first < b.first; });
  for (auto& m : mx) hosts.push_back(m.second);
  return !hosts.empty();
}

// the MX hosts of a domain, best first; the domain itself when it has no MX
bool mail_mx(const string& domain, vector<string>& hosts, string* err) {
  hosts.clear();
  unsigned char answer[4096];
  // the length can be that of an answer larger than the buffer, which then holds its start only
  int len = std::min<int>(res_query(domain.c_str(), 1, 15, answer, sizeof(answer)), (int)sizeof(answer));   // class IN, type MX
  if (len > 0) { OMNIUM_LIB_WROTE(answer, (size_t)len); mx_parse(answer, len, hosts); }
  if (hosts.empty()) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(domain.c_str(), nullptr, &hints, &res) == 0 && res) { hosts.push_back(domain); freeaddrinfo(res); }
  }
  if (hosts.empty()) { if (err) *err = "no MX and no address for " + domain; return false; }
  return true;
}

bool mail_send(const string& to, const string& subject, const string& body, string* err, bool dry_run) {
  size_t at = to.find('@');
  if (at == string::npos) { if (err) *err = "not an address: " + to; return false; }
  string domain = to.substr(at + 1);
  string host = hostname_of();
  string from = "omnium@" + host;
  string mid = fmt("<omnium.%lld.%d@%s>", (long long)now_s(), (int)getpid(), host.c_str());
  string msg;
  msg += "Date: " + rfc_date() + "\r\n";
  msg += "Message-ID: " + mid + "\r\n";
  msg += "From: omnium <" + from + ">\r\n";
  msg += "To: <" + to + ">\r\n";
  msg += "Subject: " + subject + "\r\n";
  msg += "MIME-Version: 1.0\r\nContent-Type: text/plain; charset=UTF-8\r\n\r\n";
  for (auto& l : split_lines(body)) msg += l + "\r\n";
  if (dry_run) { printf("%s", msg.c_str()); return true; }
  vector<string> hosts;
  if (!mail_mx(domain, hosts, err)) return false;
  string last;
  for (auto& h : hosts) {
    CURL* c = curl_easy_init();
    if (!c) { if (err) *err = "curl init failed"; return false; }
    Reader r{msg, 0};
    struct curl_slist* rcpt = curl_slist_append(nullptr, to.c_str());
    string url = "smtp://" + h + ":" + std::to_string(g_cfg.smtp_port);
    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_MAIL_FROM, from.c_str());
    curl_easy_setopt(c, CURLOPT_MAIL_RCPT, rcpt);
    curl_easy_setopt(c, CURLOPT_USE_SSL, (long)CURLUSESSL_TRY);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_READFUNCTION, read_cb);
    curl_easy_setopt(c, CURLOPT_READDATA, &r);
    curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(rcpt);
    curl_easy_cleanup(c);
    if (rc == CURLE_OK) return true;
    last = string(errbuf[0] ? errbuf : curl_easy_strerror(rc)) + " (" + h + ")";
  }
  if (err) *err = last;
  return false;
}

// omnium mail <to> [--subject S] [--body-file F] [--dry-run]: the same path a new inbox item takes
int cmd_mail(const Args& a) {
  string to, subject = "omnium test", bodyfile;
  bool dry = false;
  for (size_t i = 0; i < a.size(); i++) {
    const string& s = a[i];
    auto val = [&](const string& n) -> string { if (i + 1 >= a.size()) { fprintf(stderr, "%s needs a value\n", n.c_str()); exit(2); } return a[++i]; };
    if (s == "--subject") subject = val(s);
    else if (s == "--body-file") bodyfile = val(s);
    else if (s == "--dry-run") dry = true;
    else if (s == "--mx") {
      string d = i + 1 < a.size() ? a[++i] : "";
      if (d.empty()) { fprintf(stderr, "--mx needs a domain\n"); return 2; }
      vector<string> hosts;
      string e;
      if (!mail_mx(d, hosts, &e)) { fprintf(stderr, "omnium mail: %s\n", e.c_str()); return 1; }
      for (auto& h : hosts) printf("%s\n", h.c_str());
      return 0;
    }
    else if (starts_with(s, "--")) { fprintf(stderr, "usage: omnium mail <to> [--subject S] [--body-file F] [--dry-run] [--mx <domain>]\n"); return 2; }
    else to = s;
  }
  if (to.empty()) to = g_cfg.email;
  if (to.empty()) { fprintf(stderr, "omnium mail: no address given and EMAIL is not set\n"); return 2; }
  string body = bodyfile.empty() ? "omnium mail test.\n" : read_file(bodyfile);
  string err;
  if (!mail_send(to, subject, body, &err, dry)) { fprintf(stderr, "omnium mail: %s\n", err.c_str()); return 1; }
  if (!dry) printf("sent to %s\n", to.c_str());
  return 0;
}
