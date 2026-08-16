/* server/host/common/assets.h -- the server UI's static bundle, embedded in
 * the binary (docs/DESIGN.md §6.3, §6.4 "Host owns ... UI").
 *
 * Header-only on purpose. scripts/build.sh's build_server() names an exact,
 * fixed list of .c files and docs/CONVENTIONS.md/this task's own CONSTRAINTS forbid
 * touching scripts/ -- server-dev agent memory mapping-engine-profiles
 * describes the OLD workaround for that same constraint (an amalgamating
 * #include of one .c into another) and the fact that it was later replaced
 * once scripts/build.sh grew real per-file entries. This file avoids ever
 * needing that trick: it is included, never compiled standalone, so it adds
 * no translation unit for the build script to not know about. Only
 * server/host/linux/main.c #includes it.
 *
 * Vanilla HTML/CSS/JS in one document -- no npm, no framework, no CDN, same
 * reasoning that keeps clients/android/ dependency-free (docs/CONVENTIONS.md). No
 * server-side templating and no absolute URLs: every piece of state comes
 * from the JSON API at fetch time, which is what "Tauri-ready" means here
 * (a later native shell could load this exact document against the same
 * API with no rewrite) -- nothing else about Tauri is implied or wanted.
 *
 * Polls GET /api/state at 2 Hz (comfortably under the 5 Hz budget the
 * library's query API is built to tolerate -- apadserver.h's "Server UI
 * query API" section).
 */
#ifndef ATTICPAD_HOST_COMMON_ASSETS_H
#define ATTICPAD_HOST_COMMON_ASSETS_H

static const char ATTICPAD_INDEX_HTML[] =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
"<title>AtticPad Server</title>\n"
"<style>\n"
"  :root { color-scheme: dark light; }\n"
"  body { font-family: -apple-system, Segoe UI, Roboto, sans-serif; margin: 0;\n"
"         background: #14161a; color: #e6e6e6; }\n"
"  header { padding: 16px 20px; background: #1c1f26; border-bottom: 1px solid #2a2e37; }\n"
"  h1 { font-size: 18px; margin: 0 0 4px 0; }\n"
"  .ips { font-size: 20px; font-weight: 600; color: #7fd0ff; }\n"
"  .ips small { color: #9aa0aa; font-weight: 400; font-size: 13px; }\n"
"  main { padding: 16px 20px; max-width: 920px; margin: 0 auto; }\n"
"  section { background: #1c1f26; border: 1px solid #2a2e37; border-radius: 8px;\n"
"            padding: 14px 16px; margin-bottom: 16px; }\n"
"  section h2 { font-size: 14px; text-transform: uppercase; letter-spacing: .04em;\n"
"               color: #9aa0aa; margin: 0 0 10px 0; }\n"
"  table { width: 100%; border-collapse: collapse; font-size: 13px; }\n"
"  th, td { text-align: left; padding: 6px 8px; border-bottom: 1px solid #2a2e37; }\n"
"  .ok { color: #6fd88a; } .bad { color: #ff8a8a; } .muted { color: #9aa0aa; }\n"
"  button { background: #2d6fe0; color: white; border: none; border-radius: 6px;\n"
"           padding: 6px 12px; cursor: pointer; font-size: 13px; }\n"
"  button.secondary { background: #3a3f4b; }\n"
"  button:disabled { opacity: .5; cursor: default; }\n"
"  .secret { font-size: 34px; letter-spacing: .12em; font-weight: 700; color: #ffd479; }\n"
"  .pairing-grid { display: flex; gap: 20px; align-items: flex-start; flex-wrap: wrap; }\n"
"  .qr-box { background: #ffffff; padding: 8px; border-radius: 6px; line-height: 0; }\n"
"  .qr-box img { display: block; width: 200px; height: 200px; }\n"
"  .uri { font-family: ui-monospace, Menlo, Consolas, monospace; font-size: 12px;\n"
"         background: #101215; border-radius: 6px; padding: 8px 10px; word-break: break-all;\n"
"         user-select: all; cursor: text; max-width: 360px; }\n"
"  .log { background: #101215; border-radius: 6px; padding: 8px 10px; max-height: 220px;\n"
"         overflow-y: auto; font-family: ui-monospace, Menlo, Consolas, monospace;\n"
"         font-size: 11px; white-space: pre-wrap; color: #9aa0aa; }\n"
"  /* ---- visual X360 pad ------------------------------------------------\n"
"     A diagnostic drawing, not a photo: the point is that a glance answers\n"
"     \"which control did that\", which a grid of numbered boxes never did.\n"
"     Canonical A/B/X/Y colours are kept as outlines so the shape is\n"
"     recognisable, while PRESSED is always the same amber used everywhere\n"
"     else in this UI -- one meaning, one colour. */\n"
"  .pt-pad { display: block; width: 100%; max-width: 460px; height: auto;\n"
"            margin: 6px 0 10px 0; }\n"
"  /* Shell = the logo's own silhouette: a rounded body plus two flared\n"
"     grip ellipses, proportions lifted from make-assets.py's\n"
"     pad_silhouette() rather than eyeballed. Filled, never stroked --\n"
"     three overlapping stroked shapes would show their seams. */\n"
"  .pt-pad .shell { fill: #21252d; }\n"
"  .pt-pad .shell-edge { fill: none; stroke: #2a2e37; stroke-width: 2; }\n"
"  .pt-pad .part { fill: #171a20; stroke: #3a3f4b; stroke-width: 1.5;\n"
"                  transition: fill 40ms linear; }\n"
"  /* Guide gets a lighter rim so it reads as the home button and not as a\n"
"     hole punched in the shell. Must precede .part.on, which ties on\n"
"     specificity and has to win when the button is actually pressed. */\n"
"  .pt-pad .part.guide { stroke: #4a5160; }\n"
"  .pt-pad .part.on { fill: #F5B042; stroke: #F5B042; }\n"
"  .pt-pad .lbl { fill: #9aa0aa; font: 600 11px ui-sans-serif, system-ui, sans-serif;\n"
"                 text-anchor: middle; dominant-baseline: central;\n"
"                 pointer-events: none; }\n"
"  .pt-pad .part.on + .lbl { fill: #1C212B; }\n"
"  .pt-pad .knob { fill: #2d3340; stroke: #4a5160; stroke-width: 1.5; }\n"
"  .pt-pad .knob.on { fill: #F5B042; stroke: #F5B042; }\n"
"  /* D-pad arms carry no outline: the plate behind them already draws the\n"
"     cross, so outlining each arm too gave the same edge twice and made\n"
"     four squares fight the one shape they sit in. Invisible until\n"
"     pressed, then amber like everything else. */\n"
"  .pt-pad .darm { fill: none; stroke: none; }\n"
"  .pt-pad .darm.on { fill: #F5B042; }\n"
"  .pt-pad .well, .pt-pad .dwell { fill: #14161a; stroke: #2a2e37;\n"
"                                      stroke-width: 1.5; }\n"
"  .pt-pad .trig-bg { fill: #14161a; stroke: #3a3f4b; stroke-width: 1.5; }\n"
"  /* The trigger label sits OUTSIDE its pill. The fill sweeps the whole\n"
"     bar, which reads as pressure far better than a thin meter did -- but\n"
"     only works if the text is somewhere the fill never reaches, since no\n"
"     single label colour is right on both sides of a moving edge. The fill\n"
"     is square-ended and CLIPPED to the pill rather than rounded: a\n"
"     rounded trailing edge reads as a lozenge sliding along, not as a\n"
"     level rising. Duplicate clip ids across pad slots are harmless --\n"
"     every slot clips to the identical rectangle. */\n"
"  .pt-pad .trig-lbl { fill: #9aa0aa; }\n"
"  .pt-pad .trig-fill { fill: #F5B042; }\n"
"  .pt-pad .ring-a { stroke: #6fd88a; } .pt-pad .ring-b { stroke: #ff8a8a; }\n"
"  .pt-pad .ring-x { stroke: #7fd0ff; } .pt-pad .ring-y { stroke: #ffd479; }\n"
"  .pt-pad .lbl-a { fill: #6fd88a; } .pt-pad .lbl-b { fill: #ff8a8a; }\n"
"  .pt-pad .lbl-x { fill: #7fd0ff; } .pt-pad .lbl-y { fill: #ffd479; }\n"
"  .pt-unmapped { font-size: 11px; color: #9aa0aa; margin-top: 2px; }\n"
"  .pt-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(78px, 1fr));\n"
"             gap: 4px; margin-top: 6px; }\n"
"  .pt-b { background: #101215; border: 1px solid #2a2e37; border-radius: 4px; padding: 4px 6px;\n"
"          font: 12px ui-monospace, monospace; text-align: center; }\n"
"  .pt-b.on { background: #F5B042; color: #1C212B; border-color: #F5B042; font-weight: 700; }\n"
"  .pt-ax { font: 12px ui-monospace, monospace; margin-top: 6px;\n"
"           display: flex; flex-wrap: wrap; gap: 4px 18px; }\n"
/* Each axis is one unbreakable unit -- without this the label and its bar
 * wrap independently and a bar ends up orphaned on the next line. */
"  .pt-ax span.ax { white-space: nowrap; }\n"
"  .pt-bar { display: inline-block; width: 120px; height: 8px; background: #101215;\n"
"            border: 1px solid #2a2e37; border-radius: 4px; vertical-align: middle;\n"
"            position: relative; }\n"
"  .pt-bar i { position: absolute; top: 0; bottom: 0; width: 2px; background: #F5B042; }\n"
"  select { background: #101215; color: #e6e6e6; border: 1px solid #2a2e37; border-radius: 4px;\n"
"           padding: 3px 6px; }\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<header>\n"
"  <h1>AtticPad Server</h1>\n"
"  <div class=\"ips\" id=\"ips\">--</div>\n"
"</header>\n"
"<main>\n"
"  <section>\n"
"    <h2>Backend</h2>\n"
"    <div id=\"backend\">--</div>\n"
"  </section>\n"
"  <section>\n"
"    <h2>Automatic discovery</h2>\n"
"    <div id=\"mdns\">--</div>\n"
"  </section>\n"
"  <section>\n"
"    <h2>Pairing</h2>\n"
"    <div id=\"pairing\">--</div>\n"
"    <div style=\"margin-top:10px\">\n"
"      <button id=\"pairPinBtn\" onclick=\"beginPair(false)\">Show PIN</button>\n"
"      <button id=\"pairTokenBtn\" class=\"secondary\" onclick=\"beginPair(true)\">Show token (QR-shaped)</button>\n"
"      <button id=\"cancelPairBtn\" class=\"secondary\" onclick=\"cancelPair()\">Cancel</button>\n"
"    </div>\n"
"  </section>\n"
"  <section>\n"
"    <h2>Connected clients</h2>\n"
"    <table id=\"clients\"><thead><tr>\n"
"      <th>Slot</th><th>Device</th><th>Peer</th><th>Profile</th>\n"
"      <th>RTT</th><th>Battery</th><th>Auth</th><th>rx/tx</th><th></th>\n"
"    </tr></thead><tbody></tbody></table>\n"
"  </section>\n"
/*
 * "Does the virtual pad actually work?" answered IN the UI, on 127.0.0.1.
 *
 * This exists because a third-party HTML5 gamepad tester is a bad oracle
 * for this question, in a way that wastes hours:
 *
 *   - The Gamepad API is a SECURE CONTEXT feature. A tester served over
 *     plain http:// silently reports no gamepads at all, with no error.
 *     127.0.0.1 is a secure context by definition (it is "potentially
 *     trustworthy" per the W3C definition regardless of scheme), so a
 *     tester served from HERE cannot hit that.
 *   - navigator.getGamepads() deliberately hides a pad until it sends
 *     input while the page has focus, so an idle-but-present pad reads
 *     as absent. This panel says so on screen rather than leaving the
 *     reader to conclude the pad is broken.
 *
 * Purely a browser-side view: it polls the Gamepad API, never the server,
 * and it says nothing about whether AtticPad is working -- the two are
 * independent. A pad visible in the "Connected clients" table above but
 * absent here means the browser cannot see it; the reverse means it is a
 * different pad entirely (a real controller, for instance).
 */
"  <section>\n"
"    <h2>Pad test</h2>\n"
"    <div id=\"padtest\" class=\"muted\">--</div>\n"
"  </section>\n"
"  <section>\n"
"    <h2>Log</h2>\n"
"    <div class=\"log\" id=\"log\">--</div>\n"
"  </section>\n"
"</main>\n"
"<script>\n"
"var PROFILES = [];\n"
"var QR_ADDR = null;\n"     /* which own_ips entry the QR/URI currently encodes;
                              * persists across refresh() polls so picking one
                              * from the selector below does not get silently
                              * reset by the next 2 Hz poll -- see setQrAddr(). */
/*
 * A <select> the operator is actually using must survive the 2 Hz poll.
 *
 * refresh() rebuilds whole panels with innerHTML, which DESTROYS AND
 * RECREATES every element inside them -- including an open dropdown. A
 * native select popup stays open for as long as it takes a human to read
 * it, which is many multiples of the 500 ms poll, so the element vanishes
 * out from under the pointer and onchange never fires. Both dropdowns
 * (the QR address here, the per-client profile below) were unusable for
 * exactly this reason; it looked like dead wiring rather than a race.
 *
 * busy() is the guard: while focus is anywhere inside a container, that
 * container is left alone for this tick. An open select IS the focused
 * element, so this covers the popup for its whole lifetime. The cost is
 * that a countdown pauses while a dropdown is open, which is the correct
 * trade -- nobody is reading the timer while choosing an address.
 *
 * The setters then blur BEFORE refreshing, or the guard they just relied
 * on would suppress the very redraw that shows the new QR.
 */
"function busy(el) { var a = document.activeElement; return !!(a && el.contains(a) && a !== document.body); }\n"
"function setQrAddr(ip) {\n"
"  QR_ADDR = ip;\n"
"  if (document.activeElement && document.activeElement.blur) document.activeElement.blur();\n"
"  refresh();\n"
"}\n"
"function fmtIps(ips) {\n"
"  if (!ips.length) return '<span class=\"bad\">no non-loopback IPv4 address found</span>';\n"
"  return ips.map(function(a){ return a.ip + ' <small>(' + a.iface + ')</small>'; }).join(' &middot; ');\n"
"}\n"
"function fmtRtt(ms) { return (ms === null) ? '<span class=\"muted\">measuring&hellip;</span>' : (ms + ' ms'); }\n"
       /* caps bit 13 (0x2000) is APAD_CAP_BATTERY (docs/PROTOCOL.md S5) --
        * without it a device's `battery` field is always 0, which used to
        * render as "0%" and look like a dead battery on hardware that
        * simply never reports one. Shown only when the device actually
        * advertises the capability; still honours the 255 = "unknown"
        * sentinel for a device that has the capability but has not
        * reported a reading yet. */
"function fmtBattery(b, caps) {\n"
"  if (!(caps & 0x2000)) return '<span class=\"muted\">&mdash;</span>';\n"
"  return (b === 255) ? '<span class=\"muted\">unknown</span>' : (b + '%');\n"
"}\n"
"function esc(s) { return String(s).replace(/[&<>\"]/g, function(c){\n"
"  return { '&':'&amp;', '<':'&lt;', '>':'&gt;', '\"':'&quot;' }[c]; }); }\n"
"function profileOptions(current) {\n"
"  return PROFILES.map(function(p){\n"
"    return '<option value=\"' + esc(p) + '\"' + (p === current ? ' selected' : '') + '>' + esc(p) + '</option>';\n"
"  }).join('');\n"
"}\n"
"function setProfile(slot, sel) {\n"
       /* Blur before the refresh, exactly as setQrAddr() does: this select
        * still has focus at onchange time, and busy() would otherwise
        * suppress the redraw that confirms the change landed. */
"  if (sel && sel.blur) sel.blur();\n"
"  fetch('/api/client/' + slot + '/profile', {\n"
"    method: 'POST', headers: {'Content-Type':'application/json'},\n"
"    body: JSON.stringify({profile: sel.value})\n"
"  }).then(refresh);\n"
"}\n"
"function customize(slot) { location.href = '/editor?slot=' + slot; }\n"
"function beginPair(useToken) {\n"
"  fetch('/api/pair/begin' + (useToken ? '?kind=token' : '?kind=pin'), {method:'POST'}).then(refresh);\n"
"}\n"
"function cancelPair() {\n"
"  fetch('/api/pair/cancel', {method:'POST'}).then(refresh);\n"
"}\n"
"function refresh() {\n"
"  fetch('/api/state').then(function(r){ return r.json(); }).then(function(s){\n"
"    document.getElementById('ips').innerHTML = fmtIps(s.server.own_ips) +\n"
"      ' <small>UDP :' + s.server.port + '</small>';\n"
"    var b = s.server.backend;\n"
"    var bLine = '<b>' + esc(b.name) + '</b>: ';\n"
"    if (b.ok) {\n"
"      bLine += '<span class=\"ok\">ok</span>';\n"
"    } else {\n"
"      bLine += '<span class=\"bad\">' + esc(b.message) + '</span>';\n"
"      if (b.remedy) {\n"
"        bLine += b.remedy.indexOf('http') === 0\n"
"          ? ' — <a href=\"' + esc(b.remedy) + '\" target=\"_blank\">click to install</a>'\n"
"          : ' — <code>' + esc(b.remedy) + '</code>';\n"
"      }\n"
"    }\n"
"    document.getElementById('backend').innerHTML = bLine;\n"
"    var m = s.server.mdns;\n"
"    var mLine;\n"
"    if (!m.implemented) {\n"
"      mLine = '<span class=\"muted\">not implemented on this host</span>';\n"
"    } else if (m.state === 'running') {\n"
"      mLine = '<span class=\"ok\">advertising</span> <code>' + esc(m.service) + '</code>' +\n"
"        ' <small>as &ldquo;' + esc(m.instance) + '&rdquo; &middot; host ' + esc(m.host) +\n"
"        ' &middot; SRV port ' + m.service_port + '</small>' +\n"
"        '<div class=\"muted\">TXT ' + m.txt.map(function(t){ return '<code>' + esc(t) + '</code>'; }).join(' ') + '</div>' +\n"
"        '<div class=\"muted\"><small>' + m.announcements_tx + ' announcements, ' +\n"
"        m.responses_tx + ' queries answered</small></div>';\n"
"    } else {\n"
"      mLine = '<span class=\"' + (m.state === 'disabled' ? 'muted' : 'bad') + '\">' +\n"
"        esc(m.message) + '</span>';\n"
"      if (m.remedy) {\n"
"        mLine += '<div class=\"muted\">&mdash; <code>' + esc(m.remedy) + '</code></div>';\n"
"      }\n"
"    }\n"
"    document.getElementById('mdns').innerHTML = mLine;\n"
"    var p = s.pairing;\n"
"    var pd = document.getElementById('pairing');\n"
"    if (p.open) {\n"
"      var ips = s.server.own_ips;\n"
"      var uris = p.uris || [];\n"
       /* Keep the operator's selection across polls; if it vanished (an
        * interface went down mid-window) or nothing was picked yet, fall
        * back to the server's own ranked default -- ipaddr.h
        * host_pick_default_addr(): LAN before Tailscale before virtual. */
"      if (!ips.some(function(a){ return a.ip === QR_ADDR; })) {\n"
"        QR_ADDR = s.server.default_ip || (ips[0] && ips[0].ip) || null;\n"
"      }\n"
"      var addrOpts = ips.map(function(a){\n"
"        var label = a.ip + ' (' + a.iface + (a.kind !== 'lan' ? ', ' + a.kind : '') + ')';\n"
"        return '<option value=\"' + esc(a.ip) + '\"' + (a.ip === QR_ADDR ? ' selected' : '') + '>' + esc(label) + '</option>';\n"
"      }).join('');\n"
       /* Only shown with a real choice to make -- one address is not a
        * decision, and this keeps the single-homed case looking exactly
        * like it did before the selector existed. */
"      var selector = ips.length > 1\n"
"        ? '<div style=\"margin-bottom:8px\"><label class=\"muted\">Encode: <select onchange=\"setQrAddr(this.value)\">' + addrOpts + '</select></label></div>'\n"
"        : '';\n"
"      var selUri = null;\n"
"      for (var ui = 0; ui < uris.length; ui++) { if (uris[ui].ip === QR_ADDR) { selUri = uris[ui].uri; break; } }\n"
"      var qr = (QR_ADDR && selUri)\n"
"        ? '<div class=\"qr-box\"><img alt=\"pairing QR code\" src=\"/api/pair/qr.svg?addr=' + encodeURIComponent(QR_ADDR) + '&g=' + p.generation + '\"></div>'\n"
"        : '<div class=\"muted\">no address to encode -- enter this PC\\'s address on the device manually</div>';\n"
"      var uriLine = selUri ? '<div class=\"uri\">' + esc(selUri) + '</div>' : '';\n"
       /* Guarded: see busy() above. Deliberately only on the p.open
        * branch -- when the window CLOSES the panel must be cleared
        * immediately whatever has focus, because it is displaying a
        * secret that has just stopped being valid (PROTOCOL.md S10). A
        * stale-but-pretty UI is not worth showing a dead secret. */
"      if (!busy(pd))\n"
"      pd.innerHTML = selector + '<div class=\"pairing-grid\">' + qr +\n"
"        '<div><div class=\"secret\">' + esc(p.secret) + '</div>' +\n"
"        '<div>' + Math.floor(p.ms_remaining/1000) + 's left &middot; ' +\n"
"        p.attempts_remaining + ' attempts left</div>' +\n"
"        uriLine +\n"
"        '<div class=\"muted\"><small>scan the code, or select the line above &mdash; both disappear when this window closes</small></div>' +\n"
"        '</div></div>';\n"
"    } else {\n"
"      pd.innerHTML = '<span class=\"muted\">no pairing window open -- clients connect with no secret</span>';\n"
"    }\n"
"    PROFILES = s.profiles;\n"
"    var tbody = document.querySelector('#clients tbody');\n"
       /* Same guard, same reason: this tbody contains one profile
        * <select> per connected client. */
"    if (!busy(tbody))\n"
"    tbody.innerHTML = s.clients.map(function(c){\n"
"      return '<tr><td>' + c.slot + '</td><td>' + esc(c.device_name) + '</td>' +\n"
"        '<td>' + esc(c.peer) + '</td>' +\n"
"        '<td><select onchange=\"setProfile(' + c.slot + ', this)\">' + profileOptions(c.profile) + '</select></td>' +\n"
"        '<td>' + fmtRtt(c.rtt_ms) + '</td><td>' + fmtBattery(c.battery, c.caps) + '</td>' +\n"
"        '<td>' + (c.authenticated ? 'yes' : 'no') + '</td>' +\n"
"        '<td>' + c.rx_packets + '/' + c.tx_packets + '</td>' +\n"
"        '<td><button class=\"secondary\" onclick=\"customize(' + c.slot + ')\">Customize</button></td></tr>';\n"
"    }).join('') || '<tr><td colspan=\"9\" class=\"muted\">no clients connected</td></tr>';\n"
"    document.getElementById('log').textContent = s.log.join('\\n');\n"
"  });\n"
"}\n"
"refresh();\n"
/*
 * Polled on requestAnimationFrame, NOT on refresh()'s 500 ms timer: the
 * Gamepad API has no events for button/axis VALUES (only connect and
 * disconnect), so state must be sampled, and sampling at 2 Hz would make
 * a working pad look broken to anyone tapping a button. This touches
 * nothing on the server -- no fetch, no /api/state -- so the extra rate
 * costs one function call per frame and no traffic.
 */
/*
 * gamepadconnected / gamepaddisconnected fire from the browser itself, on
 * a different path from the getGamepads() poll below. Counting them
 * separates "the browser never saw a pad" from "the browser saw one and
 * the poll is not reporting it" -- two very different faults that look
 * identical in a panel that only polls.
 */
"var PT_CONN = 0, PT_DISC = 0, PT_LAST = \'\';\n"
"window.addEventListener(\'gamepadconnected\', function (e) {\n"
"  PT_CONN++; PT_LAST = e.gamepad ? e.gamepad.id : \'?\';\n"
"});\n"
"window.addEventListener(\'gamepaddisconnected\', function () { PT_DISC++; });\n"
"/* ---- visual pad -------------------------------------------------------\n"
" *\n"
" * Standard Gamepad API mapping (the \"standard\" layout every browser reports\n"
" * for an X360-class pad): 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 LT, 7 RT,\n"
" * 8 Back, 9 Start, 10 L3, 11 R3, 12..15 d-pad, 16 Guide. Axes 0/1 left\n"
" * stick, 2/3 right stick.\n"
" *\n"
" * Drawn ONCE per pad and then updated by attribute. The rest of this page\n"
" * rebuilds its innerHTML every frame, which is fine for a few spans and\n"
" * wrong for ~40 SVG nodes at 60 Hz -- reparsing the drawing on every frame\n"
" * is how a diagnostic view starts costing more than the thing it measures.\n"
" */\n"
"var PT_VIEWS = {};\n"
"\n"
"function padSvgMarkup() {\n"
"  /* One mirror line at x=180 governs everything: left stick <-> face\n"
"   * cluster, d-pad <-> right stick, LB/LT <-> RB/RT, and BACK/START about\n"
"   * the Guide. A 360 pad reads as symmetric even though the two sticks sit\n"
"   * on different rows -- it is the ROWS that are staggered, not the halves.\n"
"   * The previous drawing centred BACK/START on 190 while Guide sat at 180,\n"
"   * and that 10-unit disagreement was the thing that looked wrong. */\n"
"  var s = \'<svg class=\"pt-pad\" viewBox=\"0 2 360 240\" role=\"img\" \'\n"
"        + \'aria-label=\"gamepad state\">\';\n"
"  /* shell -- body + two grips, proportions from the mark\'s own\n"
"   * pad_solid() (radius h*0.42, grips h*0.62 wide dropping h*0.72), so the\n"
"   * tester and the app icon cannot drift apart. Every body-mounted control\n"
"   * sits INSIDE this rect: an earlier version put the d-pad and right stick\n"
"   * past its bottom edge, floating in the gap between the grips. */\n"
"  s += \'<g class=\"shell\">\'\n"
"     + \'<ellipse cx=\"66\" cy=\"173\" rx=\"50\" ry=\"57\"/>\'\n"
"     + \'<ellipse cx=\"294\" cy=\"173\" rx=\"50\" ry=\"57\"/>\'\n"
"     + \'<rect x=\"30\" y=\"60\" width=\"300\" height=\"140\" rx=\"59\"/>\'\n"
"     + \'</g>\';\n"
"  /* Shoulders sit over their own column (95 / 265 -- the stick and the face\n"
"   * cluster), and the bumper deliberately overlaps the body\'s top edge so\n"
"   * the pair reads as attached hardware rather than a floating legend. */\n"
"  s += \'<clipPath id=\"P-ltc\"><rect x=\"60\" y=\"14\" width=\"60\" height=\"24\" rx=\"12\"/></clipPath>\'\n"
"     + \'<rect class=\"trig-bg\" x=\"60\" y=\"14\" width=\"60\" height=\"24\" rx=\"12\"/>\'\n"
"     + \'<rect class=\"trig-fill\" id=\"P-lt\" x=\"60\" y=\"14\" width=\"0\" height=\"24\" clip-path=\"url(#P-ltc)\"/>\'\n"
"     + \'<text class=\"lbl trig-lbl\" x=\"46\" y=\"26\">LT</text>\';\n"
"  s += \'<clipPath id=\"P-rtc\"><rect x=\"240\" y=\"14\" width=\"60\" height=\"24\" rx=\"12\"/></clipPath>\'\n"
"     + \'<rect class=\"trig-bg\" x=\"240\" y=\"14\" width=\"60\" height=\"24\" rx=\"12\"/>\'\n"
"     + \'<rect class=\"trig-fill\" id=\"P-rt\" x=\"240\" y=\"14\" width=\"0\" height=\"24\" clip-path=\"url(#P-rtc)\"/>\'\n"
"     + \'<text class=\"lbl trig-lbl\" x=\"314\" y=\"26\">RT</text>\';\n"
"  s += \'<rect class=\"part\" id=\"P-b4\" x=\"60\" y=\"44\" width=\"60\" height=\"20\" rx=\"10\"/>\'\n"
"     + \'<text class=\"lbl\" x=\"90\" y=\"54\">LB</text>\';\n"
"  s += \'<rect class=\"part\" id=\"P-b5\" x=\"240\" y=\"44\" width=\"60\" height=\"20\" rx=\"10\"/>\'\n"
"     + \'<text class=\"lbl\" x=\"270\" y=\"54\">RB</text>\';\n"
"  /* left stick -- upper row, mirrors the face cluster at 265 */\n"
"  s += \'<circle class=\"well\" cx=\"90\" cy=\"106\" r=\"28\"/>\'\n"
"     + \'<circle class=\"knob\" id=\"P-s0\" cx=\"90\" cy=\"106\" r=\"16\"/>\';\n"
"  /* d-pad -- lower row, mirrors the right stick at 220. The cross-shaped\n"
"   * plate is ONE path, not two crossed rects: two stroked rects draw an\n"
"   * outline straight through the middle of the cross, which is what made\n"
"   * a lit up-arm read as a T sitting on a bar. Each arm is exactly its\n"
"   * quadrant of that same path, so lit and unlit regions abut with no\n"
"   * seam and no notch at the junctions. Each arm runs all the way to the\n"
"   * centre point, splitting the hub along its diagonals: a plain arm\n"
"   * stops dead on the crossbar and reads as a tab stuck on top of it,\n"
"   * where the wedge reads as a DIRECTION. The four wedges tile the plate\n"
"   * exactly -- proven by pixel coverage, since diagonal seams are\n"
"   * something bounding boxes cannot express. */\n"
"  s += \'<path class=\"dwell\" d=\"M 130 143 A 6 6 0 0 1 136 137 L 144 137 A 6 6 0 0 1 150 143 L 150 157 L 164 157 A 6 6 0 0 1 170 163 L 170 171 A 6 6 0 0 1 164 177 L 150 177 L 150 191 A 6 6 0 0 1 144 197 L 136 197 A 6 6 0 0 1 130 191 L 130 177 L 116 177 A 6 6 0 0 1 110 171 L 110 163 A 6 6 0 0 1 116 157 L 130 157 Z\"/>\';\n"
"  s += \'<path class=\"darm\" id=\"P-b12\" d=\"M 130 143 A 6 6 0 0 1 136 137 L 144 137 A 6 6 0 0 1 150 143 L 150 157 L 140 167 L 130 157 Z\"/>\'\n"
"     + \'<path class=\"darm\" id=\"P-b13\" d=\"M 150 177 L 150 191 A 6 6 0 0 1 144 197 L 136 197 A 6 6 0 0 1 130 191 L 130 177 L 140 167 Z\"/>\'\n"
"     + \'<path class=\"darm\" id=\"P-b14\" d=\"M 130 177 L 116 177 A 6 6 0 0 1 110 171 L 110 163 A 6 6 0 0 1 116 157 L 130 157 L 140 167 Z\"/>\'\n"
"     + \'<path class=\"darm\" id=\"P-b15\" d=\"M 150 157 L 164 157 A 6 6 0 0 1 170 163 L 170 171 A 6 6 0 0 1 164 177 L 150 177 L 140 167 Z\"/>\';\n"
"  /* centre cluster -- Guide on the mirror line, BACK/START straddling it */\n"
"  s += \'<circle class=\"part guide\" id=\"P-b16\" cx=\"180\" cy=\"84\" r=\"13\"/>\';\n"
"  s += \'<rect class=\"part\" id=\"P-b8\" x=\"139\" y=\"104\" width=\"34\" height=\"18\" rx=\"9\"/>\'\n"
"     + \'<text class=\"lbl\" x=\"156\" y=\"113\" style=\"font-size:8px\">BACK</text>\';\n"
"  s += \'<rect class=\"part\" id=\"P-b9\" x=\"187\" y=\"104\" width=\"34\" height=\"18\" rx=\"9\"/>\'\n"
"     + \'<text class=\"lbl\" x=\"204\" y=\"113\" style=\"font-size:8px\">START</text>\';\n"
"  /* face buttons -- label carries the canonical colour too, so the letter is\n"
"   * still identifiable when the ring is a single hairline at small sizes */\n"
"  s += \'<circle class=\"part ring-y\" id=\"P-b3\" cx=\"270\" cy=\"82\" r=\"15\"/>\'\n"
"     + \'<text class=\"lbl lbl-y\" x=\"270\" y=\"82\">Y</text>\'\n"
"     + \'<circle class=\"part ring-x\" id=\"P-b2\" cx=\"246\" cy=\"106\" r=\"15\"/>\'\n"
"     + \'<text class=\"lbl lbl-x\" x=\"246\" y=\"106\">X</text>\'\n"
"     + \'<circle class=\"part ring-b\" id=\"P-b1\" cx=\"294\" cy=\"106\" r=\"15\"/>\'\n"
"     + \'<text class=\"lbl lbl-b\" x=\"294\" y=\"106\">B</text>\'\n"
"     + \'<circle class=\"part ring-a\" id=\"P-b0\" cx=\"270\" cy=\"130\" r=\"15\"/>\'\n"
"     + \'<text class=\"lbl lbl-a\" x=\"270\" y=\"130\">A</text>\';\n"
"  /* right stick */\n"
"  s += \'<circle class=\"well\" cx=\"220\" cy=\"167\" r=\"28\"/>\'\n"
"     + \'<circle class=\"knob\" id=\"P-s1\" cx=\"220\" cy=\"167\" r=\"16\"/>\';\n"
"  s += \'</svg>\';\n"
"  return s;\n"
"}\n"
"\n"
"/* Build (once) and return the cached handles for one pad slot. */\n"
"function padView(slot, host) {\n"
"  var v = PT_VIEWS[slot], wrap, i;\n"
"  if (v && v.wrap.parentNode === host) { return v; }\n"
"  wrap = document.createElement(\'div\');\n"
"  wrap.innerHTML = \'<div class=\"muted\" style=\"font-size:12px\"></div>\'\n"
"                 + padSvgMarkup()\n"
"                 + \'<div class=\"pt-unmapped\"></div>\';\n"
"  host.appendChild(wrap);\n"
"  v = { wrap: wrap, title: wrap.firstChild, svg: wrap.getElementsByTagName(\'svg\')[0],\n"
"        extra: wrap.lastChild, btn: {}, };\n"
"  for (i = 0; i <= 16; i++) { v.btn[i] = v.svg.querySelector(\'#P-b\' + i); }\n"
"  v.lt = v.svg.querySelector(\'#P-lt\');\n"
"  v.rt = v.svg.querySelector(\'#P-rt\');\n"
"  v.s0 = v.svg.querySelector(\'#P-s0\');\n"
"  v.s1 = v.svg.querySelector(\'#P-s1\');\n"
"  PT_VIEWS[slot] = v;\n"
"  return v;\n"
"}\n"
"\n"
"function padDraw(v, g) {\n"
"  var i, b, val, extra = \'\', ax0, ax1, ax2, ax3;\n"
"  for (i = 0; i <= 16; i++) {\n"
"    b = g.buttons[i];\n"
"    if (!v.btn[i]) { continue; }\n"
"    v.btn[i].classList.toggle(\'on\', !!(b && (b.pressed || b.value > 0.12)));\n"
"  }\n"
"  /* triggers read as ANALOGUE where the pad reports it: a half-pulled\n"
"   * trigger that looks identical to a released one is the exact thing this\n"
"   * view exists to disprove. */\n"
"  val = g.buttons[6] ? g.buttons[6].value : 0;\n"
"  v.lt.setAttribute(\'width\', (60 * Math.max(0, Math.min(1, val))).toFixed(1));\n"
"  val = g.buttons[7] ? g.buttons[7].value : 0;\n"
"  v.rt.setAttribute(\'width\', (60 * Math.max(0, Math.min(1, val))).toFixed(1));\n"
"  ax0 = g.axes[0] || 0; ax1 = g.axes[1] || 0;\n"
"  ax2 = g.axes[2] || 0; ax3 = g.axes[3] || 0;\n"
"  v.s0.setAttribute(\'cx\', (90 + ax0 * 12).toFixed(1));\n"
"  v.s0.setAttribute(\'cy\', (106 + ax1 * 12).toFixed(1));\n"
"  v.s1.setAttribute(\'cx\', (220 + ax2 * 12).toFixed(1));\n"
"  v.s1.setAttribute(\'cy\', (167 + ax3 * 12).toFixed(1));\n"
"  v.s0.classList.toggle(\'on\', !!(g.buttons[10] && g.buttons[10].pressed));\n"
"  v.s1.classList.toggle(\'on\', !!(g.buttons[11] && g.buttons[11].pressed));\n"
"  v.title.innerHTML = \'<b>slot \' + g.index + \'</b> \' + esc(g.id);\n"
"  /* Anything past the standard 17 still has to be visible, or a pad with\n"
"   * extra paddles looks broken rather than unusual. */\n"
"  for (i = 17; i < g.buttons.length; i++) {\n"
"    if (g.buttons[i].pressed) { extra += (extra ? \', \' : \'\') + \'b\' + i; }\n"
"  }\n"
"  for (i = 4; i < g.axes.length; i++) {\n"
"    if (Math.abs(g.axes[i]) > 0.15) {\n"
"      extra += (extra ? \', \' : \'\') + \'axis\' + i + \' \' + g.axes[i].toFixed(2);\n"
"    }\n"
"  }\n"
"  v.extra.textContent = extra ? (\'also active: \' + extra) : \'\';\n"
"}\n"
"\n"
"function padTestFrame() {\n"
"  var el = document.getElementById(\'padtest\');\n"
"  if (el) {\n"
"    if (!navigator.getGamepads) {\n"
"      el.innerHTML = \'<span class=\"bad\">this browser has no Gamepad API</span>\';\n"
"    } else {\n"
"      var pads = navigator.getGamepads ? navigator.getGamepads() : [];\n"
"      var host = document.getElementById(\'pt-pads\');\n"
"      var diag = document.getElementById(\'pt-diag\');\n"
"      var found = 0, i, g, seen = {};\n"
"      if (!host) {\n"
"        el.innerHTML = \'<div id=\"pt-pads\"></div><div id=\"pt-diag\" class=\"muted\" \'\n"
"                     + \'style=\"margin-top:10px;font-size:12px\"></div>\';\n"
"        host = document.getElementById(\'pt-pads\');\n"
"        diag = document.getElementById(\'pt-diag\');\n"
"        PT_VIEWS = {};\n"
"      }\n"
"      for (i = 0; i < pads.length; i++) {\n"
"        g = pads[i];\n"
"        if (!g) { continue; }\n"
"        found++; seen[g.index] = 1;\n"
"        padDraw(padView(g.index, host), g);\n"
"      }\n"
"      for (var k in PT_VIEWS) {\n"
"        if (!seen[k] && PT_VIEWS[k].wrap.parentNode) {\n"
"          PT_VIEWS[k].wrap.parentNode.removeChild(PT_VIEWS[k].wrap);\n"
"          delete PT_VIEWS[k];\n"
"        }\n"
"      }\n"
"      if (!found && !document.getElementById(\'pt-empty\')) {\n"
"        host.innerHTML = \'<span id=\"pt-empty\" class=\"muted\">no gamepad visible to this browser yet. \'\n"
"            + \'The Gamepad API hides a pad until it sends input while this page \'\n"
"            + \'has focus &mdash; click this page, then press a button on your \'\n"
"            + \'device. A pad listed under Connected clients above but missing \'\n"
"            + \'here is a BROWSER-side problem, not an AtticPad one.</span>\';\n"
"      } else if (found && document.getElementById(\'pt-empty\')) {\n"
"        host.innerHTML = \'\';  /* drop the empty-state text, keep pads */\n"
"      }\n"
"      /* Always shown, found or not. When someone reports \"it shows\n"
"       * nothing\", THIS is the line worth reading: it distinguishes a\n"
"       * blocked API, a browser that never fired a connect event, and a\n"
"       * pad the browser saw and then lost. */\n"
"      diag.textContent = \'slots \' + found + \'/\' + pads.length\n"
"           + \' · connect events \' + PT_CONN\n"
"           + \' · disconnect \' + PT_DISC\n"
"           + (PT_LAST ? \' · last seen \' + PT_LAST : \'\')\n"
"           + \' · secure context \' + (window.isSecureContext ? \'yes\' : \'NO\')\n"
"           + \' · focus \' + (document.hasFocus() ? \'yes\' : \'no\');\n"
"    }\n"
"  }\n"
"  requestAnimationFrame(padTestFrame);\n"
"}\n"
"requestAnimationFrame(padTestFrame);\n"
"setInterval(refresh, 500);\n"
"</script>\n"
"</body>\n"
"</html>\n";

#endif /* ATTICPAD_HOST_COMMON_ASSETS_H */
