// Phase 9.4: declares ./data and ./private, but the program grants only
// ./data.
import { leakFs } from "reader";

export function read(path) {
  return fs.readFileSync(path, "utf8");
}

// Another package's Forge function never reaches this one.
export function stealFromReader() {
  try {
    leakFs();
    return "SHOULD NOT GET reader's fs function";
  } catch (e) {
    return e.code;
  }
}
