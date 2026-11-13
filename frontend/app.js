// CampusConnect frontend. Every notice comes from the C++ backend, which saves
// them to disk. There is no sample or demo data in the browser.
//
// Security note: hiding a button here is only for convenience. The server checks
// who is logged in, and whether they own the notice, on every request.
const API = "/api";
const TYPE_LABEL = { sell: "For sale", lend: "To borrow", lost: "Lost", found: "Found" };
const ACTION_LABEL = { sell: "Mark as sold", lend: "Mark as returned", lost: "Mark as found", found: "Mark as claimed" };

const state = { q: "", type: "", sort: "new", user: null, items: [], total: 0, offset: 0, limit: 20 };

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
  const params = new URLSearchParams({
    q: state.q, type: state.type, sort: state.sort,
    limit: state.limit, offset: state.offset,
  });
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
    if (item.type === "lost" || item.type === "found") {
      const claimsBtn = document.createElement("button");
      claimsBtn.className = "btn";
      claimsBtn.textContent = `Claims (${item.pendingClaims || 0})`;
      claimsBtn.onclick = () => openClaims(item);
      actions.append(claimsBtn);
    }

    if (!mine && state.user && state.user.role === "admin") {
      const note = document.createElement("span");
      note.className = "admin-note"; note.textContent = "as admin";
      actions.append(note);
    }
    el.append(actions);
  } else if (item.type === "lost" || item.type === "found") {
    const claimBtn = document.createElement("button");
    claimBtn.className = "btn";
    claimBtn.textContent = item.type === "found" ? "This is mine" : "I found this";
    claimBtn.onclick = () => openClaim(item);
    el.append(claimBtn);
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

// Load one page. The server only ever keeps the best (offset + limit) notices,
// so the board never sorts itself end-to-end.
async function loadPage() {
  try {
    const data = await fetchItems();
    state.items = state.items.concat(data.items);
    state.total = data.total;
    state.offset = state.items.length;
    drawBoard();
  } catch (err) {
    if (handleError(err)) return;
    // Don't leave the previous cards up: they would no longer match the filter.
    const e = document.createElement("div");
    e.className = "empty error-box";
    e.textContent = err.message;
    board.replaceChildren(e);
    statusEl.textContent = "";
    $("#loadMore").hidden = true;
  }
}

function drawBoard() {
  board.replaceChildren();
  if (!state.items.length) {
    const e = document.createElement("div");
    e.className = "empty";
    e.textContent = state.q || state.type ? "Nothing matches yet. Try another word or post a notice." : "The board is empty. Post the first notice.";
    board.append(e);
  } else {
    state.items.forEach((item) => board.append(cardEl(item)));
  }
  statusEl.textContent = state.items.length === state.total
    ? `${state.total} notice${state.total === 1 ? "" : "s"} on the board`
    : `Showing ${state.items.length} of ${state.total} notices`;
  $("#loadMore").hidden = state.items.length >= state.total;
}

// A fresh render (new search, filter, sort, or after an action) starts at page 1.
async function render() {
  state.items = [];
  state.offset = 0;
  await loadPage();
}

$("#loadMore").onclick = () => loadPage();

// ---------- switching between the login screen and the board ----------
function showAuth(message = "") {
  $("#downView").hidden = true;
  $("#headerApp").hidden = true;
  $("#appView").hidden = true;
  $("#authView").hidden = false;
  $("#authError").textContent = message;
  $("#authNotice").textContent = "";
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
  // Phase 3: unverified accounts can browse but not post (server enforces it too).
  const unverified = u.role !== "admin" && !u.emailVerified;
  $("#verifyBanner").hidden = !unverified;
  $("#openPost").disabled = unverified;
  $("#openPost").title = unverified ? "Verify your email to post a notice" : "";
  return render();
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

$("#logoutAll").onclick = async () => {
  if (!confirm("Log out on every device? You will need to log in again here too.")) return;
  try { await api("/logout-all", { method: "POST" }); } catch { /* logged out either way */ }
  state.user = null;
  board.replaceChildren();
  showAuth();
};

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
  const type = form.elements.type.value;
  $("#priceField").style.display = type === "sell" ? "" : "none";
  const claimable = type === "lost" || type === "found";
  $("#claimFields").hidden = !claimable;
  if (!claimable) {
    form.elements.proof.value = "";
    form.elements.contact.value = "";
  }
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

// ---------- email verification / password reset (Phase 3) ----------
const forgotDialog = $("#forgotDialog"), resetDialog = $("#resetDialog");
let resetToken = "";

async function finishVerification(token) {
  try {
    await api("/verify", { method: "POST", body: { token } });
    if (state.user) {
      state.user.emailVerified = true;
      await showApp();
      statusEl.textContent = "Email verified. You can post notices now.";
    } else {
      $("#authNotice").textContent = "Email verified. Log in to continue.";
    }
  } catch (err) {
    const message = err.status === 0 ? UNREACHABLE : err.message;
    if (state.user) statusEl.textContent = message;
    else $("#authNotice").textContent = message;
  }
}

function openReset(token) {
  resetToken = token;
  $("#resetError").textContent = "";
  resetDialog.showModal();
}

$("#forgotLink").onclick = () => {
  $("#forgotError").textContent = "";
  forgotDialog.showModal();
};
$("#cancelForgot").onclick = () => forgotDialog.close();
$("#forgotForm").addEventListener("submit", async (e) => {
  e.preventDefault();
  const email = new FormData(e.target).get("email");
  try {
    await api("/forgot", { method: "POST", body: { email } });
    forgotDialog.close();
    e.target.reset();
    $("#authNotice").textContent = "If that address has an account, a reset link is on its way.";
  } catch (err) {
    $("#forgotError").textContent = err.status === 0 ? UNREACHABLE : err.message;
  }
});

$("#cancelReset").onclick = () => resetDialog.close();
$("#resetForm").addEventListener("submit", async (e) => {
  e.preventDefault();
  const form = new FormData(e.target);
  if (form.get("password") !== form.get("confirm")) {
    $("#resetError").textContent = "The two passwords do not match.";
    return;
  }
  try {
    await api("/reset", { method: "POST", body: { token: resetToken, password: form.get("password") } });
    resetDialog.close();
    e.target.reset();
    state.user = null;
    board.replaceChildren();
    showAuth();
    $("#authNotice").textContent = "Password changed. Log in with your new password.";
  } catch (err) {
    $("#resetError").textContent = err.status === 0 ? UNREACHABLE : err.message;
  }
});

$("#resendVerify").onclick = async () => {
  try {
    await api("/verify/resend", { method: "POST" });
    statusEl.textContent = "Verification link sent. Check your inbox.";
  } catch (err) {
    if (!handleError(err)) statusEl.textContent = err.status === 0 ? UNREACHABLE : err.message;
  }
};

// ---------- lost & found claims (Phase 6) ----------
const claimDialog = $("#claimDialog"), claimsDialog = $("#claimsDialog"), myClaimsDialog = $("#myClaimsDialog");
let claimTarget = null;

function openClaim(item) {
  claimTarget = item;
  $("#claimItemTitle").textContent = `${item.title} - posted by ${item.owner}`;
  $("#claimError").textContent = "";
  $("#claimForm").reset();
  claimDialog.showModal();
}
$("#cancelClaim").onclick = () => claimDialog.close();
$("#claimForm").addEventListener("submit", async (e) => {
  e.preventDefault();
  const form = new FormData(e.target);
  try {
    await api(`/items/${claimTarget.id}/claims`, {
      method: "POST",
      body: { answer: form.get("answer"), message: form.get("message"), contact: form.get("contact") },
    });
    claimDialog.close();
    statusEl.textContent = "Claim sent. The poster will decide.";
    render();
  } catch (err) {
    if (!handleError(err)) $("#claimError").textContent = err.message;
  }
});

function claimCard(claim, itemId) {
  const el = document.createElement("article");
  el.className = "claim-card";

  const head = document.createElement("div");
  head.className = "claim-head";
  const who = document.createElement("strong");
  who.textContent = claim.claimantName;
  const status = document.createElement("span");
  status.className = `claim-status ${claim.status}`;
  status.textContent = claim.status;
  head.append(who, status);
  el.append(head);

  const answer = document.createElement("p");
  answer.textContent = `Answer: ${claim.answer}`;
  el.append(answer);
  if (claim.message) {
    const message = document.createElement("p");
    message.textContent = `Message: ${claim.message}`;
    el.append(message);
  }
  const contact = document.createElement("p");
  contact.textContent = `Contact: ${claim.contact}`;
  el.append(contact);

  if (claim.status === "pending") {
    const actions = document.createElement("div");
    actions.className = "claim-actions";
    const approve = document.createElement("button");
    approve.className = "btn primary";
    approve.textContent = "Approve";
    approve.onclick = () => decideClaim(itemId, claim.id, true);
    const reject = document.createElement("button");
    reject.className = "btn danger";
    reject.textContent = "Reject";
    reject.onclick = () => decideClaim(itemId, claim.id, false);
    actions.append(approve, reject);
    el.append(actions);
  }
  return el;
}

async function loadClaims(itemId) {
  const claims = await api(`/items/${itemId}/claims`);
  const list = $("#claimsList");
  list.replaceChildren();
  if (!claims.length) {
    const empty = document.createElement("p");
    empty.textContent = "No claims yet.";
    list.append(empty);
  }
  claims.forEach((claim) => list.append(claimCard(claim, itemId)));
}

async function openClaims(item) {
  try {
    $("#claimsTitle").textContent = `Claims for "${item.title}"`;
    await loadClaims(item.id);
    claimsDialog.showModal();
  } catch (err) {
    if (!handleError(err)) statusEl.textContent = err.message;
  }
}

async function decideClaim(itemId, claimId, approve) {
  try {
    await api(`/items/${itemId}/claims/${claimId}/decide`, { method: "POST", body: { approve } });
    await loadClaims(itemId);
    render();
  } catch (err) {
    if (!handleError(err)) statusEl.textContent = err.message;
  }
}
$("#closeClaims").onclick = () => claimsDialog.close();

$("#myClaims").onclick = async () => {
  try {
    const claims = await api("/claims/mine");
    const list = $("#myClaimsList");
    list.replaceChildren();
    if (!claims.length) {
      const empty = document.createElement("p");
      empty.textContent = "You have not claimed anything yet.";
      list.append(empty);
    }
    for (const claim of claims) {
      const el = document.createElement("article");
      el.className = "claim-card";
      const head = document.createElement("div");
      head.className = "claim-head";
      const title = document.createElement("strong");
      title.textContent = claim.itemTitle;
      const status = document.createElement("span");
      status.className = `claim-status ${claim.status}`;
      status.textContent = claim.status;
      head.append(title, status);
      el.append(head);
      const by = document.createElement("p");
      by.textContent = `Posted by ${claim.ownerName}`;
      el.append(by);
      if (claim.status === "approved" && claim.ownerContact) {
        const contact = document.createElement("p");
        contact.textContent = `Contact: ${claim.ownerContact}`;
        el.append(contact);
      }
      list.append(el);
    }
    myClaimsDialog.showModal();
  } catch (err) {
    if (!handleError(err)) statusEl.textContent = err.message;
  }
};
$("#closeMyClaims").onclick = () => myClaimsDialog.close();

// ---------- start ----------
// Ask the server who we are. 401 = show login. No answer at all = server not running.
async function start() {
  // A verification or reset link arrives as ?verify=... / ?reset=... . Read it,
  // then strip it from the address bar so a refresh does not replay it.
  const params = new URLSearchParams(location.search);
  const verifyToken = params.get("verify");
  const reset = params.get("reset");
  if (verifyToken || reset) history.replaceState({}, "", "/");

  try {
    state.user = await api("/me");
    showApp();
  } catch (err) {
    if (err.status === 0) { showDown(); return; }
    showAuth(err.status === 401 ? "" : err.message);
  }

  if (verifyToken) await finishVerification(verifyToken);
  if (reset) openReset(reset);
}
$("#retry").onclick = start;
start();
