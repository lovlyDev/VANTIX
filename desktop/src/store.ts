import { create } from "zustand";

export type Device = {
  id: string;
  name: string;
  vendor: string;
  driver: string;
  memory_bytes: number;
  apis: string[];
};
export type ValidatedBackend = { api: string; device_id: string; name: string };
export type Backend = "auto" | "cpu" | "vulkan" | "cuda" | "hip" | "hybrid";
export type Scheme = "12" | "24";
export type Wallet = "v5" | "v4";
export type MatchMode = "contains" | "prefix" | "suffix";
export type Status = "idle" | "initializing" | "running" | "paused" | "stopping" | "finished" | "error";

type State = {
  devices: Device[];
  validatedBackends: ValidatedBackend[];
  selectedDeviceId: string;
  status: Status;
  backend: Backend;
  scheme: Scheme;
  wallet: Wallet;
  matchMode: MatchMode;
  pattern: string;
  ignoreCase: boolean;
  gpuDuty: number;
  checked: number;
  rate: number;
  samples: number[];
  resultPath: string;
  error: string;
  set: (patch: Partial<State>) => void;
  addSample: (rate: number) => void;
};

export const useApp = create<State>((set) => ({
  devices: [],
  validatedBackends: [],
  selectedDeviceId: "",
  status: "idle",
  backend: "auto",
  scheme: "12",
  wallet: "v5",
  matchMode: "contains",
  pattern: "",
  ignoreCase: false,
  gpuDuty: 85,
  checked: 0,
  rate: 0,
  samples: [],
  resultPath: "",
  error: "",
  set: (patch) => set(patch),
  addSample: (rate) => set((state) => ({
    rate,
    samples: [...state.samples.slice(-59), rate]
  }))
}));
