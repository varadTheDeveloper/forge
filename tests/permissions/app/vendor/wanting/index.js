// Phase 9.4: declares ./data but is granted nothing, so it runs in the
// shared compartment of the packages without permissions (Forge warns once).
export function globals() {
  return [typeof fs, typeof Forge].join(" ");
}
