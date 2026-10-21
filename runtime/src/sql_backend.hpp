// marina runtime — sql driver backends.
//
// Every driver speaks the same shape: connect from a URL string, run a
// statement with bound parameters, collect rows, disconnect. The
// shared dispatch and the virtual `sql_Row` classes live in sql.cpp.
//
// Row values returned in a SqlRowSet own their string payloads (the
// caller drops them after copying into result objects).
#ifndef MARINA_SQL_BACKEND_HPP
#define MARINA_SQL_BACKEND_HPP

#include "runtime_internal.hpp"

#include <string>
#include <vector>

namespace marina_rt {

struct SqlRowSet {
    std::vector<std::string> col_names;
    std::vector<std::vector<MValue>> rows;
};

// --- sqlite (libsqlite3, sql.cpp) ------------------------------------------

void* sqlite_connect(const std::string& path);
void sqlite_disconnect(void* p);
void sqlite_execute(void* p, const std::string& sql, const std::vector<MValue>& binds,
                    int64_t* last_id, int64_t* changes);
void sqlite_query(void* p, const std::string& sql, const std::vector<MValue>& binds,
                  SqlRowSet* out);

// --- postgres (native wire protocol, sql_pg.cpp) ----------------------------

void* pg_connect(const std::string& url);
void pg_disconnect(void* p);
void pg_execute(void* p, const std::string& sql, const std::vector<MValue>& binds,
                int64_t* last_id, int64_t* changes);
void pg_query(void* p, const std::string& sql, const std::vector<MValue>& binds,
              SqlRowSet* out);

// --- mysql / mariadb (native protocol, sql_mysql.cpp) -----------------------

void* my_connect(const std::string& url);
void my_disconnect(void* p);
void my_execute(void* p, const std::string& sql, const std::vector<MValue>& binds,
                int64_t* last_id, int64_t* changes);
void my_query(void* p, const std::string& sql, const std::vector<MValue>& binds,
              SqlRowSet* out);

// Parse `scheme://user:password@host:port/database` (the scheme part is
// optional). Defaults: pg -> user postgres, db = user, port 5432;
// mysql -> user root, db empty, port 3306.
struct SqlUrl {
    std::string user;
    std::string password;
    std::string host;
    uint16_t port = 0;
    std::string database;
};

SqlUrl sql_parse_url(const std::string& url, const std::string& what, bool is_pg);

} // namespace marina_rt

#endif // MARINA_SQL_BACKEND_HPP
