/*
 * FTP4DOS update channel - Cloudflare Worker
 * ---------------------------------------------------------------------------
 * Serves the update manifest, its signature and the executable over PLAIN HTTP,
 * because mTCP has no TLS and the DOS client therefore cannot follow anything
 * to https. GitHub is unusable directly: Pages enforces HTTPS on the default
 * *.github.io domain, raw/api/codeload all answer 301, and release assets
 * require a short-lived signed URL. A *.workers.dev subdomain answers on port
 * 80 without redirecting, which is the one property this whole channel needs.
 *
 * The Worker reads the repository's LATEST RELEASE, so publishing an update is
 * exactly what it always was - create a GitHub release. There is no second
 * copy of the binary anywhere and the Worker never needs redeploying.
 *
 * On trust: this Worker is deliberately NOT a trusted component, and it does
 * NOT generate the manifest. UPDATE.INF is written and signed offline with a
 * key that never leaves the maintainer's machine, then uploaded as a release
 * asset; the client verifies the RSA-2048 signature against public keys
 * compiled into the executable. Cloudflare, GitHub, and anyone intercepting
 * the plain-HTTP hop can deny service - none of them can produce an update the
 * client will accept.
 *
 * A Worker-generated manifest would be an unsigned one, which is exactly the
 * hole the signature exists to close. Do not be tempted.
 */

const REPO = "Projanglez/FTP4DOS";
const API = `https://api.github.com/repos/${REPO}/releases/latest`;

/* Explicit allow-list of release asset names. Without it this would be an open
 * proxy and would be abused within days. */
const FILES = {
  "/UPDATE.INF":  { asset: "UPDATE.INF",  type: "text/plain; charset=us-ascii" },
  "/UPDATE.SIG":  { asset: "UPDATE.SIG",  type: "application/octet-stream" },
  "/FTP4DOS.EXE": { asset: "FTP4DOS.EXE", type: "application/octet-stream" },
};

/* GitHub's unauthenticated API allows 60 requests per hour per IP, so the
 * release lookup is cached - but only briefly. It decides how long after
 * publishing a release the channel actually goes live, and 15 minutes of
 * "there is no update" after a successful upload is confusing enough to look
 * like a broken deployment. Assets are immutable once uploaded and can be
 * cached far longer. */
const RELEASE_TTL = 60;    /* 1 min  - publish-to-live delay */
const ASSET_TTL   = 900;   /* 15 min - immutable per release */

/* The client is a 16-bit real-mode DOS program speaking HTTP/1.0. Keep the
 * response boring: an explicit Content-Length (the progress bar needs a total,
 * and chunked encoding is not legal in an HTTP/1.0 reply), no compression, and
 * nothing it would have to parse past. */
function plain(body, contentType, status = 200, extra = {}) {
  const bytes = body instanceof ArrayBuffer
    ? body
    : new TextEncoder().encode(body);
  return new Response(bytes, {
    status,
    headers: {
      "Content-Type": contentType,
      "Content-Length": String(bytes.byteLength),
      /* Never cache errors. A 404 from the window between creating a release
       * and uploading its assets would otherwise stick around long after the
       * upload succeeded, which reads as a broken channel. */
      "Cache-Control": status === 200
        ? `public, max-age=${ASSET_TTL}`
        : "no-store",
      ...extra,
    },
  });
}

function text(msg, status) {
  return plain(msg + "\r\n", "text/plain; charset=us-ascii", status);
}

export default {
  async fetch(request) {
    const url = new URL(request.url);

    if (url.pathname === "/" || url.pathname === "") {
      return plain(
        "FTP4DOS update channel\r\n" +
        "Endpoints: /UPDATE.INF /UPDATE.SIG /FTP4DOS.EXE\r\n" +
        `Source: https://github.com/${REPO}\r\n`,
        "text/plain; charset=us-ascii"
      );
    }

    /* Case-insensitive: DOS users and tooling will type either case. */
    const key = Object.keys(FILES).find(
      (k) => k.toLowerCase() === url.pathname.toLowerCase()
    );
    if (!key) return text("not found", 404);

    if (request.method !== "GET" && request.method !== "HEAD")
      return text("method not allowed", 405);

    /* ?fresh=1 skips the cached release lookup. Without it there is no way to
     * tell "the release really has no manifest" from "the lookup is a cached
     * answer from before the assets were uploaded" - and the edge cache is
     * keyed by URL, so redeploying the Worker does not clear it. */
    const fresh = url.searchParams.has("fresh");

    /* 1. Resolve the latest release. */
    let release;
    try {
      /* A cache-busting query parameter, not cacheTtl: 0. The edge cache is
       * keyed by URL, and cacheTtl only governs how long a response is STORED -
       * it does not stop an existing entry from being read. Only a different
       * URL guarantees a fresh answer. */
      const apiUrl = fresh ? `${API}?_cb=${Date.now()}` : API;
      const r = await fetch(apiUrl, {
        headers: {
          /* GitHub rejects API requests without a User-Agent. */
          "User-Agent": "ftp4dos-update-worker",
          "Accept": "application/vnd.github+json",
        },
        cf: fresh
          ? { cacheTtl: 0 }
          : { cacheEverything: true, cacheTtl: RELEASE_TTL },
      });
      if (!r.ok) return text(`github api ${r.status}`, 502);
      release = await r.json();
    } catch (e) {
      return text("github api unreachable", 502);
    }

    const want = FILES[key];
    const asset = (release.assets || []).find((a) => a.name === want.asset);
    if (!asset) {
      /* A release without the signed manifest is not a usable update. Saying
       * so plainly beats handing the client something it cannot verify. */
      return text(`release ${release.tag_name || "?"} has no ${want.asset}`, 404);
    }

    /* 2. Fetch the asset. The API asset URL plus this Accept header redirects
     * to the signed storage URL, which fetch() follows for us over HTTPS - the
     * step the DOS client cannot perform itself. */
    let upstream;
    try {
      upstream = await fetch(asset.url, {
        headers: {
          "User-Agent": "ftp4dos-update-worker",
          "Accept": "application/octet-stream",
        },
        cf: { cacheEverything: true, cacheTtl: ASSET_TTL },
      });
    } catch (e) {
      return text("asset unreachable", 502);
    }
    if (!upstream.ok) return text(`asset ${upstream.status}`, 502);

    /* Buffering is what lets us state a truthful Content-Length. The largest
     * file is the ~270 KB executable, well inside a Worker's memory budget. */
    const buf = await upstream.arrayBuffer();

    const headers = {
      "Content-Type": want.type,
      "Content-Length": String(buf.byteLength),
      "Cache-Control": `public, max-age=${ASSET_TTL}`,
      /* Handy when debugging from a modern machine; the DOS client ignores it. */
      "X-FTP4DOS-Release": release.tag_name || "",
    };

    if (request.method === "HEAD")
      return new Response(null, { status: 200, headers });

    return new Response(buf, { status: 200, headers });
  },
};
