/* devinfo site -- no build step, no dependencies. */
(function () {
  "use strict";

  var root = document.documentElement;

  /* ---------------- theme ---------------- */
  var THEME_KEY = "devinfo.theme";
  function applyTheme(t) {
    root.setAttribute("data-theme", t);
    try { localStorage.setItem(THEME_KEY, t); } catch (e) {}
  }
  var saved = null;
  try { saved = localStorage.getItem(THEME_KEY); } catch (e) {}
  if (saved === "light" || saved === "dark") applyTheme(saved);

  var themeBtn = document.getElementById("theme-btn");
  if (themeBtn) {
    themeBtn.addEventListener("click", function () {
      applyTheme(root.getAttribute("data-theme") === "light" ? "dark" : "light");
    });
  }

  /* ---------------- language ---------------- */
  var LANG_KEY = "devinfo.lang";
  function applyLang(lang) {
    root.setAttribute("data-lang", lang);
    root.setAttribute("lang", lang === "zh" ? "zh-CN" : "en");
    var nodes = document.querySelectorAll("[data-zh]");
    for (var i = 0; i < nodes.length; i++) {
      var text = lang === "zh" ? nodes[i].getAttribute("data-zh") : nodes[i].getAttribute("data-en");
      if (text === null) continue;
      var target = nodes[i].getAttribute("data-html") === null ? "textContent" : "innerHTML";
      nodes[i][target] = text;
    }
    var btns = document.querySelectorAll("[data-set-lang]");
    for (var j = 0; j < btns.length; j++) {
      btns[j].classList.toggle("on", btns[j].getAttribute("data-set-lang") === lang);
    }
    try { localStorage.setItem(LANG_KEY, lang); } catch (e) {}
  }
  var langBtns = document.querySelectorAll("[data-set-lang]");
  for (var k = 0; k < langBtns.length; k++) {
    langBtns[k].addEventListener("click", function () {
      applyLang(this.getAttribute("data-set-lang"));
    });
  }
  var initial = null;
  try { initial = localStorage.getItem(LANG_KEY); } catch (e) {}
  if (!initial) initial = (navigator.language || "zh").toLowerCase().indexOf("zh") === 0 ? "zh" : "en";
  applyLang(initial);

  /* ---------------- reveal on scroll ---------------- */
  var reveals = document.querySelectorAll(".reveal");
  if ("IntersectionObserver" in window) {
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (en) {
        if (en.isIntersecting) { en.target.classList.add("in"); io.unobserve(en.target); }
      });
    }, { rootMargin: "0px 0px -12% 0px", threshold: 0.08 });
    for (var r = 0; r < reveals.length; r++) io.observe(reveals[r]);
  } else {
    for (var r2 = 0; r2 < reveals.length; r2++) reveals[r2].classList.add("in");
  }

  /* ---------------- screenshot tabs ---------------- */
  var tabs = document.querySelectorAll(".screen-tabs button");
  var stageImg = document.getElementById("screen-img");
  var stageShot = document.getElementById("screen-shot");
  var stageCap = document.getElementById("screen-caption");
  var stageUrl = document.getElementById("screen-url");
  for (var t = 0; t < tabs.length; t++) {
    tabs[t].addEventListener("click", function () {
      for (var m = 0; m < tabs.length; m++) tabs[m].classList.remove("on");
      this.classList.add("on");
      var src = this.getAttribute("data-screen");
      if (stageImg) stageImg.src = src;
      if (stageShot) stageShot.setAttribute("data-full", src);
      if (stageCap) {
        stageCap.setAttribute("data-zh", this.getAttribute("data-caption-zh") || "");
        stageCap.setAttribute("data-en", this.getAttribute("data-caption-en") || "");
        stageCap.textContent = root.getAttribute("data-lang") === "zh"
          ? stageCap.getAttribute("data-zh") : stageCap.getAttribute("data-en");
      }
      if (stageUrl && this.getAttribute("data-url")) stageUrl.textContent = this.getAttribute("data-url");
    });
  }

  /* ---------------- lightbox ---------------- */
  var box = document.getElementById("lightbox");
  var boxImg = document.getElementById("lightbox-img");
  document.addEventListener("click", function (ev) {
    var btn = ev.target.closest ? ev.target.closest("[data-full]") : null;
    if (!btn) return;
    ev.preventDefault();
    if (!box || !boxImg) return;
    boxImg.src = btn.getAttribute("data-full");
    box.classList.add("on");
  });
  if (box) {
    box.addEventListener("click", function (ev) {
      if (ev.target === box || ev.target.tagName === "BUTTON") box.classList.remove("on");
    });
    document.addEventListener("keydown", function (ev) {
      if (ev.key === "Escape") box.classList.remove("on");
    });
  }

  /* ---------------- copy buttons ---------------- */
  document.addEventListener("click", function (ev) {
    var btn = ev.target.closest ? ev.target.closest("[data-copy]") : null;
    if (!btn) return;
    var sel = btn.getAttribute("data-copy");
    var node = document.querySelector(sel);
    if (!node) return;
    var text = node.innerText;
    var done = function () {
      var old = btn.textContent;
      btn.textContent = root.getAttribute("data-lang") === "zh" ? "已复制" : "Copied";
      setTimeout(function () { btn.textContent = old; }, 1400);
    };
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).then(done, function () {});
    } else {
      var ta = document.createElement("textarea");
      ta.value = text; document.body.appendChild(ta); ta.select();
      try { document.execCommand("copy"); done(); } catch (e) {}
      document.body.removeChild(ta);
    }
  });

  /* ---------------- nav active state ---------------- */
  var links = document.querySelectorAll(".nav-links a[href^='#']");
  var sections = [];
  for (var n = 0; n < links.length; n++) {
    var id = links[n].getAttribute("href").slice(1);
    var el = document.getElementById(id);
    if (el) sections.push({ link: links[n], el: el });
  }
  if (sections.length && "IntersectionObserver" in window) {
    var so = new IntersectionObserver(function (entries) {
      entries.forEach(function (en) {
        if (!en.isIntersecting) return;
        for (var s = 0; s < sections.length; s++) {
          sections[s].link.classList.toggle("on", sections[s].el === en.target);
        }
      });
    }, { rootMargin: "-45% 0px -50% 0px" });
    for (var s2 = 0; s2 < sections.length; s2++) so.observe(sections[s2].el);
  }
})();
