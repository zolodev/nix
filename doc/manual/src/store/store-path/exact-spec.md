# Complete Store Path Calculation

This is the complete specification for how store paths are calculated.

Regular users do *not* need to know this information --- store paths can be treated as black boxes computed from the properties of the store objects they refer to.
But for those interested in exactly how Nix works, e.g. if they are reimplementing it, this information can be useful.

```bnf
<realized-path> ::= <store-dir>/<digest>-<name>
```
where

- `<digest>` = base-32 representation of the first 160 bits of a [SHA-256] hash of `<pre>`

  Th is :the hash part of the store name

- `<pre>` = the string `<type>:sha256:<inner-digest>:<store>:<name>`;

  Note that it includes the location of the store as well as the name to make sure that changes to either of those are reflected in the hash
  (e.g. you won't get `/nix/store/<digest>-name1` and `/nix/store/<digest>-name2`, or `/gnu/store/<digest>-name1`, with equal hash parts).

- `<name>` The name of the store object.

- `<store>` The [store directory](../store-path.md#store-directory)

- `<type>` = one of:

  - ```bnf
    text:<r1>:<r2>:...<rN>
    ```

    for encoded derivations written to the store.
    `<r1> ... <rN>` are the store paths referenced by this path.
    Those are encoded in the form described by `<realized-path>`.

  - ```bnf
    source:<r1>:<r2>:...:<rN>:self
    ```

    For paths copied to the store and hashed via a [Nix Archive (NAR)] and [SHA-256][sha-256].
    Just like in the text case, we can have the store objects referenced by their paths.
    Additionally, we can have an optional `:self` label to denote self reference.

  - ```bnf
    output:<id>
    ```

    For either the outputs built from derivations,
    paths copied to the store hashed that area single file hashed directly, or the via a hash algorithm other than [SHA-256][sha-256].
    (in that case "source" is used; it's silly, but it's done that way for compatibility).

    `<id>` is the name of the output (usually, "out").
    For content-addressed store objects, `<id>`, is always "out".

- `<inner-digest>` = base-16 representation of a SHA-256 hash of `<inner-pre>`

- `<inner-pre>` = one of the following based on `<type>`:

  - if `<type>` = `text:...`:

    the string written to the resulting store path.

  - if `<type>` = `source:...`:

    the the hash of the [Nix Archive (NAR)] serialization of the [file system object](../file-system-object.md) of the store object.

  - if `<type>` = `output:<id>`:

    - For input-addressed derivation outputs:

      the hash of the derivation modulo fixed output derivations.

    - For content-addressed store paths:

      the string `fixed:out:<rec><algo>:<hash>:`, where

      - `<rec>` = one of:

        - `r:` for [Nix Archive (NAR)] (arbitrary file system object) hashes

        - `` (empty string) for flat (single file) hashes

      - `<algo>` = `md5`, `sha1` or `sha256`

      -`<hash>` = base-16 representation of the path or flat hash of the contents of the path (or expected contents of the path for fixed-output derivations).

[Nix Archive (NAR)]: @docroot@/glossary.md#gloss-NAR
[sha-256]: https://en.m.wikipedia.org/wiki/SHA-256
