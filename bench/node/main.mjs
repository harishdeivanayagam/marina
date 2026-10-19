// Node.js port of examples/todos-http-sqlite.tsm
// Requires Node >= 22.5 (built-in node:sqlite). Run: node main.mjs
import { DatabaseSync } from "node:sqlite";
import { createServer } from "node:http";

const DB_PATH = "/tmp/todos-http-sqlite.db";
const PORT = 8080;

const db = new DatabaseSync(DB_PATH);
db.exec(
  "CREATE TABLE IF NOT EXISTS todos (id INTEGER PRIMARY KEY AUTOINCREMENT, text TEXT NOT NULL, done INTEGER NOT NULL DEFAULT 0)"
);
const stmtAll = db.prepare("SELECT id, text, done FROM todos ORDER BY id");
const stmtInsert = db.prepare("INSERT INTO todos (text, done) VALUES (?, 0)");
const stmtById = db.prepare("SELECT id, text, done FROM todos WHERE id = ?");
const stmtDoneAll = db.prepare("UPDATE todos SET done = 1");

function validate(body) {
  let parsed;
  try {
    parsed = JSON.parse(body);
  } catch {
    return null;
  }
  if (typeof parsed?.text !== "string" || parsed.text.length < 4 || parsed.text.length > 20) {
    return null;
  }
  return parsed;
}

const server = createServer((req, res) => {
  const url = new URL(req.url, `http://localhost:${PORT}`);
  if (url.pathname === "/todos" || url.pathname === "/todos/") {
    if (req.method === "GET") {
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify(stmtAll.all()));
      return;
    }
    if (req.method === "POST") {
      let body = "";
      req.on("data", (c) => (body += c));
      req.on("end", () => {
        const todo = validate(body);
        if (!todo) {
          res.writeHead(400, { "Content-Type": "application/json" });
          res.end(JSON.stringify({ error: "invalid todo" }));
          return;
        }
        const result = stmtInsert.run(todo.text);
        res.writeHead(200, { "Content-Type": "application/json" });
        res.end(JSON.stringify(stmtById.all(result.lastInsertRowid)));
      });
      return;
    }
  }
  if (url.pathname === "/todos/done-all" && req.method === "POST") {
    stmtDoneAll.run();
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify(stmtAll.all()));
    return;
  }
  res.writeHead(404, { "Content-Type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(PORT, "0.0.0.0", () => {
  console.log(`Server is running on http://localhost:${PORT}/todos`);
});
