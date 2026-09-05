# Migration from IntelEngine

This fork changes the active architecture while retaining released engine identities. No new quest records, spawned actors, AI packages or actor flags are required for personal stories. The old task scripts remain as a compatibility bridge.

* IEID and IETK retain their original identifiers and readable layouts.
* IEPD stores political state inside each co-save instead of sharing a writable database across saves. Legacy external databases are read-only import sources. Retired politics do not resume simulation.
* IEBT records battle cleanup ownership and exact faction leases. Only explicitly spawned actors qualify for destructive cleanup. No global bounty erasure or assumed player faction rank is applied.
* IEPG stores versioned personal-story state. Only actual evidence advances milestones; a separate actual later conversation supplies reflection.
* Session epochs reject asynchronous work crossing a save load. Request/arm generations reject replaced work within a save. Engine objects are read on the main thread, while prompt callbacks read immutable/cached data.

Clearing a dungeon and visiting it are distinct outcomes. Gatherings require inventory supplies, attendance and directed conversation at the appointment; they do not claim food was consumed. Reunions require an exchange in both directions. Reflection stores an actual spoken intention, never an implicit follower/employment change.

The original implementation omitted some original actor values and used raw load-order-sensitive integers in some legacy actor lists. Unknown ownership is retained inert, not guessed. This migration cannot reconstruct information upstream never saved. It does not edit or clean save files. Removing the compatibility ESP from a save that already uses it is not a supported shortcut.

Unknown personal-story records and unresolved actor mappings are quarantined under an inert version; any retained bytes are diagnostic data, not future automatically recoverable state. Unknown battle records are rejected. Records too large or truncated to preserve are explicitly marked disabled on a subsequent user save and logged; no fallback silently invents a new history. Returning to the original save remains the operator's normal load operation.

