/* Analytics, and the consent it is not allowed to skip.
 *
 * ── PUT YOUR MEASUREMENT ID ON THE NEXT LINE AND NOWHERE ELSE ──────────── */
var OD_GA_ID = "G-6DXCXHCNRL";   /* Empty string = analytics off entirely. */
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
    countClicks();
  }

  /* ── THE FIVE THINGS WORTH COUNTING ───────────────────────────────────
   *
   * GA4 arrived with no key events configured at all, which meant the
   * property could say 358 people visited and nothing whatsoever about what
   * any of them did. In the 30 days to 15 Sep 2026 it recorded 784 page views,
   * 46 scrolls and 35 clicks -- and no way to tell a visitor who bounced from
   * one who found the download, because no click was distinguished from any
   * other.
   *
   * These five are the site's whole job, in order: did they reach the game,
   * did they take a build, did they go to the store page where a rating
   * lives, did they join the community, did they look at the source. Mark any
   * of them as a key event in the GA4 UI and the funnel becomes readable
   * without another line of code here.
   *
   * ONE DELEGATED LISTENER, and it sends a name and nothing else. No path, no
   * identifier, no dwell time -- the consent this file collects is for
   * counting visits, and an event that carried more than a name would be
   * collecting something the visitor was not asked about.
   *
   * It is installed only after consent, so declining leaves the page with no
   * listener rather than a listener that decides not to send. */
  function countClicks() {
    document.addEventListener("click", function (e) {
      var a = e.target.closest && e.target.closest("a[href]");
      if (!a) return;
      var href = a.getAttribute("href") || "";
      var name = null;

      if (/^\/play\/?$/.test(href)) name = "play_opened";
      else if (/\.(zip|apk|exe|dmg|AppImage|tar\.gz)$/i.test(href)) name = "download_started";
      else if (href.indexOf("itch.io/open-doctrines") !== -1) name = "itch_opened";
      else if (href.indexOf("discord.gg/") !== -1) name = "discord_opened";
      else if (href.indexOf("github.com/Pr1nted/Open-Doctrines") !== -1) name = "source_opened";

      if (name && window.gtag) gtag("event", name);
    }, true);
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
