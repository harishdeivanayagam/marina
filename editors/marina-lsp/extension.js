/**
 * marina-lsp extension: minimal hand-rolled LSP client (no npm dependencies).
 *
 * Spawns the `marina-lsp` server binary, speaks JSON-RPC over stdio with
 * Content-Length framing, forwards document open/change events, and renders
 * `publishDiagnostics` + `textDocument/hover` results in the editor.
 */
const vscode = require('vscode');
const { spawn } = require('child_process');

let server = null;
let outputChannel = null;
const pending = new Map(); // id -> {resolve, reject}
let nextId = 1;

function activate(context) {
  outputChannel = vscode.window.createOutputChannel('marina-lsp');
  context.subscriptions.push(outputChannel);

  const diagnostics = vscode.languages.createDiagnosticCollection('marina');
  context.subscriptions.push(diagnostics);

  const config = vscode.workspace.getConfiguration('marina');
  const serverPath = config.get('serverPath') || 'marina-lsp';

  server = spawn(serverPath, [], { stdio: ['pipe', 'pipe', 'pipe'] });
  server.stderr.on('data', (d) => outputChannel.append(d.toString()));
  server.on('error', (err) => {
    vscode.window.showErrorMessage(
      `marina-lsp: cannot start "${serverPath}" (${err.message}). ` +
      'Install it with: cargo install --path crates/marina'
    );
  });

  // --- framing: read Content-Length framed JSON-RPC messages ---
  let buffer = Buffer.alloc(0);
  server.stdout.on('data', (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);
    while (true) {
      const sep = buffer.indexOf('\r\n\r\n');
      if (sep < 0) break;
      const header = buffer.slice(0, sep).toString();
      const m = /Content-Length: (\d+)/i.exec(header);
      if (!m) { buffer = buffer.slice(sep + 4); continue; }
      const len = parseInt(m[1], 10);
      if (buffer.length < sep + 4 + len) break;
      const body = buffer.slice(sep + 4, sep + 4 + len).toString();
      buffer = buffer.slice(sep + 4 + len);
      try { handleMessage(JSON.parse(body), diagnostics); }
      catch (e) { outputChannel.appendLine('parse error: ' + e.message); }
    }
  });

  // --- client -> server ---
  function send(msg) {
    const body = Buffer.from(JSON.stringify(msg), 'utf8');
    const header = Buffer.from(`Content-Length: ${body.length}\r\n\r\n`, 'utf8');
    server.stdin.write(Buffer.concat([header, body]));
  }
  function notify(method, params) { send({ jsonrpc: '2.0', method, params }); }
  function request(method, params) {
    return new Promise((resolve, reject) => {
      const id = nextId++;
      pending.set(id, { resolve, reject });
      send({ jsonrpc: '2.0', id, method, params });
    });
  }

  function docUri(doc) { return doc.uri.toString(); }

  function pushDocument(doc) {
    notify('textDocument/didOpen', {
      textDocument: {
        uri: docUri(doc),
        languageId: 'marina',
        version: doc.version,
        text: doc.getText(),
      },
    });
  }

  // --- initialize handshake ---
  request('initialize', {
    processId: process.pid,
    capabilities: {},
  }).then((result) => {
    notify('initialized', {});
    // open any .tsm documents already visible
    for (const editor of vscode.window.visibleTextEditors) {
      if (editor.document.languageId === 'marina') pushDocument(editor.document);
    }
  }).catch((e) => outputChannel.appendLine('initialize failed: ' + e.message));

  // --- events ---
  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument((doc) => {
      if (doc.languageId === 'marina') pushDocument(doc);
    }),
    vscode.workspace.onDidChangeTextDocument((e) => {
      const doc = e.document;
      if (doc.languageId !== 'marina') return;
      notify('textDocument/didChange', {
        textDocument: { uri: docUri(doc), version: doc.version },
        contentChanges: [{ text: doc.getText() }], // full sync
      });
    }),
    vscode.workspace.onDidCloseTextDocument((doc) => {
      notify('textDocument/didClose', {
        textDocument: { uri: docUri(doc) },
      });
      diagnostics.delete(doc.uri);
    }),
    // hover provider
    vscode.languages.registerHoverProvider('marina', {
      async provideHover(document, position) {
        const result = await request('textDocument/hover', {
          textDocument: { uri: docUri(document) },
          position: { line: position.line, character: position.character },
        });
        if (!result || !result.contents) return null;
        const value = result.contents.value || '';
        const md = new vscode.MarkdownString(value);
        md.isTrusted = false;
        return new vscode.Hover(md);
      },
    })
  );
}

function handleMessage(msg, diagnostics) {
  if (msg.id === undefined && msg.method) return; // notifications we ignore
  if (msg.method === 'textDocument/publishDiagnostics') {
    const uri = vscode.Uri.parse(msg.params.uri);
    const diags = (msg.params.diagnostics || []).map((d) => {
      const start = new vscode.Position(d.range.start.line, d.range.start.character);
      const end = new vscode.Position(d.range.end.line, d.range.end.character);
      const sev = d.severity === 2
        ? vscode.DiagnosticSeverity.Warning
        : vscode.DiagnosticSeverity.Error;
      const item = new vscode.Diagnostic(new vscode.Range(start, end), d.message, sev);
      item.source = d.source || 'marina';
      return item;
    });
    diagnostics.set(uri, diags);
    return;
  }
  if (msg.id !== undefined && pending.has(msg.id)) {
    const p = pending.get(msg.id);
    pending.delete(msg.id);
    if (msg.error) p.reject(new Error(msg.error.message));
    else p.resolve(msg.result);
  }
}

function deactivate() {
  if (server) {
    try {
      const body = Buffer.from(JSON.stringify({ jsonrpc: '2.0', id: 9999, method: 'shutdown', params: null }));
      server.stdin.write(Buffer.from(`Content-Length: ${body.length}\r\n\r\n`, 'utf8') + body.toString());
      server.stdin.end();
    } catch (_) {}
    server.kill();
    server = null;
  }
}

module.exports = { activate, deactivate };