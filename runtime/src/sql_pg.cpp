// marina runtime — std module: sql driver for PostgreSQL.
//
// A client-side implementation of the PostgreSQL wire protocol (v3.0)
// over TCP — no libpq dependency. Supports:
//   * cleartext, MD5 and SCRAM-SHA-256 password auth
//   * the extended-query protocol (Parse/Bind/Execute) so values are
//     always bound as parameters, never string-concatenated
//   * text result format, decoded by type OID
//
// URL: postgres://user:password@host:port/database  (postgresql:// also
// accepted; defaults: user "postgres", database = user, port 5432).
//
// TLS is not negotiated (the driver never sends SSLRequest); servers
// that require hostssl connections reject the session themselves.
#include "sql_backend.hpp"
#include "sql_wire.hpp"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace marina_rt {

namespace pg {

// Type OIDs we decode beyond the default string fallback.
constexpr int32_t OID_BOOL = 16;
constexpr int32_t OID_INT8 = 20;
constexpr int32_t OID_INT2 = 21;
constexpr int32_t OID_INT4 = 23;
constexpr int32_t OID_OID = 26;
constexpr int32_t OID_FLOAT4 = 700;
constexpr int32_t OID_FLOAT8 = 701;
constexpr int32_t OID_NUMERIC = 1700;

struct PgConn {
    int fd;
    std::mutex mu; // serializes statements (async queries can race)
};

struct PgMsg {
    char type = 0;
    std::string payload;
};

void pg_send(PgConn& c, char type, const std::string& body) {
    std::string buf;
    buf.reserve(body.size() + 5);
    buf.push_back(type);
    uint32_t len = (uint32_t)(body.size() + 4);
    for (int i = 0; i < 4; i++) buf.push_back((char)(len >> (24 - i * 8)));
    buf += body;
    dbsock::write_all(c.fd, buf, "pg");
}

PgMsg pg_recv(PgConn* c) {
    PgMsg m;
    uint8_t hdr[5];
    dbsock::read_full(c->fd, hdr, 5, "pg");
    m.type = (char)hdr[0];
    uint32_t len = ((uint32_t)hdr[1] << 24) | ((uint32_t)hdr[2] << 16) |
                   ((uint32_t)hdr[3] << 8) | (uint32_t)hdr[4];
    if (len < 4) {
        dbsock::close_fd(c->fd);
        rt_panic("pg: malformed message from server");
    }
    m.payload.resize(len - 4);
    if (len > 4) {
        dbsock::read_full(c->fd, reinterpret_cast<uint8_t*>(m.payload.data()), len - 4, "pg");
    }
    return m;
}

// First NUL-terminated string of `payload` starting at `*off`.
std::string pg_cstr(const std::string& payload, size_t* off) {
    size_t start = *off;
    size_t end = payload.find('\0', start);
    if (end == std::string::npos) {
        *off = payload.size();
        return payload.substr(start);
    }
    *off = end + 1;
    return payload.substr(start, end - start);
}

// ErrorResponse fields -> "SEVERITY SQLSTATE: message".
std::string pg_error_text(const PgMsg& m) {
    std::string severity = "ERROR";
    std::string code;
    std::string message;
    size_t off = 0;
    while (off < m.payload.size()) {
        char field = m.payload[off++];
        if (field == 0) break;
        std::string v = pg_cstr(m.payload, &off);
        switch (field) {
            case 'S': severity = v; break;
            case 'C': code = v; break;
            case 'M': message = v; break;
            default: break;
        }
    }
    return severity + " " + code + ": " + message;
}

// Encode an MValue as a Postgres text parameter. Returns false for NULL.
bool pg_param_text(const MValue& v, std::string* out) {
    switch (v.tag) {
        case TAG_INT:
            *out = std::to_string(v.i);
            return true;
        case TAG_FLOAT:
            *out = fmt_float(v.f);
            return true;
        case TAG_BOOL:
            *out = v.i != 0 ? "t" : "f";
            return true;
        case TAG_STRING: {
            void* obj = v.p;
            size_t len = (size_t)str_len(obj);
            const uint8_t* bytes = str_bytes(obj);
            if (len > 0 && std::memchr(bytes, 0, len) != nullptr) {
                rt_panic("pg: parameter contains a NUL byte");
            }
            out->assign(reinterpret_cast<const char*>(bytes), len);
            return true;
        }
        case TAG_VOID:
            return false;
        default:
            rt_panic("pg: cannot bind a value of tag " + std::to_string(v.tag) +
                     " (bind an integer, float, string, bool or null)");
    }
}

// Text-format column value -> MValue, decoded by the column's type OID.
MValue pg_decode_text(const std::string& s, int32_t oid) {
    switch (oid) {
        case OID_BOOL:
            return mv_bool(s == "t" || s == "true" || s == "yes" || s == "1");
        case OID_INT2:
        case OID_INT4:
        case OID_INT8:
        case OID_OID: {
            int64_t i = 0;
            if (parse_i64_strict(s, &i)) return mv_int(i);
            return mv_str_owned(utf8_lossy(reinterpret_cast<const uint8_t*>(s.data()),
                                           s.size()));
        }
        case OID_FLOAT4:
        case OID_FLOAT8:
        case OID_NUMERIC: {
            char* end = nullptr;
            double f = std::strtod(s.c_str(), &end);
            (void)end;
            MValue v = mv_void();
            v.tag = TAG_FLOAT;
            v.f = f;
            return v;
        }
        default:
            return mv_str_owned(
                utf8_lossy(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
    }
}

// CommandComplete tag -> affected-row count ("INSERT 0 3" -> 3).
int64_t pg_tag_changes(const std::string& tag) {
    size_t end = tag.size();
    while (end > 0 && isspace((unsigned char)tag[end - 1])) end--;
    size_t start = end;
    while (start > 0 && !isspace((unsigned char)tag[start - 1])) start--;
    if (start == end) return 0;
    int64_t n = 0;
    if (!parse_i64_strict(tag.substr(start, end - start), &n)) return 0;
    return n;
}

// --- SCRAM-SHA-256 (RFC 5802 / RFC 7677) -----------------------------------

struct ScramState {
    std::string client_first_bare;
    std::string client_nonce;
    std::string server_nonce;
    std::vector<uint8_t> salt;
    uint32_t iters = 0;
    std::string client_final;
    std::string expected_server_sig_b64;
};

// Consume the server-first-message and build the client-final-message.
void scram_build_final(ScramState& st, const std::string& server_first,
                       const std::string& password) {
    size_t pos = 0;
    bool got_r = false, got_s = false, got_i = false;
    std::string nonce_b64;
    while (pos < server_first.size()) {
        size_t comma = server_first.find(',', pos);
        std::string field = server_first.substr(pos, comma == std::string::npos
                                                          ? std::string::npos
                                                          : comma - pos);
        pos = comma == std::string::npos ? server_first.size() : comma + 1;
        if (field.size() < 2 || field[1] != '=') continue;
        char k = field[0];
        std::string v = field.substr(2);
        if (k == 'r') {
            st.server_nonce = v;
            got_r = true;
        } else if (k == 's') {
            nonce_b64 = v;
            got_s = true;
        } else if (k == 'i') {
            uint64_t parsed = 0;
            for (char ch : v) {
                if (ch < '0' || ch > '9') break;
                parsed = parsed * 10 + (uint64_t)(ch - '0');
            }
            st.iters = (uint32_t)parsed;
            got_i = true;
        }
    }
    if (!got_r || !got_s || !got_i || st.iters == 0) {
        rt_panic("pg: malformed SCRAM server-first-message");
    }
    if (st.server_nonce.size() <= st.client_nonce.size() ||
        st.server_nonce.compare(0, st.client_nonce.size(), st.client_nonce) != 0) {
        rt_panic("pg: SCRAM server nonce does not extend the client nonce");
    }
    st.salt = dbhash::b64_dec(nonce_b64);
    if (st.salt.empty()) {
        rt_panic("pg: malformed SCRAM salt");
    }

    uint8_t salted[32];
    dbhash::pbkdf2_sha256(password, st.salt.data(), st.salt.size(), st.iters, salted);

    const std::string client_key_msg = "Client Key";
    uint8_t client_key[32];
    dbhash::hmac_sha256(salted, 32,
                        reinterpret_cast<const uint8_t*>(client_key_msg.data()),
                        client_key_msg.size(), client_key);
    uint8_t stored_key[32];
    dbhash::sha256(client_key, 32, stored_key);

    std::string without_proof = "c=biws,r=" + st.server_nonce;
    std::string auth_message =
        st.client_first_bare + "," + server_first + "," + without_proof;

    const std::string server_key_msg = "Server Key";
    uint8_t server_key[32];
    dbhash::hmac_sha256(salted, 32,
                        reinterpret_cast<const uint8_t*>(server_key_msg.data()),
                        server_key_msg.size(), server_key);
    uint8_t server_sig[32];
    dbhash::hmac_sha256(server_key, 32,
                        reinterpret_cast<const uint8_t*>(auth_message.data()),
                        auth_message.size(), server_sig);
    st.expected_server_sig_b64 =
        dbhash::b64_enc(server_sig, 32);

    uint8_t client_sig[32];
    dbhash::hmac_sha256(stored_key, 32,
                        reinterpret_cast<const uint8_t*>(auth_message.data()),
                        auth_message.size(), client_sig);
    uint8_t proof[32];
    for (int i = 0; i < 32; i++) proof[i] = client_key[i] ^ client_sig[i];
    st.client_final = without_proof + ",p=" + dbhash::b64_enc(proof, 32);
}

// One Parse/Bind/Execute/Sync round trip. Returns false on a server
// error (message in *err_out) when `soft`; panics otherwise. Sets
// *got_rows_out when the statement returned a row description.
bool pg_run(PgConn& c, const std::string& sql, const std::vector<MValue>& binds,
            SqlRowSet* out, std::string* tag_out, bool soft, std::string* err_out,
            bool* got_rows_out = nullptr) {
    std::lock_guard<std::mutex> lk(c.mu);

    if (sql.find('\0') != std::string::npos) {
        rt_panic("pg: statement contains a NUL byte");
    }
    // marina uses '?' placeholders (like sqlite/mysql); PostgreSQL wants
    // '$1, $2, ...'. Rewrite them, skipping string literals.
    std::string q;
    q.reserve(sql.size() + 8);
    {
        int nparam = 0;
        bool in_str = false;
        for (size_t i = 0; i < sql.size(); i++) {
            char ch = sql[i];
            if (in_str) {
                q += ch;
                if (ch == '\'') {
                    if (i + 1 < sql.size() && sql[i + 1] == '\'') {
                        q += '\''; // '' escape inside a string literal
                        i++;
                    } else {
                        in_str = false;
                    }
                }
                continue;
            }
            if (ch == '\'') {
                in_str = true;
                q += ch;
                continue;
            }
            if (ch == '?') {
                nparam++;
                q += '$';
                q += std::to_string(nparam);
                continue;
            }
            q += ch;
        }
    }
    // Parse (unnamed statement, zero type OIDs).
    std::string parse;
    parse.push_back('\0');
    parse += q;
    parse.push_back('\0');
    parse.push_back('\0');
    parse.push_back('\0');
    pg_send(c, 'P', parse);

    // Bind (all parameters and results in text format).
    std::string bind;
    bind.push_back('\0'); // unnamed portal
    bind.push_back('\0'); // unnamed statement
    bind.push_back('\0');
    bind.push_back('\0'); // zero parameter format codes -> all text
    uint16_t n = (uint16_t)binds.size();
    bind.push_back((char)(n >> 8));
    bind.push_back((char)n);
    for (const MValue& v : binds) {
        std::string text;
        if (!pg_param_text(v, &text)) {
            const uint32_t null_len = 0xffffffff;
            uint8_t nb[4];
            nb[0] = (uint8_t)(null_len >> 24);
            nb[1] = (uint8_t)(null_len >> 16);
            nb[2] = (uint8_t)(null_len >> 8);
            nb[3] = (uint8_t)null_len;
            bind.append(reinterpret_cast<const char*>(nb), 4);
        } else {
            uint32_t len = (uint32_t)text.size();
            uint8_t lb[4];
            lb[0] = (uint8_t)(len >> 24);
            lb[1] = (uint8_t)(len >> 16);
            lb[2] = (uint8_t)(len >> 8);
            lb[3] = (uint8_t)len;
            bind.append(reinterpret_cast<const char*>(lb), 4);
            bind += text;
        }
    }
    bind.push_back('\0');
    bind.push_back('\0'); // zero result-format codes -> all text
    pg_send(c, 'B', bind);

    // Describe the portal so the server sends a RowDescription (without it
    // the extended protocol never tells us the column layout).
    std::string desc;
    desc.push_back('P'); // portal kind
    desc.push_back('\0'); // unnamed portal
    pg_send(c, 'D', desc);

    // Execute (0 rows = fetch everything) + Sync.
    std::string exec;
    exec.push_back('\0'); // unnamed portal
    exec.push_back('\0');
    exec.push_back('\0');
    exec.push_back('\0');
    exec.push_back('\0');
    pg_send(c, 'E', exec);
    pg_send(c, 'S', std::string());

    bool got_rowdesc = false;
    std::vector<int32_t> col_types;
    bool err = false;
    for (;;) {
        PgMsg m = pg_recv(&c);
        switch (m.type) {
            case 'T': { // RowDescription
                if (m.payload.size() < 2) break;
                uint16_t ncols =
                    (uint16_t)(((uint16_t)(uint8_t)m.payload[0] << 8) |
                               (uint8_t)m.payload[1]);
                size_t off = 2;
                for (uint16_t i = 0; i < ncols && off < m.payload.size(); i++) {
                    std::string name = pg_cstr(m.payload, &off);
                    // table oid(4) attnum(2) type oid(4) typlen(2) typmod(4) fmt(2)
                    if (off + 18 > m.payload.size()) break;
                    int32_t type_oid =
                        (int32_t)(((uint32_t)(uint8_t)m.payload[off + 6] << 24) |
                                  ((uint32_t)(uint8_t)m.payload[off + 7] << 16) |
                                  ((uint32_t)(uint8_t)m.payload[off + 8] << 8) |
                                  (uint32_t)(uint8_t)m.payload[off + 9]);
                    off += 18;
                    col_types.push_back(type_oid);
                    if (out != nullptr) out->col_names.push_back(name);
                }
                got_rowdesc = true;
                if (got_rows_out != nullptr) *got_rows_out = true;
                break;
            }
            case 'D': { // DataRow
                if (!got_rowdesc || out == nullptr || m.payload.size() < 2) break;
                uint16_t ncols =
                    (uint16_t)(((uint16_t)(uint8_t)m.payload[0] << 8) |
                               (uint8_t)m.payload[1]);
                size_t off = 2;
                std::vector<MValue> row;
                row.reserve(ncols);
                for (uint16_t i = 0; i < ncols && off + 4 <= m.payload.size(); i++) {
                    uint32_t len = ((uint32_t)(uint8_t)m.payload[off] << 24) |
                                   ((uint32_t)(uint8_t)m.payload[off + 1] << 16) |
                                   ((uint32_t)(uint8_t)m.payload[off + 2] << 8) |
                                   (uint32_t)(uint8_t)m.payload[off + 3];
                    off += 4;
                    if (len == 0xffffffff) {
                        row.push_back(mv_void()); // NULL
                        continue;
                    }
                    if (off + len > m.payload.size()) break;
                    int32_t oid = i < col_types.size() ? col_types[i] : 0;
                    std::string text = m.payload.substr(off, len);
                    off += len;
                    row.push_back(pg_decode_text(text, oid));
                }
                out->rows.push_back(std::move(row));
                break;
            }
            case 'C': { // CommandComplete
                if (tag_out != nullptr) {
                    size_t off = 0;
                    *tag_out = pg_cstr(m.payload, &off);
                }
                break;
            }
            case 'E': { // ErrorResponse
                if (err_out != nullptr) *err_out = pg_error_text(m);
                err = true;
                break;
            }
            case 'Z': { // ReadyForQuery — the exchange is finished
                if (err && !soft) {
                    rt_panic("pg: " + (err_out != nullptr ? *err_out
                                                          : std::string("query failed")));
                }
                return !err;
            }
            default:
                break; // '1' ParseComplete, '2' BindComplete, 'n' NoData,
                       // 'I' EmptyQueryResponse, 'S'/'K'/'N' — ignore
        }
    }
    (void)got_rowdesc;
}

// Best-effort `SELECT lastval()` after an INSERT (0 when there is no
// sequence to read).
int64_t pg_lastval(PgConn& c) {
    SqlRowSet rs;
    std::string err;
    if (!pg_run(c, "SELECT lastval()", {}, &rs, nullptr, /*soft=*/true, &err)) return 0;
    if (rs.rows.empty() || rs.rows[0].empty()) return 0;
    const MValue& v = rs.rows[0][0];
    if (v.tag == TAG_INT) return v.i;
    if (v.tag == TAG_STRING && v.p != nullptr) {
        int64_t i = 0;
        std::string s(reinterpret_cast<const char*>(str_bytes(v.p)), (size_t)str_len(v.p));
        if (parse_i64_strict(s, &i)) return i;
    }
    return 0;
}

} // namespace pg

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

void* pg_connect(const std::string& url) {
    SqlUrl u = sql_parse_url(url, "pg", /*is_pg=*/true);
    int fd = dbsock::tcp_connect(u.host, u.port, "pg");
    pg::PgConn conn{fd};

    // StartupMessage: protocol version 3.0 + user/database parameters.
    std::string body;
    body.push_back('\0');
    body.push_back('\x03');
    body.push_back('\0');
    body.push_back('\0');
    auto put = [&body](const std::string& k, const std::string& v) {
        body += k;
        body.push_back('\0');
        body += v;
        body.push_back('\0');
    };
    put("user", u.user);
    if (!u.database.empty()) put("database", u.database);
    body.push_back('\0');
    {
        std::string pkt;
        uint32_t len = (uint32_t)(body.size() + 4);
        for (int i = 0; i < 4; i++) pkt.push_back((char)(len >> (24 - i * 8)));
        pkt += body;
        dbsock::write_all(fd, pkt, "pg");
    }

    bool scram_active = false;
    pg::ScramState scram;

    for (;;) {
        pg::PgMsg m = pg::pg_recv(&conn);
        if (m.type == 'E') {
            dbsock::close_fd(fd);
            rt_panic("pg: connection failed: " + pg::pg_error_text(m));
        }
        if (m.type == 'Z') break; // ReadyForQuery — authentication complete
        if (m.type != 'R') continue; // ParameterStatus / BackendKeyData / Notice
        if (m.payload.size() < 4) {
            dbsock::close_fd(fd);
            rt_panic("pg: malformed authentication request");
        }
        uint32_t atype = ((uint32_t)(uint8_t)m.payload[0] << 24) |
                         ((uint32_t)(uint8_t)m.payload[1] << 16) |
                         ((uint32_t)(uint8_t)m.payload[2] << 8) |
                         (uint32_t)(uint8_t)m.payload[3];
        std::string rest = m.payload.substr(4);
        switch (atype) {
            case 0: // AuthenticationOk
                break;
            case 3: { // AuthenticationCleartextPassword
                std::string p = u.password;
                p.push_back('\0');
                pg::pg_send(conn, 'p', p);
                break;
            }
            case 5: { // AuthenticationMD5Password (4-byte salt in rest)
                if (rest.size() < 4) {
                    dbsock::close_fd(fd);
                    rt_panic("pg: malformed MD5 authentication request");
                }
                std::string inner = dbhash::md5_hex(
                    reinterpret_cast<const uint8_t*>(u.password.data()),
                    u.password.size());
                inner += u.user;
                std::string outer = dbhash::md5_hex(
                    reinterpret_cast<const uint8_t*>(inner.data()), inner.size());
                outer.append(rest, 0, 4); // raw salt bytes
                std::string resp = "md5" + dbhash::md5_hex(
                                              reinterpret_cast<const uint8_t*>(outer.data()),
                                              outer.size());
                resp.push_back('\0');
                pg::pg_send(conn, 'p', resp);
                break;
            }
            case 10: { // AuthenticationSASL — mechanism list
                std::string mechs = rest;
                bool has_scram = false;
                size_t off = 0;
                for (;;) {
                    std::string mech = pg::pg_cstr(mechs, &off);
                    if (mech.empty()) break;
                    if (mech == "SCRAM-SHA-256") has_scram = true;
                }
                if (!has_scram) {
                    dbsock::close_fd(fd);
                    rt_panic("pg: server requires an unsupported SASL mechanism "
                             "(only SCRAM-SHA-256 is supported)");
                }
                scram_active = true;
                scram.client_nonce = dbsock::random_hex(18);
                scram.client_first_bare = "n=" + u.user + ",r=" + scram.client_nonce;
                std::string client_first = "n,," + scram.client_first_bare;
                std::string pbody = "SCRAM-SHA-256";
                pbody.push_back('\0');
                uint32_t flen = (uint32_t)client_first.size();
                pbody.push_back((char)(flen >> 24));
                pbody.push_back((char)(flen >> 16));
                pbody.push_back((char)(flen >> 8));
                pbody.push_back((char)flen);
                pbody += client_first;
                pg::pg_send(conn, 'p', pbody);
                break;
            }
            case 11: { // AuthenticationSASLContinue — server-first-message
                if (!scram_active) {
                    dbsock::close_fd(fd);
                    rt_panic("pg: unexpected SASL continue message");
                }
                pg::scram_build_final(scram, rest, u.password);
                pg::pg_send(conn, 'p', scram.client_final);
                break;
            }
            case 12: { // AuthenticationSASLFinal — server-final-message
                if (!scram_active) {
                    dbsock::close_fd(fd);
                    rt_panic("pg: unexpected SASL final message");
                }
                if (rest.size() < 3 || rest.compare(0, 2, "v=") != 0 ||
                    rest.substr(2) != scram.expected_server_sig_b64) {
                    dbsock::close_fd(fd);
                    rt_panic("pg: SCRAM server signature mismatch");
                }
                break;
            }
            default:
                dbsock::close_fd(fd);
                rt_panic("pg: unsupported authentication method " +
                         std::to_string(atype) +
                         " (supported: cleartext, md5, SCRAM-SHA-256)");
        }
    }
    void* p = new pg::PgConn{fd};
    return p;
}

void pg_disconnect(void* p) {
    pg::PgConn* c = reinterpret_cast<pg::PgConn*>(p);
    pg::pg_send(*c, 'X', std::string()); // Terminate
    dbsock::close_fd(c->fd);
    delete c;
}

void pg_execute(void* p, const std::string& sql, const std::vector<MValue>& binds,
                int64_t* last_id, int64_t* changes) {
    pg::PgConn* c = reinterpret_cast<pg::PgConn*>(p);
    std::string tag;
    std::string err;
    bool got_rows = false;
    pg::pg_run(*c, sql, binds, /*out=*/nullptr, &tag, /*soft=*/false, &err, &got_rows);
    if (got_rows) {
        rt_panic("pg: statement returned rows (use db.query() instead): `" + sql + "`");
    }
    *changes = pg::pg_tag_changes(tag);
    *last_id = 0;
    // Best-effort lastval(): works when the INSERT consumed a sequence
    // (serial/bigserial); stays 0 otherwise.
    if (tag.rfind("INSERT", 0) == 0 || tag.rfind("MERGE", 0) == 0) {
        *last_id = pg::pg_lastval(*c);
    }
}

void pg_query(void* p, const std::string& sql, const std::vector<MValue>& binds,
              SqlRowSet* out) {
    pg::PgConn* c = reinterpret_cast<pg::PgConn*>(p);
    std::string err;
    pg::pg_run(*c, sql, binds, out, nullptr, /*soft=*/false, &err);
}

// ---------------------------------------------------------------------------
// URL parsing (shared with the mysql driver)
// ---------------------------------------------------------------------------

// Parses scheme://user:password@host:port/database. The scheme itself is
// not validated here (the driver string selects it).
SqlUrl sql_parse_url(const std::string& url, const std::string& what, bool is_pg) {
    SqlUrl u;
    std::string rest = url;
    size_t scheme = rest.find("://");
    if (scheme != std::string::npos) rest = rest.substr(scheme + 3);
    size_t q = rest.find_first_of("?#");
    if (q != std::string::npos) rest = rest.substr(0, q);
    // database = everything after the first '/'
    size_t slash = rest.find('/');
    std::string database;
    if (slash != std::string::npos) {
        database = rest.substr(slash + 1);
        rest = rest.substr(0, slash);
    }
    // userinfo (the password may contain '@', hence rfind)
    size_t at = rest.rfind('@');
    if (at != std::string::npos) {
        std::string userinfo = rest.substr(0, at);
        rest = rest.substr(at + 1);
        size_t colon = userinfo.find(':');
        if (colon == std::string::npos) {
            u.user = userinfo;
        } else {
            u.user = userinfo.substr(0, colon);
            u.password = userinfo.substr(colon + 1);
        }
    }
    // host / port, with IPv6 literals in brackets
    if (!rest.empty() && rest[0] == '[') {
        size_t rb = rest.find(']');
        if (rb == std::string::npos) {
            rt_panic(what + ": malformed URL (unclosed IPv6 literal): '" + url + "'");
        }
        u.host = rest.substr(1, rb - 1);
        if (rb + 1 < rest.size() && rest[rb + 1] == ':') {
            u.port = (uint16_t)std::atoi(rest.c_str() + rb + 2);
        }
    } else {
        size_t colon = rest.find(':');
        if (colon == std::string::npos) {
            u.host = rest;
        } else {
            u.host = rest.substr(0, colon);
            u.port = (uint16_t)std::atoi(rest.c_str() + colon + 1);
        }
    }
    if (u.port == 0) u.port = (uint16_t)(is_pg ? 5432 : 3306);
    if (u.user.empty()) u.user = is_pg ? "postgres" : "root";
    if (u.database.empty()) u.database = database;
    if (is_pg && u.database.empty()) u.database = u.user; // libpq default
    if (u.host.empty()) {
        rt_panic(what + ": URL is missing a host: '" + url + "'");
    }
    return u;
}

} // namespace marina_rt
