/* Kinoklip website: small, no dependencies. */
(function () {
  'use strict';
  var $ = function (s, r) { return (r || document).querySelector(s); };
  var $$ = function (s, r) { return Array.prototype.slice.call((r || document).querySelectorAll(s)); };
  var script = document.currentScript || $('script[data-root]');
  var root = script ? script.getAttribute('data-root') || '' : '';

  /* Mobile menu */
  var toggle = $('.nav-toggle'), nav = $('#site-nav');
  if (toggle && nav) {
    toggle.addEventListener('click', function () {
      var open = nav.classList.toggle('open');
      toggle.setAttribute('aria-expanded', open ? 'true' : 'false');
    });
    document.addEventListener('keydown', function (e) {
      if (e.key === 'Escape' && nav.classList.contains('open')) { nav.classList.remove('open'); toggle.setAttribute('aria-expanded', 'false'); toggle.focus(); }
    });
  }

  /* Motion, as in the app's welcome window: items rise 10 px while fading in over 520 ms, 70 ms apart.
     Nothing is hidden unless this script runs, and not at all with reduced motion. */
  var reduce = window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  if (!reduce) {
    document.documentElement.classList.add('anim');
    $$('.hero .wrap > *, .page-head .wrap > *, .guide-head .wrap > *, .prose > .crumb, .prose > h1').forEach(function (el, i) {
      if (!el.classList.contains('hero-icon')) el.classList.add('hero-item');
      el.style.setProperty('--i', i);
    });
    var targets = $$('.section > .wrap, .feat-body > section');
    $$('[data-stagger], .refgroups, .cols-list, .hl').forEach(function (g) {
      g.setAttribute('data-stagger', '');
      Array.prototype.forEach.call(g.children, function (c, i) { c.style.setProperty('--i', i); });
      targets.push(g);
    });
    targets.forEach(function (el) { if (!el.hasAttribute('data-stagger')) el.setAttribute('data-reveal', ''); });
    if ('IntersectionObserver' in window) {
      var io = new IntersectionObserver(function (entries) {
        entries.forEach(function (en) {
          if (en.isIntersecting) { en.target.classList.add('in'); io.unobserve(en.target); }
        });
      }, { threshold: 0.12 });
      targets.forEach(function (el) { io.observe(el); });
    } else {
      targets.forEach(function (el) { el.classList.add('in'); });
    }
  }

  /* Guide contents: folded on a phone, always open on a wide screen. */
  var navbox = $('.navbox');
  if (navbox && window.matchMedia) {
    var wide = window.matchMedia('(max-width: 860px)');
    var fold = function () { navbox.open = !wide.matches; };
    fold();
    if (wide.addEventListener) wide.addEventListener('change', fold);
  }

  /* Theme: follows the system unless a choice is stored. */
  $$('[data-theme-set]').forEach(function (b) {
    b.addEventListener('click', function () {
      var v = b.getAttribute('data-theme-set');
      try { if (v === 'auto') localStorage.removeItem('kk-theme'); else localStorage.setItem('kk-theme', v); } catch (e) { /* private mode */ }
      if (v === 'auto') document.documentElement.removeAttribute('data-theme'); else document.documentElement.setAttribute('data-theme', v);
    });
  });

  /* Guide search: loads the index on first use, scores title, keywords, headings, summary and text. */
  var index = null, loading = null;
  function load() {
    if (index) return Promise.resolve(index);
    if (!loading) loading = fetch(root + 'guide/search.json').then(function (r) { return r.json(); }).then(function (d) { index = d; return d; });
    return loading;
  }
  function esc(s) { return s.replace(/[&<>"]/g, function (c) { return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]; }); }
  function mark(text, words) {
    var out = esc(text);
    words.forEach(function (w) {
      if (w.length < 2) return;
      var safe = w.replace(/[^a-z0-9\u0080-￿ ]/gi, '\\$&');
      out = out.replace(new RegExp('(' + safe + ')', 'ig'), '<mark>$1</mark>');
    });
    return out;
  }
  function search(q) {
    var words = q.toLowerCase().split(/\s+/).filter(Boolean);
    var res = [];
    index.forEach(function (a) {
      var title = a.title.toLowerCase(), kw = a.keywords.join(' ').toLowerCase(), sum = a.summary.toLowerCase(), text = a.text.toLowerCase();
      var heads = a.headings.map(function (h) { return h.t.toLowerCase(); });
      var score = 0, hit = null;
      for (var i = 0; i < words.length; i++) {
        var w = words[i], s = 0;
        if (title.indexOf(w) >= 0) s += 10;
        if (kw.indexOf(w) >= 0) s += 6;
        for (var j = 0; j < heads.length; j++) if (heads[j].indexOf(w) >= 0) { s += 4; if (!hit) hit = a.headings[j]; break; }
        if (sum.indexOf(w) >= 0) s += 3;
        if (text.indexOf(w) >= 0) s += 1;
        if (!s) { score = 0; break; }
        score += s;
      }
      if (score) res.push({ a: a, score: score, hit: hit });
    });
    res.sort(function (x, y) { return y.score - x.score; });
    return { words: words, items: res.slice(0, 8) };
  }
  $$('[data-search]').forEach(function (input) {
    var box = input.parentNode, list = $('.results', box), cur = -1;
    function render() {
      var q = input.value.trim();
      if (q.length < 2) { list.hidden = true; list.innerHTML = ''; return; }
      load().then(function () {
        if (input.value.trim() !== q) return;
        var r = search(q);
        cur = -1;
        if (!r.items.length) { list.innerHTML = '<li class="none">Nothing found for &ldquo;' + esc(q) + '&rdquo;.</li>'; list.hidden = false; return; }
        list.innerHTML = r.items.map(function (x) {
          var a = x.a, href = root + 'guide/' + a.id + '/' + (x.hit ? '#' + x.hit.a : '');
          var sub = x.hit ? 'Section: ' + x.hit.t : (a.summary || '');
          return '<li><a href="' + href + '"><b>' + mark(a.title, r.words) + '</b><small>' + mark(sub, r.words) + '</small></a></li>';
        }).join('');
        list.hidden = false;
      }).catch(function () { list.innerHTML = '<li class="none">Search needs the site to be served over http.</li>'; list.hidden = false; });
    }
    input.addEventListener('input', render);
    input.addEventListener('focus', function () { load().catch(function () {}); });
    input.addEventListener('keydown', function (e) {
      var links = $$('a', list);
      if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
        if (!links.length) return;
        e.preventDefault();
        cur = (cur + (e.key === 'ArrowDown' ? 1 : -1) + links.length) % links.length;
        links.forEach(function (l, i) { l.classList.toggle('on', i === cur); });
        links[cur].scrollIntoView({ block: 'nearest' });
      } else if (e.key === 'Enter' && links.length) {
        e.preventDefault();
        links[cur >= 0 ? cur : 0].click();
      } else if (e.key === 'Escape') { list.hidden = true; }
    });
    document.addEventListener('click', function (e) { if (!box.contains(e.target)) list.hidden = true; });
    var m = /[?&]q=([^&]*)/.exec(location.search);
    if (m && input.hasAttribute('data-autofill')) { input.value = decodeURIComponent(m[1].replace(/\+/g, ' ')); render(); }
  });
  /* "/" focuses the search on guide pages. */
  document.addEventListener('keydown', function (e) {
    var t = e.target && e.target.tagName;
    if (e.key === '/' && !e.ctrlKey && !e.metaKey && t !== 'INPUT' && t !== 'TEXTAREA' && t !== 'SELECT') {
      var s = $('[data-search]');
      if (s) { e.preventDefault(); s.focus(); }
    }
  });

  /* On this page: highlight the section being read. */
  var tocLinks = $$('.toc a');
  if (tocLinks.length && 'IntersectionObserver' in window) {
    var byId = {};
    tocLinks.forEach(function (a) { byId[a.getAttribute('href').slice(1)] = a; });
    var heads = Object.keys(byId).map(function (id) { return document.getElementById(id); }).filter(Boolean);
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (en) {
        if (en.isIntersecting) {
          tocLinks.forEach(function (a) { a.classList.remove('on'); });
          byId[en.target.id].classList.add('on');
        }
      });
    }, { rootMargin: '-70px 0px -70% 0px' });
    heads.forEach(function (h) { io.observe(h); });
  }

  /* Key demo: J K L, I O, B and Shift+Delete on a small timeline. */
  var demo = $('[data-demo]');
  if (demo) {
    var tl = $('.demo-tl', demo), head = $('.demo-head', demo), range = $('.demo-range', demo);
    var tcOut = $('.tc', demo), msgOut = $('.demo-msg', demo), clipLayer = $('.demo-clips', demo);
    var st;
    var fresh = function () {
      return { clips: [{ id: 1, name: 'beach.mp4', start: 0, len: 45 }, { id: 2, name: 'pier.mov', start: 45, len: 30 }, { id: 3, name: 'sunset.mp4', start: 75, len: 25 }], head: 30, play: 0, inP: null, outP: null, next: 4 };
    };
    st = fresh();
    var say = function (m) { msgOut.textContent = m; };
    var draw = function () {
      var have = {};
      $$('.demo-clip', clipLayer).forEach(function (el) { have[el.getAttribute('data-id')] = el; });
      var seenIds = {};
      st.clips.forEach(function (c) {
        seenIds[c.id] = 1;
        var el = have[c.id];
        if (!el) {
          el = document.createElement('div'); el.className = 'demo-clip'; el.setAttribute('data-id', c.id);
          clipLayer.appendChild(el);
        }
        el.textContent = c.name;
        el.style.left = 'calc(' + c.start + '% + 1px)';
        el.style.width = 'calc(' + c.len + '% - 2px)';
      });
      Object.keys(have).forEach(function (id) { if (!seenIds[id]) have[id].remove(); });
      $$('.demo-clip', clipLayer).forEach(function (el, i) { el.style.background = i % 2 ? 'var(--clip-video2)' : 'var(--clip-video)'; });
      head.style.left = st.head + '%';
      head.classList.toggle('run', !!st.play);
      var a = st.inP === null ? 0 : st.inP, b = st.outP === null ? 100 : st.outP;
      if (b < a) { var x = a; a = b; b = x; }
      range.hidden = st.inP === null && st.outP === null;
      range.style.left = a + '%'; range.style.width = (b - a) + '%';
      var secs = st.head * 0.6, s = Math.floor(secs), f = Math.floor((secs - s) * 24);
      tcOut.textContent = '01:00:' + (s < 10 ? '0' : '') + s + ':' + (f < 10 ? '0' : '') + f;
    };
    var actions = {
      j: function () { st.play = -1; say('J: playing backwards'); },
      k: function () { st.play = 0; say('K: stopped'); },
      l: function () { st.play = 1; say('L: playing'); },
      i: function () { st.inP = st.head; say('I: in point set'); },
      o: function () { st.outP = st.head; say('O: out point set'); },
      b: function () {
        var h = st.head, out = [], cut = false;
        st.clips.forEach(function (c) {
          if (h > c.start + 1 && h < c.start + c.len - 1) {
            out.push({ id: c.id, name: c.name, start: c.start, len: h - c.start });
            out.push({ id: st.next++, name: c.name, start: h, len: c.start + c.len - h });
            cut = true;
          } else out.push(c);
        });
        st.clips = out;
        say(cut ? 'B: clip cut at the playhead' : 'B: move the playhead over a clip first');
      },
      del: function () {
        var h = st.head, hit = null;
        st.clips.forEach(function (c) { if (!hit && h >= c.start && h < c.start + c.len) hit = c; });
        if (!hit) { say('Nothing under the playhead'); return; }
        st.clips = st.clips.filter(function (c) { return c !== hit; }).map(function (c) {
          return c.start > hit.start ? { id: c.id, name: c.name, start: c.start - hit.len, len: c.len } : c;
        });
        say('Shift+Del: clip removed, the rest moved left');
      }
    };
    var press = function (k) {
      if (actions[k]) { actions[k](); draw(); }
    };
    $$('[data-key]', demo).forEach(function (btn) {
      btn.addEventListener('click', function () { press(btn.getAttribute('data-key')); });
    });
    var flash = function (k) {
      var btn = $('[data-key="' + k + '"]', demo);
      if (!btn) return;
      btn.classList.add('hit'); setTimeout(function () { btn.classList.remove('hit'); }, 140);
    };
    tl.addEventListener('keydown', function (e) {
      if (e.ctrlKey || e.metaKey || e.altKey) return;
      var k = e.key.toLowerCase();
      if (k === 'delete' && e.shiftKey) k = 'del';
      else if (e.shiftKey) return;
      if (actions[k]) { e.preventDefault(); flash(k); press(k); }
    });
    $('[data-reset]', demo).addEventListener('click', function () {
      st = fresh(); say('Click a key below, or focus the timeline and press it'); draw();
    });
    // Playback runs only while the demo is on screen.
    var onScreen = true;
    if ('IntersectionObserver' in window) new IntersectionObserver(function (e) { onScreen = e[0].isIntersecting; }).observe(demo);
    setInterval(function () {
      if (!st.play || !onScreen) return;
      var h = st.head + st.play * 1.2;
      if (h > 100 || h < 0) { st.head = Math.max(0, Math.min(100, h)); st.play = 0; say('Stopped at the end'); }
      else st.head = h;
      draw();
    }, 120);
    draw();
  }

  /* Bug report form: builds a prefilled GitHub issue address and opens it. */
  var form = $('[data-bugform]');
  if (form) {
    var base = form.getAttribute('data-issue-url');
    var build = function () {
      var v = function (n) { return (form.elements[n].value || '').trim(); };
      var p = [];
      var add = function (k, val) { if (val) p.push(encodeURIComponent(k) + '=' + encodeURIComponent(val)); };
      add('template', 'bug_report.yml');
      var what = v('actual');
      add('title', '[Bug] ' + what.split('\n')[0].slice(0, 70));
      add('labels', 'bug');
      add('version', v('version'));
      add('environment', v('environment'));
      add('page', v('page'));
      add('steps', v('steps'));
      add('actual', what);
      add('expected', v('expected'));
      return base + '?' + p.join('&');
    };
    var out = $('[data-issue-preview]', form);
    form.addEventListener('submit', function (e) {
      e.preventDefault();
      if (!form.reportValidity()) return;
      var url = build();
      if (out) { out.hidden = false; out.href = url; }
      window.open(url, '_blank', 'noopener');
    });
  }

  /* Download button: the download starts, and a small window asks about a donation. */
  var donate = $('#donate');
  if (donate && typeof donate.showModal === 'function') {
    $$('[data-donate]').forEach(function (a) {
      a.addEventListener('click', function () {
        setTimeout(function () { if (!donate.open) donate.showModal(); }, 300);
      });
    });
  }
})();
