# Bedrock signed records

Every wipe certificate and every health record Bedrock writes is one JSON file
that anyone can check, offline, at https://erdem6.com/verify. Nothing is uploaded:
the page checks the signature in the browser.

## Envelope

```json
{
  "format": "bedrock-signed-1",
  "kind": "sanitization",
  "payload": "<base64 of the payload JSON, exact bytes>",
  "signature": "<base64 of the 64-byte Ed25519 signature over those bytes>",
  "public_key": "<base64 of the 32-byte Ed25519 public key>",
  "key_id": "<first 16 hex digits of SHA-512(public key)>"
}
```

- `kind` is `sanitization` (a wipe certificate) or `health` (a test record). It
  is repeated inside the payload, which is what the signature covers.
- The signature covers the decoded payload bytes, so no JSON canonicalisation is
  needed. Base64 is standard (RFC 4648 section 4) with padding.
- Ed25519 is RFC 8032 (pure Ed25519, no prehash).

## Keys

Each Bedrock stick makes its own key pair on first use and keeps it on the stick's
FAT partition in `bedrock/keys/`. The key ID is printed on every certificate and
shown on screen. A signature proves the record was not changed after this key
signed it; it does not prove who holds the key. An organisation that wants that
publishes its key ID, and checkers compare it (`/verify?key=<key_id>`).

## Payload

The payload is the machine record (`record_json()` in `tools/src/record.c`) with
these extra top-level keys:

| key | meaning |
|---|---|
| `kind` | `sanitization` or `health` |
| `operator`, `organization`, `location`, `asset_tag` | from `bedrock.cfg` or typed at the station |
| `media` | sanitization only: `{model, serial, kind, capacity_bytes, interface}` |
| `sanitization` | sanitization only: `{category, technique, details, verification, result, started, finished}` |

`category` is `Clear` or `Purge`, the NIST SP 800-88 Rev. 2 categories. Bedrock
records the fields the guideline lists; it is not a certification of compliance.
