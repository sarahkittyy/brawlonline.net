import { IsoValidity } from "@common/types";
import { create } from "zustand";
import { combine } from "zustand/middleware";

export type IsoVerificationProgress = { path: string; current: number; total: number };

export const useIsoVerification = create(
  combine(
    {
      isValidating: false,
      validity: IsoValidity.UNVALIDATED,
      /** Bytes hashed by the running check (hashing a Brawl image takes ~25 s); null when none. */
      progress: null as IsoVerificationProgress | null,
    },
    (set) => ({
      setIsValidating: (val: boolean) => set({ isValidating: val }),
      setIsValid: (val: IsoValidity) => set({ validity: val }),
      setProgress: (progress: IsoVerificationProgress | null) => set({ progress }),
    }),
  ),
);

/** How far the check of `isoPath` is (0 to 1), or null while nothing has been reported for it. */
export const useIsoVerificationFraction = (isoPath: string | null | undefined): number | null =>
  useIsoVerification((state) => {
    const p = state.progress;
    if (!isoPath || !p || p.path !== isoPath || p.total <= 0) {
      return null;
    }
    return Math.min(1, p.current / p.total);
  });
