# Deletion: unnaming and collecting

Removing a file is two separate problems. **Unnaming** takes the name out of the tracker — what a user means by
"delete". **Collecting** frees the chunks no remaining file uses — what bounds storage. Content addressing makes the
second one dangerous: two files that share a chunk store it once, so a file's chunks cannot simply be deleted with it.
This guide covers the decisions and the alternatives considered for each; every decision links to its full entry in
[`ARCHITECTURE.md`](../ARCHITECTURE.md). The design was first written down in D-023.

> **Status:** decided; being built.

## The decisions

### The owner's secret — [D-089](../ARCHITECTURE.md#d-089--a-name-is-deleted-by-its-owners-secret-the-uploader-chooses-it-the-tracker-keeps-only-its-hash-and-the-deletion-is-a-raft-entry)

```
REGISTER <name> <manifest-key> <owner-hash>     the uploader keeps the secret; the tracker keeps SHA-1 of it
UNREGISTER <name> <secret>                      -> OK | MISSING | DENIED
```

`client put` prints a random secret once; `client delete <name>` sends it back. The leader hashes it and the deletion
is a Raft entry like a registration, so every replica removes the name at the same point in the log. No secret is ever
in the log. A name registered without an owner (the old three-word form) can never be deleted.

| Alternative | Why not |
|---|---|
| **A token the tracker returns** | a retried registration must answer `OK` — and would hand the token to anyone who knows the name and key |
| **User accounts** | removed by D-004; one secret per name is enough |
| **Anyone may delete** | a vandalism tool |
| **Storing the secret** | every owner's secret in every replica's log file |
