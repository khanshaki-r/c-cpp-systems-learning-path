// Per-browser study progress: a "Mark as done" button on every numbered module page, a tick in
// the sidebar for finished modules, and per-course counters on the home page.
// Progress lives in localStorage of this browser only; the page works normally without it.
(function () {
  "use strict";

  var KEY = "lp-progress-v1";

  function load() {
    try {
      var raw = window.localStorage.getItem(KEY);
      var data = raw ? JSON.parse(raw) : {};
      return data && typeof data === "object" ? data : {};
    } catch (e) {
      return {};
    }
  }

  function save(data) {
    try {
      window.localStorage.setItem(KEY, JSON.stringify(data));
    } catch (e) {
      /* storage blocked: progress just is not remembered */
    }
  }

  // Site-relative key such as "02_Modern_CPP/03_raii_the_core_idea/".
  function keyFor(url) {
    var base = new URL(document.querySelector("base") ? document.querySelector("base").href : ".", location.href);
    var root = new URL(base.href.replace(/[^/]*$/, ""), location.href);
    var siteRoot = root.pathname;
    // Walk up to the site root: it is the directory that holds the home page link (the logo).
    var logo = document.querySelector("a.md-logo");
    if (logo) siteRoot = new URL(logo.href, location.href).pathname;
    var path = new URL(url, location.href).pathname;
    if (path.indexOf(siteRoot) === 0) path = path.slice(siteRoot.length);
    return path.replace(/index\.html$/, "");
  }

  // A module page is a numbered page inside a course folder, e.g. 04_Concurrency/05_condition.../
  function isModule(key) {
    return /^\d\d_[^/]+\/(?:[^/]+\/)*\d\d_[^/]+\/$/.test(key);
  }

  function courseOf(key) {
    return key.split("/")[0];
  }

  function decorateNav(done) {
    var links = document.querySelectorAll(".md-nav__link[href]");
    for (var i = 0; i < links.length; i++) {
      var k = keyFor(links[i].getAttribute("href"));
      if (done[k]) links[i].classList.add("lp-done");
      else links[i].classList.remove("lp-done");
    }
  }

  function addButton(done) {
    var key = keyFor(location.href);
    if (!isModule(key)) return;
    var h1 = document.querySelector(".md-content__inner h1");
    if (!h1 || document.querySelector(".lp-mark")) return;
    var btn = document.createElement("button");
    btn.type = "button";
    btn.className = "lp-mark md-button";
    function render() {
      var isDone = !!done[key];
      btn.textContent = isDone ? "✓ Done (click to undo)" : "Mark as done";
      btn.classList.toggle("md-button--primary", !isDone);
    }
    btn.addEventListener("click", function () {
      if (done[key]) delete done[key];
      else done[key] = new Date().toISOString().slice(0, 10);
      save(done);
      render();
      decorateNav(done);
    });
    render();
    h1.insertAdjacentElement("afterend", btn);
  }

  function homeCounters(done) {
    var spans = document.querySelectorAll("[data-lp-course]");
    var total = 0;
    var finished = 0;
    for (var i = 0; i < spans.length; i++) {
      var course = spans[i].getAttribute("data-lp-course");
      var max = parseInt(spans[i].getAttribute("data-lp-total"), 10) || 0;
      var n = 0;
      for (var k in done) {
        if (Object.prototype.hasOwnProperty.call(done, k) && courseOf(k) === course && isModule(k)) n++;
      }
      n = Math.min(n, max);
      spans[i].textContent = n + " done";
      total += max;
      finished += n;
    }
    var overall = document.querySelector("[data-lp-overall]");
    if (overall && total > 0) {
      var pct = Math.round((100 * finished) / total);
      overall.innerHTML =
        '<div class="lp-bar"><div class="lp-bar__fill" style="width:' + pct + '%"></div></div>' +
        "<p>" + finished + " of " + total + " modules done (" + pct + "%)</p>";
    }
  }

  function run() {
    var done = load();
    addButton(done);
    decorateNav(done);
    homeCounters(done);
  }

  // Material's instant navigation swaps pages without a reload; document$ fires on each swap.
  if (typeof window.document$ !== "undefined" && window.document$.subscribe) {
    window.document$.subscribe(run);
  } else {
    document.addEventListener("DOMContentLoaded", run);
  }
})();
