import electronLog from "electron-log";
import { mkdir, readFile, rename, writeFile } from "node:fs/promises";
import path from "path";

const log = electronLog.scope("accounts/sessions");

/** Encrypts/decrypts at rest. In the app this is Electron's `safeStorage`. */
export type Cipher = {
  isEncryptionAvailable(): boolean;
  encryptString(plain: string): Buffer;
  decryptString(encrypted: Buffer): string;
};

type SessionFile = {
  version: 1;
  /** uid -> token. Prefixed `enc:` (base64 of the OS-encrypted bytes) or `plain:`. */
  sessions: Record<string, string>;
};

/**
 * Stores one opaque session token per account, the way Firebase persisted its
 * refresh tokens for Slippi's multi-account login. Tokens are encrypted with the
 * OS keychain (Electron safeStorage) when it is available.
 */
export class SessionStore {
  private cache: Record<string, string> | null = null;

  constructor(private readonly filePath: string, private readonly cipher: Cipher | null) {}

  static defaultPath(userDataDir: string): string {
    return path.join(userDataDir, "sessions.json");
  }

  async get(uid: string): Promise<string | undefined> {
    const sessions = await this._load();
    const stored = sessions[uid];
    if (!stored) {
      return undefined;
    }
    try {
      return this._decode(stored);
    } catch (err) {
      log.warn(`Could not decrypt the stored session for ${uid}; it will need a new login`, err);
      return undefined;
    }
  }

  async set(uid: string, token: string): Promise<void> {
    const sessions = await this._load();
    sessions[uid] = this._encode(token);
    await this._save(sessions);
  }

  async delete(uid: string): Promise<void> {
    const sessions = await this._load();
    if (uid in sessions) {
      delete sessions[uid];
      await this._save(sessions);
    }
  }

  private _encode(token: string): string {
    if (this.cipher && this.cipher.isEncryptionAvailable()) {
      return `enc:${this.cipher.encryptString(token).toString("base64")}`;
    }
    return `plain:${token}`;
  }

  private _decode(stored: string): string {
    if (stored.startsWith("enc:")) {
      if (!this.cipher) {
        throw new Error("no cipher");
      }
      return this.cipher.decryptString(Buffer.from(stored.slice(4), "base64"));
    }
    if (stored.startsWith("plain:")) {
      return stored.slice(6);
    }
    throw new Error("unknown session encoding");
  }

  private async _load(): Promise<Record<string, string>> {
    if (this.cache) {
      return this.cache;
    }
    try {
      const parsed = JSON.parse(await readFile(this.filePath, "utf8")) as SessionFile;
      this.cache = parsed && parsed.version === 1 && parsed.sessions ? { ...parsed.sessions } : {};
    } catch {
      this.cache = {};
    }
    return this.cache;
  }

  private async _save(sessions: Record<string, string>): Promise<void> {
    this.cache = sessions;
    const data: SessionFile = { version: 1, sessions };
    await mkdir(path.dirname(this.filePath), { recursive: true });
    const tmp = `${this.filePath}.tmp`;
    await writeFile(tmp, JSON.stringify(data, null, 2), { mode: 0o600 });
    await rename(tmp, this.filePath);
  }
}
