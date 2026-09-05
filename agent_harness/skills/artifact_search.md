# Skill: search for and retrieve a published artifact

You have been given an **Artifact** node. Your job is **acquisition, not
implementation**: find the real, published file this node names, download it,
verify it, place it exactly where the node says, and record where it came
from. You are not writing code, not summarising the data, and not
reconstructing it from memory — a hand-typed approximation of a published
dataset is the exact failure this node type exists to prevent.

## 1. Identify the canonical source

Work out what the artifact actually is from the node description, then find its
**primary** home — in this order of trust:

1. The project or author's own repository / release page (GitHub releases, a
   lab or university page, an official mirror named by the project).
2. A recognised data registry or archive (Zenodo, the ACL Anthology, Hugging
   Face for models/datasets, data.gov, an RFC/ISO/Unicode publication page).
3. A well-known redistribution used by the ecosystem (a language's package
   registry, a maintained `-data` package) **only** when it is clearly a
   verbatim copy.

Avoid: blog reposts, Stack Overflow pastes, random Gists, SEO "download"
sites, anything that has visibly reformatted or "cleaned" the data. If two
sources disagree on content, prefer the one closest to the author.

Use your web-search and fetch tools for this. If the node or the harness
brief already gives a `source:` URL, start there but still sanity-check it is
the primary source and is still live.

## 2. Retrieve it

- Download the file itself, not an HTML page about it. Follow redirects.
- If the source only offers an archive (`.zip`, `.tar.gz`), extract exactly
  the file(s) the node needs and discard the rest.
- If the node pins a `sha256:`, the bytes you save MUST hash to it. If they
  don't, you have the wrong file or a truncated download — do not "fix" it by
  editing the file or changing the expectation.
- Do not re-encode, re-sort, normalise line endings, or strip comments/headers
  unless the node explicitly asks for a transformed form. Save it as it is
  published.
- If the full file is very large and the node only needs a bounded seed, save
  the full file if the budget allows; otherwise take a **documented,
  deterministic** slice (e.g. "first N rows of the upstream file, unmodified")
  and say so in the provenance note and your summary.

## 3. Verify before you place it

- The size is in a plausible range for what this is (a sentiment lexicon is
  tens to hundreds of KB, not 2 KB and not 2 GB).
- The format matches the node's description — open it and look. Column count,
  delimiter, header row, encoding.
- Spot-check 3–5 entries against anything concrete the node description says
  ("must contain the Loughran–McDonald `uncertainty` category", "VADER scores
  are roughly -4..+4"). You are checking it is the right artifact, not
  auditing every row.

## 4. Place it and record provenance

- Write the file to the **exact** repo-relative path the node names. Create
  parent directories as needed. Nothing outside your allowed paths.
- Alongside it, write `<path>.provenance.json`:

  ```json
  {
    "artifact": "<node name>",
    "source_url": "<the URL you actually downloaded from>",
    "canonical_page": "<the human landing page, if different>",
    "retrieved_utc": "<ISO-8601 timestamp>",
    "sha256": "<hex digest of the file you saved>",
    "bytes": <size>,
    "license": "<SPDX id or short name, or 'unknown'>",
    "transform": "none | 'first 400 rows of upstream, unmodified' | ...",
    "retrieved_by": "artifact_search skill"
  }
  ```

- In your summary: the source, the sha256, the license, and any transform.

## 5. If you genuinely cannot get it

Only for a real, nameable blocker — the source is behind a login or paywall,
there is no primary source and every copy is visibly altered, the download
requires a tool you do not have. Then:

- Do **not** fabricate the file or fill it with plausible-looking rows.
- Leave `<path>.provenance.json` with `"status": "unresolved"` and a
  `"blocker"` sentence, and put a `HARNESS-PARTIAL(<node name>): <blocker>`
  marker in it.
- Say so plainly in your summary so a later pass (or a human) can finish it.
