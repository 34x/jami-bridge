#include "api_docs.h"

namespace jami {
namespace docs {

const std::string& index_html() {
    static const std::string html = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Jami SDK — REST API</title>
<style>
  :root{--bg:#0d1117;--card:#161b22;--border:#30363d;--text:#c9d1d9;--heading:#f0f6fc;--link:#58a6ff;--green:#3fb950;--orange:#d29922;--red:#f85149;--blue:#58a6ff;--purple:#bc8cff}
  *{margin:0;padding:0;box-sizing:border-box}
  body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Helvetica,Arial,sans-serif;background:var(--bg);color:var(--text);line-height:1.6;padding:2rem;max-width:900px;margin:0 auto}
  h1{color:var(--heading);font-size:2rem;margin-bottom:.25rem}h1 small{font-size:.7em;color:var(--green);font-weight:normal}
  .subtitle{color:var(--text);opacity:.7;margin-bottom:2rem}
  .ep{background:var(--card);border:1px solid var(--border);border-radius:8px;margin-bottom:.6rem;overflow:hidden}
  .ep-h{display:flex;align-items:center;gap:.75rem;padding:.6rem 1rem;cursor:pointer}
  .ep-h:hover{background:rgba(88,166,255,.06)}
  .m{font-weight:700;font-size:.75rem;padding:.15rem .5rem;border-radius:4px;min-width:50px;text-align:center}
  .m-GET{background:rgba(63,185,80,.15);color:var(--green)}.m-POST{background:rgba(88,166,255,.15);color:var(--blue)}.m-DELETE{background:rgba(248,81,73,.15);color:var(--red)}
  .p{font-family:'SF Mono',Consolas,monospace;color:var(--heading);font-size:.88rem}
  .d{color:var(--text);opacity:.65;font-size:.82rem;margin-left:auto}
  .ep-b{display:none;padding:0 1rem .6rem;border-top:1px solid var(--border)}.ep-b.open{display:block}
  .ep-b table{width:100%;border-collapse:collapse;margin-top:.4rem}
  .ep-b th{text-align:left;color:var(--heading);font-size:.78rem;padding:.2rem .5rem;border-bottom:1px solid var(--border)}
  .ep-b td{padding:.3rem .5rem;font-size:.82rem}.ep-b td:first-child{font-family:'SF Mono',Consolas,monospace;color:var(--purple)}
  .try{margin-top:.4rem}.try code{background:rgba(88,166,255,.1);color:var(--link);padding:.15rem .35rem;border-radius:3px;font-size:.78rem}
  .sec{color:var(--heading);font-size:1.1rem;font-weight:600;margin:1.5rem 0 .5rem;padding-bottom:.3rem;border-bottom:1px solid var(--border)}
</style>
</head>
<body>
<h1>Jami SDK <small>REST API</small></h1>
<p class="subtitle">Programmable Jami messaging — self-contained, no DBus required</p>

<div class="sec">System</div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-GET">GET</span><span class="p">/api/ping</span><span class="d">Health check</span></div><div class="ep-b"></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-GET">GET</span><span class="p">/api/version</span><span class="d">Version and build info</span></div><div class="ep-b"></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-GET">GET</span><span class="p">/api/stats</span><span class="d">Runtime statistics</span></div><div class="ep-b"><table><tr><th>Field</th><th>Type</th><th>Description</th></tr><tr><td>uptime_seconds</td><td>integer</td><td>Seconds since SDK started</td></tr><tr><td>messages_received</td><td>integer</td><td>Total messages received</td></tr><tr><td>messages_sent</td><td>integer</td><td>Total messages sent</td></tr><tr><td>hook_invocations</td><td>integer</td><td>Hook commands invoked</td></tr><tr><td>hook_replies</td><td>integer</td><td>Successful hook replies sent</td></tr><tr><td>hook_timeouts</td><td>integer</td><td>Hooks that timed out</td></tr><tr><td>hook_errors</td><td>integer</td><td>Hooks that exited with error</td></tr></table></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-POST">POST</span><span class="p">/api/shutdown</span><span class="d">Shut down the server</span></div><div class="ep-b"><p>Gracefully stops the server. The process exits after sending the response.</p></div></div>

<div class="sec">Accounts</div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-GET">GET</span><span class="p">/api/accounts</span><span class="d">List accounts</span></div><div class="ep-b"></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-POST">POST</span><span class="p">/api/accounts</span><span class="d">Create account</span></div><div class="ep-b"><table><tr><th>Param</th><th>Type</th><th>Description</th></tr><tr><td>alias</td><td>string</td><td>Account alias</td></tr><tr><td>password</td><td>string</td><td>Account password (optional)</td></tr></table></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-GET">GET</span><span class="p">/api/accounts/:id</span><span class="d">Account details</span></div><div class="ep-b"></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-GET">GET</span><span class="p">/api/accounts/:id/status</span><span class="d">Registration status</span></div><div class="ep-b"></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-DELETE">DELETE</span><span class="p">/api/accounts/:id</span><span class="d">Remove account</span></div><div class="ep-b"></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-POST">POST</span><span class="p">/api/accounts/:id/export</span><span class="d">Export account archive</span></div><div class="ep-b"><table><tr><th>Param</th><th>Type</th><th>Description</th></tr><tr><td>path</td><td>string</td><td>Export file path</td></tr><tr><td>password</td><td>string</td><td>Encryption password</td></tr></table></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-POST">POST</span><span class="p">/api/accounts/import</span><span class="d">Import account archive</span></div><div class="ep-b"><table><tr><th>Param</th><th>Type</th><th>Description</th></tr><tr><td>path</td><td>string</td><td>Archive file path</td></tr><tr><td>password</td><td>string</td><td>Decryption password</td></tr></table></div></div>

<div class="sec">Conversations</div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-GET">GET</span><span class="p">/api/accounts/:id/conversations</span><span class="d">List conversations</span></div><div class="ep-b"></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-POST">POST</span><span class="p">/api/accounts/:id/conversations</span><span class="d">Create conversation</span></div><div class="ep-b"><table><tr><th>Param</th><th>Type</th><th>Description</th></tr><tr><td>title</td><td>string</td><td>Room title (optional)</td></tr></table></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-GET">GET</span><span class="p">/api/accounts/:id/conversations/:conv</span><span class="d">Conversation info &amp; members</span></div><div class="ep-b"></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-DELETE">DELETE</span><span class="p">/api/accounts/:id/conversations/:conv</span><span class="d">Leave conversation</span></div><div class="ep-b"></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-POST">POST</span><span class="p">/api/accounts/:id/conversations/:conv/invite</span><span class="d">Invite member</span></div><div class="ep-b"><table><tr><th>Param</th><th>Type</th><th>Description</th></tr><tr><td>uri</td><td>string</td><td>Jami URI to invite</td></tr></table></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-POST">POST</span><span class="p">/api/accounts/:id/conversations/:conv/remove</span><span class="d">Remove member</span></div><div class="ep-b"><table><tr><th>Param</th><th>Type</th><th>Description</th></tr><tr><td>uri</td><td>string</td><td>Jami URI to remove</td></tr></table></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-POST">POST</span><span class="p">/api/accounts/:id/conversations/:conv/update</span><span class="d">Update conversation info</span></div><div class="ep-b"><table><tr><th>Param</th><th>Type</th><th>Description</th></tr><tr><td>title</td><td>string</td><td>New title</td></tr><tr><td>description</td><td>string</td><td>New description</td></tr></table></div></div>

<div class="sec">Messaging</div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-POST">POST</span><span class="p">/api/accounts/:id/conversations/:conv/messages</span><span class="d">Send message</span></div><div class="ep-b"><table><tr><th>Param</th><th>Type</th><th>Description</th></tr><tr><td>body</td><td>string</td><td>Message text</td></tr><tr><td>parent_id</td><td>string</td><td>Reply-to message ID (optional)</td></tr></table></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-GET">GET</span><span class="p">/api/accounts/:id/conversations/:conv/messages</span><span class="d">Load messages</span></div><div class="ep-b"><p>Query params: <code>from</code> (message ID to start from), <code>count</code> (default: 64)</p></div></div>

<div class="sec">Conversation Requests</div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-GET">GET</span><span class="p">/api/accounts/:id/requests</span><span class="d">List conversation requests</span></div><div class="ep-b"></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-POST">POST</span><span class="p">/api/accounts/:id/requests/:conv/accept</span><span class="d">Accept request</span></div><div class="ep-b"></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-POST">POST</span><span class="p">/api/accounts/:id/requests/:conv/decline</span><span class="d">Decline request</span></div><div class="ep-b"></div></div>

<div class="sec">Contacts &amp; Trust</div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-GET">GET</span><span class="p">/api/accounts/:id/contacts</span><span class="d">List contacts</span></div><div class="ep-b"></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-POST">POST</span><span class="p">/api/accounts/:id/contacts</span><span class="d">Add contact</span></div><div class="ep-b"><table><tr><th>Param</th><th>Type</th><th>Description</th></tr><tr><td>uri</td><td>string</td><td>Jami URI to add</td></tr></table></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-DELETE">DELETE</span><span class="p">/api/accounts/:id/contacts/:uri</span><span class="d">Remove contact</span></div><div class="ep-b"><p>Query param: <code>ban=true</code> to also ban.</p></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-POST">POST</span><span class="p">/api/accounts/:id/trust-request</span><span class="d">Send trust request</span></div><div class="ep-b"><table><tr><th>Param</th><th>Type</th><th>Description</th></tr><tr><td>uri</td><td>string</td><td>Jami URI</td></tr></table></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-GET">GET</span><span class="p">/api/accounts/:id/trust-requests</span><span class="d">List trust requests</span></div><div class="ep-b"></div></div>

<div class="sec">Other</div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-GET">GET</span><span class="p">/api/accounts/:id/lookup/:name</span><span class="d">Look up a registered name</span></div><div class="ep-b"></div></div>
<div class="ep"><div class="ep-h" onclick="t(this)"><span class="m m-POST">POST</span><span class="p">/api/accounts/:id/active</span><span class="d">Enable/disable account</span></div><div class="ep-b"><table><tr><th>Param</th><th>Type</th><th>Description</th></tr><tr><td>active</td><td>boolean</td><td>true to activate, false to deactivate</td></tr></table></div></div>

<footer><a href="/api/openapi.json" style="color:var(--link)">OpenAPI 3.0 JSON</a> · jami-sdk v0.1.0</footer>
<script>function t(el){el.nextElementSibling.classList.toggle('open');}</script>
</body>
</html>)rawliteral";
    return html;
}

const std::string& openapi_json() {
    static const std::string spec = R"rawliteral({
  "openapi": "3.0.3",
  "info": { "title": "Jami SDK", "version": "0.1.0", "description": "Programmable Jami messaging via HTTP REST API" },
  "servers": [ { "url": "http://localhost:8090" } ],
  "paths": {
    "/api/ping": { "get": { "summary": "Health check", "responses": { "200": { "description": "OK" } } } },
    "/api/version": { "get": { "summary": "Version and build info", "responses": { "200": { "description": "Version info" } } } },
    "/api/stats": { "get": { "summary": "Runtime statistics (uptime, message counts, hook stats)", "responses": { "200": { "description": "Statistics" } } } },
    "/api/shutdown": { "post": { "summary": "Shut down the server", "responses": { "200": { "description": "Shutting down" } } } },
    "/api/accounts": {
      "get": { "summary": "List accounts", "responses": { "200": { "description": "Account list" } } },
      "post": { "summary": "Create account", "requestBody": { "content": { "application/json": { "schema": { "type": "object", "properties": { "alias": { "type": "string" }, "password": { "type": "string" } } } } } }, "responses": { "200": { "description": "Account created" } } }
    },
    "/api/accounts/{id}": {
      "get": { "summary": "Account details", "parameters": [ { "name": "id", "in": "path" } ], "responses": { "200": { "description": "Account details" } } },
      "delete": { "summary": "Remove account", "parameters": [ { "name": "id", "in": "path" } ], "responses": { "200": { "description": "Account removed" } } }
    },
    "/api/accounts/{id}/status": { "get": { "summary": "Registration status", "parameters": [ { "name": "id", "in": "path" } ], "responses": { "200": { "description": "Volatile status" } } } },
    "/api/accounts/{id}/export": { "post": { "summary": "Export account archive", "parameters": [ { "name": "id", "in": "path" } ], "requestBody": { "content": { "application/json": { "schema": { "type": "object", "properties": { "path": { "type": "string" }, "password": { "type": "string" } } } } } }, "responses": { "200": { "description": "Account exported" } } } },
    "/api/accounts/import": { "post": { "summary": "Import account archive", "requestBody": { "content": { "application/json": { "schema": { "type": "object", "properties": { "path": { "type": "string" }, "password": { "type": "string" } } } } } }, "responses": { "200": { "description": "Account imported" } } } },
    "/api/accounts/{id}/conversations": {
      "get": { "summary": "List conversations", "parameters": [ { "name": "id", "in": "path" } ], "responses": { "200": { "description": "Conversation list" } } },
      "post": { "summary": "Create conversation", "parameters": [ { "name": "id", "in": "path" } ], "requestBody": { "content": { "application/json": { "schema": { "type": "object", "properties": { "title": { "type": "string" } } } } } }, "responses": { "200": { "description": "Conversation created" } } }
    },
    "/api/accounts/{id}/conversations/{conv}": {
      "get": { "summary": "Conversation info and members", "parameters": [ { "name": "id", "in": "path" }, { "name": "conv", "in": "path" } ], "responses": { "200": { "description": "Info + members" } } },
      "delete": { "summary": "Leave conversation", "parameters": [ { "name": "id", "in": "path" }, { "name": "conv", "in": "path" } ], "responses": { "200": { "description": "Left conversation" } } }
    },
    "/api/accounts/{id}/conversations/{conv}/messages": {
      "get": { "summary": "Load messages", "parameters": [ { "name": "id", "in": "path" }, { "name": "conv", "in": "path" }, { "name": "from", "in": "query" }, { "name": "count", "in": "query" } ], "responses": { "200": { "description": "Message list" } } },
      "post": { "summary": "Send message", "parameters": [ { "name": "id", "in": "path" }, { "name": "conv", "in": "path" } ], "requestBody": { "content": { "application/json": { "schema": { "type": "object", "properties": { "body": { "type": "string" }, "parent_id": { "type": "string" } } } } } }, "responses": { "200": { "description": "Message sent" } } }
    },
    "/api/accounts/{id}/conversations/{conv}/invite": { "post": { "summary": "Invite member", "parameters": [ { "name": "id", "in": "path" }, { "name": "conv", "in": "path" } ], "requestBody": { "content": { "application/json": { "schema": { "type": "object", "properties": { "uri": { "type": "string" } } } } } }, "responses": { "200": { "description": "Member invited" } } } },
    "/api/accounts/{id}/conversations/{conv}/remove": { "post": { "summary": "Remove member", "parameters": [ { "name": "id", "in": "path" }, { "name": "conv", "in": "path" } ], "requestBody": { "content": { "application/json": { "schema": { "type": "object", "properties": { "uri": { "type": "string" } } } } } }, "responses": { "200": { "description": "Member removed" } } } },
    "/api/accounts/{id}/conversations/{conv}/update": { "post": { "summary": "Update conversation info", "parameters": [ { "name": "id", "in": "path" }, { "name": "conv", "in": "path" } ], "responses": { "200": { "description": "Updated" } } } },
    "/api/accounts/{id}/requests": { "get": { "summary": "List conversation requests", "parameters": [ { "name": "id", "in": "path" } ], "responses": { "200": { "description": "Request list" } } } },
    "/api/accounts/{id}/requests/{conv}/accept": { "post": { "summary": "Accept request", "parameters": [ { "name": "id", "in": "path" }, { "name": "conv", "in": "path" } ], "responses": { "200": { "description": "Accepted" } } } },
    "/api/accounts/{id}/requests/{conv}/decline": { "post": { "summary": "Decline request", "parameters": [ { "name": "id", "in": "path" }, { "name": "conv", "in": "path" } ], "responses": { "200": { "description": "Declined" } } } },
    "/api/accounts/{id}/contacts": { "get": { "summary": "List contacts", "parameters": [ { "name": "id", "in": "path" } ], "responses": { "200": { "description": "Contact list" } } }, "post": { "summary": "Add contact", "parameters": [ { "name": "id", "in": "path" } ], "requestBody": { "content": { "application/json": { "schema": { "type": "object", "properties": { "uri": { "type": "string" } } } } } }, "responses": { "200": { "description": "Contact added" } } } },
    "/api/accounts/{id}/contacts/{uri}": { "delete": { "summary": "Remove contact", "parameters": [ { "name": "id", "in": "path" }, { "name": "uri", "in": "path" } ], "responses": { "200": { "description": "Contact removed" } } } },
    "/api/accounts/{id}/trust-request": { "post": { "summary": "Send trust request", "parameters": [ { "name": "id", "in": "path" } ], "requestBody": { "content": { "application/json": { "schema": { "type": "object", "properties": { "uri": { "type": "string" } } } } } }, "responses": { "200": { "description": "Request sent" } } } },
    "/api/accounts/{id}/trust-requests": { "get": { "summary": "List trust requests", "parameters": [ { "name": "id", "in": "path" } ], "responses": { "200": { "description": "Trust request list" } } } },
    "/api/accounts/{id}/lookup/{name}": { "get": { "summary": "Look up name", "parameters": [ { "name": "id", "in": "path" }, { "name": "name", "in": "path" } ], "responses": { "200": { "description": "Lookup requested" } } } },
    "/api/accounts/{id}/active": { "post": { "summary": "Activate/deactivate account", "parameters": [ { "name": "id", "in": "path" } ], "requestBody": { "content": { "application/json": { "schema": { "type": "object", "properties": { "active": { "type": "boolean" } } } } } }, "responses": { "200": { "description": "Account activation state changed" } } } }
  }
})rawliteral";
    return spec;
}

} // namespace docs
} // namespace jami