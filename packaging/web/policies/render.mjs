// The privacy policy and terms, as pages a browser will actually render.
//
// WHY THIS EXISTS. The account service already serves both documents, and the
// game links to it -- but it serves them as text/markdown, so a browser shows
// the source with its ## and ** on display. That is fine for the game, which
// only ever fetches the text; it is not fine for a link published on a Discord
// application profile, which is a thing strangers and reviewers click.
//
// Discord will not accept a workers.dev URL for a policy link at all: it is a
// shared suffix, so anyone can have one. pages.dev it does accept, which is
// why these are generated into the web deploy rather than added to the Worker.
//
// THE SOURCE IS THE SAME FILE THE WORKER IMPORTS. net/PRIVACY.md is read here
// and `import PRIVACY_POLICY from "../PRIVACY.md"` there, so the rendered page
// and the served text are the same document by construction. Two hand-kept
// copies of a legal document is exactly the failure worth designing out.

import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { marked } from 'marked';

const root = new URL('../../../', import.meta.url);
const here = new URL('./', import.meta.url);

const SITE = 'https://opendoctrines.pages.dev';

const PAGES = [
    { md: 'net/PRIVACY.md', out: 'privacy.html', slug: 'privacy', title: 'Privacy policy',
      desc: 'What the account service stores, why, and how long it keeps it.' },
    { md: 'net/TERMS.md',   out: 'terms.html',   slug: 'terms',   title: 'Terms of use',
      desc: 'The terms that cover the game, the website and the account service.' },
];

// Deliberately plain: no scripts, no fonts, no requests. A policy page that
// depends on anything is a policy page that can fail to be readable.
// The link card. Meta tags only -- consistent with the no-requests rule above,
// because nothing here is fetched BY the page: og:image is pulled by whatever
// is drawing the preview, and only when somebody shares the link.
//
// Generated rather than written into the HTML, because these two files are
// build output -- packaging/web/policies/.gitignore ignores *.html -- so tags
// added to the pages by hand survive exactly until the next render.
const card = ({ slug, title, desc }) => `<meta property="og:type" content="article">
<meta property="og:site_name" content="OpenDoctrines">
<meta property="og:title" content="OpenDoctrines — ${title}">
<meta property="og:description" content="${desc}">
<meta property="og:url" content="${SITE}/${slug}">
<meta property="og:image" content="${SITE}/card.png">
<meta property="og:image:width" content="1280">
<meta property="og:image:height" content="640">
<meta name="twitter:card" content="summary_large_image">
<meta name="twitter:title" content="OpenDoctrines — ${title}">
<meta name="twitter:description" content="${desc}">
<meta name="twitter:image" content="${SITE}/card.png">
<meta name="theme-color" content="#C9A227">`;

const shell = (page, body) => `<!DOCTYPE html>
<html lang="en"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>OpenDoctrines — ${page.title}</title>
${card(page)}
<style>
  :root{color-scheme:dark light}
  body{margin:0;background:#0b0f1c;color:#c8ccd8;
       font:16px/1.65 system-ui,-apple-system,Segoe UI,sans-serif}
  main{max-width:44em;margin:0 auto;padding:3em 1.4em 6em}
  h1,h2,h3{color:#ffd700;line-height:1.25}
  h1{font-size:1.7em;margin:0 0 .2em}
  h2{font-size:1.2em;margin:2.2em 0 .5em;border-top:1px solid #1d2740;padding-top:1.2em}
  h3{font-size:1.02em;margin:1.6em 0 .3em;color:#e8c65a}
  a{color:#7fb3ff}
  code{background:#141c33;padding:.12em .38em;border-radius:3px;font-size:.9em}
  pre{background:#141c33;padding:1em;border-radius:6px;overflow-x:auto}
  pre code{background:none;padding:0}
  hr{border:0;border-top:1px solid #1d2740;margin:2.4em 0}
  blockquote{margin:1.2em 0;padding:.1em 1.1em;border-left:3px solid #2a3a5e;color:#9aa4bb}
  table{border-collapse:collapse;width:100%;margin:1.2em 0;display:block;overflow-x:auto}
  th,td{border:1px solid #1d2740;padding:.5em .7em;text-align:left}
  .home{display:inline-block;margin-bottom:2.4em;color:#7c869e;text-decoration:none;
        font-size:.9em;letter-spacing:.04em}
  .home:hover{color:#c8ccd8}
  footer{margin-top:4em;padding-top:1.4em;border-top:1px solid #1d2740;
         color:#5d677d;font-size:.86em}
</style></head><body><main>
<a class="home" href="/">&larr; OpenDoctrines</a>
${body}
<footer>The game's account service serves this same document at
<code>/${page.slug}</code>; both are generated
from one file, so they cannot say different things.</footer>
</main></body></html>
`;

let wrote = 0;
for (const p of PAGES) {
    const md = readFileSync(new URL(p.md, root), 'utf8');
    // A legal document that silently rendered as nothing would be worse than
    // an error, so refuse anything implausibly short rather than publish it.
    if (md.length < 2000) throw new Error(`${p.md} is only ${md.length} bytes -- refusing to publish it`);
    const html = shell(p, marked.parse(md));
    writeFileSync(new URL(p.out, here), html);
    console.log(`  ${p.out.padEnd(14)} ${(html.length / 1024).toFixed(0)} KB  from ${p.md}`);
    wrote++;
}
if (wrote !== PAGES.length) throw new Error('not every page was written');
