// CampusConnect frontend. Talks to the C++ backend; if it is not running,
// falls back to in-browser sample data so the UI can still be demoed.
const API = location.protocol === "file:" ? "http://localhost:8080/api" : "/api";
const TYPE_LABEL = { sell: "For sale", lend: "To borrow", lost: "Lost", found: "Found" };
const ACTION_LABEL = { sell: "Mark as sold", lend: "Mark as returned", lost: "Mark as found", found: "Mark as claimed" };

const state = { q: "", type: "", sort: "new", offline: false };
let offlineItems = [
  { id: 1, type: "sell", title: "Engineering Mathematics Vol 2", description: "Barely used, no markings", category: "Books", location: "Hostel B", owner: "Riya", price: 250, createdAt: 3 },
  { id: 2, type: "lend", title: "Scientific Calculator fx-991", description: "Lend for exams, return in 3 days", category: "Electronics", location: "Library", owner: "Aman", price: 0, createdAt: 2 },
  { id: 3, type: "lost", title: "Black wallet", description: "Lost near canteen, has college ID", category: "ID or wallet", location: "Canteen", owner: "Kabir", price: 0, createdAt: 1 },
];

const $ = (s) => document.querySelector(s);
const board = $("#board"), statusEl = $("#status"), dialog = $("#postDialog"), form = $("#postForm");

// ---------- data layer ----------
async function fetchItems() {
  const params = new URLSearchParams({ q: state.q, type: state.type, sort: state.sort });
  try {
    const res = await fetch(`${API}/items?${params}`);
    if (!res.ok) throw new Error(res.status);
    state.offline = false;
    return await res.json();
  } catch {
    state.offline = true;
    const words = state.q.toLowerCase().split(/\s+/).filter(Boolean);
    let list = offlineItems.filter((i) =>
      (!state.type || i.type === state.type) &&
      words.every((w) => i.title.toLowerCase().split(/\s+/).some((t) => t.startsWith(w))));
    if (state.sort === "cheap") list.sort((a, b) => a.price - b.price);
    else if (state.sort === "dear") list.sort((a, b) => b.price - a.price);
    else list.sort((a, b) => b.createdAt - a.createdAt);
    return list;
  }
}

async function createItem(data) {
  if (!state.offline) {
    const res = await fetch(`${API}/items`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(data) });
    if (!res.ok) throw new Error((await res.json()).error || "Could not post");
    return;
  }
  offlineItems.push({ ...data, id: Date.now(), createdAt: Date.now() });
}

async function closeItem(id) {
  if (!state.offline) await fetch(`${API}/items/${id}/close`, { method: "POST" });
  else offlineItems = offlineItems.filter((i) => i.id !== id);
}

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
  const meta = document.createElement("div");
  meta.className = "meta";
  meta.textContent = `${item.owner}${item.location ? " at " + item.location : ""} · ${timeAgo(item.createdAt)}`;
  el.append(meta);

  const btn = document.createElement("button");
  btn.className = "btn"; btn.textContent = ACTION_LABEL[item.type];
  btn.onclick = async () => { await closeItem(item.id); render(); };
  el.append(btn);
  return el;
}

async function render() {
  const items = await fetchItems();
  board.replaceChildren();
  if (!items.length) {
    const e = document.createElement("div");
    e.className = "empty";
    e.textContent = state.q || state.type ? "Nothing matches yet. Try another word or post a notice." : "The board is empty. Post the first notice.";
    board.append(e);
  } else items.forEach((i) => board.append(cardEl(i)));
  statusEl.textContent = state.offline
    ? "Backend not reachable. Showing sample notices stored in this browser."
    : `${items.length} notice${items.length === 1 ? "" : "s"} on the board`;
}

// ---------- events ----------
let timer;
$("#search").addEventListener("input", (e) => {
  clearTimeout(timer);
  timer = setTimeout(() => { state.q = e.target.value.trim(); render(); }, 200);   // debounce
});
$("#sort").addEventListener("change", (e) => { state.sort = e.target.value; render(); });
document.querySelectorAll(".chip").forEach((c) => c.addEventListener("click", () => {
  document.querySelector(".chip.active").classList.remove("active");
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
    $("#formError").textContent = err.message;
  }
});

render();
