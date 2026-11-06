# Contributing Documentation

This document describes how Bless documentation is organized and how to
contribute to it.

For the broader development-to-documentation-to-skill workflow, see the
[Chinese project development guide](project-development-documentation-and-skills.md).

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
| Reference (`docs/guides/`, `docs/reference/`, `docs/reference/protocols/`) | How do I use, build, extend, and verify? |

Keep each document focused on its layer and link to related material.
Put build commands in guides/reference and detailed implementation tradeoffs in
the architecture or design documents.

## Directory Structure

```text
docs/
├── overview.md                    Documentation hub and architecture
├── concepts/                      Theory, metrics and interpretation
├── guides/                        Configuration, operation and diagnosis
├── reference/                     API, build, tests, evidence and plans
│   └── protocols/                 DNS/NTP/HTTP, IPv6 and QUIC constructors
├── design/                        Design rationale and proposed boundaries
├── zh-CN/                         Maintained Chinese explanations
│   ├── overview.md                Chinese entry point
│   ├── design/                    Design decisions and tradeoffs
│   └── guides/                    Runtime and scenario explanations
└── contributing/                  Documentation maintenance
```

## Where to Put New Content

| You are writing about... | Put it in... |
|--------------------------|-------------|
| Why Bless exists, what gap it fills | `README.md` |
| Information theory, entropy math, control theory | `docs/concepts/` |
| System architecture, design rationale, module layout | `docs/overview.md` |
| How to configure/run/deploy Bless | `docs/guides/` |
| API endpoints, build instructions, internal concurrency model | `docs/reference/` |
| A specific protocol's packet construction | `docs/reference/protocols/` |
| A proposed architecture or implementation boundary | `docs/design/` |
| A Chinese explanation of an existing topic | `docs/zh-CN/`, linked to its maintained reference |
| Benchmark methodology or results | `docs/reference/benchmarks.md` |

If you are unsure, start in `docs/guides/`.  A guide can always be promoted to
`concepts/` later.  Moving a concept back to `guides/` is harder.

## Style Conventions

### Theory documents (`concepts/`)

- Lead with definitions, assumptions and interpretation. Keep setup commands
  in guides; label implementation-specific sections and link to source/reference
  when necessary to explain what is actually measured.
- Use readable plaintext math or GitHub LaTeX; preserve the existing notation
  when it remains unambiguous.
- Link to the architecture/reference for implementation details.
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
- Use Markdown links relative to the containing file, for example
  `[Entropy theory](../concepts/entropy-theory.md)` from a guide.
- Prefer source filenames and symbols to unstable line numbers. Mark historical
  measurements and audit statuses with their revision and scope.

### Character Set

| Context | Rule |
|---------|------|
| C source, comments, config, shell | ASCII preferred |
| Theory documents (`concepts/`) | GitHub LaTeX allowed: `$$H(X) = -\sum_i p_i \log_2 p_i$$` |
| English prose | Prefer simple punctuation; avoid ornamental Unicode |
| Chinese prose | Chinese text and standard punctuation are expected; keep code identifiers unchanged |
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


## Chinese documentation and legacy material

Chinese explanations live under `docs/zh-CN/` and are listed in the main hub.
They may consolidate several old translations into one task-oriented guide.
Link to the current parameter/API/coverage references instead of maintaining
separate copied tables without an update owner.
When changing behavior explained in Chinese, update the corresponding Chinese
page and its reference in the same change.

When absorbing a legacy document:

1. Preserve useful reasoning and experimental scenarios.
2. Check numeric limits, defaults, units and mutability against current code.
3. Distinguish intended policy, implemented behavior, automated checks and
   measured hardware results.
4. Replace universal thresholds with workload assumptions and sample evidence.
5. Record what was absorbed or rejected in the
   [legacy reconciliation record](legacy-reconciliation.md).

Run `python3 tools/ci_doc_links.py` after edits.
That check validates paths only; separately check the hub contains every Markdown
page and review headings used by anchor links.
