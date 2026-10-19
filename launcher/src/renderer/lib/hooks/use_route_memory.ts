import { create } from "zustand";

interface RouteMemoryState {
  routeMemory: Record<string, string>;
  setLastRoute: (routeId: string, path: string) => void;
  clearLastRoute: (routeId: string) => void;
}

export const useRouteMemory = create<RouteMemoryState>((set) => ({
  routeMemory: {},
  setLastRoute: (routeId, path) => set((s) => ({ routeMemory: { ...s.routeMemory, [routeId]: path } })),
  clearLastRoute: (routeId) =>
    set((s) => {
      const { [routeId]: _, ...rest } = s.routeMemory;
      return { routeMemory: rest };
    }),
}));
