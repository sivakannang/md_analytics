# ADR-0001: Record architecture decisions as ADRs

- **Status:** Accepted
- **Date:** 2026-10-19
- **Deciders:** Sivakannan, Claude

## Context

md_analytics is a long-lived project with an explicit learning goal. Many decisions —
language, libraries, IPC, storage, threading model — will be made over months and
revisited as the product grows. Without a record:

- the *reason* behind a decision is forgotten, so it gets re-argued or wrongly reversed;
- alternatives that were rejected (and why) are lost, which is exactly the knowledge
  interviews probe;
- code reviews lack a reference for "why is it built this way?".

## Options considered

### Option A — No formal record (comments, commit messages, memory)
- Pros: zero overhead.
- Cons: reasoning is scattered or lost; rejected alternatives never written down.

### Option B — One large design document
- Pros: single place to read.
- Cons: becomes stale; history of *when* and *why* something changed is lost in edits.

### Option C — Architecture Decision Records (one short file per decision)
- Pros: lightweight; immutable history; each decision reviewable on its own; supersession is explicit.
- Cons: small discipline cost per decision; needs an index to stay navigable.

## Decision

Use ADRs (Option C), stored in `docs/adr/`, numbered sequentially
(`NNNN-kebab-title.md`), following [`template.md`](template.md), and listed in the
[index](README.md).

## Rationale

ADRs give us the "why" alongside the "what" at almost no cost, and their
*options considered* section directly serves the learning goal. Accepted ADRs are not
rewritten; a changed decision gets a new ADR that supersedes the old one, preserving
history.

## Consequences

- Positive: decisions are explainable and reviewable; the ADR set doubles as interview preparation.
- Negative: each significant decision costs ~15 minutes of writing.
- What needs an ADR: choices that are hard to reverse, affect multiple components,
  add a dependency, or were contested. Small, local choices go in design notes or code comments.
- Large components additionally get a design note in `docs/design/` describing
  interfaces and behaviour; ADRs record the decisions inside them.

## Learning notes

- ADRs were popularized by Michael Nygard (2011). Many trading firms use similar
  "design review" documents before building infrastructure.
- Interview angle: "Tell me about a technical decision you made and the alternatives
  you rejected" — each ADR is a ready answer.
