/* Analytics, and the consent it is not allowed to skip.
 *
 * ── PUT YOUR MEASUREMENT ID ON THE NEXT LINE AND NOWHERE ELSE ──────────── */
var OD_GA_ID = "";         /* e.g. "G-XXXXXXXXXX". Empty = analytics off. */
/* ─────────────────────────────────────────────────────────────────────────
 *
 * WHY THIS IS NOT JUST THE SNIPPET GOOGLE GIVES YOU.
 *
 * The developer is in the EU and so is a large share of the audience. Under
 * the ePrivacy rules, analytics cookies are not "necessary", and consent has
 * to come BEFORE they are set -- not be assumed, and not be collected by a
 * banner that has already loaded the tracker behind it. Pasting gtag straight
 * into the page sets its cookies on first paint, which is the exact thing that
 * is not allowed.
 *
 * So nothing google loads until someone presses Accept. Decline stores the
 * refusal and asks no more; a visitor who does neither is never tracked.
 *
 * NOT ON THE GAME. This file is included by the site pages only. /play/ stays
 * free of it, which keeps "the game collects nothing" true, keeps the itch.io
 * and Discord frames clean, and means a school can open the game without a
 * consent dialog appearing in front of a class.
 */
(function () {
  var KEY = "od-consent-analytics";

  function remembered() {
    try { return localStorage.getItem(KEY); } catch (e) { return null; }
  }
  function remember(v) {
    try { localStorage.setItem(KEY, v); } catch (e) { /* private mode: ask again */ }
  }

  function loadGA() {
    if (!OD_GA_ID) return;
    window.dataLayer = window.dataLayer || [];
    window.gtag = function () { window.dataLayer.push(arguments); };
    gtag("js", new Date());
    // IP anonymisation is on by default in GA4; this is the rest of the
    // tightening -- no ad signals, no cross-site identifiers.
    gtag("config", OD_GA_ID, { anonymize_ip: true, allow_google_signals: false,
                               allow_ad_personalization_signals: false });
    var s = document.createElement("script");
    s.async = true;
    s.src = "https://www.googletagmanager.com/gtag/js?id=" + encodeURIComponent(OD_GA_ID);
    document.head.appendChild(s);
  }

  function banner() {
    var el = document.createElement("div");
    el.className = "cookiebar";
    el.setAttribute("role", "dialog");
    el.setAttribute("aria-label", "Analytics cookies");
    el.innerHTML =
      '<p>We would like to count visits, using Google Analytics. It sets cookies, ' +
      'so we are asking first. Decline and the site works exactly the same. ' +
      '<a href="/cookies">What this collects</a></p>' +
      '<div class="cookiebar-b">' +
      '<button type="button" data-od="no" class="btn ghost">Decline</button>' +
      '<button type="button" data-od="yes" class="btn">Accept</button></div>';
    el.addEventListener("click", function (e) {
      var b = e.target.closest("button[data-od]");
      if (!b) return;
      var yes = b.dataset.od === "yes";
      remember(yes ? "granted" : "denied");
      el.remove();
      if (yes) loadGA();
    });
    document.body.appendChild(el);
  }

  // No ID configured means there is nothing to consent to, so do not ask.
  if (!OD_GA_ID) return;

  var choice = remembered();
  if (choice === "granted") loadGA();
  else if (choice !== "denied") {
    if (document.readyState === "loading")
      document.addEventListener("DOMContentLoaded", banner);
    else banner();
  }
})();
