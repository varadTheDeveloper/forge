// A package whose export changes after it finished evaluating. Importers in
// another compartment see the value it had then (PHASE9_DESIGN.md, 5).
export let count = 0;
export function bump() {
  count += 1;
  return count;
}
