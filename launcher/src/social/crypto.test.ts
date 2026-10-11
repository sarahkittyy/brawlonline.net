import { describe, expect, it } from "vitest";

import { cleanBytes, cleanText, MAX_TEXT_BYTES } from "./chat_text";
import {
  boxAd,
  dmGroup,
  encodePlaintext,
  fromHex,
  identityPublic,
  isValidIdentityKey,
  kxPublic,
  open,
  pairKey,
  parsePlaintext,
  seal,
  signKx,
  signMessage,
  toHex,
  verifyKx,
  verifyMessage,
} from "./crypto";

// docs/chat-protocol.md §4's test vectors (also server/crates/common/src/chat.rs and Dolphin's
// ChatCryptoTest.cpp).
const GROUP = "room-KFQB-12";
const SENDER = "11111111-1111-4111-8111-111111111111";
const RECIPIENT = "22222222-2222-4222-8222-222222222222";
const TEXT = "hello \uff57\uff4f\uff52\uff4c\uff44 \u306d\u3053";
const bytes = (start: number, n: number) => new Uint8Array(Array.from({ length: n }, (_, i) => (start + i) & 0xff));

const V = {
  senderIdKey: "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8",
  recipientIdKey: "29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc9322966dd7",
  senderKx: "79a631eede1bf9c98f12032cdeadd0e7a079398fc786b88cc846ec89af85a51a",
  recipientKx: "675dd574ed7789310b3d2e7681f3790b466c773b1521fecf36577958371ea52f",
  senderKxSig:
    "47ec39ad5b0e785b3ad40d3afb9d876439e9cbfc6ccc4c36bc8e0e2778b11581ff51f27cce788697fdf08e404a897a7a4fff7b0b2f084cbfab00288ce939250d",
  pairKey: "b58346539534a6603561da2c02479171669efd69a12ccc8114ba6d570ec54fc5",
  messageSig:
    "655a160d056df1f602c0aa45e60e8c9002956eff9d3ec1f96c65a447d180715e748199ea5b8c356e62a120f1278ecd49dbd038dd5114d1d4f7115c2ac168660a",
  plaintext:
    "010100000000000000655a160d056df1f602c0aa45e60e8c9002956eff9d3ec1f96c65a447d180715e748199ea5b8c356e62a120f1278ecd49dbd038dd5114d1d4f7115c2ac168660a68656c6c6f20efbd97efbd8fefbd92efbd8cefbd8420e381ade38193",
  ad: "627261776c6f6e6c696e652d636861742d626f782d763100726f6f6d2d4b4651422d31320031313131313131312d313131312d343131312d383131312d3131313131313131313131310032323232323232322d323232322d343232322d383232322d323232323232323232323232",
  box: "808182838485868788898a8b8c8d8e8f90919293949596972006179349a1893a3f0f1207a3f24df6a4210813228f3094a7319f3945640c4c55a3a6b54d78bff9f56e520e248754480102d74ec7034e692b38caddc70c057d50bff23530fa4e1d4f4a563f23b3e9b4eecd2101486f3a1a5a0111b29cbdd0d3168dc69757a22428c3b191ac6cb7289fd9dd2f6722",
};

describe("chat crypto", () => {
  const senderSeed = bytes(0x00, 32);
  const recipientSeed = bytes(0x20, 32);
  const senderKxSecret = bytes(0x40, 32);
  const recipientKxSecret = bytes(0x60, 32);
  const text = new TextEncoder().encode(TEXT);

  it("matches the protocol's test vectors", () => {
    expect(toHex(identityPublic(senderSeed))).toBe(V.senderIdKey);
    expect(toHex(identityPublic(recipientSeed))).toBe(V.recipientIdKey);
    const skx = kxPublic(senderKxSecret);
    const rkx = kxPublic(recipientKxSecret);
    expect(toHex(skx)).toBe(V.senderKx);
    expect(toHex(rkx)).toBe(V.recipientKx);
    expect(toHex(signKx(senderSeed, GROUP, skx))).toBe(V.senderKxSig);
    const key = pairKey(GROUP, senderKxSecret, rkx)!;
    expect(toHex(key)).toBe(V.pairKey);
    expect(toHex(pairKey(GROUP, recipientKxSecret, skx)!)).toBe(V.pairKey);
    const sig = signMessage(senderSeed, GROUP, SENDER, 1n, text);
    expect(toHex(sig)).toBe(V.messageSig);
    const pt = encodePlaintext(1n, sig, text);
    expect(toHex(pt)).toBe(V.plaintext);
    const ad = boxAd(GROUP, SENDER, RECIPIENT);
    expect(toHex(ad)).toBe(V.ad);
    expect(toHex(seal(key, bytes(0x80, 24), ad, pt))).toBe(V.box);
  });

  it("opens and verifies the vector box, and refuses tampering", () => {
    const key = fromHex(V.pairKey)!;
    const ad = fromHex(V.ad)!;
    const sealed = fromHex(V.box)!;
    const pt = open(key, ad, sealed)!;
    const parsed = parsePlaintext(pt)!;
    expect(parsed.seq).toBe(1n);
    expect(new TextDecoder().decode(parsed.text)).toBe(TEXT);
    expect(verifyMessage(fromHex(V.senderIdKey)!, GROUP, SENDER, 1n, parsed.text, parsed.sig)).toBe(true);
    expect(verifyMessage(fromHex(V.senderIdKey)!, GROUP, SENDER, 2n, parsed.text, parsed.sig)).toBe(false);
    expect(verifyKx(fromHex(V.senderIdKey)!, GROUP, fromHex(V.senderKx)!, fromHex(V.senderKxSig)!)).toBe(true);
    expect(verifyKx(fromHex(V.senderIdKey)!, "room-KFQB-13", fromHex(V.senderKx)!, fromHex(V.senderKxSig)!)).toBe(
      false,
    );
    for (const i of [0, 30, sealed.length - 1]) {
      const bad = sealed.slice();
      bad[i] ^= 1;
      expect(open(key, ad, bad)).toBeUndefined();
    }
    expect(open(key, boxAd(GROUP, SENDER, SENDER), sealed)).toBeUndefined();
    expect(open(key, ad, sealed.subarray(0, 39))).toBeUndefined();
  });

  it("refuses small-order keys and bad plaintexts", () => {
    expect(pairKey(GROUP, senderKxSecret, new Uint8Array(32))).toBeUndefined();
    expect(isValidIdentityKey(fromHex(V.senderIdKey)!)).toBe(true);
    expect(isValidIdentityKey(new Uint8Array(32))).toBe(false);
    const ident = new Uint8Array(32);
    ident[0] = 1; // the identity point
    expect(isValidIdentityKey(ident)).toBe(false);
    const sig = new Uint8Array(64);
    expect(parsePlaintext(encodePlaintext(1n, sig, new Uint8Array(0)))).toBeUndefined();
    expect(parsePlaintext(encodePlaintext(1n, sig, new Uint8Array(MAX_TEXT_BYTES + 1)))).toBeUndefined();
    const v2 = encodePlaintext(1n, sig, new Uint8Array([65]));
    v2[0] = 2;
    expect(parsePlaintext(v2)).toBeUndefined();
    expect(fromHex("abc")).toBeUndefined();
    expect(fromHex("zz")).toBeUndefined();
    expect(fromHex("00", 2)).toBeUndefined();
    expect(fromHex(7)).toBeUndefined();
  });

  it("names DM groups the same from both sides", () => {
    expect(dmGroup(SENDER, RECIPIENT)).toBe(`dm-${SENDER}-${RECIPIENT}`);
    expect(dmGroup(RECIPIENT, SENDER)).toBe(dmGroup(SENDER, RECIPIENT));
  });
});

describe("chat text", () => {
  const clean = (s: string) => cleanText(s);

  it("drops invalid UTF-8", () => {
    const cases = [
      [0xc0, 0xaf],
      [0xe0, 0x80, 0xaf],
      [0xed, 0xa0, 0x80],
      [0xf4, 0x90, 0x80, 0x80],
      [0x6f, 0x6b, 0x20, 0xe3, 0x81],
      [0xff],
      [0x61, 0x80, 0x62],
    ];
    for (const c of cases) {
      expect(cleanBytes(new Uint8Array(c))).toBeUndefined();
    }
    expect(cleanText("a\ud800b")).toBeUndefined();
  });

  it("removes controls and invisible characters", () => {
    expect(clean("a\u0000b\u0007c\td\ne\u007ff\u0080g\u009fh")).toBe("abcdefgh");
    expect(clean("a\u00ad\u034f\u061c\u115f\u1160\u17b4\u17b5\u180b\u180fb")).toBe("ab");
    expect(clean("x\u200b\u200f\u2028\u202e\u2060\u206fy")).toBe("xy");
    expect(clean("x\u3164\ufe00\ufe0f\ufeff\uffa0\ufff0\ufffby")).toBe("xy");
    expect(clean(`x${String.fromCodePoint(0xe000, 0xf8ff, 0x1d173, 0xe0001, 0xe0fff, 0xf0000, 0x10fffd)}y`)).toBe("xy");
    expect(clean(`x${String.fromCodePoint(0xfdd0, 0xfdef, 0xfffe, 0xffff, 0x1fffe, 0x10ffff)}y`)).toBe("xy");
    expect(clean("\u200b\ufeff\u202e")).toBeUndefined();
    expect(clean("")).toBeUndefined();
  });

  it("collapses whitespace and limits combining marks", () => {
    expect(clean("  a \u00a0\u3000 b\u2003\u2003c   ")).toBe("a b c");
    expect(clean("a \u200b b")).toBe("a b");
    expect(clean("\u200b a")).toBe("a");
    expect(clean("e\u0301\u0302\u0303\u0304")).toBe("e\u0301\u0302");
    expect(clean("e\u0301\u200b\u0302\u0303")).toBe("e\u0301\u0302");
    expect(clean("e\u0301\u0302 \u0303\u0304\u0305")).toBe("e\u0301\u0302 \u0303\u0304");
  });

  it("cuts to 100 code points and 300 bytes, and is idempotent", () => {
    expect(clean("a".repeat(99) + " b")).toBe("a".repeat(99));
    expect(clean("a".repeat(150))).toBe("a".repeat(100));
    expect(clean("\u306d".repeat(150))).toBe("\u306d".repeat(100));
    const emoji = clean("\u{1f600}".repeat(100))!;
    expect(Buffer.byteLength(emoji)).toBe(300);
    expect([...emoji].length).toBe(75);
    for (const s of ["  hi  there ", "e\u0301\u0302\u0303", "a".repeat(99) + " b"]) {
      expect(clean(clean(s)!)).toBe(clean(s));
    }
  });
});
