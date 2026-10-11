/**
 * The chat crypto of docs/chat-protocol.md §4, for the launcher's direct messages
 * (docs/social-protocol.md). Byte for byte the same as Dolphin's (Monocypher) and the server's
 * (`common::chat`, RustCrypto): Ed25519 identity keys and signatures, X25519 session keys,
 * BLAKE2b-256 pair keys and XChaCha20-Poly1305 boxes laid out as `nonce ‖ mac ‖ ciphertext`.
 *
 * Main process only (the renderer never sees a key).
 */

import { xchacha20poly1305 } from "@noble/ciphers/chacha";
import { ed25519, x25519 } from "@noble/curves/ed25519";
import { blake2b } from "@noble/hashes/blake2b";
import { randomBytes } from "crypto";

const KX_LABEL = "brawlonline-chat-kx-v1";
const KEY_LABEL = "brawlonline-chat-key-v1";
const MSG_LABEL = "brawlonline-chat-msg-v1";
const BOX_LABEL = "brawlonline-chat-box-v1";

/** Longest message text in bytes. */
export const MAX_TEXT_BYTES = 300;
/** Longest box the server relays. */
export const MAX_BOX_BYTES = 512;
const PLAINTEXT_VERSION = 1;
const PLAINTEXT_OVERHEAD = 1 + 8 + 64;
const NONCE_BYTES = 24;
const MAC_BYTES = 16;

const utf8 = new TextEncoder();

function concat(...parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
}

const ZERO = new Uint8Array([0]);

/** `label ‖ 0 ‖ group ‖ 0` */
function labelled(label: string, group: string): Uint8Array {
  return concat(utf8.encode(label), ZERO, utf8.encode(group), ZERO);
}

function u64le(n: bigint): Uint8Array {
  const b = new Uint8Array(8);
  new DataView(b.buffer).setBigUint64(0, n, true);
  return b;
}

export function toHex(b: Uint8Array): string {
  return Buffer.from(b).toString("hex");
}

/** Decodes hex of exactly `bytes` bytes (either case); undefined otherwise. */
export function fromHex(s: unknown, bytes?: number): Uint8Array | undefined {
  if (typeof s !== "string" || s.length % 2 !== 0 || !/^[0-9a-fA-F]*$/.test(s)) {
    return undefined;
  }
  if (bytes != null && s.length !== bytes * 2) {
    return undefined;
  }
  return new Uint8Array(Buffer.from(s, "hex"));
}

export function randomSecret(): Uint8Array {
  return new Uint8Array(randomBytes(32));
}

export function identityPublic(seed: Uint8Array): Uint8Array {
  return ed25519.getPublicKey(seed);
}

export function kxPublic(secret: Uint8Array): Uint8Array {
  return x25519.getPublicKey(secret);
}

function verify(idKey: Uint8Array, msg: Uint8Array, sig: Uint8Array): boolean {
  try {
    // Strict (RFC 8032) like the server's `verify_strict`, not ZIP 215.
    return ed25519.verify(sig, msg, idKey, { zip215: false });
  } catch {
    return false;
  }
}

/** True for an identity key the server would take: a valid point that is not of small order. */
export function isValidIdentityKey(key: Uint8Array): boolean {
  try {
    const p = ed25519.ExtendedPoint.fromHex(key);
    return !p.isSmallOrder();
  } catch {
    return false;
  }
}

export function signKx(idSeed: Uint8Array, group: string, kx: Uint8Array): Uint8Array {
  return ed25519.sign(concat(labelled(KX_LABEL, group), kx), idSeed);
}

export function verifyKx(idKey: Uint8Array, group: string, kx: Uint8Array, sig: Uint8Array): boolean {
  return verify(idKey, concat(labelled(KX_LABEL, group), kx), sig);
}

/** The key two sides share in `group`; undefined when their key is of small order. */
export function pairKey(group: string, ownSecret: Uint8Array, theirKx: Uint8Array): Uint8Array | undefined {
  let shared: Uint8Array;
  try {
    shared = x25519.getSharedSecret(ownSecret, theirKx);
  } catch {
    return undefined;
  }
  if (shared.every((b) => b === 0)) {
    return undefined;
  }
  const own = kxPublic(ownSecret);
  const [lo, hi] = Buffer.compare(Buffer.from(own), Buffer.from(theirKx)) <= 0 ? [own, theirKx] : [theirKx, own];
  return blake2b(concat(labelled(KEY_LABEL, group), shared, lo, hi), { dkLen: 32 });
}

function messageSignedBytes(group: string, from: string, seq: bigint, text: Uint8Array): Uint8Array {
  return concat(labelled(MSG_LABEL, group), utf8.encode(from), ZERO, u64le(seq), text);
}

export function signMessage(idSeed: Uint8Array, group: string, from: string, seq: bigint, text: Uint8Array) {
  return ed25519.sign(messageSignedBytes(group, from, seq, text), idSeed);
}

export function verifyMessage(
  idKey: Uint8Array,
  group: string,
  from: string,
  seq: bigint,
  text: Uint8Array,
  sig: Uint8Array,
): boolean {
  return verify(idKey, messageSignedBytes(group, from, seq, text), sig);
}

export function encodePlaintext(seq: bigint, sig: Uint8Array, text: Uint8Array): Uint8Array {
  return concat(new Uint8Array([PLAINTEXT_VERSION]), u64le(seq), sig, text);
}

/** A plaintext's parts; undefined for another version or a text that is empty or too long. */
export function parsePlaintext(p: Uint8Array): { seq: bigint; sig: Uint8Array; text: Uint8Array } | undefined {
  if (p.length <= PLAINTEXT_OVERHEAD || p.length > PLAINTEXT_OVERHEAD + MAX_TEXT_BYTES || p[0] !== PLAINTEXT_VERSION) {
    return undefined;
  }
  const seq = new DataView(p.buffer, p.byteOffset + 1, 8).getBigUint64(0, true);
  return { seq, sig: p.slice(9, PLAINTEXT_OVERHEAD), text: p.slice(PLAINTEXT_OVERHEAD) };
}

export function boxAd(group: string, from: string, to: string): Uint8Array {
  return concat(labelled(BOX_LABEL, group), utf8.encode(from), ZERO, utf8.encode(to));
}

/** `nonce(24) ‖ mac(16) ‖ ciphertext` (Monocypher's `crypto_aead_lock` layout). */
export function seal(key: Uint8Array, nonce: Uint8Array, ad: Uint8Array, plaintext: Uint8Array): Uint8Array {
  const out = xchacha20poly1305(key, nonce, ad).encrypt(plaintext);
  const ct = out.subarray(0, out.length - MAC_BYTES);
  const mac = out.subarray(out.length - MAC_BYTES);
  return concat(nonce, mac, ct);
}

export function randomNonce(): Uint8Array {
  return new Uint8Array(randomBytes(NONCE_BYTES));
}

/** Opens a box made by `seal`; undefined if it is too short or does not authenticate. */
export function open(key: Uint8Array, ad: Uint8Array, sealed: Uint8Array): Uint8Array | undefined {
  if (sealed.length < NONCE_BYTES + MAC_BYTES) {
    return undefined;
  }
  const nonce = sealed.subarray(0, NONCE_BYTES);
  const mac = sealed.subarray(NONCE_BYTES, NONCE_BYTES + MAC_BYTES);
  const ct = sealed.subarray(NONCE_BYTES + MAC_BYTES);
  try {
    return xchacha20poly1305(key, nonce, ad).decrypt(concat(ct, mac));
  } catch {
    return undefined;
  }
}

/** The group a launcher signs its session key under (docs/social-protocol.md §2). */
export function sessionGroup(uid: string): string {
  return `dm-${uid}`;
}

/** The group of a DM conversation: both uids in byte order. */
export function dmGroup(a: string, b: string): string {
  return a <= b ? `dm-${a}-${b}` : `dm-${b}-${a}`;
}
