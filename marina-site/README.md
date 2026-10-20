# marina-site

The [Marina language](../README.md) website — built with [Eleventy (11ty)](https://www.11ty.dev) as a static site, plain HTML + CSS, deployed to Vercel at **https://marina-lang.vercel.app**.

## Pages

| URL | Source | Content |
|---|---|---|
| `/` | `src/index.html` | Hero, feature cards, quickstart, benchmark preview with progress bars |
| `/docs/language` | `src/docs/language.html` | Language features: types, let/const, arrays, objects/classes, generics, interfaces, async, control flow, std modules, compilation pipeline |
| `/docs/wave` | `src/docs/wave.html` | Wave — the compiler + package manager CLI: build, commands, projects, one-off files, editor support |
| `/docs/packages` | `src/docs/packages.html` | The decentralized package manager: registry model, `WAVE_REGISTRY`, `wave add` from local paths / git URLs, authoring packages |
| `/benchmark` | `src/benchmark.html` | Full benchmarks (throughput, latency, memory, concurrency sweep) rendered as progress bars from `src/_data/benchmarks.json` |

Benchmark numbers come from `../benchmark.md` and are normalized to percentage widths (throughput bars scale to the fastest runtime, memory bars to the heaviest).

## Develop

```sh
npm install
npm run dev     # eleventy --serve at http://localhost:8080
npm run build   # outputs to _site/
```

## Deploy to Vercel

The repo already contains `vercel.json` (build: `npm run build`, output: `_site`, cleanUrls).

**Option A — Dashboard:** Vercel → *Add New Project* → import the repo → set **Root Directory** to `marina-site` → Deploy → set the domain to `marina-lang.vercel.app` under *Project Settings → Domains*.

**Option B — CLI:**

```sh
cd marina-site
npx vercel login
npx vercel --prod            # first run links the project; name it marina-lang
npx vercel domains ...       # or assign marina-lang.vercel.app in the dashboard
```

Framework preset: **Other** (it's plain Eleventy — no auto-detection needed since `vercel.json` specifies the build command and output directory).