// Code-mode SDK for the f3-recomp tools: f3recomp (static analysis) and f3rt-tool / the game binary (runtime).
// Zero dependencies; runs as-is under Node 22+, Bun, and Deno: import { openGame } from "./tools/sdk/index.ts".

export {
  Game,
  RecompClient,
  RecompError,
  closeDefaultRecompClient,
  defaultRecompClient,
  disasmHex,
  lowerHex,
  mergeProfiles,
  openGame,
  resolveRecompExecutable,
} from "./recomp.ts";
export type { ClientOptions, GameOptions, OpenGameOptions } from "./recomp.ts";

export { RunHandle, RunRecord, decodeSpriteList, dumpLocation, openRun, runGame, runMotionProof, runSpriteCheck } from "./runtime.ts";
export type { RunHandleInit } from "./runtime.ts";

export { compareImage, compareWav, decodeBmp, decodeImage, decodePng, encodeBmp, encodeWav, readWav } from "./media.ts";
export type { WavData } from "./media.ts";

export { runDifferential } from "./differential.ts";

export { REPO_ROOT } from "./util.ts";

export type * from "./types.ts";
