// CampusConnect frontend. Every notice comes from the C++ backend, which saves
// them to disk. There is no sample or demo data in the browser.
//
// Security note: hiding a button here is only for convenience. The server checks
// who is logged in, and whether they own the notice, on every request.
const API = "/api";
const TYPE_LABEL = { sell: "For sale", lend: "To borrow", lost: "Lost", found: "Found" };
const ACTION_LABEL = { sell: "Mark as sold", lend: "Mark as returned", lost: "Mark as found", found: "Mark as claimed" };

const state = { q: "", type: "", sort: "new", user: null };

const $ = (s) => document.querySelector(s);
const board = $("#board"), statusEl = $("#status"), dialog = $("#postDialog"), form = $("#postForm");

// ---------- talking to the server ----------
class ApiError extends Error {
  constructor(message, status) { super(message); this.status = status; }
}

// status 0 = the request never reached the server (it is stopped, or the network is down).
const UNREACHABLE = "Can't reach the CampusConnect server. Check that it is still running, then try again.";

async function api(path, { method = "GET", body } = {}) {
  let res;
  try {
    res = await fetch(API + path, {
      method,
      credentials: "same-origin",   // send the session cookie
      headers: body ? { "Content-Type": "application/json" } : {},
      body: body ? JSON.stringify(body) : undefined,
    });
  } catch {
    throw new ApiError(UNREACHABLE, 0);
  }
  let data = null;
  try { data = await res.json(); } catch { /* empty or non-JSON body */ }
  if (!res.ok) throw new ApiError((data && data.error) || `Request failed (${res.status})`, res.status);
  return data;
}

// Any request that comes back 401 means the session expired or was logged out elsewhere.
function handleError(err) {
  if (err.status === 401) {
    state.user = null;
    showAuth("Your session has ended. Please log in again.");
    return true;
  }
  return false;
}

// ---------- data layer ----------
async function fetchItems() {
  const params = new URLSearchParams({ q: state.q, type: state.type, sort: state.sort });
  return api(`/items?${params}`);
}

const createItem = (data) => api("/items", { method: "POST", body: data });
const closeItem = (id) => api(`/items/${id}/close`, { method: "POST" });
const deleteItem = (id) => api(`/items/${id}`, { method: "DELETE" });

// ---------- rendering ----------
function timeAgo(ts) {
  if (ts < 1e9) return "just now";
  const m = Math.floor((Date.now() / 1000 - ts) / 60);
  if (m < 1) return "just now";
  if (m < 60) return `${m} min ago`;
  if (m < 1440) return `${Math.floor(m / 60)} h ago`;
  return `${Math.floor(m / 1440)} d ago`;
}

function cardEl(item) {
  const el = document.createElement("article");
  el.className = "card";
  el.dataset.type = item.type;

  const tag = document.createElement("span");
  tag.className = "tag"; tag.textContent = `${TYPE_LABEL[item.type]} · ${item.category}`;
  const h = document.createElement("h3"); h.textContent = item.title;
  el.append(tag, h);

  if (item.type === "sell") {
    const p = document.createElement("div");
    p.className = "price"; p.textContent = `₹${item.price}`;
    el.append(p);
  }
  if (item.description) {
    const d = document.createElement("p"); d.textContent = item.description; el.append(d);
  }
  const mine = state.user && item.ownerId === state.user.id;
  const meta = document.createElement("div");
  meta.className = "meta";
  meta.textContent = `${mine ? "You" : item.owner}${item.location ? " at " + item.location : ""} · ${timeAgo(item.createdAt)}`;
  el.append(meta);

  // Only the poster or an admin gets these buttons (the server enforces the same rule).
  if (item.canClose) {
    const actions = document.createElement("div");
    actions.className = "card-actions";

    const btn = document.createElement("button");
    btn.className = "btn"; btn.textContent = ACTION_LABEL[item.type];
    btn.onclick = () => act(() => closeItem(item.id));

    const del = document.createElement("button");
    del.className = "btn danger"; del.textContent = "Delete";
    del.onclick = () => {
      if (confirm(`Delete "${item.title}"? This cannot be undone.`)) act(() => deleteItem(item.id));
    };
    actions.append(btn, del);

    if (!mine && state.user && state.user.role === "admin") {
      const note = document.createElement("span");
      note.className = "admin-note"; note.textContent = "as admin";
      actions.append(note);
    }
    el.append(actions);
  }
  return el;
}

async function act(fn) {
  try {
    await fn();
  } catch (err) {
    if (handleError(err)) return;
    statusEl.textContent = err.message;
    return;
  }
  render();
}

async function render() {
  let items;
  try {
    items = await fetchItems();
  } catch (err) {
    if (handleError(err)) return;
    // Don't leave the previous cards up: they would no longer match the chosen filter or sort.
    const e = document.createElement("div");
    e.className = "empty error-box";
    e.textContent = err.message;
    board.replaceChildren(e);
    statusEl.textContent = "";
    return;
  }
  board.replaceChildren();
  if (!items.length) {
    const e = document.createElement("div");
    e.className = "empty";
    e.textContent = state.q || state.type ? "Nothing matches yet. Try another word or post a notice." : "The board is empty. Post the first notice.";
    board.append(e);
  } else items.forEach((i) => board.append(cardEl(i)));
  statusEl.textContent = `${items.length} notice${items.length === 1 ? "" : "s"} on the board`;
}

// ---------- switching between the login screen and the board ----------
function showAuth(message = "") {
  $("#downView").hidden = true;
  $("#headerApp").hidden = true;
  $("#appView").hidden = true;
  $("#authView").hidden = false;
  $("#authError").textContent = message;
  if (dialog.open) dialog.close();
}

function showApp() {
  $("#downView").hidden = true;
  $("#authView").hidden = true;
  $("#headerApp").hidden = false;
  $("#appView").hidden = false;
  const u = state.user;
  $("#whoName").textContent = u.name;
  $("#whoRole").hidden = u.role !== "admin";
  $("#postAs").textContent = u.name;
  render();
}

function switchAuthTab(which) {
  document.querySelectorAll("[data-auth]").forEach((b) => {
    const on = b.dataset.auth === which;
    b.classList.toggle("active", on);
    b.setAttribute("aria-pressed", on);
  });
  $("#loginForm").hidden = which !== "login";
  $("#registerForm").hidden = which !== "register";
  $("#authError").textContent = "";
}

document.querySelectorAll("[data-auth]").forEach((b) => b.addEventListener("click", () => switchAuthTab(b.dataset.auth)));

async function submitAuth(e, path) {
  e.preventDefault();
  const formEl = e.target;
  const submit = formEl.querySelector("button[type=submit]");
  submit.disabled = true;
  try {
    state.user = await api(path, { method: "POST", body: Object.fromEntries(new FormData(formEl)) });
    formEl.reset();
    showApp();
  } catch (err) {
    $("#authError").textContent = err.message;
  } finally {
    submit.disabled = false;
  }
}
$("#loginForm").addEventListener("submit", (e) => submitAuth(e, "/login"));
$("#registerForm").addEventListener("submit", (e) => submitAuth(e, "/register"));

$("#logout").onclick = async () => {
  try { await api("/logout", { method: "POST" }); } catch { /* logged out either way */ }
  state.user = null;
  board.replaceChildren();
  showAuth();
};

// ---------- board events ----------
let timer;
$("#search").addEventListener("input", (e) => {
  clearTimeout(timer);
  timer = setTimeout(() => { state.q = e.target.value.trim(); render(); }, 200);   // debounce
});
$("#sort").addEventListener("change", (e) => { state.sort = e.target.value; render(); });
document.querySelectorAll(".filters .chip").forEach((c) => c.addEventListener("click", () => {
  document.querySelector(".filters .chip.active").classList.remove("active");
  c.classList.add("active");
  state.type = c.dataset.type;
  render();
}));

$("#openPost").onclick = () => { $("#formError").textContent = ""; dialog.showModal(); };
$("#cancelPost").onclick = () => dialog.close();
form.elements.type.addEventListener("change", () => {
  const sell = form.elements.type.value === "sell";
  $("#priceField").style.display = sell ? "" : "none";
});

form.addEventListener("submit", async (e) => {
  e.preventDefault();
  const data = Object.fromEntries(new FormData(form));
  data.price = data.type === "sell" ? Number(data.price) || 0 : 0;
  try {
    await createItem(data);
    form.reset();
    $("#priceField").style.display = "";
    dialog.close();
    render();
  } catch (err) {
    if (!handleError(err)) $("#formError").textContent = err.message;
  }
});

// Shown when the page cannot reach the server at all (for example, index.html opened as a file).
function showDown() {
  $("#authView").hidden = true;
  $("#headerApp").hidden = true;
  $("#appView").hidden = true;
  $("#downView").hidden = false;
}

// ---------- start ----------
// Ask the server who we are. 401 = show login. No answer at all = server not running.
async function start() {
  try {
    state.user = await api("/me");
    showApp();
  } catch (err) {
    if (err.status === 0) showDown();
    else showAuth(err.status === 401 ? "" : err.message);
  }
}
$("#retry").onclick = start;
start();
