# Contributing Documentation

This document describes how Bless documentation is organized and how to
contribute to it.

---

## Architecture

Bless documentation follows a **five-layer model** where each layer answers
exactly one question:

| Layer | Question |
|-------|----------|
| Identity (`README.md`) | What is Bless and why does it exist? |
| Theory (`docs/concepts/`) | What is the scientific basis? |
| Architecture (`docs/overview.md`) | How does Bless work and why these design choices? |
| Methodology (`docs/reference/benchmarks.md`) | How do we measure and validate performance? |
| Reference (`docs/guides/`, `docs/reference/`, `docs/protocols/`) | How do I use, build, extend, and verify? |

No content crosses layer boundaries.  A build instruction does not belong in
the theory document.  A design rationale does not belong in the user guide.

## Directory Structure

```
docs/
├── overview.md                    <- Documentation hub (the entry point)
│
├── concepts/                      <- Theory and foundations (paper-level, no code)
│   ├── entropy-theory.md
│   ├── min-entropy.md
│   ├── mutual-info.md
│   └── rate-psd.md
│
├── guides/                        <- User-facing how-to (task-oriented)
│   ├── config.md
│   ├── runtime.md
│   ├── docker.md
│   ├── profiling.md
│   ├── observability.md
│   ├── dashboard.md
│   ├── security.md
│   ├── parsing.md
│   └── error-handling.md
│
├── reference/                     <- Developer-facing (API, build, methodology)
│   ├── benchmarks.md              <- Frozen -- see below
│   ├── BUILDING.md
│   ├── repository-publication.md
│   ├── api.md
│   ├── concurrency.md
│   ├── fuzzing.md
│   └── priority.md
│
└── protocols/                     <- Per-protocol feature docs
    ├── dns.md
    ├── ipv6.md
    └── quic.md
```

## Where to Put New Content

| You are writing about... | Put it in... |
|--------------------------|-------------|
| Why Bless exists, what gap it fills | `README.md` |
| Information theory, entropy math, control theory | `docs/concepts/` |
| System architecture, design rationale, module layout | `docs/overview.md` |
| How to configure/run/deploy Bless | `docs/guides/` |
| API endpoints, build instructions, internal concurrency model | `docs/reference/` |
| A specific protocol's packet construction | `docs/protocols/` |
| Benchmark methodology or results | `docs/reference/benchmarks.md` |

If you are unsure, start in `docs/guides/`.  A guide can always be promoted to
`concepts/` later.  Moving a concept back to `guides/` is harder.

## Style Conventions

### Theory documents (`concepts/`)

- **No code.**  No function names, no file paths, no YAML snippets, no API
  references.  These documents should be readable by someone who has never seen
  the BLESS source.
- **Mathematical notation** in plaintext: `H(X) = -SUM p_i log_2(p_i)`.
- **Last line:** "For implementation details, see the Architecture & Design
  overview."
- **Self-contained.**  Can be distributed independently.  Think "paper
  appendix."

### Guides (`guides/`)

- **Task-oriented.**  Start with what the reader wants to accomplish, not what
  the feature does.
- **YAML and CLI examples** are encouraged.  Show the exact command or config
  block.
- **Cross-reference** to `concepts/` for theory, `reference/` for API details.

### Reference (`reference/`)

- **Exhaustive, not narrative.**  Tables of parameters, endpoints, error codes.
- **Keep up to date with the code.**  Stale reference docs are worse than no
  reference docs.  When you change a CLI flag or API endpoint, update the
  corresponding reference doc in the same commit.

### General

- **No AI-isms.**  Avoid phrases like "it is noteworthy that," "interestingly,"
  "it is important to note."  State the fact directly.
- **One sentence per line** in source (for clean diffs).  Rendered output
  wraps normally.
- **Cross-reference with relative paths from `docs/` root:**
  `docs/concepts/entropy-theory.md`.

### Character Set

| Context | Rule |
|---------|------|
| C source, comments, config, shell | ASCII preferred |
| Theory documents (`concepts/`) | GitHub LaTeX allowed: `$$H(X) = -\sum_i p_i \log_2 p_i$$` |
| General prose | No em-dashes, smart quotes, or ornamental Unicode |
| ASCII art diagrams | Box-drawing characters permitted |

Do not convert LaTeX to ASCII fallbacks (`SUM`, `Hinf`, `<=`).

## Frozen Documents

`docs/reference/benchmarks.md` is **frozen**.  It has reached diminishing
returns on wording polish.

Modify it only for:
- New benchmark tiers with measured data
- Re-running existing tiers with methodology changes that produce new numbers
- Inserting genuinely new measurements

Do NOT modify for wording, phrasing, clarity, or restructuring.  If you are
asked to polish benchmark-doc wording, push back.

## The Documentation Hub

`docs/overview.md` is the single entry point.  It contains:

1. A five-layer model table
2. A categorized index of every document (Concepts / User Guide / Developer
   Guide / Reference / Protocols)
3. The actual architecture and design content (chapters 1-4)
4. A "Further Reading" pointer back to the index

When you add, remove, or rename a document, you **must** update the index in
`docs/overview.md`.  The hub is the source of truth for what documentation
exists.
