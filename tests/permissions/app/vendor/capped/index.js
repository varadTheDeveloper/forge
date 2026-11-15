// Phase 9.4: declares ./other and the program grants it, but the command
// line does not allow it: the person running Forge always has the last word.
export function read(path) {
  return fs.readFileSync(path, "utf8");
}
