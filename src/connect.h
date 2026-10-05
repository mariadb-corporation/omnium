// Created by Roel Van de Paar, MariaDB
// connect.h - the client library, and the one connect call every in-process client goes through
#pragma once
#include "common.h"
#include <mysql/mysql.h>

// mysql_real_connect on the endpoint: the unix socket, or TCP on 127.0.0.1 when e.tcp
bool endpoint_connect(MYSQL* m, const Endpoint& e, const char* user, const char* db, unsigned long flags);
// the MTR replay's client timeouts: the connect, and one statement's answer (longer on a sanitizer
// build); gives the answer cap back, in seconds
unsigned replay_client_options(MYSQL* m, bool san);
// the lib/plugin of a plain MariaDB build that holds the caching_sha2_password client module, or ""
string client_plugin_dir(const string& test_dir);
