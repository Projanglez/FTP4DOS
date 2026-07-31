# FTP4DOS update channel (Cloudflare Worker)

Serves the update manifest, its signature and the executable over **plain
HTTP**, because mTCP has no TLS and the DOS client cannot follow a redirect to
HTTPS.

## Why a Worker and not GitHub directly

Every GitHub route was tried and measured first:

| Route | Result on port 80 |
|---|---|
| GitHub Pages, default `*.github.io` domain | 301 to https — "Enforce HTTPS" is **required** for the default domain and cannot be turned off |
| Release assets (`release-assets.githubusercontent.com`) | needs a short-lived signed URL; unsigned gives `618 jwt:jwt-not-provided` |
| `raw.` / `api.` / `codeload.` / `gist.` / `media.` | 301 to https |
| GitLab Pages, Codeberg, Neocities, SourceForge, surge.sh | all force HTTPS |
| **`*.workers.dev`** | **200 OK, no redirect** |

A `*.github.io` Pages site created before the requirement (`octocat.github.io`,
for instance) still answers on port 80, which is misleading — new sites do not.

## What it serves

The Worker reads the repository's **latest GitHub release** and hands out three
of its assets:

| Path | Release asset |
|---|---|
| `/UPDATE.INF` | `UPDATE.INF` — the signed manifest |
| `/UPDATE.SIG` | `UPDATE.SIG` — RSA-2048 signature over the manifest bytes |
| `/FTP4DOS.EXE` | `FTP4DOS.EXE` |

So publishing an update is exactly what it always was: create a GitHub release.
No second copy of the binary anywhere, and the Worker never needs redeploying.

## Trust model

**This Worker is not a trusted component, and it does not generate the
manifest.** `UPDATE.INF` is written and signed offline with a key that never
leaves the maintainer's machine; the client verifies the RSA-2048 signature
against public keys compiled into the executable. Cloudflare, GitHub, and anyone
intercepting the plain-HTTP hop can deny service, but none of them can produce
an update the client accepts.

That is what makes an unencrypted transport acceptable here, and it is why the
signature was built before the download path rather than bolted on afterwards.

Letting the Worker synthesise the manifest from release metadata would be
convenient and would quietly remove that protection — the client would be
trusting Cloudflare and GitHub again. Don't.

## Deploying

Needs a free Cloudflare account and Node.js 18 or newer. No credit card, and
nothing is billed at this volume — the free plan allows 100,000 requests a day,
and workers.dev is meant for exactly this kind of hobby traffic.

**1. Create the account** at <https://dash.cloudflare.com/sign-up> — e-mail plus
password, confirm the address. You do not need to add a domain: this Worker runs
on a `workers.dev` subdomain, not on a zone of your own.

**2. Authorise the CLI.** From the repository root:

```bash
cd tools/worker
npx wrangler login
```

`npx` fetches wrangler on demand, so nothing is installed globally. A browser
window opens asking you to authorise "Wrangler" against your account; confirm,
then return to the terminal.

**3. Deploy.**

```bash
npx wrangler deploy
```

On the very first deploy Cloudflare asks you to choose an account-wide
`workers.dev` subdomain (for example `bjoern`, giving `*.bjoern.workers.dev`).
It cannot be changed afterwards, so pick something neutral. Wrangler then prints
the URL, of the form `ftp4dos-update.<subdomain>.workers.dev`.

**4. Verify plain HTTP.** This is the property the whole channel rests on, so
check it explicitly rather than assuming:

```bash
curl -s -o /dev/null -w '%{http_code} %{redirect_url}\n' \
  http://ftp4dos-update.<subdomain>.workers.dev/
```

`200` means the channel works. A `301` would mean the request was redirected to
HTTPS, which the DOS client cannot follow — stop and investigate rather than
building on it.

Note the endpoints only return content once a release carries `UPDATE.INF`,
`UPDATE.SIG` and `FTP4DOS.EXE` as assets; until then `/UPDATE.INF` reports that
the release has no manifest.

Put the host into `src/update.h` (`UPD_HOST`) and rebuild, or override it per
machine in `MTCP.CFG`:

```
FTP4DOS_UPDHOST ftp4dos-update.your-subdomain.workers.dev
FTP4DOS_UPDPATH /
```

Verify it answers on port 80 **without** a redirect — this is the property the
whole channel depends on:

```bash
curl -s -o /dev/null -w '%{http_code} %{redirect_url}\n' \
  http://ftp4dos-update.<subdomain>.workers.dev/UPDATE.INF
# expected: 200   (a 301 here means the channel is unusable)
```

## Publishing an update

The Worker never needs redeploying. It proxies the repository's latest GitHub
release, so publishing is creating a release and attaching the signed assets —
see `tools/publish-update.ps1`.

## Notes

- Responses carry an explicit `Content-Length`. The Worker buffers the file to
  produce it, which the progress bar on the DOS side needs and which avoids
  chunked encoding (illegal in the HTTP/1.0 replies the client asks for).
- The path allow-list in `worker.js` is deliberate. Without it this would be an
  open proxy and would be abused.
- Cloudflare documents workers.dev as a free-tier, non-business-critical
  domain. That matches this use, but it is the reason the client must fail
  gracefully when the channel is unreachable.
